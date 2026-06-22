/**
 * Artist art fetching from fanart.tv API.
 *
 * Phase 7 of the indexer pipeline. Fetches artist thumbnail images
 * keyed by MusicBrainz artist MBID. Uses libsoup3 for HTTP and
 * json-glib for API response parsing.
 *
 * Rate-limited to 2 req/s with exponential backoff on 429/5xx.
 * Delta detection: skips artists whose artwork directory already exists.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/database.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <vips/vips.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>

#define FANART_API_URL "https://webservice.fanart.tv/v3.2/music"
/* fanart.tv application key — defined in CMakeLists.txt alongside the other
 * bundled application identifiers (ACOUSTID_APPLICATION_KEY, MUSICBRAINZ_USER_AGENT). */
#ifndef FANART_TV_APPLICATION_KEY
#error "FANART_TV_APPLICATION_KEY must be defined by CMakeLists.txt"
#endif
#define MAX_THUMBS              5
#define MAX_RETRIES             3
#define MAX_BACKOFF_MS          30000
#define INITIAL_BACKOFF_MS      1000
#define PROGRESS_THROTTLE_US    (100 * 1000) // 100ms
#define CONSECUTIVE_ERROR_LIMIT 3
#define PHASE_TIMEOUT_US        ((int64_t)30 * 60 * G_USEC_PER_SEC) // 30 minutes

// =============================================================================
// Rate Limiter
// =============================================================================

typedef struct {
    int64_t last_request_us;
    int base_interval_ms;
    int current_backoff_ms;
} rate_limiter_t;

static void
rate_limiter_init(rate_limiter_t *rl, int interval_ms)
{
    rl->last_request_us = 0;
    rl->base_interval_ms = interval_ms;
    rl->current_backoff_ms = 0;
}

static void
rate_limiter_wait(rate_limiter_t *rl)
{
    int wait_ms = rl->base_interval_ms + rl->current_backoff_ms;
    int64_t now = g_get_monotonic_time();
    int64_t elapsed_us = now - rl->last_request_us;
    int64_t wait_us = (int64_t)wait_ms * 1000;

    if (elapsed_us < wait_us) {
        g_usleep((gulong)(wait_us - elapsed_us));
    }
    rl->last_request_us = g_get_monotonic_time();
}

static void
rate_limiter_backoff(rate_limiter_t *rl)
{
    if (rl->current_backoff_ms == 0) {
        rl->current_backoff_ms = INITIAL_BACKOFF_MS;
    } else {
        rl->current_backoff_ms = MIN(rl->current_backoff_ms * 2, MAX_BACKOFF_MS);
    }
    g_debug("artist_art: backoff increased to %d ms", rl->current_backoff_ms);
}

static void
rate_limiter_reset_backoff(rate_limiter_t *rl)
{
    rl->current_backoff_ms = 0;
}

// =============================================================================
// JSON Parsing — extract image URLs sorted by likes
// =============================================================================

typedef struct {
    char *url;
    int likes;
} image_entry_t;

static int
compare_image_likes_desc(const void *a, const void *b)
{
    const image_entry_t *ea = a;
    const image_entry_t *eb = b;
    return eb->likes - ea->likes; // descending
}

/**
 * Parse fanart.tv API response and extract top N image URLs for a given key.
 * json_key is one of "artistthumb", "musicbanner", "artistbackground".
 * Returns number of URLs written to out_urls (caller must g_free each).
 */
static size_t
parse_image_urls(
    const char *json_data, gsize json_len, const char *json_key, char **out_urls, size_t max_urls)
{
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json_data, (gssize)json_len, &error)) {
        g_debug("artist_art: JSON parse error: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return 0;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return 0;
    }

    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, json_key)) {
        g_object_unref(parser);
        return 0;
    }

    JsonArray *images = json_object_get_array_member(obj, json_key);
    guint len = json_array_get_length(images);
    if (len == 0) {
        g_object_unref(parser);
        return 0;
    }

    // Collect all entries
    image_entry_t *entries = g_malloc(len * sizeof(image_entry_t));
    guint valid = 0;

    for (guint i = 0; i < len; i++) {
        JsonObject *entry = json_array_get_object_element(images, i);
        if (!entry)
            continue;

        const char *url = json_object_get_string_member_with_default(entry, "url", NULL);
        if (!url)
            continue;

        const char *likes_str = json_object_get_string_member_with_default(entry, "likes", "0");
        entries[valid].url = g_strdup(url);
        entries[valid].likes = atoi(likes_str);
        valid++;
    }

    // Sort by likes descending
    qsort(entries, valid, sizeof(image_entry_t), compare_image_likes_desc);

    // Take top N
    size_t result_count = MIN(valid, max_urls);
    for (size_t i = 0; i < result_count; i++) {
        out_urls[i] = entries[i].url;
        entries[i].url = NULL; // Transfer ownership
    }

    // Free remaining
    for (guint i = 0; i < valid; i++) {
        g_free(entries[i].url);
    }
    g_free(entries);
    g_object_unref(parser);

    return result_count;
}

