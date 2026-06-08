/**
 * MusicBrainz resolver — DB-driven metadata resolution.
 *
 * Producer-consumer pipeline:
 * - Triage: split albums into tagged (have MUSICBRAINZ_ALBUMID) vs untagged
 * - Producer: fingerprint thread pool processes untagged albums, pushes results
 *   to a GAsyncQueue
 * - Consumer: single-threaded batch resolver drains queue, fetches release
 *   metadata from PG in batches of 50, writes to SQLite
 *
 * Tagged albums bypass fingerprinting entirely — their release_ids go straight
 * into the resolve queue.
 *
 * No HTTP. No file modifications. All data stays in the database.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/database.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include <libavformat/avformat.h>

// Various Artists MB ID
#define VA_MUSICBRAINZ_ID "89ad4ac3-39f7-470e-963a-56509c546377"

// Queue drain timeout (microseconds) — how long batch consumer waits for more items
#define QUEUE_DRAIN_TIMEOUT_US (200 * 1000)  // 200ms

// =============================================================================
// Resolution tier tracking
// =============================================================================

typedef enum {
    RESOLVE_TIER_TAGGED,    // Had MUSICBRAINZ_ALBUMID tag in file metadata
    RESOLVE_TIER_ISRC,      // Resolved via ISRC lookup against MB PG
    RESOLVE_TIER_SOLR,      // Resolved via Solr text search
    RESOLVE_TIER_ACOUSTID,  // Resolved via AcoustID fingerprinting
    RESOLVE_TIER_NONE,      // No match found
    RESOLVE_TIER_COUNT
} resolve_tier_t;

// =============================================================================
// Resolver Context
// =============================================================================

struct mb_resolver {
    quadrature_db_t* db;

    /* Backend instances. The resolver creates 3 separate backends, all from
     * the same URI: one for the main batch fetch loop, one for the prefetch
     * thread, and one with N slots for the fingerprint workers. Each backend
     * owns its own connection pool so they can run concurrently without
     * cross-locking. */
    mb_backend_t* backend;             /* Main batch consumer (1 slot) */
    mb_conn_t*    backend_conn;        /* Cached slot 0 of backend */
    mb_backend_t* backend_prefetch;    /* Prefetch overlap (1 slot, may be NULL) */
    mb_conn_t*    backend_prefetch_conn; /* Cached slot 0 of backend_prefetch */
    mb_backend_t* backend_fp_pool;     /* Fingerprint workers (N slots, may be NULL) */

    char* solr_url;                    /* Solr URL (set if backend has SOLR cap) */
    char* acoustid_index_url;          /* acoustid-index HTTP URL (may be NULL) */
    mb_resolver_options_t options;
    mb_resolver_progress_cb callback;
    void* user_data;

    char* library_root;
    char* data_root;                    /* Where meta DB lives (may equal library_root) */
    quadrature_meta_db_t* meta_db;      /* Recording relations DB (may be NULL — non-fatal) */

    mb_resolver_progress_t progress;
    GMutex progress_mutex;

    /* Per-tier resolution stats (updated from worker threads via __atomic builtins) */
    volatile size_t tier_count[RESOLVE_TIER_COUNT];
    volatile int64_t tier_ns[RESOLVE_TIER_COUNT];

    volatile bool cancelled;
};

// =============================================================================
// Resolve Queue Item (passed from fingerprint producer → batch consumer)
// =============================================================================

typedef struct {
    int64_t album_id;
    char* release_id;  /* owned, may be NULL if fingerprint found no match */
    bool service_error; /* true if lookup failed due to service unavailability */
} resolve_queue_item_t;

// =============================================================================
// Profiling — zero-overhead pipeline timing (vDSO clock_gettime ~25ns/call)
// =============================================================================

typedef struct {
    size_t batch_count;
    size_t albums_written;

    // Batch-level (nanoseconds, accumulated)
    int64_t queue_wait_ns;
    int64_t triage_ns;
    int64_t pg_fetch_ns;
    int64_t sqlite_write_ns;

    // Per-album breakdown (accumulated across all albums)
    int64_t get_tracks_ns;
    int64_t match_tracks_ns;
    int64_t artist_lookup_ns;    // all db_get_or_create_artist calls
    int64_t album_update_ns;     // db_update_album_artist + db_update_album_mb
    int64_t track_update_ns;     // db_update_track_title + db_set_track_artists
    int64_t fts_sync_ns;
    int64_t meta_album_ns;       // meta DB writes per album
} mb_profile_stats_t;

static inline int64_t profile_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void resolve_queue_item_free(resolve_queue_item_t* item) {
    if (!item) return;
    g_free(item->release_id);
    g_free(item);
}

// =============================================================================
// Progress Helpers
// =============================================================================

static void resolver_update_progress(mb_resolver_t* ctx) {
    if (!ctx->callback) return;

    g_mutex_lock(&ctx->progress_mutex);
    mb_resolver_progress_t copy = ctx->progress;
    g_mutex_unlock(&ctx->progress_mutex);

    ctx->callback(&copy, ctx->user_data);
}

static void resolver_set_phase(mb_resolver_t* ctx, mb_resolve_phase_t phase) {
    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.phase = phase;
    g_mutex_unlock(&ctx->progress_mutex);
    resolver_update_progress(ctx);
}

/**
 * Convert MB artist credits to db_track_artist_t array.
 * @param artist_cache  Optional MBID→artist_id cache (may be NULL).
 *                      Populated on cache miss; caller owns the table.
 */
static void mb_credits_to_track_artists(
    quadrature_db_t* db,
    const mb_artist_t* credits, size_t count,
    db_track_artist_t** out, size_t* out_count,
    GHashTable* artist_cache) {

    *out = g_new0(db_track_artist_t, count);
    *out_count = count;

    for (size_t i = 0; i < count; i++) {
        int64_t artist_id = 0;

        // Check cache first (keyed on MBID)
        if (artist_cache && credits[i].id && credits[i].id[0]) {
            gpointer cached = g_hash_table_lookup(artist_cache, credits[i].id);
            if (cached) {
                artist_id = (int64_t)GPOINTER_TO_SIZE(cached);
            }
        }

        if (artist_id == 0) {
            artist_id = db_get_or_create_artist(
                db, credits[i].name, credits[i].sort_name, credits[i].id);

            // Populate cache
            if (artist_cache && credits[i].id && credits[i].id[0] && artist_id > 0) {
                g_hash_table_insert(artist_cache,
                    g_strdup(credits[i].id), GSIZE_TO_POINTER((gsize)artist_id));
            }
        }

        (*out)[i].artist_id   = artist_id;
        (*out)[i].name        = credits[i].name ? g_strdup(credits[i].name) : NULL;
        (*out)[i].position    = (int)i;
        (*out)[i].join_phrase = credits[i].joinphrase ? g_strdup(credits[i].joinphrase) : g_strdup("");
    }
}

// =============================================================================
// String Similarity (for track matching fallback)
// =============================================================================

static double title_similarity(const char* a, const char* b) {
    if (!a || !b) return 0.0;

    char* la = g_utf8_strdown(a, -1);
    char* lb = g_utf8_strdown(b, -1);

    // Simple Jaccard on words
    char** wa = g_strsplit_set(la, " \t-_()[]", -1);
    char** wb = g_strsplit_set(lb, " \t-_()[]", -1);

    size_t na = g_strv_length(wa);
    size_t nb = g_strv_length(wb);
    if (na == 0 || nb == 0) {
        g_strfreev(wa);
        g_strfreev(wb);
        g_free(la);
        g_free(lb);
        return 0.0;
    }

    size_t matches = 0;
    for (size_t i = 0; i < na; i++) {
        if (!wa[i][0]) continue;
        for (size_t j = 0; j < nb; j++) {
            if (!wb[j][0]) continue;
            if (g_strcmp0(wa[i], wb[j]) == 0) {
                matches++;
                break;
            }
        }
    }

    double sim = (double)matches / (double)(na > nb ? na : nb);
    g_strfreev(wa);
    g_strfreev(wb);
    g_free(la);
    g_free(lb);
    return sim;
}

// =============================================================================
// Lightweight Tag Reading (no audio decode, ~1ms per file)
// =============================================================================

/**
 * Tags read from an audio file for resolution purposes.
 * All fields are owned strings (caller frees with resolve_tags_free).
 */
typedef struct {
    char* isrc;            // ISRC code (may be NULL)
    char* album;           // Album title from metadata tag (may be NULL)
    char* artist;          // Artist name from metadata tag (may be NULL)
    char* album_artist;    // Album artist from metadata tag (may be NULL)
} resolve_tags_t;

static void resolve_tags_free(resolve_tags_t* tags) {
    if (!tags) return;
    g_free(tags->isrc);
    g_free(tags->album);
    g_free(tags->artist);
    g_free(tags->album_artist);
}

/**
 * Read resolution-relevant tags from an audio file.
 * Uses FFmpeg format context only — no codec open, no audio decode.
 */
static void read_resolve_tags(const char* audio_path, resolve_tags_t* out) {
    memset(out, 0, sizeof(*out));

    // Tags live in container headers — available after avformat_open_input()
    // without the expensive avformat_find_stream_info() media probing.
    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) != 0)
        return;

    AVDictionaryEntry* tag = NULL;
    while ((tag = av_dict_get(fmt->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if ((strcasecmp(tag->key, "isrc") == 0 ||
             strcasecmp(tag->key, "TSRC") == 0) && !out->isrc) {
            out->isrc = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "album") == 0 && !out->album) {
            out->album = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "artist") == 0 && !out->artist) {
            out->artist = g_strdup(tag->value);
        } else if ((strcasecmp(tag->key, "album_artist") == 0 ||
                    strcasecmp(tag->key, "albumartist") == 0) && !out->album_artist) {
            out->album_artist = g_strdup(tag->value);
        }
    }

    avformat_close_input(&fmt);
}

// =============================================================================
// Release Matching (ISRC → Solr text search → AcoustID fingerprint)
// =============================================================================

