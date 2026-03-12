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
    mb_pg_client_t* pg_client;          /* MusicBrainz PostgreSQL client (batch consumer) */
    mb_pg_client_t* pg_client_prefetch; /* Second PG client for prefetch overlap (may be NULL) */
    char* acoustid_index_url;           /* acoustid-index HTTP URL (may be NULL) */
    char* solr_url;                     /* MusicBrainz Solr URL (may be NULL) */
    mb_resolver_options_t options;
    mb_resolver_progress_cb callback;
    void* user_data;

    char* library_root;
    quadrature_meta_db_t* meta_db;      /* Recording relations DB (may be NULL — non-fatal) */

    mb_resolver_progress_t progress;
    GMutex progress_mutex;

    /* Fingerprint worker PG pool (one connection per worker thread) */
    mb_pg_pool_t* pg_pool;

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
    int64_t artist_lookup_ns;    // all db_get_or_create_artist_mb calls
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
            artist_id = db_get_or_create_artist_mb(
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
            if (strcmp(wa[i], wb[j]) == 0) {
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
                                          mb_pg_client_t* mb_pg,
                                          mb_pg_client_t* acoustid_pg,
                                          mb_http_conn_t* http_conn,
                                          bool* service_error) {
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

        if (isrc_count >= 2) {
            mb_acoustid_response_t isrc_response;
            quadrature_result_t isrc_res = mb_isrc_lookup(mb_pg, isrcs, isrc_count,
                                                           &isrc_response);
            if (isrc_res == QUADRATURE_OK && isrc_response.count > 0) {
                tally_votes(&isrc_response, rg_counts, rg_best_release,
                            release_counts, &best_rg, &best_rg_count);
                mb_acoustid_response_free(&isrc_response);

                double confidence = (double)best_rg_count / (double)isrc_count;
                if (confidence >= MB_MATCH_CONFIDENCE) {
                    g_debug("ISRC lookup resolved album %" G_GINT64_FORMAT
                            " → %s (%.0f%% confidence, %zu ISRCs)",
                            album_id, best_rg, confidence * 100, isrc_count);
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

    if (!best_release && !ctx->cancelled && ctx->solr_url
        && tag_album && tag_artist) {
        best_release = mb_solr_search_release(mb_pg, ctx->solr_url,
            tag_album, tag_artist, track_count, total_duration_ms);
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

    if (!best_release && !isrc_resolved && !ctx->cancelled && acoustid_pg && http_conn) {
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
            quadrature_result_t lookup_res = mb_acoustid_lookup(mb_pg, acoustid_pg,
                                                                 http_conn, &fp, &response);
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
                if (ctx->solr_url && tag_album && tag_artist) {
                    char* solr_rel = mb_solr_search_release(mb_pg, ctx->solr_url,
                        tag_album, tag_artist, track_count, total_duration_ms);
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
            if (best_release) tier = RESOLVE_TIER_ACOUSTID;
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
    for (size_t i = 0; i < release->recording_count && matched < max_matches; i++) {
        for (size_t j = 0; j < local_count; j++) {
            if (local_used[j]) continue;
            if (release->recordings[i].disc_number == local_tracks[j].disc_num &&
                release->recordings[i].position == local_tracks[j].track_num) {
                matches[matched].mb_idx = i;
                matches[matched].local_idx = j;
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
// Write resolved album to SQLite (extracted from old resolve_album)
// =============================================================================

/**
 * Write a resolved album's MB metadata to the database.
 * Called by the batch consumer for each album that has a release.
 *
 * @param ctx        Resolver context
 * @param album_id   Local album ID
 * @param release    Fetched MB release data
 * @param links      Recording links for this release (may be NULL)
 * @param link_count Number of link rows
 */
static void resolve_album_write(mb_resolver_t* ctx, int64_t album_id,
                                 mb_release_t* release,
                                 mb_recording_link_row_t** links, size_t link_count,
                                 GHashTable* artist_cache,
                                 mb_profile_stats_t* stats) {
    int64_t t0, t1;

    // Get local tracks for matching
    db_track_t* tracks = NULL;
    size_t track_count = 0;
    t0 = profile_now_ns();
    quadrature_result_t res = db_get_tracks_by_album(ctx->db, album_id, &tracks, &track_count);
    t1 = profile_now_ns();
    stats->get_tracks_ns += (t1 - t0);
    if (res != QUADRATURE_OK || track_count == 0) {
        db_set_album_mb_status(ctx->db, album_id, MB_STATUS_FAILED, (int64_t)time(NULL));
        if (tracks) db_tracks_free(tracks, track_count);
        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return;
    }

    // Match MB recordings to local tracks
    track_match_t* matches = NULL;
    size_t match_count = 0;
    t0 = profile_now_ns();
    match_tracks(release, tracks, track_count, &matches, &match_count);
    t1 = profile_now_ns();
    stats->match_tracks_ns += (t1 - t0);

    // Caller (write_resolve_batch) owns the transaction — no per-album begin/commit.

    // Check if Various Artists
    bool is_va = false;
    if (release->artist_count > 0 && release->artists[0].id) {
        is_va = (strcmp(release->artists[0].id, VA_MUSICBRAINZ_ID) == 0);
    }

    // Create/update album artist (use cache for MBID→artist_id dedup)
    t0 = profile_now_ns();
    if (release->artist_count > 0 && !is_va) {
        int64_t album_artist_id = 0;
        const char* mbid = release->artists[0].id;
        if (artist_cache && mbid && mbid[0]) {
            gpointer cached = g_hash_table_lookup(artist_cache, mbid);
            if (cached) album_artist_id = (int64_t)GPOINTER_TO_SIZE(cached);
        }
        if (album_artist_id == 0) {
            album_artist_id = db_get_or_create_artist_mb(
                ctx->db, release->artists[0].name,
                release->artists[0].sort_name, mbid);
            if (artist_cache && mbid && mbid[0] && album_artist_id > 0)
                g_hash_table_insert(artist_cache,
                    g_strdup(mbid), GSIZE_TO_POINTER((gsize)album_artist_id));
        }
        t1 = profile_now_ns();
        stats->artist_lookup_ns += (t1 - t0);
        t0 = profile_now_ns();
        db_update_album_artist(ctx->db, album_id, album_artist_id, false);
    } else if (is_va) {
        int64_t va_id = 0;
        if (artist_cache) {
            gpointer cached = g_hash_table_lookup(artist_cache, VA_MUSICBRAINZ_ID);
            if (cached) va_id = (int64_t)GPOINTER_TO_SIZE(cached);
        }
        if (va_id == 0) {
            va_id = db_get_or_create_artist_mb(
                ctx->db, "Various Artists", "Various Artists", VA_MUSICBRAINZ_ID);
            if (artist_cache && va_id > 0)
                g_hash_table_insert(artist_cache,
                    g_strdup(VA_MUSICBRAINZ_ID), GSIZE_TO_POINTER((gsize)va_id));
        }
        t1 = profile_now_ns();
        stats->artist_lookup_ns += (t1 - t0);
        t0 = profile_now_ns();
        db_update_album_artist(ctx->db, album_id, va_id, true);
    } else {
        t1 = profile_now_ns();
        stats->artist_lookup_ns += (t1 - t0);
        t0 = profile_now_ns();
    }

    // Update album with MB metadata
    uint16_t year = parse_mb_year(release->date);
    db_update_album_mb(ctx->db, album_id,
        release->title,
        release->id, release->release_group_id,
        year, MB_STATUS_RESOLVED);
    t1 = profile_now_ns();
    stats->album_update_ns += (t1 - t0);

    // Update each matched track
    t0 = profile_now_ns();
    for (size_t m = 0; m < match_count; m++) {
        mb_recording_t* rec = &release->recordings[matches[m].mb_idx];
        db_track_t* local = &tracks[matches[m].local_idx];

        db_update_track_title(ctx->db, local->id, rec->title);

        if (rec->artist_count > 0) {
            int64_t artist_t0 = profile_now_ns();
            db_track_artist_t* ta = NULL;
            size_t ta_count = 0;
            mb_credits_to_track_artists(ctx->db, rec->artists, rec->artist_count,
                                         &ta, &ta_count, artist_cache);
            int64_t artist_t1 = profile_now_ns();
            stats->artist_lookup_ns += (artist_t1 - artist_t0);

            db_set_track_artists(ctx->db, local->id, ta, ta_count);
            for (size_t k = 0; k < ta_count; k++) {
                g_free(ta[k].name);
                g_free(ta[k].join_phrase);
            }
            g_free(ta);
        }

        // Merge MB release genres into track's genre field
        if (release->genres && release->genres[0]) {
            db_merge_track_genres(ctx->db, local->id, release->genres);
        }
    }
    t1 = profile_now_ns();
    stats->track_update_ns += (t1 - t0);

    // Bulk sync tracks_fts — all title + artist_display changes applied above
    t0 = profile_now_ns();
    db_sync_album_fts(ctx->db, album_id);
    t1 = profile_now_ns();
    stats->fts_sync_ns += (t1 - t0);

    /* Write release + recording relations to quadrature-metadata.sqlite (non-fatal).
     * Transaction is managed at the batch level by write_resolve_batch(). */
    t0 = profile_now_ns();
    if (ctx->meta_db) {
        db_meta_upsert_release(ctx->meta_db, release->id,
            release->date, release->type, release->label,
            release->catalog_number, release->barcode, release->genres);
        for (size_t m = 0; m < match_count; m++) {
            mb_recording_t* rec = &release->recordings[matches[m].mb_idx];
            db_meta_upsert_recording(ctx->meta_db,
                rec->id, release->id, rec->disc_number, rec->position);
            db_meta_delete_recording_links(ctx->meta_db, rec->id);
        }
        /* Pre-deduplicate link_types and artists: a release with 30 links
         * often has only ~5 unique link_types and ~8 unique artists.
         * Skipping redundant INSERT OR REPLACE saves B-tree lookups. */
        GHashTable* seen_link_types = g_hash_table_new(g_str_hash, g_str_equal);
        GHashTable* seen_artists = g_hash_table_new(g_str_hash, g_str_equal);
        for (size_t i = 0; i < link_count; i++) {
            if (!g_hash_table_contains(seen_link_types, links[i]->link_type_gid)) {
                db_meta_upsert_link_type(ctx->meta_db,
                    links[i]->link_type_gid, links[i]->link_type_name, links[i]->link_type_desc);
                g_hash_table_add(seen_link_types, (gpointer)links[i]->link_type_gid);
            }
            if (!g_hash_table_contains(seen_artists, links[i]->artist_mbid)) {
                db_meta_upsert_artist(ctx->meta_db,
                    links[i]->artist_mbid, links[i]->artist_name,
                    links[i]->artist_sort_name, links[i]->artist_type);
                g_hash_table_add(seen_artists, (gpointer)links[i]->artist_mbid);
            }
            db_meta_insert_recording_link(ctx->meta_db,
                links[i]->recording_mbid, links[i]->artist_mbid, links[i]->link_type_gid,
                links[i]->entity0_credit, links[i]->attributes);
        }
        g_hash_table_destroy(seen_link_types);
        g_hash_table_destroy(seen_artists);
    }
    t1 = profile_now_ns();
    stats->meta_album_ns += (t1 - t0);

    db_tracks_free(tracks, track_count);
    g_free(matches);

    stats->albums_written++;

    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.albums_resolved++;
    g_mutex_unlock(&ctx->progress_mutex);
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

static int fp_claim_slot(mb_pg_pool_t* pool) {
    gpointer stored = g_private_get(&fp_slot_key);
    if (stored) return GPOINTER_TO_INT(stored) - 1;
    int slot = g_atomic_int_add(&pool->next_slot, 1) % (int)pool->count;
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

    int slot = fp_claim_slot(ctx->pg_pool);
    mb_pg_client_t* mb_pg = ctx->pg_pool->mb_conns[slot];
    mb_pg_client_t* acoustid_pg = ctx->pg_pool->acoustid_conns[slot];
    mb_http_conn_t* http_conn = ctx->pg_pool->http_conns[slot];

    bool svc_error = false;
    char* release_id = find_release_by_fingerprint(ctx, work->album_id,
                                                    mb_pg, acoustid_pg, http_conn,
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
    mb_pg_client_t* pg;
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
    pf->result = mb_fetch_all_batch(pf->pg, (const char**)pf->release_ids,
                                     pf->release_count, &pf->releases, &pf->links);
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
            db_set_album_mb_status(ctx->db, batch[i]->album_id,
                                    MB_STATUS_FAILED, (int64_t)time(NULL));
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.albums_failed++;
            ctx->progress.albums_processed++;
            g_mutex_unlock(&ctx->progress_mutex);
        } else {
            db_set_album_mb_status(ctx->db, batch[i]->album_id,
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
 * Write batch results to SQLite. Accepts pre-fetched releases/links
 * (from prefetch thread or inline fetch).
 */
static void write_resolve_batch(mb_resolver_t* ctx,
                                 const char* const* release_ids, int64_t* album_ids,
                                 size_t release_count,
                                 GHashTable* releases, GHashTable* all_links,
                                 mb_profile_stats_t* stats) {
    // Artist MBID → artist_id cache: avoids repeated SQLite lookups for the
    // same artist across albums in this batch (e.g. album artist + track credits).
    GHashTable* artist_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    // Batch meta DB transaction — one BEGIN/COMMIT for all albums instead of per-album
    if (ctx->meta_db) db_meta_begin(ctx->meta_db);

    // Batch SQLite transaction — one fsync for the entire batch instead of per-album
    quadrature_result_t txn_res = db_begin_transaction(ctx->db);
    if (txn_res != QUADRATURE_OK) {
        g_warning("write_resolve_batch: failed to begin transaction");
        if (ctx->meta_db) db_meta_commit(ctx->meta_db);
        g_hash_table_destroy(artist_cache);
        return;
    }

    for (size_t i = 0; i < release_count && !ctx->cancelled; i++) {
        mb_release_t* release = releases
            ? g_hash_table_lookup(releases, release_ids[i]) : NULL;

        if (!release) {
            db_set_album_mb_status(ctx->db, album_ids[i],
                                    MB_STATUS_FAILED, (int64_t)time(NULL));
            g_mutex_lock(&ctx->progress_mutex);
            ctx->progress.albums_failed++;
            ctx->progress.albums_processed++;
            g_mutex_unlock(&ctx->progress_mutex);
            resolver_update_progress(ctx);
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

        resolve_album_write(ctx, album_ids[i], release, links, link_count, artist_cache, stats);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_processed++;
        ctx->progress.progress = ctx->progress.albums_total > 0
            ? (double)ctx->progress.albums_processed / (double)ctx->progress.albums_total
            : 1.0;
        g_mutex_unlock(&ctx->progress_mutex);
        resolver_update_progress(ctx);
    }

    // Commit batch SQLite transaction — single WAL fsync for all albums
    if (db_commit(ctx->db) != QUADRATURE_OK) {
        db_rollback(ctx->db);
        g_warning("write_resolve_batch: commit failed, batch rolled back");
    }

    // Commit batch meta transaction
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

    // Consolidated batch fetch: releases + links in one PG round-trip
    GHashTable* releases = NULL;
    GHashTable* all_links = NULL;
    t0 = profile_now_ns();
    mb_fetch_all_batch(ctx->pg_client,
        (const char**)release_ids, release_count, &releases, &all_links);
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

    // PostgreSQL connection is required
    if (!options->pg_conninfo || !options->pg_conninfo[0]) {
        g_warning("MusicBrainz PostgreSQL connection info is required");
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    mb_resolver_t* ctx = g_new0(mb_resolver_t, 1);
    ctx->db = db;
    ctx->options = *options;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->cancelled = false;
    ctx->library_root = options->library_root ? g_strdup(options->library_root) : NULL;
    ctx->acoustid_index_url = options->acoustid_index_url
        ? g_strdup(options->acoustid_index_url) : NULL;
    ctx->solr_url = options->mb_solr_url
        ? g_strdup(options->mb_solr_url) : NULL;
    g_mutex_init(&ctx->progress_mutex);

    // MusicBrainz PostgreSQL client (for batch consumer)
    quadrature_result_t res = mb_pg_client_create(options->pg_conninfo, &ctx->pg_client);
    if (res != QUADRATURE_OK) {
        g_mutex_clear(&ctx->progress_mutex);
        g_free(ctx->library_root);
        g_free(ctx->acoustid_index_url);
        g_free(ctx->solr_url);
        g_free(ctx);
        return res;
    }
    // MB PG tables live in the 'musicbrainz' schema — set search_path
    mb_pg_set_schema(ctx->pg_client, "musicbrainz");
    // Install session-local batch function for consolidated fetching
    mb_pg_install_batch_function(ctx->pg_client);

    // Second PG client for prefetch overlap (non-fatal if it fails)
    ctx->pg_client_prefetch = NULL;
    res = mb_pg_client_create(options->pg_conninfo, &ctx->pg_client_prefetch);
    if (res == QUADRATURE_OK) {
        mb_pg_set_schema(ctx->pg_client_prefetch, "musicbrainz");
        mb_pg_install_batch_function(ctx->pg_client_prefetch);
    } else {
        g_info("mb_resolver_create: prefetch PG connection failed — running without overlap");
        ctx->pg_client_prefetch = NULL;
    }

    // Metadata DB for recording relations (non-fatal if it fails to open)
    ctx->meta_db = NULL;
    if (ctx->library_root) {
        if (db_meta_open(ctx->library_root, &ctx->meta_db) != QUADRATURE_OK) {
            g_warning("mb_resolver_create: failed to open metadata DB — relations will not be written");
            ctx->meta_db = NULL;
        }
    }

    // PG connection pool for fingerprint workers (created lazily when needed)
    ctx->pg_pool = NULL;

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
        char* release_id = db_get_album_musicbrainz_release_id(ctx->db, album_ids[i]);
        if (release_id) {
            resolve_queue_item_t* item = g_new0(resolve_queue_item_t, 1);
            item->album_id = album_ids[i];
            item->release_id = release_id;
            g_ptr_array_add(tagged_items, item);
        } else {
            int64_t* id_copy = g_new(int64_t, 1);
            *id_copy = album_ids[i];
            g_ptr_array_add(untagged_ids, id_copy);
        }
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
    bool has_fingerprint_support = ctx->options.acoustid_pg_conninfo
                                   && ctx->options.acoustid_pg_conninfo[0];

    if (untagged_ids->len > 0 && has_fingerprint_support && !ctx->cancelled) {
        resolver_set_phase(ctx, MB_RESOLVE_FINGERPRINTING);

        // Determine worker count (1:1 with PG connections since libpq is not thread-safe).
        // Capped at 8 to avoid overloading PG with connections.
        int parallelism = ctx->options.parallelism > 0
            ? ctx->options.parallelism : (int)g_get_num_processors();
        if (parallelism > 8) parallelism = 8;
        if (parallelism > (int)untagged_ids->len) parallelism = (int)untagged_ids->len;

        // Create PG pool for fingerprint workers
        res = mb_pg_pool_create(ctx->options.pg_conninfo,
            ctx->options.acoustid_pg_conninfo,
            ctx->acoustid_index_url,
            (size_t)parallelism, &ctx->pg_pool);

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
            g_warning("MB resolver: failed to create PG pool, fingerprinting disabled");
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

            // acoustid_pg=NULL, http_conn=NULL → Stage 3 (fingerprinting) skipped,
            // but Stages 1 (ISRC) and 2 (Solr) run normally.
            char* release_id = find_release_by_fingerprint(ctx, album_id,
                ctx->pg_client, NULL, NULL, NULL);

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
    bool can_prefetch = (ctx->pg_client_prefetch != NULL);

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
        int64_t* album_ids = NULL;
        size_t release_count = triage_batch(ctx, batch, batch_count,
                                             &release_ids, &album_ids);
        t1 = profile_now_ns();
        prof.triage_ns += (t1 - t0);

        if (release_count == 0) {
            triage_free(release_ids, album_ids, 0);
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

            /* Save previous batch state before overwriting */
            GHashTable* prev_releases = pf.releases;
            GHashTable* prev_links = pf.links;
            char** prev_ids = pending_ids;
            int64_t* prev_album_ids = pending_album_ids;
            size_t prev_count = pending_count;

            /* Start prefetch for THIS batch */
            pending_ids = release_ids;
            pending_album_ids = album_ids;
            pending_count = release_count;
            pf = (prefetch_ctx_t){
                .pg = ctx->pg_client_prefetch,
                .release_ids = (const char* const*)release_ids,
                .release_count = release_count,
            };
            pf_thread = g_thread_new("mb-prefetch", prefetch_thread_func, &pf);

            /* Write PREVIOUS batch to SQLite (overlaps with PG prefetch) */
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
            pending_album_ids = album_ids;
            pending_count = release_count;
            pf = (prefetch_ctx_t){
                .pg = ctx->pg_client_prefetch,
                .release_ids = (const char* const*)release_ids,
                .release_count = release_count,
            };
            pf_thread = g_thread_new("mb-prefetch", prefetch_thread_func, &pf);
        } else {
            /* No prefetch client — fetch + write inline */
            GHashTable* rel = NULL;
            GHashTable* lnk = NULL;
            t0 = profile_now_ns();
            mb_fetch_all_batch(ctx->pg_client,
                (const char**)release_ids, release_count, &rel, &lnk);
            t1 = profile_now_ns();
            prof.pg_fetch_ns += (t1 - t0);

            t0 = profile_now_ns();
            write_resolve_batch(ctx, (const char* const*)release_ids, album_ids,
                                 release_count, rel, lnk, &prof);
            t1 = profile_now_ns();
            prof.sqlite_write_ns += (t1 - t0);
            prof.batch_count++;

            if (rel) g_hash_table_destroy(rel);
            if (lnk) g_hash_table_destroy(lnk);
            triage_free(release_ids, album_ids, release_count);
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
    if (ctx->pg_pool) {
        mb_pg_pool_destroy(ctx->pg_pool);
        ctx->pg_pool = NULL;
    }

    resolver_set_phase(ctx, MB_RESOLVE_COMPLETE);

    return ctx->cancelled ? QUADRATURE_ERROR_CANCELLED : QUADRATURE_OK;
}

void mb_resolver_cancel(mb_resolver_t* ctx) {
    if (ctx) ctx->cancelled = true;
}

void mb_resolver_destroy(mb_resolver_t* ctx) {
    if (!ctx) return;
    mb_pg_client_destroy(ctx->pg_client);
    if (ctx->pg_client_prefetch) mb_pg_client_destroy(ctx->pg_client_prefetch);
    if (ctx->pg_pool) mb_pg_pool_destroy(ctx->pg_pool);
    if (ctx->meta_db) {
        db_meta_checkpoint(ctx->meta_db);
        db_meta_close(ctx->meta_db);
    }
    g_mutex_clear(&ctx->progress_mutex);
    g_free(ctx->library_root);
    g_free(ctx->acoustid_index_url);
    g_free(ctx->solr_url);
    g_free(ctx);
}