// =============================================================================
// Image Download — raw storage (no resize)
// =============================================================================

/**
 * Download an image URL and save raw bytes to disk.
 * No resizing — originals are stored as-is for later atlas builds.
 */
static quadrature_result_t
download_and_save_image(SoupSession *session,
                        const char *url,
                        const char *output_path,
                        GCancellable *cancellable)
{
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg)
        return QUADRATURE_ERROR_INTERNAL;

    GError *error = NULL;
    GBytes *body = soup_session_send_and_read(session, msg, cancellable, &error);

    guint status = soup_message_get_status(msg);
    g_object_unref(msg);

    if (error) {
        g_debug("artist_art: download failed for %s: %s", url, error->message);
        g_error_free(error);
        if (body)
            g_bytes_unref(body);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (status != 200 || !body) {
        if (body)
            g_bytes_unref(body);
        return QUADRATURE_ERROR_INTERNAL;
    }

    gsize data_size;
    const char *data = g_bytes_get_data(body, &data_size);

    // Write raw response bytes to disk (fanart.tv serves JPEG)
    gboolean ok = g_file_set_contents(output_path, data, (gssize)data_size, &error);
    g_bytes_unref(body);

    if (!ok) {
        g_debug("artist_art: failed to write %s: %s", output_path, error->message);
        g_error_free(error);
        return QUADRATURE_ERROR_INTERNAL;
    }

    return QUADRATURE_OK;
}

// =============================================================================
// Delta Detection
// =============================================================================

/**
 * Check if a directory exists and has at least one non-hidden file.
 */
static bool
dir_has_files(const char *dir_path)
{
    GDir *gdir = g_dir_open(dir_path, 0, NULL);
    if (!gdir)
        return false;

    const char *entry;
    bool has_files = false;
    while ((entry = g_dir_read_name(gdir)) != NULL) {
        if (entry[0] != '.') {
            has_files = true;
            break;
        }
    }
    g_dir_close(gdir);
    return has_files;
}

/**
 * Check if artist already has cached thumbnails (keyed by MBID).
 * Returns true if artwork/artists/{mbid}/ exists and has at least one file.
 */
static bool
artist_has_cached_art(const char *artwork_dir, const char *mbid)
{
    char *dir = g_strdup_printf("%s/artists/%s", artwork_dir, mbid);
    bool found = dir_has_files(dir);
    g_free(dir);
    return found;
}

/**
 * Copy artist art from another library's artwork dir.
 * Returns true if art was found and successfully copied.
 */
static bool
copy_art_from_other_library(const char *const *other_dirs,
                            size_t other_count,
                            const char *mbid,
                            const char *dest_artwork_dir)
{
    for (size_t i = 0; i < other_count; i++) {
        char *src_dir = g_strdup_printf("%s/artists/%s", other_dirs[i], mbid);
        if (!dir_has_files(src_dir)) {
            g_free(src_dir);
            continue;
        }

        /* Found cached art — copy all files */
        char *dst_dir = g_strdup_printf("%s/artists/%s", dest_artwork_dir, mbid);
        g_mkdir_with_parents(dst_dir, 0755);

        GDir *gdir = g_dir_open(src_dir, 0, NULL);
        if (!gdir) {
            g_free(src_dir);
            g_free(dst_dir);
            continue;
        }

        const char *entry;
        bool copied_any = false;
        while ((entry = g_dir_read_name(gdir)) != NULL) {
            if (entry[0] == '.')
                continue;
            char *src_path = g_strdup_printf("%s/%s", src_dir, entry);
            char *dst_path = g_strdup_printf("%s/%s", dst_dir, entry);

            char *contents = NULL;
            gsize len = 0;
            if (g_file_get_contents(src_path, &contents, &len, NULL)) {
                if (g_file_set_contents(dst_path, contents, (gssize)len, NULL))
                    copied_any = true;
                g_free(contents);
            }
            g_free(src_path);
            g_free(dst_path);
        }
        g_dir_close(gdir);
        g_free(src_dir);
        g_free(dst_dir);

        if (copied_any) {
            g_debug("artist_art: copied %s art from %s", mbid, other_dirs[i]);
            return true;
        }
    }
    return false;
}