/**
 * Dump full per-release vote distribution at decision time.
 *
 * Diagnostic only — runs at g_debug() level. Use to understand why one
 * release MBID won over another within the same release-group, especially
 * in the ISRC/AcoustID vote-tally path which has no Picard-style scoring
 * (status, country, format, date) and picks purely by vote count.
 *
 * Output (one g_debug per line):
 *   vote-dump album=<id> tier=<stage> release_groups=<N>
 *     RG <rgid> votes=<n> picked_release=<rel> [WINNING_RG]
 *     release <rel> votes=<n> [PICKED]
 */
static gint cmp_count_desc(gconstpointer a, gconstpointer b, gpointer counts) {
    int va = GPOINTER_TO_INT(g_hash_table_lookup((GHashTable *)counts, a));
    int vb = GPOINTER_TO_INT(g_hash_table_lookup((GHashTable *)counts, b));
    return vb - va;
}

static void dump_vote_distribution(int64_t album_id, const char *stage,
                                    GHashTable *rg_counts,
                                    GHashTable *rg_best_release,
                                    GHashTable *release_counts,
                                    const char *picked_rg,
                                    const char *picked_release) {
    guint rg_total = g_hash_table_size(rg_counts);
    if (rg_total == 0) return;

    g_debug("vote-dump album=%" G_GINT64_FORMAT " tier=%s release_groups=%u",
            album_id, stage, rg_total);

    GList *rg_list = g_hash_table_get_keys(rg_counts);
    rg_list = g_list_sort_with_data(rg_list, cmp_count_desc, rg_counts);
    for (GList *r = rg_list; r; r = r->next) {
        const char *rg = r->data;
        int votes = GPOINTER_TO_INT(g_hash_table_lookup(rg_counts, rg));
        const char *rg_pick = g_hash_table_lookup(rg_best_release, rg);
        gboolean winning = (picked_rg && g_strcmp0(picked_rg, rg) == 0);
        g_debug("  RG %s votes=%d picked_release=%s%s",
                rg, votes, rg_pick ? rg_pick : "(none)",
                winning ? " [WINNING_RG]" : "");
    }
    g_list_free(rg_list);

    GList *rel_list = g_hash_table_get_keys(release_counts);
    rel_list = g_list_sort_with_data(rel_list, cmp_count_desc, release_counts);
    for (GList *r = rel_list; r; r = r->next) {
        const char *rel = r->data;
        int votes = GPOINTER_TO_INT(g_hash_table_lookup(release_counts, rel));
        gboolean picked = (picked_release && g_strcmp0(picked_release, rel) == 0);
        g_debug("    release %s votes=%d%s",
                rel, votes, picked ? " [PICKED]" : "");
    }
    g_list_free(rel_list);
}

/**
 * Find the best-matching MusicBrainz release for an album.
 * Fallback chain: ISRC → Solr text search → AcoustID fingerprint.
 * Uses per-thread PG connections and persistent HTTP connection from the pool.
 *
 * @param ctx        Resolver context (for library_root, cancel)
 * @param album_id   Album to fingerprint
 * @param mb_pg      Per-thread MusicBrainz PG client
 * @param acoustid_pg Per-thread AcoustID PG client (may be NULL)
 * @param http_conn      Per-thread persistent HTTP connection to acoustid-index (may be NULL)
 * @param service_error  Out: set to true if lookup failed due to service unavailability
 * @return release_id string (caller owns) or NULL if no match
 */
/**
 * Tally a set of (release_id, release_group_id) results into voting tables.
 * One vote per release_group per call (de-duplicated).
 * Updates best_rg / best_rg_count if a group takes the lead.
 */
static void tally_votes(const mb_acoustid_response_t* response,
                         GHashTable* rg_counts,
                         GHashTable* rg_best_release,
                         GHashTable* release_counts,
                         char** best_rg, int* best_rg_count) {
    GHashTable* seen_rgs = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t j = 0; j < response->count; j++) {
        const char* rg_id = response->results[j].release_group_id;
        const char* rel_id = response->results[j].release_id;
        if (!rg_id || !rel_id) continue;

        if (!g_hash_table_contains(seen_rgs, rg_id)) {
            g_hash_table_add(seen_rgs, (gpointer)rg_id);
            gpointer cnt = g_hash_table_lookup(rg_counts, rg_id);
            int new_count = GPOINTER_TO_INT(cnt) + 1;
            g_hash_table_insert(rg_counts, g_strdup(rg_id),
                                GINT_TO_POINTER(new_count));
            if (new_count > *best_rg_count) {
                *best_rg_count = new_count;
                g_free(*best_rg);
                *best_rg = g_strdup(rg_id);
            }
        }

        gpointer rcnt = g_hash_table_lookup(release_counts, rel_id);
        int new_rcnt = GPOINTER_TO_INT(rcnt) + 1;
        g_hash_table_insert(release_counts, g_strdup(rel_id),
                            GINT_TO_POINTER(new_rcnt));

        const char* cur_best = g_hash_table_lookup(rg_best_release, rg_id);
        if (!cur_best || new_rcnt > GPOINTER_TO_INT(
                g_hash_table_lookup(release_counts, cur_best))) {
            g_hash_table_insert(rg_best_release, g_strdup(rg_id),
                                g_strdup(rel_id));
        }
    }
    g_hash_table_destroy(seen_rgs);
}

