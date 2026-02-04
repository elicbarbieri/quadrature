/**
 * MusicBrainz resolver — DB-driven metadata resolution.
 *
 * Queries the database for unresolved albums, uses two-tier resolution:
 * 1. Check file tags for existing MUSICBRAINZ_ALBUMID
 * 2. If no tags → read cached fingerprints from DB → local AcoustID PG → consensus vote
 * Then: fetch full release from MusicBrainz PG → match tracks → write to SQLite.
 *
 * No HTTP. No file modifications. All data stays in the database.
 */

#include "internal.h"
#include "quadrature/quadrature_database.h"
#include <string.h>
#include <math.h>
#include <time.h>

// Various Artists MB ID
#define VA_MUSICBRAINZ_ID "89ad4ac3-39f7-470e-963a-56509c546377"

// =============================================================================
// Resolver Context
// =============================================================================

struct mb_resolver {
    quadrature_db_t* db;
    mb_pg_client_t* pg_client;
    mb_resolver_options_t options;
    mb_resolver_progress_cb callback;
    void* user_data;

    mb_resolver_progress_t progress;
    GMutex progress_mutex;

    volatile bool cancelled;
};

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

static void resolver_set_current(mb_resolver_t* ctx, const char* album) {
    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.current_album = album;
    g_mutex_unlock(&ctx->progress_mutex);
}

// =============================================================================
// Joinphrase → Role Mapper
// =============================================================================

static bool joinphrase_is_featuring(const char* jp) {
    if (!jp) return false;
    char* lower = g_ascii_strdown(jp, -1);
    bool result = (strstr(lower, "feat.") != NULL ||
                   strstr(lower, "feat ") != NULL ||
                   strstr(lower, "featuring") != NULL ||
                   strstr(lower, "ft.") != NULL);
    g_free(lower);
    return result;
}