// =============================================================================
// Album Cover Art — piggyback on artist API responses
// =============================================================================

/**
 * Build a lookup map of release_group_id → album_id for albums that:
 *   1. Have a musicbrainz_release_group_id in the DB
 *   2. Are NOT present in the current album atlas (or have fallback placeholder art)
 *
 * Also populates artist_mbids_needing_fetch — the set of artist MBIDs whose
 * albums need cover art. These artists must be API-fetched even if their artist
 * art is already cached.
 *
 * Returns a GHashTable (string → gint64*) or NULL if no albums need covers.
 */
static GHashTable *
build_missing_album_map(quadrature_db_t *db,
                        const char *artwork_dir,
                        int thumb_size,
                        GHashTable **artist_mbids_needing_fetch)
{
    // Get all albums with release group IDs + their artist MBIDs
    int64_t *album_ids = NULL;
    char **rg_ids = NULL;
    char **a_mbids = NULL;
    size_t count = 0;
    quadrature_result_t res
        = db_get_albums_with_release_group_id(db, &album_ids, &rg_ids, &a_mbids, &count);
    if (res != QUADRATURE_OK || count == 0) {
        g_free(album_ids);
        g_strfreev(rg_ids);
        g_strfreev(a_mbids);
        return NULL;
    }

    // Load existing album atlas — the reader exposes has_no_art() via sorted
    // no_art_ids section (v3) or memcmp-derived set (v2 backward compat).
    // Albums with real artwork are in the sorted album_ids array.
    char *atlas_path = artwork_find_latest_atlas(artwork_dir, thumb_size);
    artwork_atlas_reader_t *reader = NULL;
    if (atlas_path) {
        reader = artwork_atlas_reader_open(atlas_path);
        g_free(atlas_path);
    }

    // Build map of only missing albums: release_group_id → album_id
    // An album is "missing" if it's not in the atlas art entries (binary search)
    // AND not in the no_art section AND not already cached on disk.
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *need_fetch = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *covers_dir = g_strdup_printf("%s/fanart_album_covers", artwork_dir);

    for (size_t i = 0; i < count; i++) {
        if (!rg_ids[i] || !a_mbids[i])
            continue;

        // Skip albums that already have real art in the atlas (O(log n) lookup)
        if (reader && artwork_atlas_reader_lookup(reader, album_ids[i]) >= 0)
            continue;
        // Albums in no_art are candidates — they have no local/embedded art,
        // so fanart.tv is their best chance. Don't skip them.

        // Check if we already have a cached fanart cover for this release group
        char cache_path[INDEXER_PATH_MAX];
        g_snprintf(cache_path, sizeof(cache_path), "%s/%s.jpg", covers_dir, rg_ids[i]);
        if (g_file_test(cache_path, G_FILE_TEST_EXISTS))
            continue;

        gint64 *val = g_new(gint64, 1);
        *val = album_ids[i];
        g_hash_table_insert(map, g_strdup(rg_ids[i]), val);
        g_hash_table_add(need_fetch, g_strdup(a_mbids[i]));
    }
    g_free(covers_dir);

    if (reader)
        artwork_atlas_reader_close(reader);
    g_free(album_ids);
    g_strfreev(rg_ids);
    g_strfreev(a_mbids);

    if (g_hash_table_size(map) == 0) {
        g_hash_table_destroy(map);
        g_hash_table_destroy(need_fetch);
        return NULL;
    }

    *artist_mbids_needing_fetch = need_fetch;
    return map;
}

/**
 * Parse album cover URLs from a fanart.tv JSON response.
 *
 * The actual API format is:
 *   "albums": [
 *     { "release_group_id": "uuid", "albumcover": [ {"url": ..., "likes": ...} ] },
 *     ...
 *   ]
 *
 * For each release_group_id found in missing_map, picks the highest-liked cover URL.
 * Writes (album_id, url) pairs to out arrays. Caller must g_free each URL.
 *
 * Returns number of covers found.
 */