static char* find_release_by_fingerprint(mb_resolver_t* ctx, int64_t album_id,
                                          mb_backend_t* be,
                                          mb_conn_t* conn,
                                          bool fingerprint_capable,
                                          bool* service_error) {
    g_assert(be != NULL);
    g_assert(conn != NULL);
    db_track_t* tracks = NULL;
    size_t track_count = 0;
    if (db_get_tracks_by_album(ctx->db, album_id, &tracks, &track_count) != QUADRATURE_OK
        || track_count == 0) {
        return NULL;
    }

    // Pre-compute total duration for text search validation
    int64_t total_duration_ms = 0;
    for (size_t i = 0; i < track_count; i++)
        total_duration_ms += tracks[i].duration_ms;

    // Vote on release_group_id (not release_id) to handle multiple editions
    GHashTable* rg_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable* rg_best_release = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable* release_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char* best_rg = NULL;
    int best_rg_count = 0;
    bool isrc_resolved = false;
    size_t fingerprinted = 0;

    // Album/artist from metadata tags (for text search — NOT from folder name)
    char* tag_album = NULL;
    char* tag_artist = NULL;

    // =========================================================================
    // Stage 1: Read tags + ISRC lookup
    // Read ISRCs from all tracks in one pass. Also capture album/artist tags
    // from the first file for the text search fallback (Stage 3).
    // =========================================================================

    int64_t stage_t0 = profile_now_ns();

    if (!ctx->cancelled && ctx->library_root) {
        const char** isrcs = g_new0(const char*, track_count);
        resolve_tags_t* all_tags = g_new0(resolve_tags_t, track_count);
        size_t isrc_count = 0;

        for (size_t i = 0; i < track_count && !ctx->cancelled; i++) {
            if (!tracks[i].path || !tracks[i].album_path) continue;
            char* audio_path = g_build_filename(ctx->library_root,
                                                 tracks[i].album_path,
                                                 tracks[i].path, NULL);
            read_resolve_tags(audio_path, &all_tags[i]);
            g_free(audio_path);

            if (all_tags[i].isrc) {
                isrcs[isrc_count] = all_tags[i].isrc;
                isrc_count++;
            }

            // Capture album/artist from first file that has them
            if (!tag_album && all_tags[i].album)
                tag_album = g_strdup(all_tags[i].album);
            if (!tag_artist) {
                if (all_tags[i].album_artist)
                    tag_artist = g_strdup(all_tags[i].album_artist);
                else if (all_tags[i].artist)
                    tag_artist = g_strdup(all_tags[i].artist);
            }
        }

        // Folder-path fallback (Picard's album_artist_from_path equivalent):
        // When files have no album/artist tags, extract from the album's directory
        // path. For "BRONSON/BRONSON (2020)/01.flac", album_path = "BRONSON/BRONSON (2020)"
        // → artist = "BRONSON", album = "BRONSON (2020)" → strip year → "BRONSON".
        if (!tag_album && track_count > 0 && tracks[0].album_path) {
            const char* apath = tracks[0].album_path;
            const char* last_sep = strrchr(apath, '/');
            if (last_sep && last_sep > apath) {
                tag_album = g_strdup(last_sep + 1);

                char* parent = g_strndup(apath, last_sep - apath);
                const char* artist_start = strrchr(parent, '/');
                if (!tag_artist)
                    tag_artist = g_strdup(artist_start ? artist_start + 1 : parent);
                g_free(parent);

                // Strip trailing year suffix: "Album (2020)" → "Album"
                char* paren = strrchr(tag_album, '(');
                if (paren && paren > tag_album && *(paren - 1) == ' '
                    && strlen(paren) == 6 && paren[5] == ')'
                    && g_ascii_isdigit(paren[1]) && g_ascii_isdigit(paren[2])
                    && g_ascii_isdigit(paren[3]) && g_ascii_isdigit(paren[4])) {
                    *(paren - 1) = '\0';
                }

                g_debug("folder-path fallback: album='%s' artist='%s' (from '%s')",
                        tag_album, tag_artist, apath);
            }
        }

        if (isrc_count >= 2) {
            mb_acoustid_response_t isrc_response;
            quadrature_result_t isrc_res = mb_backend_isrc_lookup(be, conn,
                                                                   isrcs, isrc_count,
                                                                   &isrc_response);
            if (isrc_res == QUADRATURE_OK && isrc_response.count > 0) {
                tally_votes(&isrc_response, rg_counts, rg_best_release,
                            release_counts, &best_rg, &best_rg_count);
                mb_acoustid_response_free(&isrc_response);

                double confidence = (double)best_rg_count / (double)isrc_count;
                if (confidence >= MB_MATCH_CONFIDENCE) {
                    const char *isrc_pick = g_hash_table_lookup(rg_best_release, best_rg);
                    g_debug("ISRC lookup resolved album %" G_GINT64_FORMAT
                            " → %s (%.0f%% confidence, %zu ISRCs)",
                            album_id, best_rg, confidence * 100, isrc_count);
                    dump_vote_distribution(album_id, "ISRC", rg_counts,
                                            rg_best_release, release_counts,
                                            best_rg, isrc_pick);
                    isrc_resolved = true;
                }
            } else if (isrc_res == QUADRATURE_OK) {
                mb_acoustid_response_free(&isrc_response);
            }
        }

        for (size_t i = 0; i < track_count; i++)
            resolve_tags_free(&all_tags[i]);
        g_free(all_tags);
        g_free(isrcs);
    }

    int64_t isrc_ns = profile_now_ns() - stage_t0;

    // =========================================================================
    // Stage 2: Solr text search (skip if ISRC already resolved)
    // Runs BEFORE fingerprinting — ~5-10ms vs ~10-20s per album.
    // Uses metadata tags, NOT folder name — folder names often contain year
    // suffixes like "Solace (2018)" that don't match MusicBrainz.
    // =========================================================================

    char* best_release = NULL;
    resolve_tier_t tier = RESOLVE_TIER_NONE;

    if (isrc_resolved && best_rg) {
        const char* rel = g_hash_table_lookup(rg_best_release, best_rg);
        if (rel) {
            best_release = g_strdup(rel);
            tier = RESOLVE_TIER_ISRC;
        }
    }

    stage_t0 = profile_now_ns();

    if (!best_release && !ctx->cancelled
        && (be->caps & MB_CAP_SOLR_SEARCH)
        && tag_album && tag_artist) {
        mb_backend_solr_search(be, conn, tag_album, tag_artist,
                                track_count, total_duration_ms, &best_release);
        if (best_release) {
            tier = RESOLVE_TIER_SOLR;
            g_debug("Solr search resolved album %" G_GINT64_FORMAT
                    " '%s' by '%s' → %s",
                    album_id, tag_album, tag_artist, best_release);
        }
    }

    int64_t solr_ns = profile_now_ns() - stage_t0;

    // =========================================================================
    // Stage 3: Fingerprint + AcoustID (last resort — only if ISRC + Solr missed)
    // =========================================================================

    stage_t0 = profile_now_ns();

    if (!best_release && !isrc_resolved && !ctx->cancelled
        && fingerprint_capable && (be->caps & MB_CAP_FINGERPRINT)) {
        size_t tracks_to_check = track_count < (size_t)MB_FINGERPRINT_TRACKS
            ? track_count : (size_t)MB_FINGERPRINT_TRACKS;

        for (size_t i = 0; i < tracks_to_check && !ctx->cancelled; i++) {
            if (!tracks[i].path) continue;
            if (!ctx->library_root || !tracks[i].album_path) continue;
            char* audio_path = g_build_filename(ctx->library_root,
                                                 tracks[i].album_path,
                                                 tracks[i].path, NULL);

            mb_fingerprint_t fp = {0};
            quadrature_result_t fp_res = mb_fingerprint_generate(audio_path, &fp);
            g_free(audio_path);
            if (fp_res != QUADRATURE_OK) continue;
            fingerprinted++;

            mb_acoustid_response_t response;
            quadrature_result_t lookup_res = mb_backend_fingerprint_lookup(be, conn,
                                                                            &fp, &response);
            if (lookup_res == QUADRATURE_ERROR_SERVICE_UNAVAILABLE) {
                if (service_error) *service_error = true;
                mb_fingerprint_free(&fp);
                break;
            }
            if (lookup_res == QUADRATURE_OK) {
                tally_votes(&response, rg_counts, rg_best_release,
                            release_counts, &best_rg, &best_rg_count);
                mb_acoustid_response_free(&response);
            }
            mb_fingerprint_free(&fp);

            // Early exit: if first 2+ fingerprints all agree on same release group
            if (fingerprinted >= 2 && best_rg_count > 0) {
                double confidence = (double)best_rg_count / (double)fingerprinted;
                if (confidence >= 1.0) break;
            }
        }

        // Pick release from fingerprint voting results
        if (fingerprinted > 0 && best_rg_count > 0) {
            double confidence = (double)best_rg_count / (double)fingerprinted;

            if (fingerprinted == 1) {
                // Single-track: require Solr cross-validation
                if ((be->caps & MB_CAP_SOLR_SEARCH) && tag_album && tag_artist) {
                    char* solr_rel = NULL;
                    mb_backend_solr_search(be, conn, tag_album, tag_artist,
                                            track_count, total_duration_ms,
                                            &solr_rel);
                    if (solr_rel) {
                        g_free(solr_rel);
                        const char* rel = g_hash_table_lookup(rg_best_release, best_rg);
                        if (rel) best_release = g_strdup(rel);
                    } else {
                        g_debug("Single-track album %" G_GINT64_FORMAT
                                " — fingerprint match not confirmed by Solr",
                                album_id);
                    }
                }
            } else if (confidence >= MB_MATCH_CONFIDENCE) {
                const char* rel = g_hash_table_lookup(rg_best_release, best_rg);
                if (rel) best_release = g_strdup(rel);
            }
            if (best_release) {
                tier = RESOLVE_TIER_ACOUSTID;
                dump_vote_distribution(album_id, "AcoustID", rg_counts,
                                        rg_best_release, release_counts,
                                        best_rg, best_release);
            }
        }
    }

    int64_t acoustid_ns = profile_now_ns() - stage_t0;

    // Update per-tier stats (GCC atomics — safe from worker threads)
    __atomic_fetch_add(&ctx->tier_count[tier], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ctx->tier_ns[RESOLVE_TIER_ISRC], isrc_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ctx->tier_ns[RESOLVE_TIER_SOLR], solr_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ctx->tier_ns[RESOLVE_TIER_ACOUSTID], acoustid_ns, __ATOMIC_RELAXED);

    db_tracks_free(tracks, track_count);

    g_free(tag_album);
    g_free(tag_artist);
    g_free(best_rg);
    g_hash_table_destroy(rg_counts);
    g_hash_table_destroy(rg_best_release);
    g_hash_table_destroy(release_counts);
    return best_release;
}

// =============================================================================
// Track Matching (MB recordings → local tracks)
// =============================================================================

typedef struct {
    size_t mb_idx;
    size_t local_idx;
    reconcile_confidence_t confidence;  /* EXACT = Pass 1, FUZZY = Pass 2 */
} track_match_t;

static void match_tracks(const mb_release_t* release,
                          const db_track_t* local_tracks, size_t local_count,
                          track_match_t** matches_out, size_t* match_count_out) {
    size_t max_matches = local_count < release->recording_count
        ? local_count : release->recording_count;

    track_match_t* matches = g_new0(track_match_t, max_matches);
    bool* mb_used = g_new0(bool, release->recording_count);
    bool* local_used = g_new0(bool, local_count);
    size_t matched = 0;

    // Pass 1: Exact match by (disc_number, position)
    // Only taken when both local and MB have a nonzero position — otherwise
    // ten untagged local tracks (all position 0) would all "match" MB's
    // position 0 if any existed.
    for (size_t i = 0; i < release->recording_count && matched < max_matches; i++) {
        if (release->recordings[i].position <= 0) continue;
        for (size_t j = 0; j < local_count; j++) {
            if (local_used[j]) continue;
            if (local_tracks[j].track_num <= 0) continue;
            if (release->recordings[i].disc_number == local_tracks[j].disc_num &&
                release->recordings[i].position == local_tracks[j].track_num) {
                matches[matched].mb_idx = i;
                matches[matched].local_idx = j;
                matches[matched].confidence = RECONCILE_CONFIDENCE_EXACT;
                mb_used[i] = true;
                local_used[j] = true;
                matched++;
                break;
            }
        }
    }

    // Pass 2: Score remaining by duration similarity + title similarity
    for (size_t i = 0; i < release->recording_count && matched < max_matches; i++) {
        if (mb_used[i]) continue;

        double best_score = 0.0;
        size_t best_j = 0;
        bool found = false;

        for (size_t j = 0; j < local_count; j++) {
            if (local_used[j]) continue;

            // Duration similarity (60% weight)
            double dur_diff = fabs((double)release->recordings[i].duration_ms -
                                   (double)local_tracks[j].duration_ms);
            double dur_score = 1.0 - (dur_diff / 30000.0); // 30s tolerance
            if (dur_score < 0.0) dur_score = 0.0;

            // Title similarity (40% weight)
            double title_score = title_similarity(
                release->recordings[i].title, local_tracks[j].title);

            double combined = dur_score * 0.6 + title_score * 0.4;
            if (combined > best_score) {
                best_score = combined;
                best_j = j;
                found = true;
            }
        }

        if (found && best_score > 0.5) {
            matches[matched].mb_idx = i;
            matches[matched].local_idx = best_j;
            matches[matched].confidence = RECONCILE_CONFIDENCE_FUZZY;
            mb_used[i] = true;
            local_used[best_j] = true;
            matched++;
        }
    }

    g_free(mb_used);
    g_free(local_used);

    *matches_out = matches;
    *match_count_out = matched;
}

// =============================================================================
// Parse Year from MB Date String
// =============================================================================

static uint16_t parse_mb_year(const char* date) {
    if (!date || strlen(date) < 4) return 0;
    int year = atoi(date);
    return (year > 0 && year < 10000) ? (uint16_t)year : 0;
}

// =============================================================================
// Resolved-album work item — prepare phase outputs these so the full batch
// can be passed to db_reconcile_albums in one call.
// =============================================================================

/**
 * All per-album state needed across prepare → reconcile → meta-write.
 * Owned memory is freed by album_work_free.
 */