static void mb_credits_to_track_artists(
    quadrature_db_t* db,
    const mb_artist_t* credits, size_t count,
    db_track_artist_t** out, size_t* out_count) {

    *out = g_new0(db_track_artist_t, count);
    *out_count = count;
    bool seen_featuring = false;

    for (size_t i = 0; i < count; i++) {
        int64_t artist_id = db_get_or_create_artist_mb(
            db, credits[i].name, credits[i].sort_name, credits[i].id);

        (*out)[i].artist_id = artist_id;
        (*out)[i].role = seen_featuring ? ARTIST_ROLE_FEATURING : ARTIST_ROLE_PRIMARY;
        (*out)[i].position = (int)i;

        if (joinphrase_is_featuring(credits[i].joinphrase)) {
            seen_featuring = true;
        }
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
// Release Matching (fingerprint → AcoustID → consensus vote)
// =============================================================================

static char* check_tags_for_release_id(quadrature_db_t* db, int64_t album_id) {
    // Get a sample track from this album and check its file tags
    db_track_t* tracks = NULL;
    size_t count = 0;
    if (db_get_tracks_by_album(db, album_id, &tracks, &count) != QUADRATURE_OK || count == 0) {
        return NULL;
    }

    char* release_id = NULL;
    for (size_t i = 0; i < count && !release_id; i++) {
        if (!tracks[i].path) continue;

        mb_tags_t tags;
        if (mb_tags_read(tracks[i].path, &tags) == QUADRATURE_OK) {
            if (tags.release_id && tags.release_id[0]) {
                release_id = g_strdup(tags.release_id);
            }
            mb_tags_free(&tags);
        }
    }

    db_tracks_free(tracks, count);
    return release_id;
}

static char* find_release_by_fingerprint(mb_resolver_t* ctx, int64_t album_id) {
    db_track_t* tracks = NULL;
    size_t track_count = 0;
    if (db_get_tracks_by_album(ctx->db, album_id, &tracks, &track_count) != QUADRATURE_OK
        || track_count == 0) {
        return NULL;
    }

    size_t tracks_to_check = track_count < (size_t)MB_MIN_TRACKS_FOR_MATCH
        ? track_count : (size_t)MB_MIN_TRACKS_FOR_MATCH;

    GHashTable* release_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    size_t fingerprinted = 0;

    for (size_t i = 0; i < tracks_to_check && !ctx->cancelled; i++) {
        if (!tracks[i].path) continue;

        resolver_set_phase(ctx, MB_RESOLVE_FINGERPRINTING);

        // Try to read cached fingerprint from DB first
        char* cached_fp = NULL;
        int cached_duration = 0;
        mb_fingerprint_t fp;
        bool have_fp = false;

        if (db_get_track_fingerprint(ctx->db, tracks[i].id, &cached_fp, &cached_duration) == QUADRATURE_OK
            && cached_fp) {
            fp.fingerprint = cached_fp;
            fp.duration = cached_duration;
            have_fp = true;
        } else {
            // Fall back to generating fingerprint (shouldn't happen if indexer ran)
            if (mb_fingerprint_generate(tracks[i].path, &fp) == QUADRATURE_OK) {
                have_fp = true;
                // Cache it for next time
                db_set_track_fingerprint(ctx->db, tracks[i].id, fp.fingerprint, fp.duration);
            }
        }

        if (!have_fp) continue;
        fingerprinted++;

        resolver_set_phase(ctx, MB_RESOLVE_MATCHING);

        mb_acoustid_response_t response;
        if (mb_acoustid_lookup(ctx->pg_client, &fp, &response) == QUADRATURE_OK) {
            for (size_t j = 0; j < response.count; j++) {
                if (response.results[j].release_id) {
                    gpointer cnt = g_hash_table_lookup(release_counts,
                                                        response.results[j].release_id);
                    g_hash_table_insert(release_counts,
                                        g_strdup(response.results[j].release_id),
                                        GINT_TO_POINTER(GPOINTER_TO_INT(cnt) + 1));
                }
            }
            mb_acoustid_response_free(&response);
        }

        if (cached_fp) {
            g_free(cached_fp);
        } else {
            mb_fingerprint_free(&fp);
        }
    }

    db_tracks_free(tracks, track_count);

    // Find release with most matches
    char* best_release = NULL;
    int best_count = 0;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, release_counts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        int count = GPOINTER_TO_INT(value);
        if (count > best_count) {
            best_count = count;
            g_free(best_release);
            best_release = g_strdup((char*)key);
        }
    }
    g_hash_table_destroy(release_counts);

    // Enforce confidence threshold
    if (fingerprinted > 0 && best_count > 0) {
        double confidence = (double)best_count / (double)fingerprinted;
        if (confidence < MB_MATCH_CONFIDENCE && fingerprinted > 1) {
            g_debug("Release %s confidence %.0f%% below threshold", best_release, confidence * 100);
            g_free(best_release);
            return NULL;
        }
    } else {
        g_free(best_release);
        return NULL;
    }

    return best_release;
}

// =============================================================================
// Track Matching (MB recordings → local tracks)
// =============================================================================

typedef struct {
    size_t mb_idx;
    size_t local_idx;
    double score;
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
                matches[matched].score = 1.0;
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
            matches[matched].score = best_score;
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
// Resolve Single Album
// =============================================================================

static quadrature_result_t resolve_album(mb_resolver_t* ctx, int64_t album_id) {
    if (ctx->cancelled) return QUADRATURE_ERROR_CANCELLED;

    // Get album path for progress display
    db_track_t* tracks = NULL;
    size_t track_count = 0;
    quadrature_result_t res = db_get_tracks_by_album(ctx->db, album_id, &tracks, &track_count);
    if (res != QUADRATURE_OK || track_count == 0) {
        db_set_album_mb_status(ctx->db, album_id, MB_STATUS_FAILED, (int64_t)time(NULL));
        if (tracks) db_tracks_free(tracks, track_count);
        return QUADRATURE_OK;
    }

    // Set progress to album path (use first track's album name)
    resolver_set_current(ctx, tracks[0].album ? tracks[0].album : "Unknown");

    // Step 1: Check existing file tags for release ID
    resolver_set_phase(ctx, MB_RESOLVE_QUERYING);
    char* release_id = check_tags_for_release_id(ctx->db, album_id);

    // Step 2: If no tags → fingerprint + AcoustID
    if (!release_id && !ctx->cancelled) {
        release_id = find_release_by_fingerprint(ctx, album_id);
    }

    if (!release_id) {
        g_debug("No MB match for album %" G_GINT64_FORMAT, album_id);
        db_set_album_mb_status(ctx->db, album_id, MB_STATUS_NO_MATCH, (int64_t)time(NULL));
        db_tracks_free(tracks, track_count);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_no_match++;
        g_mutex_unlock(&ctx->progress_mutex);
        return QUADRATURE_OK;
    }

    if (ctx->cancelled) {
        g_free(release_id);
        db_tracks_free(tracks, track_count);
        return QUADRATURE_ERROR_CANCELLED;
    }

    // Step 3: Fetch full release from MB PG
    resolver_set_phase(ctx, MB_RESOLVE_FETCHING);

    mb_release_t release;
    res = mb_fetch_release(ctx->pg_client, release_id, &release);
    g_free(release_id);

    if (res != QUADRATURE_OK) {
        g_warning("Failed to fetch release for album %" G_GINT64_FORMAT ": %d", album_id, res);
        db_set_album_mb_status(ctx->db, album_id, MB_STATUS_FAILED, (int64_t)time(NULL));
        db_tracks_free(tracks, track_count);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return QUADRATURE_OK;
    }

    if (ctx->cancelled) {
        mb_release_free(&release);
        db_tracks_free(tracks, track_count);
        return QUADRATURE_ERROR_CANCELLED;
    }

    // Step 4: Match MB recordings to local tracks
    track_match_t* matches = NULL;
    size_t match_count = 0;
    match_tracks(&release, tracks, track_count, &matches, &match_count);

    // Step 5: Write to DB in a single transaction
    resolver_set_phase(ctx, MB_RESOLVE_WRITING);

    res = db_begin_transaction(ctx->db);
    if (res != QUADRATURE_OK) {
        mb_release_free(&release);
        db_tracks_free(tracks, track_count);
        g_free(matches);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return QUADRATURE_OK;
    }

    // Check if Various Artists
    bool is_va = false;
    if (release.artist_count > 0 && release.artists[0].id) {
        is_va = (strcmp(release.artists[0].id, VA_MUSICBRAINZ_ID) == 0);
    }

    // Create/update album artist
    if (release.artist_count > 0 && !is_va) {
        int64_t album_artist_id = db_get_or_create_artist_mb(
            ctx->db, release.artists[0].name,
            release.artists[0].sort_name, release.artists[0].id);
        db_update_album_artist(ctx->db, album_id, album_artist_id, album_artist_id, false);
    } else if (is_va) {
        int64_t va_id = db_get_or_create_artist_mb(
            ctx->db, "Various Artists", "Various Artists", VA_MUSICBRAINZ_ID);
        db_update_album_artist(ctx->db, album_id, va_id, va_id, true);
    }

    // Update album with MB metadata
    uint16_t year = parse_mb_year(release.date);
    db_update_album_mb(ctx->db, album_id,
        release.id, release.release_group_id,
        release.type, release.label, release.barcode,
        year, MB_STATUS_RESOLVED);

    // Update each matched track
    for (size_t m = 0; m < match_count; m++) {
        mb_recording_t* rec = &release.recordings[matches[m].mb_idx];
        db_track_t* local = &tracks[matches[m].local_idx];

        // Update track with recording ID and title
        db_update_track_mb(ctx->db, local->id, rec->id, rec->title);

        // Convert artist credits to track_artists with roles
        if (rec->artist_count > 0) {
            db_track_artist_t* ta = NULL;
            size_t ta_count = 0;
            mb_credits_to_track_artists(ctx->db, rec->artists, rec->artist_count,
                                         &ta, &ta_count);
            db_set_track_artists(ctx->db, local->id, ta, ta_count);
            g_free(ta);
        }
    }

    res = db_commit(ctx->db);
    if (res != QUADRATURE_OK) {
        db_rollback(ctx->db);
        mb_release_free(&release);
        db_tracks_free(tracks, track_count);
        g_free(matches);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return QUADRATURE_OK;
    }

    mb_release_free(&release);
    db_tracks_free(tracks, track_count);
    g_free(matches);

    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.albums_resolved++;
    g_mutex_unlock(&ctx->progress_mutex);

    return QUADRATURE_OK;
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

    g_mutex_init(&ctx->progress_mutex);

    // PostgreSQL client (for MusicBrainz + AcoustID lookups)
    quadrature_result_t res = mb_pg_client_create(options->pg_conninfo, &ctx->pg_client);
    if (res != QUADRATURE_OK) {
        g_mutex_clear(&ctx->progress_mutex);
        g_free(ctx);
        return res;
    }

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

    // Get unresolved albums from DB
    int64_t* album_ids = NULL;
    size_t album_count = 0;
    quadrature_result_t res = db_get_unresolved_albums(ctx->db, &album_ids, &album_count);
    if (res != QUADRATURE_OK) return res;

    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.albums_total = album_count;
    g_mutex_unlock(&ctx->progress_mutex);
    resolver_update_progress(ctx);

    g_debug("MB resolver: %zu unresolved albums", album_count);

    // Process each album
    for (size_t i = 0; i < album_count && !ctx->cancelled; i++) {
        resolve_album(ctx, album_ids[i]);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_processed++;
        ctx->progress.progress = album_count > 0
            ? (double)ctx->progress.albums_processed / (double)album_count
            : 1.0;
        g_mutex_unlock(&ctx->progress_mutex);
        resolver_update_progress(ctx);
    }

    g_free(album_ids);

    resolver_set_phase(ctx, MB_RESOLVE_COMPLETE);
    resolver_set_current(ctx, NULL);

    return ctx->cancelled ? QUADRATURE_ERROR_CANCELLED : QUADRATURE_OK;
}

void mb_resolver_cancel(mb_resolver_t* ctx) {
    if (ctx) ctx->cancelled = true;
}

void mb_resolver_destroy(mb_resolver_t* ctx) {
    if (!ctx) return;
    mb_pg_client_destroy(ctx->pg_client);
    g_mutex_clear(&ctx->progress_mutex);
    g_free(ctx);
}