static size_t
parse_album_covers(const char *json_data,
                   gsize json_len,
                   GHashTable *missing_map,
                   int64_t *out_album_ids,
                   char **out_release_group_ids,
                   char **out_urls,
                   size_t max_out)
{
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json_data, (gssize)json_len, &error)) {
        g_clear_error(&error);
        g_object_unref(parser);
        return 0;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return 0;
    }

    JsonObject *obj = json_node_get_object(root);
    if (!json_object_has_member(obj, "albums")) {
        g_object_unref(parser);
        return 0;
    }

    // "albums" is an array of objects, each with "release_group_id" and "albumcover"
    JsonNode *albums_node = json_object_get_member(obj, "albums");
    if (!albums_node || !JSON_NODE_HOLDS_ARRAY(albums_node)) {
        g_object_unref(parser);
        return 0;
    }

    JsonArray *albums = json_node_get_array(albums_node);
    guint album_count = json_array_get_length(albums);

    size_t found = 0;

    for (guint a = 0; a < album_count && found < max_out; a++) {
        JsonObject *album_obj = json_array_get_object_element(albums, a);
        if (!album_obj)
            continue;

        const char *rg_id
            = json_object_get_string_member_with_default(album_obj, "release_group_id", NULL);
        if (!rg_id)
            continue;

        // Check if this release group is in our missing-album map
        gint64 *value = g_hash_table_lookup(missing_map, rg_id);
        if (!value)
            continue;
        int64_t album_id = *value;

        if (!json_object_has_member(album_obj, "albumcover"))
            continue;
        JsonArray *covers = json_object_get_array_member(album_obj, "albumcover");
        if (!covers)
            continue;
        guint len = json_array_get_length(covers);
        if (len == 0)
            continue;

        // Find the highest-liked cover
        const char *best_url = NULL;
        int best_likes = -1;

        for (guint i = 0; i < len; i++) {
            JsonObject *entry = json_array_get_object_element(covers, i);
            if (!entry)
                continue;
            const char *url = json_object_get_string_member_with_default(entry, "url", NULL);
            if (!url)
                continue;
            const char *likes_str = json_object_get_string_member_with_default(entry, "likes", "0");
            int likes = atoi(likes_str);
            if (likes > best_likes) {
                best_likes = likes;
                best_url = url;
            }
        }

        if (best_url) {
            out_album_ids[found] = album_id;
            out_release_group_ids[found] = g_strdup(rg_id);
            out_urls[found] = g_strdup(best_url);
            found++;
        }
    }

    g_object_unref(parser);
    return found;
}

/**
 * Download an album cover image from a URL and save the raw bytes to a
 * persistent cache file. Returns QUADRATURE_OK on success.
 */
static quadrature_result_t
download_album_cover(SoupSession *session,
                     const char *url,
                     const char *cache_path,
                     GCancellable *cancellable)
{
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg)
        return QUADRATURE_ERROR_INTERNAL;

    GError *error = NULL;
    GBytes *body = soup_session_send_and_read(session, msg, cancellable, &error);
    guint status = soup_message_get_status(msg);
    g_object_unref(msg);

    if (error) {
        g_clear_error(&error);
        if (body)
            g_bytes_unref(body);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (status != 200 || !body) {
        if (body)
            g_bytes_unref(body);
        return QUADRATURE_ERROR_INTERNAL;
    }

    gsize data_size;
    const char *data = g_bytes_get_data(body, &data_size);
    gboolean ok = g_file_set_contents(cache_path, data, (gssize)data_size, &error);
    g_bytes_unref(body);

    if (!ok) {
        g_debug("artist_art: failed to write album cover %s: %s", cache_path, error->message);
        g_clear_error(&error);
        return QUADRATURE_ERROR_INTERNAL;
    }

    return QUADRATURE_OK;
}

// =============================================================================
// Main Entry Point
// =============================================================================