typedef struct {
    bool                valid;          /* false → skip reconcile (failed/no-tracks) */
    int64_t             album_id;
    mb_release_t*       release;        /* borrowed from releases hashtable */
    mb_recording_link_row_t** links;    /* borrowed */
    size_t              link_count;

    /* Owned memory that must outlive the batch reconcile. */
    db_track_t*                tracks;
    size_t                     track_count;
    track_match_t*             matches;
    size_t                     match_count;
    desired_track_t*           desired_tracks;       /* references strings in tracks/release */
    desired_track_artist_t**   owned_credits;        /* per-matched-track credits */
    size_t*                    owned_credit_counts;

    desired_album_state_t      desired;              /* ready to hand to reconciler */
} album_work_t;

static void album_work_free(album_work_t* w) {
    if (!w) return;
    if (w->owned_credits) {
        for (size_t m = 0; m < w->match_count; m++) {
            if (!w->owned_credits[m]) continue;
            for (size_t k = 0; k < w->owned_credit_counts[m]; k++) {
                g_free((char*)w->owned_credits[m][k].name);
                g_free((char*)w->owned_credits[m][k].join_phrase);
            }
            g_free(w->owned_credits[m]);
        }
        g_free(w->owned_credits);
    }
    g_free(w->owned_credit_counts);
    g_free(w->desired_tracks);
    if (w->tracks) db_tracks_free(w->tracks, w->track_count);
    g_free(w->matches);
}

/**
 * Prepare one album's desired state from an MB release.
 *
 * Does all read/match/artist-resolve work. Does NOT call the reconciler — the
 * batch caller accumulates prepared works and calls db_reconcile_albums once.
 *
 * On unrecoverable failure (tracks missing), marks album MB_STATUS_FAILED via
 * the fast path and leaves work->valid = false.
 *
 * Confidence for track position writeback:
 *   - Pass 1 (exact disc+position match) → EXACT confidence
 *   - Pass 2 (duration + title similarity) → FUZZY confidence
 *   RECONCILE_POLICY_MB accepts both.
 */
static void resolve_album_prepare(mb_resolver_t* ctx, int64_t album_id,
                                   mb_release_t* release,
                                   mb_recording_link_row_t** links, size_t link_count,
                                   GHashTable* artist_cache,
                                   mb_profile_stats_t* stats,
                                   album_work_t* work) {
    int64_t t0, t1;
    work->album_id = album_id;
    work->release = release;
    work->links = links;
    work->link_count = link_count;
    work->valid = false;

    /* --- Load local tracks --- */
    t0 = profile_now_ns();
    quadrature_result_t res = db_get_tracks_by_album(ctx->db, album_id,
                                                      &work->tracks, &work->track_count);
    t1 = profile_now_ns();
    stats->get_tracks_ns += (t1 - t0);
    if (res != QUADRATURE_OK || work->track_count == 0) {
        db_reconcile_album_mb_status(ctx->db, album_id, MB_STATUS_FAILED, (int64_t)time(NULL));
        if (work->tracks) { db_tracks_free(work->tracks, work->track_count); work->tracks = NULL; }
        work->track_count = 0;
        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return;
    }

    /* --- Match MB recordings to local tracks --- */
    t0 = profile_now_ns();
    match_tracks(release, work->tracks, work->track_count,
                 &work->matches, &work->match_count);
    t1 = profile_now_ns();
    stats->match_tracks_ns += (t1 - t0);

    /* --- Resolve album artist --- */
    t0 = profile_now_ns();
    bool is_va = (release->artist_count > 0 && release->artists[0].id &&
                  g_strcmp0(release->artists[0].id, VA_MUSICBRAINZ_ID) == 0);

    int64_t album_artist_id = 0;
    if (release->artist_count > 0 && !is_va) {
        const char* mbid = release->artists[0].id;
        if (artist_cache && mbid && mbid[0]) {
            gpointer cached = g_hash_table_lookup(artist_cache, mbid);
            if (cached) album_artist_id = (int64_t)GPOINTER_TO_SIZE(cached);
        }
        if (album_artist_id == 0) {
            album_artist_id = db_get_or_create_artist(
                ctx->db, release->artists[0].name,
                release->artists[0].sort_name, mbid);
            if (artist_cache && mbid && mbid[0] && album_artist_id > 0)
                g_hash_table_insert(artist_cache,
                    g_strdup(mbid), GSIZE_TO_POINTER((gsize)album_artist_id));
        }
    } else if (is_va) {
        if (artist_cache) {
            gpointer cached = g_hash_table_lookup(artist_cache, VA_MUSICBRAINZ_ID);
            if (cached) album_artist_id = (int64_t)GPOINTER_TO_SIZE(cached);
        }
        if (album_artist_id == 0) {
            album_artist_id = db_get_or_create_artist(
                ctx->db, "Various Artists", "Various Artists", VA_MUSICBRAINZ_ID);
            if (artist_cache && album_artist_id > 0)
                g_hash_table_insert(artist_cache,
                    g_strdup(VA_MUSICBRAINZ_ID), GSIZE_TO_POINTER((gsize)album_artist_id));
        }
    }
    t1 = profile_now_ns();
    stats->artist_lookup_ns += (t1 - t0);

    /* --- Build per-track desired state (matched tracks only) --- */
    work->desired_tracks       = g_new0(desired_track_t, work->match_count);
    work->owned_credits        = g_new0(desired_track_artist_t*, work->match_count);
    work->owned_credit_counts  = g_new0(size_t, work->match_count);

    for (size_t m = 0; m < work->match_count; m++) {
        mb_recording_t* rec = &release->recordings[work->matches[m].mb_idx];
        db_track_t* local   = &work->tracks[work->matches[m].local_idx];

        desired_track_artist_t* credits = NULL;
        size_t credit_count = 0;
        if (rec->artist_count > 0) {
            int64_t art_t0 = profile_now_ns();
            db_track_artist_t* ta = NULL;
            size_t ta_count = 0;
            mb_credits_to_track_artists(ctx->db, rec->artists, rec->artist_count,
                                         &ta, &ta_count, artist_cache);
            int64_t art_t1 = profile_now_ns();
            stats->artist_lookup_ns += (art_t1 - art_t0);

            credits = g_new0(desired_track_artist_t, ta_count);
            for (size_t k = 0; k < ta_count; k++) {
                credits[k].artist_id   = ta[k].artist_id;
                credits[k].name        = ta[k].name;         /* ownership transferred */
                credits[k].join_phrase = ta[k].join_phrase;  /* ownership transferred */
                credits[k].position    = ta[k].position;
            }
            credit_count = ta_count;
            g_free(ta);
        }
        work->owned_credits[m]       = credits;
        work->owned_credit_counts[m] = credit_count;

        desired_track_t* dt = &work->desired_tracks[m];
        dt->path = local->path;  /* borrowed — valid until album_work_free */
        dt->present_fields = DESIRED_TRACK_TITLE | DESIRED_TRACK_NUM | DESIRED_TRACK_DISC;
        dt->title     = rec->title;
        dt->track_num = (uint16_t)rec->position;
        dt->disc_num  = (uint16_t)(rec->disc_number > 0 ? rec->disc_number : 1);
        dt->position_confidence = work->matches[m].confidence;

        if (credit_count > 0) {
            dt->present_fields |= DESIRED_TRACK_ARTISTS;
            dt->artists = credits;
            dt->artist_count = credit_count;
        }

        if (release->genres && release->genres[0]) {
            dt->present_fields |= DESIRED_TRACK_GENRE_MERGE;
            dt->genre = release->genres;
        }
    }

    /* --- Album-level desired state --- */
    uint16_t year = parse_mb_year(release->date);
    work->desired = (desired_album_state_t){
        .source         = RECONCILE_SOURCE_MB,
        .present_fields =
            DESIRED_ALBUM_TITLE            |
            DESIRED_ALBUM_MB_RELEASE_ID    |
            DESIRED_ALBUM_MB_RELEASE_GROUP |
            DESIRED_ALBUM_MB_STATUS        |
            DESIRED_ALBUM_MB_RESOLVED_AT   |
            (year > 0 ? DESIRED_ALBUM_YEAR : 0) |
            (album_artist_id > 0 ? (DESIRED_ALBUM_ARTIST_ID | DESIRED_ALBUM_COMPILATION) : 0),
        .title                        = release->title,
        .artist_id                    = album_artist_id,
        .is_compilation               = is_va,
        .year                         = year,
        .musicbrainz_release_id       = release->id,
        .musicbrainz_release_group_id = release->release_group_id,
        .mb_status                    = MB_STATUS_RESOLVED,
        .mb_resolved_at               = (int64_t)time(NULL),
        .tracks                       = work->desired_tracks,
        .track_count                  = work->match_count,
    };

    work->valid = true;
}

/**
 * Post-reconcile: write release + recording relations to quadrature-metadata.sqlite.
 * Non-fatal; runs inside the batch meta_db transaction.
 */
static void resolve_album_write_meta(mb_resolver_t* ctx, const album_work_t* w,
                                      mb_profile_stats_t* stats) {
    if (!ctx->meta_db || !w->valid) return;

    int64_t t0 = profile_now_ns();
    mb_release_t* release = w->release;
    db_meta_upsert_release(ctx->meta_db, release->id,
        release->date, release->type, release->label,
        release->catalog_number, release->barcode, release->genres);
    for (size_t m = 0; m < w->match_count; m++) {
        mb_recording_t* rec = &release->recordings[w->matches[m].mb_idx];
        db_meta_upsert_recording(ctx->meta_db,
            rec->id, release->id, rec->disc_number, rec->position);
        db_meta_delete_recording_links(ctx->meta_db, rec->id);
    }
    GHashTable* seen_link_types = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable* seen_artists    = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t i = 0; i < w->link_count; i++) {
        mb_recording_link_row_t* l = w->links[i];
        if (!g_hash_table_contains(seen_link_types, l->link_type_gid)) {
            db_meta_upsert_link_type(ctx->meta_db,
                l->link_type_gid, l->link_type_name, l->link_type_desc);
            g_hash_table_add(seen_link_types, (gpointer)l->link_type_gid);
        }
        if (!g_hash_table_contains(seen_artists, l->artist_mbid)) {
            db_meta_upsert_artist(ctx->meta_db,
                l->artist_mbid, l->artist_name, l->artist_sort_name, l->artist_type);
            g_hash_table_add(seen_artists, (gpointer)l->artist_mbid);
        }
        db_meta_insert_recording_link(ctx->meta_db,
            l->recording_mbid, l->artist_mbid, l->link_type_gid,
            l->entity0_credit, l->attributes);
    }
    g_hash_table_destroy(seen_link_types);
    g_hash_table_destroy(seen_artists);
    int64_t t1 = profile_now_ns();
    stats->meta_album_ns += (t1 - t0);
}

// =============================================================================
// Fingerprint Worker (producer thread pool)
// =============================================================================

typedef struct {
    int64_t album_id;
    GAsyncQueue* resolve_queue;
} fp_work_t;

/**
 * Claim a pool slot for this thread. Workers may outnumber PG connections
 * (fingerprinting is CPU-bound; PG is <1% of wall time). Multiple workers
 * sharing a slot use the per-slot mutex to serialize PG access.
 */
static GPrivate fp_slot_key = G_PRIVATE_INIT(NULL);

static int fp_claim_slot(mb_backend_t* be) {
    gpointer stored = g_private_get(&fp_slot_key);
    if (stored) return GPOINTER_TO_INT(stored) - 1;
    int slot = mb_backend_claim_round_robin(be);
    g_private_set(&fp_slot_key, GINT_TO_POINTER(slot + 1));
    return slot;
}

static void fp_worker(gpointer data, gpointer user_data) {
    fp_work_t* work = data;
    mb_resolver_t* ctx = user_data;
    if (ctx->cancelled) {
        g_free(work);
        return;
    }

    int slot = fp_claim_slot(ctx->backend_fp_pool);
    mb_conn_t* conn = mb_backend_claim_slot(ctx->backend_fp_pool, slot);

    bool svc_error = false;
    char* release_id = find_release_by_fingerprint(ctx, work->album_id,
                                                    ctx->backend_fp_pool, conn,
                                                    /*fingerprint_capable=*/true,
                                                    &svc_error);

    resolve_queue_item_t* item = g_new0(resolve_queue_item_t, 1);
    item->album_id = work->album_id;
    item->release_id = release_id;
    item->service_error = svc_error;
    g_async_queue_push(work->resolve_queue, item);

    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.fingerprint_processed++;
    if (svc_error)
        ctx->progress.acoustid_error = true;
    g_mutex_unlock(&ctx->progress_mutex);
    resolver_update_progress(ctx);

    g_free(work);
}

// =============================================================================
// Prefetch: overlap PG fetch with SQLite write
// =============================================================================

typedef struct {
    mb_backend_t* be;
    mb_conn_t*    conn;
    const char* const* release_ids;  /* borrowed from triage — valid until triage_free */
    size_t release_count;
    GHashTable* releases;        /* out: fetched releases (caller destroys) */
    GHashTable* links;           /* out: recording links (caller destroys) */
    quadrature_result_t result;
} prefetch_ctx_t;

static gpointer prefetch_thread_func(gpointer data) {
    prefetch_ctx_t* pf = data;
    pf->releases = NULL;
    pf->links = NULL;
    pf->result = mb_backend_batch_fetch(pf->be, pf->conn,
                                         (const char**)pf->release_ids,
                                         pf->release_count,
                                         &pf->releases, &pf->links);
    return NULL;
}

// =============================================================================
// Batch Consumer: drain queue and process batches
// =============================================================================

/**
 * Triage batch: separate items with release_ids from no-match items.
 * Returns owned copies of release_ids and album_ids (caller frees with
 * triage_free). Strings are copied so batch items can be freed independently.
 */
static size_t triage_batch(mb_resolver_t* ctx,
                            resolve_queue_item_t** batch, size_t batch_count,
                            char*** out_release_ids,
                            int64_t** out_album_ids) {
    char** release_ids = g_new0(char*, batch_count);
    int64_t* album_ids = g_new0(int64_t, batch_count);
    size_t release_count = 0;

    for (size_t i = 0; i < batch_count; i++) {
        if (batch[i]->release_id) {
            release_ids[release_count] = g_strdup(batch[i]->release_id);
            album_ids[release_count] = batch[i]->album_id;
            release_count++;
        } else if (batch[i]->service_error) {
            db_reconcile_album_mb_status(ctx->db, batch[i]->album_id,
                                    MB_STATUS_FAILED, (int64_t)time(NULL));
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.albums_failed++;
            ctx->progress.albums_processed++;
            g_mutex_unlock(&ctx->progress_mutex);
        } else {
            db_reconcile_album_mb_status(ctx->db, batch[i]->album_id,
                                    MB_STATUS_NO_MATCH, (int64_t)time(NULL));
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.albums_no_match++;
            ctx->progress.albums_processed++;
            g_mutex_unlock(&ctx->progress_mutex);
        }
    }
    *out_release_ids = release_ids;
    *out_album_ids = album_ids;
    return release_count;
}

static void triage_free(char** release_ids, int64_t* album_ids, size_t count) {
    for (size_t i = 0; i < count; i++)
        g_free(release_ids[i]);
    g_free(release_ids);
    g_free(album_ids);
}

/**
 * Write batch results to SQLite.
 *
 * Three phases inside one SQLite transaction:
 *   1. Prepare — per album: load tracks, match recordings, resolve artists,
 *      build desired_album_state. Albums with no local tracks are marked
 *      MB_STATUS_FAILED via the fast path; they don't enter the batch.
 *   2. Reconcile — single db_reconcile_albums call drives three bulk SELECTs
 *      (albums/tracks/track_artists via json_each) + per-album diff/UPDATE
 *      using cached prepared statements.
 *   3. Meta — per album: upsert release/recording/link rows into the
 *      quadrature-metadata.sqlite side-DB (separate transaction).
 */
static void write_resolve_batch(mb_resolver_t* ctx,
                                 const char* const* release_ids, int64_t* album_ids,
                                 size_t release_count,
                                 GHashTable* releases, GHashTable* all_links,
                                 mb_profile_stats_t* stats) {
    GHashTable* artist_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (ctx->meta_db) db_meta_begin(ctx->meta_db);

    quadrature_result_t txn_res = db_begin_transaction(ctx->db);
    if (txn_res != QUADRATURE_OK) {
        g_warning("write_resolve_batch: failed to begin transaction");
        if (ctx->meta_db) db_meta_commit(ctx->meta_db);
        g_hash_table_destroy(artist_cache);
        return;
    }

    album_work_t* works = g_new0(album_work_t, release_count);

    /* --- Phase 1: prepare desired states --- */
    for (size_t i = 0; i < release_count && !ctx->cancelled; i++) {
        mb_release_t* release = releases
            ? g_hash_table_lookup(releases, release_ids[i]) : NULL;

        if (!release) {
            db_reconcile_album_mb_status(ctx->db, album_ids[i],
                                         MB_STATUS_FAILED, (int64_t)time(NULL));
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.albums_failed++;
            ctx->progress.albums_processed++;
            g_mutex_unlock(&ctx->progress_mutex);
            continue;
        }

        mb_recording_link_row_t** links = NULL;
        size_t link_count = 0;
        if (all_links) {
            GPtrArray* link_arr = g_hash_table_lookup(all_links, release_ids[i]);
            if (link_arr) {
                links = (mb_recording_link_row_t**)link_arr->pdata;
                link_count = link_arr->len;
            }
        }

        resolve_album_prepare(ctx, album_ids[i], release, links, link_count,
                              artist_cache, stats, &works[i]);
    }

    /* --- Phase 2: single batched reconcile --- */
    int64_t t0 = profile_now_ns();
    int64_t* batch_ids = g_new(int64_t, release_count);
    desired_album_state_t* batch_desireds = g_new(desired_album_state_t, release_count);
    size_t batch_n = 0;
    for (size_t i = 0; i < release_count; i++) {
        if (!works[i].valid) continue;
        batch_ids[batch_n] = works[i].album_id;
        batch_desireds[batch_n] = works[i].desired;
        batch_n++;
    }
    if (batch_n > 0) {
        db_reconcile_albums(ctx->db, batch_ids, batch_desireds, batch_n,
                             &RECONCILE_POLICY_MB, NULL);
    }
    g_free(batch_ids);
    g_free(batch_desireds);
    int64_t t1 = profile_now_ns();
    stats->album_update_ns += (t1 - t0);

    /* --- Phase 3: meta DB writes + progress/cleanup --- */
    for (size_t i = 0; i < release_count; i++) {
        if (works[i].valid) {
            resolve_album_write_meta(ctx, &works[i], stats);
            stats->albums_written++;
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.albums_resolved++;
            ctx->progress.albums_processed++;
            ctx->progress.progress = ctx->progress.albums_total > 0
                ? (double)ctx->progress.albums_processed / (double)ctx->progress.albums_total
                : 1.0;
            g_mutex_unlock(&ctx->progress_mutex);
        }
        album_work_free(&works[i]);
    }
    g_free(works);

    resolver_update_progress(ctx);

    if (db_commit(ctx->db) != QUADRATURE_OK) {
        db_rollback(ctx->db);
        g_warning("write_resolve_batch: commit failed, batch rolled back");
    }

    if (ctx->meta_db) db_meta_commit(ctx->meta_db);

    g_hash_table_destroy(artist_cache);
}