quadrature_result_t
artist_art_fetch_all(const artist_art_config_t *config, artist_art_progress_cb cb, void *user_data)
{
    if (!config || !config->artwork_dir || !config->db)
        return QUADRATURE_ERROR_INVALID_PARAM;

    // Query artists with MBIDs
    int64_t *artist_ids = NULL;
    char **mbids = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_artists_with_mbid(config->db, &artist_ids, &mbids, &count);
    if (res != QUADRATURE_OK)
        return res;

    if (count == 0) {
        g_message("Phase 7: no artists with MusicBrainz IDs — skipping artist art");
        return QUADRATURE_OK;
    }

    // Ensure artists directory exists
    char *artists_dir = g_strdup_printf("%s/artists", config->artwork_dir);
    g_mkdir_with_parents(artists_dir, 0755);

    // Build missing-album lookup map BEFORE pre-filter so we know which
    // cached artists need re-fetching for album cover extraction
    GHashTable *missing_album_map = NULL;
    GHashTable *artist_mbids_needing_fetch = NULL;

    if (config->album_artwork_dir && config->album_thumb_size > 0) {
        missing_album_map = build_missing_album_map(config->db,
                                                    config->album_artwork_dir,
                                                    config->album_thumb_size,
                                                    &artist_mbids_needing_fetch);
        if (missing_album_map) {
            g_message("Phase 7: %u albums missing artwork with release group IDs "
                      "(%u artists to re-fetch for covers)",
                      g_hash_table_size(missing_album_map),
                      g_hash_table_size(artist_mbids_needing_fetch));
        }
    }

    // Pre-filter: check filesystem cache and cross-library copies upfront
    // so progress.total reflects only artists that need HTTP fetches.
    // Exception: artists whose albums need cover art are always included.
    char **work_mbids = g_malloc(count * sizeof(char *));
    size_t work_count = 0;
    size_t cached_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (config->cancel_flag && atomic_load(config->cancel_flag))
            break;

        bool needs_album_covers = artist_mbids_needing_fetch
                                  && g_hash_table_contains(artist_mbids_needing_fetch, mbids[i]);

        if (artist_has_cached_art(config->artwork_dir, mbids[i]) && !needs_album_covers) {
            cached_count++;
            continue;
        }

        if (!needs_album_covers && config->other_artwork_dirs
            && config->other_artwork_dirs_count > 0) {
            if (copy_art_from_other_library(config->other_artwork_dirs,
                                            config->other_artwork_dirs_count,
                                            mbids[i],
                                            config->artwork_dir)) {
                cached_count++;
                continue;
            }
        }

        work_mbids[work_count] = mbids[i]; // borrows from mbids[]
        work_count++;
    }

    g_message("Phase 7: %zu artists with MBIDs (%zu cached, %zu to fetch)",
              count,
              cached_count,
              work_count);

    // Create cancellable for interrupting in-flight HTTP requests
    GCancellable *cancellable = g_cancellable_new();

    // Create HTTP session
    SoupSession *session
        = soup_session_new_with_options("timeout", 30, "user-agent", "Quadrature/1.0", NULL);

    rate_limiter_t rl;
    rate_limiter_init(&rl, config->rate_limit_ms > 0 ? config->rate_limit_ms : 500);

    artist_art_progress_t progress = { .total = work_count };
    int64_t last_progress_us = 0;
    bool abort_phase = false;
    int consecutive_errors = 0;
    int64_t phase_start = g_get_monotonic_time();

    // Fire initial progress so UI gets the real total immediately
    if (cb)
        cb(&progress, user_data);

    for (size_t i = 0; i < work_count && !abort_phase; i++) {
        // Check cancellation — also cancel in-flight HTTP requests
        if (config->cancel_flag && atomic_load(config->cancel_flag)) {
            g_cancellable_cancel(cancellable);
            g_message("Phase 7: cancelled at artist %zu/%zu", i, work_count);
            break;
        }

        // Phase timeout
        if (g_get_monotonic_time() - phase_start > PHASE_TIMEOUT_US) {
            g_warning("Phase 7: timeout after 30 minutes — %zu/%zu artists processed",
                      progress.processed,
                      work_count);
            break;
        }

        const char *mbid = work_mbids[i];

        // Rate limit
        rate_limiter_wait(&rl);

        // Build API URL — project key always required; personal key optional
        char *api_url
            = config->personal_api_key
                  ? g_strdup_printf("%s/%s?api_key=%s&client_key=%s",
                                    FANART_API_URL,
                                    mbid,
                                    FANART_TV_APPLICATION_KEY,
                                    config->personal_api_key)
                  : g_strdup_printf(
                        "%s/%s?api_key=%s", FANART_API_URL, mbid, FANART_TV_APPLICATION_KEY);

        // Fetch with retry
        SoupMessage *msg = soup_message_new("GET", api_url);
        g_free(api_url);

        if (!msg) {
            progress.errors++;
            progress.processed++;
            continue;
        }

        GError *error = NULL;
        GBytes *body = NULL;
        guint status = 0;
        int retries = 0;
        bool artist_http_error = false;

        while (retries <= MAX_RETRIES) {
            body = soup_session_send_and_read(session, msg, cancellable, &error);
            status = soup_message_get_status(msg);

            if (error) {
                g_debug("artist_art: request failed for %s: %s", mbid, error->message);
                g_clear_error(&error);
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }
                progress.errors++;
                artist_http_error = true;
                break;
            }

            if (status == 200) {
                rate_limiter_reset_backoff(&rl);
                break;
            } else if (status == 404) {
                // No images for this artist — write sentinel so pre-filter skips on re-index
                progress.no_images++;
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }

                char *artist_dir = g_strdup_printf("%s/artists/%s", config->artwork_dir, mbid);
                g_mkdir_with_parents(artist_dir, 0755);
                char *marker = g_strdup_printf("%s/no-images", artist_dir);
                g_file_set_contents(marker, "", 0, NULL);
                g_free(marker);
                g_free(artist_dir);

                break;
            } else if (status == 401) {
                g_warning("Phase 7: fanart.tv returned 401 (unauthorized) — aborting phase");
                abort_phase = true;
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }
                break;
            } else if (status == 403) {
                g_warning("Phase 7: fanart.tv returned 403 (invalid API key) — aborting phase");
                abort_phase = true;
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }
                break;
            } else if (status == 429) {
                rate_limiter_backoff(&rl);
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }
                retries++;
                if (retries <= MAX_RETRIES) {
                    rate_limiter_wait(&rl);
                    // Re-create message for retry
                    g_object_unref(msg);
                    char *retry_url = config->personal_api_key
                                          ? g_strdup_printf("%s/%s?api_key=%s&client_key=%s",
                                                            FANART_API_URL,
                                                            mbid,
                                                            FANART_TV_APPLICATION_KEY,
                                                            config->personal_api_key)
                                          : g_strdup_printf("%s/%s?api_key=%s",
                                                            FANART_API_URL,
                                                            mbid,
                                                            FANART_TV_APPLICATION_KEY);
                    msg = soup_message_new("GET", retry_url);
                    g_free(retry_url);
                    if (!msg)
                        break;
                }
                continue;
            } else {
                // 5xx or other
                g_debug("artist_art: HTTP %u for %s — skipping", status, mbid);
                rate_limiter_backoff(&rl);
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }
                progress.errors++;
                artist_http_error = true;
                break;
            }
        }

        g_clear_object(&msg);

        if (abort_phase) {
            if (body)
                g_bytes_unref(body);
            progress.processed++;
            break;
        }

        // Consecutive error tracking and telemetry
        if (artist_http_error) {
            if (config->http_errors)
                atomic_fetch_add(config->http_errors, 1);
            consecutive_errors++;
            if (consecutive_errors >= CONSECUTIVE_ERROR_LIMIT) {
                g_warning("Phase 7: aborting after %d consecutive network errors",
                          CONSECUTIVE_ERROR_LIMIT);
                abort_phase = true;
                if (body) {
                    g_bytes_unref(body);
                    body = NULL;
                }
                progress.processed++;
                break;
            }
        } else {
            consecutive_errors = 0;
        }

        if (status == 200 && body) {
            gsize body_size;
            const char *body_data = g_bytes_get_data(body, &body_size);

            // Download three image types from the same API response
            const char *image_types[] = { "artistthumb", "musicbanner", "artistbackground" };
            const char *prefixes[] = { "thumb", "banner", "background" };
            size_t total_downloaded_for_artist = 0;

            // Create artist directory keyed by MBID (globally stable across libraries)
            char *artist_dir = g_strdup_printf("%s/artists/%s", config->artwork_dir, mbid);
            bool dir_created = false;

            for (int t = 0; t < 3; t++) {
                char *urls[MAX_THUMBS] = { 0 };
                size_t url_count
                    = parse_image_urls(body_data, body_size, image_types[t], urls, MAX_THUMBS);

                for (size_t u = 0; u < url_count; u++) {
                    // Check cancellation between downloads
                    if (config->cancel_flag && atomic_load(config->cancel_flag)) {
                        g_cancellable_cancel(cancellable);
                        for (size_t f = u; f < url_count; f++)
                            g_free(urls[f]);
                        goto images_done;
                    }

                    if (!dir_created) {
                        g_mkdir_with_parents(artist_dir, 0755);
                        dir_created = true;
                    }

                    char *path = g_strdup_printf("%s/%s_%zu.jpg", artist_dir, prefixes[t], u);
                    quadrature_result_t dl_res
                        = download_and_save_image(session, urls[u], path, cancellable);
                    if (dl_res == QUADRATURE_OK) {
                        progress.downloaded++;
                        total_downloaded_for_artist++;
                    } else {
                        g_debug("artist_art: failed to download %s_%zu for artist %s",
                                prefixes[t],
                                u,
                                mbid);
                    }
                    g_free(path);
                    g_free(urls[u]);
                }
            }

        images_done:
            if (total_downloaded_for_artist == 0) {
                // 200 but no artist images (only album art) — write sentinel
                // so pre-filter skips this artist on future runs
                g_mkdir_with_parents(artist_dir, 0755);
                char *marker = g_strdup_printf("%s/no-images", artist_dir);
                g_file_set_contents(marker, "", 0, NULL);
                g_free(marker);
                progress.no_images++;
            }

            // Extract album cover art from the same response (zero extra API calls)
            // Save to persistent cache at {artwork_dir}/fanart_album_covers/{album_id}.jpg
            // Phase 4 will pick these up as a fallback on current and future reindexes.
            if (missing_album_map && g_hash_table_size(missing_album_map) > 0
                && config->album_artwork_dir) {
                int64_t cover_album_ids[32];
                char *cover_rg_ids[32];
                char *cover_urls[32];
                size_t cover_count = parse_album_covers(body_data,
                                                        body_size,
                                                        missing_album_map,
                                                        cover_album_ids,
                                                        cover_rg_ids,
                                                        cover_urls,
                                                        32);

                if (cover_count > 0) {
                    char *covers_dir
                        = g_strdup_printf("%s/fanart_album_covers", config->album_artwork_dir);
                    g_mkdir_with_parents(covers_dir, 0755);
                    g_message("Phase 7: saving %zu album covers to %s", cover_count, covers_dir);

                    for (size_t c = 0; c < cover_count; c++) {
                        if (config->cancel_flag && atomic_load(config->cancel_flag)) {
                            for (size_t f = c; f < cover_count; f++) {
                                g_free(cover_urls[f]);
                                g_free(cover_rg_ids[f]);
                            }
                            break;
                        }

                        char *cache_path
                            = g_strdup_printf("%s/%s.jpg", covers_dir, cover_rg_ids[c]);

                        if (download_album_cover(session, cover_urls[c], cache_path, cancellable)
                            == QUADRATURE_OK) {
                            progress.album_covers++;
                            g_debug("artist_art: saved album cover %s → %s",
                                    cover_rg_ids[c],
                                    cache_path);

                            // Remove from map so we don't re-download
                            g_hash_table_remove(missing_album_map, cover_rg_ids[c]);
                        }
                        g_free(cache_path);
                        g_free(cover_urls[c]);
                        g_free(cover_rg_ids[c]);
                    }
                    g_free(covers_dir);
                }
            }

            g_free(artist_dir);
            g_bytes_unref(body);
        }

        progress.processed++;

        // Throttled progress callback
        if (cb) {
            int64_t now = g_get_monotonic_time();
            if (now - last_progress_us >= PROGRESS_THROTTLE_US) {
                last_progress_us = now;
                cb(&progress, user_data);
            }
        }
    }

    // Final progress callback
    if (cb)
        cb(&progress, user_data);

    // =========================================================================
    // Global artist atlas: flock() serialize, diff filesystem vs atlas, rewrite if dirty
    // =========================================================================

    if (config->atlas_path && config->atlas_lock_path) {
        int atlas_thumb = config->art_thumb_size > 0 ? config->art_thumb_size : 48;

        // Ensure atlas directory exists
        char *atlas_dir = g_path_get_dirname(config->atlas_path);
        g_mkdir_with_parents(atlas_dir, 0755);
        g_free(atlas_dir);

        // Acquire file lock for exclusive atlas access
        int lock_fd = open(config->atlas_lock_path, O_CREAT | O_RDWR, 0644);
        if (lock_fd < 0) {
            g_warning("Phase 7: failed to open lockfile %s", config->atlas_lock_path);
            goto skip_atlas;
        }

        if (flock(lock_fd, LOCK_EX) != 0) {
            g_warning("Phase 7: flock() failed on %s", config->atlas_lock_path);
            close(lock_fd);
            goto skip_atlas;
        }

        g_message("Phase 7: acquired atlas lock, scanning %zu MBIDs", count);

        // Create builder and seed with existing atlas entries
        artist_atlas_builder_t *atlas = NULL;
        res = artist_atlas_builder_create(config->atlas_path, atlas_thumb, &atlas);
        if (res != QUADRATURE_OK) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            goto skip_atlas;
        }

        artist_atlas_builder_load_existing(atlas);

        // Full scan: for each MBID in this library, check filesystem and update atlas
        bool dirty = false;
        for (size_t i = 0; i < count; i++) {
            if (config->cancel_flag && atomic_load(config->cancel_flag))
                break;

            uint8_t uuid_bin[ARTIST_ATLAS_UUID_SIZE];
            if (!mbid_parse(mbids[i], uuid_bin))
                continue;

            char *thumb_path
                = g_strdup_printf("%s/artists/%s/thumb_0.jpg", config->artwork_dir, mbids[i]);
            char *no_images_path
                = g_strdup_printf("%s/artists/%s/no-images", config->artwork_dir, mbids[i]);

            bool has_thumb = g_file_test(thumb_path, G_FILE_TEST_EXISTS);
            bool has_no_images = g_file_test(no_images_path, G_FILE_TEST_EXISTS);

            if (has_thumb) {
                /* Load + resize; require a strict 3-band RGB result so atlas
                 * rows stay well-formed. Grayscale/RGBA inputs are skipped. */
                VipsImage *img = NULL;
                if (vips_thumbnail(thumb_path,
                                   &img,
                                   atlas_thumb,
                                   "height",
                                   atlas_thumb,
                                   "crop",
                                   VIPS_INTERESTING_CENTRE,
                                   NULL)
                    == 0) {
                    size_t pixel_size = 0;
                    void *pixel_data = vips_image_write_to_memory(img, &pixel_size);
                    size_t expected = (size_t)atlas_thumb * atlas_thumb * ARTWORK_ATLAS_CHANNELS;
                    if (pixel_data && pixel_size == expected
                        && artist_atlas_builder_add_art(atlas, uuid_bin, pixel_data, expected)
                               == QUADRATURE_OK) {
                        dirty = true;
                    }
                    g_free(pixel_data);
                    g_object_unref(img);
                } else {
                    vips_error_clear();
                }
            } else if (has_no_images) {
                // Known no-artwork — add sentinel
                if (artist_atlas_builder_add_no_art(atlas, uuid_bin) == QUADRATURE_OK)
                    dirty = true;
            }

            g_free(thumb_path);
            g_free(no_images_path);
        }

        if (dirty) {
            res = artist_atlas_builder_finish(atlas);
            if (res == QUADRATURE_OK) {
                g_message("Phase 7: global artist atlas updated: %s", config->atlas_path);
            } else {
                g_warning("Phase 7: failed to write global artist atlas");
            }
        } else {
            g_message("Phase 7: global artist atlas unchanged");
        }

        artist_atlas_builder_destroy(atlas);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
skip_atlas:
    (void)0;

    g_message("Phase 7: done — %zu fetched, %zu downloaded, %zu cached (pre-filtered), "
              "%zu no images, %zu errors, %zu album covers",
              progress.processed,
              progress.downloaded,
              cached_count,
              progress.no_images,
              progress.errors,
              progress.album_covers);

    // Cleanup
    if (missing_album_map)
        g_hash_table_destroy(missing_album_map);
    if (artist_mbids_needing_fetch)
        g_hash_table_destroy(artist_mbids_needing_fetch);
    g_object_unref(cancellable);
    g_object_unref(session);
    g_free(artists_dir);
    g_free(work_mbids); // borrowed strings — freed via g_strfreev(mbids)
    g_free(artist_ids);
    g_strfreev(mbids);

    return abort_phase ? QUADRATURE_ERROR_INTERNAL : QUADRATURE_OK;
}