static void process_resolve_batch(mb_resolver_t* ctx,
                                   resolve_queue_item_t** batch, size_t batch_count,
                                   mb_profile_stats_t* stats) {
    int64_t t0, t1;

    t0 = profile_now_ns();
    char** release_ids = NULL;
    int64_t* album_ids = NULL;
    size_t release_count = triage_batch(ctx, batch, batch_count,
                                         &release_ids, &album_ids);
    t1 = profile_now_ns();
    stats->triage_ns += (t1 - t0);

    if (release_count == 0) {
        triage_free(release_ids, album_ids, 0);
        resolver_update_progress(ctx);
        return;
    }

    // Consolidated batch fetch: releases + links via backend
    GHashTable* releases = NULL;
    GHashTable* all_links = NULL;
    t0 = profile_now_ns();
    mb_backend_batch_fetch(ctx->backend, ctx->backend_conn,
        (const char**)(void*)release_ids, release_count, &releases, &all_links);
    t1 = profile_now_ns();
    stats->pg_fetch_ns += (t1 - t0);

    t0 = profile_now_ns();
    write_resolve_batch(ctx, (const char* const*)release_ids, album_ids,
                         release_count, releases, all_links, stats);
    t1 = profile_now_ns();
    stats->sqlite_write_ns += (t1 - t0);

    stats->batch_count++;

    if (releases) g_hash_table_destroy(releases);
    if (all_links) g_hash_table_destroy(all_links);
    triage_free(release_ids, album_ids, release_count);
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_resolver_create(mb_resolver_t** out,
    quadrature_db_t* db,
    const mb_resolver_options_t* options,
    mb_resolver_progress_cb callback,
    void* user_data) {

    if (!out || !db || !options) return QUADRATURE_ERROR_INVALID_PARAM;

    /* Backend selection: pg_conninfo present → PG backend; otherwise HTTP. */
    const bool use_pg = (options->pg_conninfo && options->pg_conninfo[0]);

    mb_resolver_t* ctx = g_new0(mb_resolver_t, 1);
    ctx->db = db;
    ctx->options = *options;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->cancelled = false;
    ctx->library_root = options->library_root ? g_strdup(options->library_root) : NULL;
    ctx->data_root = options->data_root ? g_strdup(options->data_root)
                    : (options->library_root ? g_strdup(options->library_root) : NULL);
    ctx->acoustid_index_url = options->acoustid_index_url
        ? g_strdup(options->acoustid_index_url) : NULL;
    ctx->solr_url = options->mb_solr_url
        ? g_strdup(options->mb_solr_url) : NULL;
    g_mutex_init(&ctx->progress_mutex);

    /* Build backend URI + config based on PG-vs-HTTP selection. */
    char* uri = use_pg
        ? g_strdup_printf("pg://%s", options->pg_conninfo)
        : g_strdup("mb+https://");
    mb_backend_config_t cfg = {
        /* PG-side fields */
        .mb_conninfo        = options->pg_conninfo,
        .acoustid_conninfo  = options->acoustid_pg_conninfo,
        .acoustid_index_url = options->acoustid_index_url,
        .mb_solr_url        = options->mb_solr_url,
        /* HTTP-side fields. acoustid_api_key is NULL → backend uses the
         * bundled QUADRATURE_BUNDLED_ACOUSTID_KEY. The field exists only so
         * downstream builds (distros / forks) can pass a different key via
         * a future CMake override path; user settings does not expose it. */
        .mb_user_agent      = MUSICBRAINZ_USER_AGENT,
        .acoustid_api_key   = NULL,
        .mb_base_url        = NULL,
        .acoustid_base_url  = NULL,
    };

    /* Main backend (1 slot — was pg_client) */
    quadrature_result_t res = mb_backend_create(uri, &cfg, 1, &ctx->backend);
    if (res != QUADRATURE_OK) {
        g_free(uri);
        g_mutex_clear(&ctx->progress_mutex);
        g_free(ctx->library_root);
        g_free(ctx->data_root);
        g_free(ctx->acoustid_index_url);
        g_free(ctx->solr_url);
        g_free(ctx);
        return res;
    }
    ctx->backend_conn = mb_backend_claim_slot(ctx->backend, 0);

    /* Prefetch backend (1 slot — was pg_client_prefetch). Non-fatal if it fails. */
    ctx->backend_prefetch = NULL;
    ctx->backend_prefetch_conn = NULL;
    res = mb_backend_create(uri, &cfg, 1, &ctx->backend_prefetch);
    if (res == QUADRATURE_OK) {
        ctx->backend_prefetch_conn = mb_backend_claim_slot(ctx->backend_prefetch, 0);
    } else {
        g_info("mb_resolver_create: prefetch backend failed — running without overlap");
        ctx->backend_prefetch = NULL;
    }

    g_free(uri);

    /* Metadata DB for recording relations — ATTACHed to the main DB so that
     * meta writes commit atomically inside db_commit_batch. Non-fatal. */
    ctx->meta_db = NULL;
    if (ctx->data_root) {
        if (db_meta_open_attached(ctx->db, ctx->data_root, &ctx->meta_db) != QUADRATURE_OK) {
            g_warning("mb_resolver_create: failed to attach metadata DB — relations will not be written");
            ctx->meta_db = NULL;
        }
    }

    /* Fingerprint pool created lazily in mb_resolver_run when needed. */
    ctx->backend_fp_pool = NULL;

    *out = ctx;
    return QUADRATURE_OK;
}

quadrature_result_t mb_resolver_run(mb_resolver_t* ctx) {
    if (!ctx) return QUADRATURE_ERROR_INVALID_PARAM;

    // Reset progress
    g_mutex_lock(&ctx->progress_mutex);
    memset(&ctx->progress, 0, sizeof(mb_resolver_progress_t));
    ctx->progress.phase = MB_RESOLVE_QUERYING;
    g_mutex_unlock(&ctx->progress_mutex);
    resolver_update_progress(ctx);

    // =========================================================================
    // 1. TRIAGE: get all unresolved albums, check which have release_ids
    // =========================================================================

    int64_t* album_ids = NULL;
    size_t album_count = 0;
    int64_t retry_before = (int64_t)time(NULL) - MB_NO_MATCH_RETRY_SECONDS;
    quadrature_result_t res = db_get_unresolved_albums(ctx->db, retry_before,
                                                        &album_ids, &album_count);
    if (res != QUADRATURE_OK) return res;

    if (album_count == 0) {
        g_free(album_ids);
        resolver_set_phase(ctx, MB_RESOLVE_COMPLETE);
        return QUADRATURE_OK;
    }


    // Classify: tagged (have musicbrainz_release_id) vs untagged (need fingerprinting)
    GPtrArray* tagged_items = g_ptr_array_new();      // resolve_queue_item_t*
    GPtrArray* untagged_ids = g_ptr_array_new();      // int64_t* (just album_ids)

    for (size_t i = 0; i < album_count && !ctx->cancelled; i++) {
        db_album_t* album = NULL;
        db_get_album_by_id(ctx->db, album_ids[i], &album);
        const char* rid = (album && album->musicbrainz_release_id && album->musicbrainz_release_id[0])
                            ? album->musicbrainz_release_id : NULL;
        if (rid) {
            resolve_queue_item_t* item = g_new0(resolve_queue_item_t, 1);
            item->album_id = album_ids[i];
            item->release_id = g_strdup(rid);
            g_ptr_array_add(tagged_items, item);
        } else {
            int64_t* id_copy = g_new(int64_t, 1);
            *id_copy = album_ids[i];
            g_ptr_array_add(untagged_ids, id_copy);
        }
        if (album) db_albums_free(album, 1);
    }
    g_free(album_ids);


    // Tagged albums are resolved by their embedded MUSICBRAINZ_ALBUMID tag
    __atomic_store_n(&ctx->tier_count[RESOLVE_TIER_TAGGED], tagged_items->len, __ATOMIC_RELAXED);

    // Set progress totals
    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.albums_total = album_count;
    ctx->progress.fingerprint_total = untagged_ids->len;
    ctx->progress.fingerprint_processed = 0;
    g_mutex_unlock(&ctx->progress_mutex);
    resolver_update_progress(ctx);

    // =========================================================================
    // 2. Set up resolve queue + seed with tagged albums
    // =========================================================================

    GAsyncQueue* resolve_queue = g_async_queue_new();

    for (guint i = 0; i < tagged_items->len; i++) {
        g_async_queue_push(resolve_queue, g_ptr_array_index(tagged_items, i));
    }
    g_ptr_array_free(tagged_items, TRUE);  // Items now owned by queue

    // =========================================================================
    // 3. Start fingerprint thread pool (producer)
    // =========================================================================

    GThreadPool* fp_pool = NULL;
    /* PG path: needs acoustid_pg_conninfo. HTTP path: always — the HTTP backend
     * ships a bundled AcoustID app key; user setting only overrides the quota
     * source. See QUADRATURE_BUNDLED_ACOUSTID_KEY in mb_http_backend.c. */
    const bool resolver_use_pg = (ctx->options.pg_conninfo && ctx->options.pg_conninfo[0]);
    bool has_fingerprint_support = resolver_use_pg
        ? (ctx->options.acoustid_pg_conninfo && ctx->options.acoustid_pg_conninfo[0])
        : true;

    if (untagged_ids->len > 0 && has_fingerprint_support && !ctx->cancelled) {
        resolver_set_phase(ctx, MB_RESOLVE_FINGERPRINTING);

        // Determine worker count (1:1 with PG connections since libpq is not thread-safe).
        // Capped at 8 to avoid overloading PG with connections.
        int parallelism = ctx->options.parallelism > 0
            ? ctx->options.parallelism : (int)g_get_num_processors();
        if (parallelism > 8) parallelism = 8;
        if (parallelism > (int)untagged_ids->len) parallelism = (int)untagged_ids->len;

        // Create backend pool for fingerprint workers (mirrors mb_resolver_create choice)
        const bool fp_use_pg = (ctx->options.pg_conninfo && ctx->options.pg_conninfo[0]);
        char* fp_uri = fp_use_pg
            ? g_strdup_printf("pg://%s", ctx->options.pg_conninfo)
            : g_strdup("mb+https://");
        mb_backend_config_t fp_cfg = {
            .mb_conninfo        = ctx->options.pg_conninfo,
            .acoustid_conninfo  = ctx->options.acoustid_pg_conninfo,
            .acoustid_index_url = ctx->acoustid_index_url,
            .mb_solr_url        = ctx->solr_url,
            .mb_user_agent      = MUSICBRAINZ_USER_AGENT,
            .acoustid_api_key   = NULL,  /* HTTP backend uses bundled key */
        };
        res = mb_backend_create(fp_uri, &fp_cfg, (size_t)parallelism, &ctx->backend_fp_pool);
        g_free(fp_uri);

        if (res == QUADRATURE_OK) {
            fp_pool = g_thread_pool_new(fp_worker, ctx, parallelism, TRUE, NULL);
            if (fp_pool) {
                for (guint i = 0; i < untagged_ids->len && !ctx->cancelled; i++) {
                    fp_work_t* work = g_new0(fp_work_t, 1);
                    work->album_id = *(int64_t*)g_ptr_array_index(untagged_ids, i);
                    work->resolve_queue = resolve_queue;
                    g_thread_pool_push(fp_pool, work, NULL);
                }
            }
        } else {
            g_warning("MB resolver: failed to create fingerprint backend pool, fingerprinting disabled");
            // Push all untagged as service_error so they get MB_STATUS_FAILED, not NO_MATCH
            for (guint i = 0; i < untagged_ids->len; i++) {
                resolve_queue_item_t* item = g_new0(resolve_queue_item_t, 1);
                item->album_id = *(int64_t*)g_ptr_array_index(untagged_ids, i);
                item->release_id = NULL;
                item->service_error = true;
                g_async_queue_push(resolve_queue, item);
            }
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.acoustid_error = true;
            ctx->progress.fingerprint_processed = untagged_ids->len;
            g_mutex_unlock(&ctx->progress_mutex);
        }
    } else if (untagged_ids->len > 0 && !has_fingerprint_support) {
        // No AcoustID — try ISRC + Solr text search (no fingerprinting).
        // Runs sequentially on the resolver thread using ctx->pg_client
        // (no fp workers exist, so no contention).
        resolver_set_phase(ctx, MB_RESOLVE_FINGERPRINTING);

        for (guint i = 0; i < untagged_ids->len && !ctx->cancelled; i++) {
            int64_t album_id = *(int64_t*)g_ptr_array_index(untagged_ids, i);

            /* fingerprint_capable=false → Stage 3 (fingerprinting) skipped,
             * but Stages 1 (ISRC) and 2 (Solr) run normally on the main backend. */
            char* release_id = find_release_by_fingerprint(ctx, album_id,
                ctx->backend, ctx->backend_conn,
                /*fingerprint_capable=*/false, NULL);

            resolve_queue_item_t* item = g_new0(resolve_queue_item_t, 1);
            item->album_id = album_id;
            item->release_id = release_id;
            g_async_queue_push(resolve_queue, item);

            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.fingerprint_processed++;
            g_mutex_unlock(&ctx->progress_mutex);
            resolver_update_progress(ctx);
        }
    }

    // Free untagged_ids (album_id copies)
    for (guint i = 0; i < untagged_ids->len; i++)
        g_free(g_ptr_array_index(untagged_ids, i));
    g_ptr_array_free(untagged_ids, TRUE);

    // =========================================================================
    // 4. Batch resolver loop (consumer) — runs on current thread
    //    When a prefetch PG client is available, the loop overlaps:
    //      PG fetch of batch N+1 (prefetch thread)
    //      SQLite write of batch N (main thread)
    // =========================================================================

    resolver_set_phase(ctx, MB_RESOLVE_FETCHING);

    int64_t wall_start = profile_now_ns();
    mb_profile_stats_t prof = {0};

    size_t total_processed = 0;
    resolve_queue_item_t** batch = g_new0(resolve_queue_item_t*, MB_BATCH_SIZE);
    bool can_prefetch = (ctx->backend_prefetch != NULL);
    int consecutive_pg_failures = 0;

    /* Pending prefetch state — valid only when pf_thread != NULL */
    GThread* pf_thread = NULL;
    prefetch_ctx_t pf = {0};
    /* Pending write state — the triaged batch whose PG data is being prefetched.
     * pending_ids are owned copies (from triage_batch), freed via triage_free. */
    char** pending_ids = NULL;
    int64_t* pending_album_ids = NULL;
    size_t pending_count = 0;

    while (total_processed < album_count && !ctx->cancelled) {
        // Drain up to MB_BATCH_SIZE items from queue
        size_t batch_count = 0;
        int64_t t0, t1;

        // First item: blocking wait with timeout
        t0 = profile_now_ns();
        resolve_queue_item_t* item = g_async_queue_timeout_pop(resolve_queue,
            QUEUE_DRAIN_TIMEOUT_US);
        if (!item) {
            // Timeout — check if fingerprint pool is done
            if (!fp_pool || g_thread_pool_unprocessed(fp_pool) == 0) {
                // Try one more non-blocking pop to catch stragglers
                item = g_async_queue_try_pop(resolve_queue);
                if (!item) break;  // Queue truly empty and no more producers
            } else {
                continue;  // Fingerprinting still in progress, wait more
            }
        }

        batch[batch_count++] = item;

        // Try to fill rest of batch (non-blocking)
        while (batch_count < MB_BATCH_SIZE) {
            item = g_async_queue_try_pop(resolve_queue);
            if (!item) break;
            batch[batch_count++] = item;
        }
        t1 = profile_now_ns();
        prof.queue_wait_ns += (t1 - t0);

        /* Triage: separate release_ids from no-match items */
        t0 = profile_now_ns();
        char** release_ids = NULL;
        int64_t* batch_album_ids = NULL;
        size_t release_count = triage_batch(ctx, batch, batch_count,
                                             &release_ids, &batch_album_ids);
        t1 = profile_now_ns();
        prof.triage_ns += (t1 - t0);

        if (release_count == 0) {
            triage_free(release_ids, batch_album_ids, 0);
            resolver_update_progress(ctx);
        } else if (pf_thread) {
            /* A prefetch is running for the PREVIOUS batch.
             * Join it, save its results, start prefetch for THIS batch,
             * then write the previous batch to SQLite (overlapping I/O). */
            t0 = profile_now_ns();
            g_thread_join(pf_thread);
            pf_thread = NULL;
            t1 = profile_now_ns();
            prof.pg_fetch_ns += (t1 - t0);

            /* If prefetch failed, attempt reconnect + retry once */
            if (pf.result != QUADRATURE_OK) {
                g_warning("Phase 6: prefetch batch failed, attempting reconnect");
                mb_backend_reset(ctx->backend_prefetch, ctx->backend_prefetch_conn);
                pf.releases = NULL;
                pf.links = NULL;
                pf.result = mb_backend_batch_fetch(pf.be, pf.conn,
                                                    (const char**)pf.release_ids,
                                                    pf.release_count,
                                                    &pf.releases, &pf.links);
                if (pf.result != QUADRATURE_OK) {
                    g_warning("Phase 6: backend retry failed for prefetched batch");
                    consecutive_pg_failures++;
                } else {
                    consecutive_pg_failures = 0;
                }
            } else {
                consecutive_pg_failures = 0;
            }

            if (consecutive_pg_failures >= 3) {
                g_warning("Phase 6: circuit breaker tripped — skipping remaining albums after 3 consecutive PG failures");
                /* Write whatever we have (NULL releases → albums marked FAILED) */
                write_resolve_batch(ctx, (const char* const*)pending_ids, pending_album_ids,
                                     pending_count, pf.releases, pf.links, &prof);
                prof.batch_count++;
                if (pf.releases) g_hash_table_destroy(pf.releases);
                if (pf.links) g_hash_table_destroy(pf.links);
                triage_free(pending_ids, pending_album_ids, pending_count);
                pending_ids = NULL;
                pending_album_ids = NULL;
                pending_count = 0;
                triage_free(release_ids, batch_album_ids, release_count);
                for (size_t i = 0; i < batch_count; i++)
                    resolve_queue_item_free(batch[i]);
                total_processed += batch_count;
                break;
            }

            /* Save previous batch state before overwriting */
            GHashTable* prev_releases = pf.releases;
            GHashTable* prev_links = pf.links;
            char** prev_ids = pending_ids;
            int64_t* prev_album_ids = pending_album_ids;
            size_t prev_count = pending_count;

            /* Start prefetch for THIS batch */
            pending_ids = release_ids;
            pending_album_ids = batch_album_ids;
            pending_count = release_count;
            pf = (prefetch_ctx_t){
                .be   = ctx->backend_prefetch,
                .conn = ctx->backend_prefetch_conn,
                .release_ids = (const char* const*)release_ids,
                .release_count = release_count,
            };
            pf_thread = g_thread_new("mb-prefetch", prefetch_thread_func, &pf);

            /* Write PREVIOUS batch to SQLite (overlaps with backend prefetch) */
            t0 = profile_now_ns();
            write_resolve_batch(ctx, (const char* const*)prev_ids, prev_album_ids,
                                 prev_count, prev_releases, prev_links, &prof);
            t1 = profile_now_ns();
            prof.sqlite_write_ns += (t1 - t0);
            prof.batch_count++;

            if (prev_releases) g_hash_table_destroy(prev_releases);
            if (prev_links) g_hash_table_destroy(prev_links);
            triage_free(prev_ids, prev_album_ids, prev_count);
        } else if (can_prefetch) {
            /* No prefetch running — start one for this batch.
             * The prefetch thread fetches from PG; next iteration we'll
             * drain another batch from the queue while it runs. */
            pending_ids = release_ids;
            pending_album_ids = batch_album_ids;
            pending_count = release_count;
            pf = (prefetch_ctx_t){
                .be   = ctx->backend_prefetch,
                .conn = ctx->backend_prefetch_conn,
                .release_ids = (const char* const*)release_ids,
                .release_count = release_count,
            };
            pf_thread = g_thread_new("mb-prefetch", prefetch_thread_func, &pf);
        } else {
            /* No prefetch client — fetch + write inline */
            GHashTable* rel = NULL;
            GHashTable* lnk = NULL;
            t0 = profile_now_ns();
            quadrature_result_t fetch_res = mb_backend_batch_fetch(ctx->backend, ctx->backend_conn,
                (const char**)(void*)release_ids, release_count, &rel, &lnk);
            t1 = profile_now_ns();
            prof.pg_fetch_ns += (t1 - t0);

            /* Retry once on failure after reconnect */
            if (fetch_res != QUADRATURE_OK) {
                g_warning("Phase 6: backend batch fetch failed, attempting reconnect");
                mb_backend_reset(ctx->backend, ctx->backend_conn);
                rel = NULL;
                lnk = NULL;
                t0 = profile_now_ns();
                fetch_res = mb_backend_batch_fetch(ctx->backend, ctx->backend_conn,
                    (const char**)(void*)release_ids, release_count, &rel, &lnk);
                t1 = profile_now_ns();
                prof.pg_fetch_ns += (t1 - t0);
                if (fetch_res != QUADRATURE_OK) {
                    g_warning("Phase 6: PG retry failed, marking batch as FAILED");
                    consecutive_pg_failures++;
                } else {
                    consecutive_pg_failures = 0;
                }
            } else {
                consecutive_pg_failures = 0;
            }

            t0 = profile_now_ns();
            write_resolve_batch(ctx, (const char* const*)release_ids, batch_album_ids,
                                 release_count, rel, lnk, &prof);
            t1 = profile_now_ns();
            prof.sqlite_write_ns += (t1 - t0);
            prof.batch_count++;

            if (rel) g_hash_table_destroy(rel);
            if (lnk) g_hash_table_destroy(lnk);
            triage_free(release_ids, batch_album_ids, release_count);

            if (consecutive_pg_failures >= 3) {
                g_warning("Phase 6: circuit breaker tripped — skipping remaining albums after 3 consecutive PG failures");
                for (size_t i = 0; i < batch_count; i++)
                    resolve_queue_item_free(batch[i]);
                total_processed += batch_count;
                break;
            }
        }

        // Free batch items
        for (size_t i = 0; i < batch_count; i++) {
            resolve_queue_item_free(batch[i]);
        }
        total_processed += batch_count;
    }

    /* Drain final prefetch if one is outstanding */
    if (pf_thread) {
        int64_t t0 = profile_now_ns();
        g_thread_join(pf_thread);
        pf_thread = NULL;
        int64_t t1 = profile_now_ns();
        prof.pg_fetch_ns += (t1 - t0);

        /* Retry once on PG failure after reconnect */
        if (pf.result != QUADRATURE_OK) {
            g_warning("Phase 6: final prefetch batch failed, attempting reconnect");
            mb_backend_reset(ctx->backend_prefetch, ctx->backend_prefetch_conn);
            pf.releases = NULL;
            pf.links = NULL;
            pf.result = mb_backend_batch_fetch(pf.be, pf.conn,
                                                (const char**)pf.release_ids,
                                                pf.release_count,
                                                &pf.releases, &pf.links);
            if (pf.result != QUADRATURE_OK)
                g_warning("Phase 6: final retry failed, marking batch as FAILED");
        }

        t0 = profile_now_ns();
        write_resolve_batch(ctx, (const char* const*)pending_ids, pending_album_ids,
                             pending_count, pf.releases, pf.links, &prof);
        t1 = profile_now_ns();
        prof.sqlite_write_ns += (t1 - t0);
        prof.batch_count++;

        if (pf.releases) g_hash_table_destroy(pf.releases);
        if (pf.links) g_hash_table_destroy(pf.links);
        triage_free(pending_ids, pending_album_ids, pending_count);
    }

    g_free(batch);

    // =========================================================================
    // 5. Wait for fingerprint pool to drain
    // =========================================================================

    if (fp_pool) {
        g_thread_pool_free(fp_pool, FALSE, TRUE);
        fp_pool = NULL;
    }

    // =========================================================================
    // 6. Process remaining items in queue (stragglers from fingerprinting)
    // =========================================================================

    resolve_queue_item_t** remaining = g_new0(resolve_queue_item_t*, MB_BATCH_SIZE);
    while (!ctx->cancelled) {
        size_t rem_count = 0;
        while (rem_count < MB_BATCH_SIZE) {
            resolve_queue_item_t* item = g_async_queue_try_pop(resolve_queue);
            if (!item) break;
            remaining[rem_count++] = item;
        }
        if (rem_count == 0) break;

        process_resolve_batch(ctx, remaining, rem_count, &prof);
        for (size_t i = 0; i < rem_count; i++)
            resolve_queue_item_free(remaining[i]);
    }
    g_free(remaining);

    // =========================================================================
    // 7. Profile summary
    // =========================================================================

    int64_t wall_ns = profile_now_ns() - wall_start;
    if (prof.albums_written > 0) {
        double wall_ms = (double)wall_ns / 1e6;
        double write_ms = (double)prof.sqlite_write_ns / 1e6;
        double n = (double)prof.albums_written;

        g_info("=== MB Resolver Profile (%zu albums in %zu batches) ===",
               prof.albums_written, prof.batch_count);
        g_info("  Batch-level:  queue_wait=%.0fms  triage=%.0fms  pg_fetch=%.0fms  sqlite_write=%.0fms",
               (double)prof.queue_wait_ns / 1e6,
               (double)prof.triage_ns / 1e6,
               (double)prof.pg_fetch_ns / 1e6,
               write_ms);
        g_info("  Per-album avg (%.2fms total):",
               write_ms / n);
        g_info("    get_tracks=%.2fms  match=%.2fms  artist_lookup=%.2fms",
               (double)prof.get_tracks_ns / 1e6 / n,
               (double)prof.match_tracks_ns / 1e6 / n,
               (double)prof.artist_lookup_ns / 1e6 / n);
        g_info("    album_update=%.2fms  track_update=%.2fms  fts_sync=%.2fms  meta_db=%.2fms",
               (double)prof.album_update_ns / 1e6 / n,
               (double)prof.track_update_ns / 1e6 / n,
               (double)prof.fts_sync_ns / 1e6 / n,
               (double)prof.meta_album_ns / 1e6 / n);
        g_info("  Throughput: %.1f albums/sec (write only), %.1f albums/sec (wall)",
               write_ms > 0.0 ? n / (write_ms / 1000.0) : 0.0,
               wall_ms > 0.0 ? n / (wall_ms / 1000.0) : 0.0);
    }

    // =========================================================================
    // 8. Resolution tier summary
    // =========================================================================

    {
        size_t n_tagged  = __atomic_load_n(&ctx->tier_count[RESOLVE_TIER_TAGGED], __ATOMIC_RELAXED);
        size_t n_isrc    = __atomic_load_n(&ctx->tier_count[RESOLVE_TIER_ISRC], __ATOMIC_RELAXED);
        size_t n_solr    = __atomic_load_n(&ctx->tier_count[RESOLVE_TIER_SOLR], __ATOMIC_RELAXED);
        size_t n_acoust  = __atomic_load_n(&ctx->tier_count[RESOLVE_TIER_ACOUSTID], __ATOMIC_RELAXED);
        size_t n_none    = __atomic_load_n(&ctx->tier_count[RESOLVE_TIER_NONE], __ATOMIC_RELAXED);
        size_t n_total   = n_tagged + n_isrc + n_solr + n_acoust + n_none;

        double isrc_ms    = (double)__atomic_load_n(&ctx->tier_ns[RESOLVE_TIER_ISRC], __ATOMIC_RELAXED) / 1e6;
        double solr_ms    = (double)__atomic_load_n(&ctx->tier_ns[RESOLVE_TIER_SOLR], __ATOMIC_RELAXED) / 1e6;
        double acoust_ms  = (double)__atomic_load_n(&ctx->tier_ns[RESOLVE_TIER_ACOUSTID], __ATOMIC_RELAXED) / 1e6;

        // Number of untagged albums that attempted each tier
        size_t untagged = n_isrc + n_solr + n_acoust + n_none;

        g_message("=== Resolution Summary — %s (%zu albums) ===",
                  ctx->library_root ? ctx->library_root : "(unknown)", n_total);
        g_message("  Tagged (MUSICBRAINZ_ALBUMID): %zu", n_tagged);
        g_message("  ISRC lookup:    %zu resolved    (%.1f albums/sec)",
                  n_isrc,
                  isrc_ms > 0.0 ? (double)untagged / (isrc_ms / 1000.0) : 0.0);
        g_message("  Solr search:    %zu resolved    (%.1f albums/sec)",
                  n_solr,
                  solr_ms > 0.0 ? (double)(untagged - n_isrc) / (solr_ms / 1000.0) : 0.0);
        g_message("  AcoustID:       %zu resolved    (%.1f albums/sec)",
                  n_acoust,
                  acoust_ms > 0.0 ? (double)(untagged - n_isrc - n_solr) / (acoust_ms / 1000.0) : 0.0);
        g_message("  Unresolved:     %zu", n_none);
    }

    // Cleanup
    g_async_queue_unref(resolve_queue);
    if (ctx->backend_fp_pool) {
        mb_backend_destroy(ctx->backend_fp_pool);
        ctx->backend_fp_pool = NULL;
    }

    resolver_set_phase(ctx, MB_RESOLVE_COMPLETE);

    return ctx->cancelled ? QUADRATURE_ERROR_CANCELLED : QUADRATURE_OK;
}

void mb_resolver_destroy(mb_resolver_t* ctx) {
    if (!ctx) return;
    if (ctx->backend) mb_backend_destroy(ctx->backend);
    if (ctx->backend_prefetch) mb_backend_destroy(ctx->backend_prefetch);
    if (ctx->backend_fp_pool) mb_backend_destroy(ctx->backend_fp_pool);
    if (ctx->meta_db) {
        db_meta_checkpoint(ctx->meta_db);
        db_meta_close(ctx->meta_db);
    }
    g_mutex_clear(&ctx->progress_mutex);
    g_free(ctx->library_root);
    g_free(ctx->data_root);
    g_free(ctx->acoustid_index_url);
    g_free(ctx->solr_url);
    g_free(ctx);
}
