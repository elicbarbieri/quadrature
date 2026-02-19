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

#define FANART_API_URL      "https://webservice.fanart.tv/v3.2/music"
#define FANART_PROJECT_KEY  "400cfe22b47e1fb93bfeb3ca452059c8"
#define MAX_THUMBS 5
#define MAX_RETRIES 3
#define MAX_BACKOFF_MS 30000
#define INITIAL_BACKOFF_MS 1000
#define PROGRESS_THROTTLE_US (100 * 1000)  // 100ms

// =============================================================================
// Rate Limiter
// =============================================================================

typedef struct {
    int64_t last_request_us;
    int base_interval_ms;
    int current_backoff_ms;
} rate_limiter_t;

static void rate_limiter_init(rate_limiter_t* rl, int interval_ms) {
    rl->last_request_us = 0;
    rl->base_interval_ms = interval_ms;
    rl->current_backoff_ms = 0;
}

static void rate_limiter_wait(rate_limiter_t* rl) {
    int wait_ms = rl->base_interval_ms + rl->current_backoff_ms;
    int64_t now = g_get_monotonic_time();
    int64_t elapsed_us = now - rl->last_request_us;
    int64_t wait_us = (int64_t)wait_ms * 1000;

    if (elapsed_us < wait_us) {
        g_usleep((gulong)(wait_us - elapsed_us));
    }
    rl->last_request_us = g_get_monotonic_time();
}

static void rate_limiter_backoff(rate_limiter_t* rl) {
    if (rl->current_backoff_ms == 0) {
        rl->current_backoff_ms = INITIAL_BACKOFF_MS;
    } else {
        rl->current_backoff_ms = MIN(rl->current_backoff_ms * 2, MAX_BACKOFF_MS);
    }
    g_debug("artist_art: backoff increased to %d ms", rl->current_backoff_ms);
}

static void rate_limiter_reset_backoff(rate_limiter_t* rl) {
    rl->current_backoff_ms = 0;
}

// =============================================================================
// JSON Parsing — extract image URLs sorted by likes
// =============================================================================

typedef struct {
    char* url;
    int likes;
} image_entry_t;

static int compare_image_likes_desc(const void* a, const void* b) {
    const image_entry_t* ea = a;
    const image_entry_t* eb = b;
    return eb->likes - ea->likes;  // descending
}

/**
 * Parse fanart.tv API response and extract top N image URLs for a given key.
 * json_key is one of "artistthumb", "musicbanner", "artistbackground".
 * Returns number of URLs written to out_urls (caller must g_free each).
 */
static size_t parse_image_urls(const char* json_data, gsize json_len,
                               const char* json_key,
                               char** out_urls, size_t max_urls) {
    JsonParser* parser = json_parser_new();
    GError* error = NULL;

    if (!json_parser_load_from_data(parser, json_data, (gssize)json_len, &error)) {
        g_debug("artist_art: JSON parse error: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return 0;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return 0;
    }

    JsonObject* obj = json_node_get_object(root);
    if (!json_object_has_member(obj, json_key)) {
        g_object_unref(parser);
        return 0;
    }

    JsonArray* images = json_object_get_array_member(obj, json_key);
    guint len = json_array_get_length(images);
    if (len == 0) {
        g_object_unref(parser);
        return 0;
    }

    // Collect all entries
    image_entry_t* entries = g_malloc(len * sizeof(image_entry_t));
    guint valid = 0;

    for (guint i = 0; i < len; i++) {
        JsonObject* entry = json_array_get_object_element(images, i);
        if (!entry) continue;

        const char* url = json_object_get_string_member_with_default(entry, "url", NULL);
        if (!url) continue;

        const char* likes_str = json_object_get_string_member_with_default(entry, "likes", "0");
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
        entries[i].url = NULL;  // Transfer ownership
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
static quadrature_result_t download_and_save_image(SoupSession* session,
                                                    const char* url,
                                                    const char* output_path,
                                                    GCancellable* cancellable) {
    SoupMessage* msg = soup_message_new("GET", url);
    if (!msg) return QUADRATURE_ERROR_INTERNAL;

    GError* error = NULL;
    GBytes* body = soup_session_send_and_read(session, msg, cancellable, &error);

    guint status = soup_message_get_status(msg);
    g_object_unref(msg);

    if (error) {
        g_debug("artist_art: download failed for %s: %s", url, error->message);
        g_error_free(error);
        if (body) g_bytes_unref(body);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (status != 200 || !body) {
        if (body) g_bytes_unref(body);
        return QUADRATURE_ERROR_INTERNAL;
    }

    gsize data_size;
    const char* data = g_bytes_get_data(body, &data_size);

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
static bool dir_has_files(const char* dir_path) {
    GDir* gdir = g_dir_open(dir_path, 0, NULL);
    if (!gdir) return false;

    const char* entry;
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
static bool artist_has_cached_art(const char* artwork_dir, const char* mbid) {
    char* dir = g_strdup_printf("%s/artists/%s", artwork_dir, mbid);
    bool found = dir_has_files(dir);
    g_free(dir);
    return found;
}

/**
 * Copy artist art from another library's artwork dir.
 * Returns true if art was found and successfully copied.
 */
static bool copy_art_from_other_library(const char* const* other_dirs, size_t other_count,
                                         const char* mbid, const char* dest_artwork_dir) {
    for (size_t i = 0; i < other_count; i++) {
        char* src_dir = g_strdup_printf("%s/artists/%s", other_dirs[i], mbid);
        if (!dir_has_files(src_dir)) {
            g_free(src_dir);
            continue;
        }

        /* Found cached art — copy all files */
        char* dst_dir = g_strdup_printf("%s/artists/%s", dest_artwork_dir, mbid);
        g_mkdir_with_parents(dst_dir, 0755);

        GDir* gdir = g_dir_open(src_dir, 0, NULL);
        if (!gdir) {
            g_free(src_dir);
            g_free(dst_dir);
            continue;
        }

        const char* entry;
        bool copied_any = false;
        while ((entry = g_dir_read_name(gdir)) != NULL) {
            if (entry[0] == '.') continue;
            char* src_path = g_strdup_printf("%s/%s", src_dir, entry);
            char* dst_path = g_strdup_printf("%s/%s", dst_dir, entry);

            char* contents = NULL;
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
// Main Entry Point
// =============================================================================

quadrature_result_t artist_art_fetch_all(const artist_art_config_t* config,
                                          artist_art_progress_cb cb, void* user_data) {
    if (!config || !config->artwork_dir || !config->db)
        return QUADRATURE_ERROR_INVALID_PARAM;

    // Query artists with MBIDs
    int64_t* artist_ids = NULL;
    char** mbids = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_artists_with_mbid(config->db, &artist_ids, &mbids, &count);
    if (res != QUADRATURE_OK) return res;

    if (count == 0) {
        g_message("Phase 7: no artists with MusicBrainz IDs — skipping artist art");
        return QUADRATURE_OK;
    }

    // Ensure artists directory exists
    char* artists_dir = g_strdup_printf("%s/artists", config->artwork_dir);
    g_mkdir_with_parents(artists_dir, 0755);

    // Pre-filter: check filesystem cache and cross-library copies upfront
    // so progress.total reflects only artists that need HTTP fetches.
    char** work_mbids = g_malloc(count * sizeof(char*));
    size_t work_count = 0;
    size_t cached_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (config->cancel_flag && atomic_load(config->cancel_flag)) break;

        if (artist_has_cached_art(config->artwork_dir, mbids[i])) {
            cached_count++;
            continue;
        }

        if (config->other_artwork_dirs && config->other_artwork_dirs_count > 0) {
            if (copy_art_from_other_library(config->other_artwork_dirs,
                    config->other_artwork_dirs_count, mbids[i], config->artwork_dir)) {
                cached_count++;
                continue;
            }
        }

        work_mbids[work_count] = mbids[i];  // borrows from mbids[]
        work_count++;
    }

    g_message("Phase 7: %zu artists with MBIDs (%zu cached, %zu to fetch)",
              count, cached_count, work_count);

    // Create cancellable for interrupting in-flight HTTP requests
    GCancellable* cancellable = g_cancellable_new();

    // Create HTTP session
    SoupSession* session = soup_session_new_with_options(
        "timeout", 30,
        "user-agent", "Quadrature/1.0",
        NULL);

    rate_limiter_t rl;
    rate_limiter_init(&rl, config->rate_limit_ms > 0 ? config->rate_limit_ms : 500);

    artist_art_progress_t progress = { .total = work_count };
    int64_t last_progress_us = 0;
    bool abort_phase = false;

    // Fire initial progress so UI gets the real total immediately
    if (cb) cb(&progress, user_data);

    for (size_t i = 0; i < work_count && !abort_phase; i++) {
        // Check cancellation — also cancel in-flight HTTP requests
        if (config->cancel_flag && atomic_load(config->cancel_flag)) {
            g_cancellable_cancel(cancellable);
            g_message("Phase 7: cancelled at artist %zu/%zu", i, work_count);
            break;
        }

        const char* mbid = work_mbids[i];

        // Rate limit
        rate_limiter_wait(&rl);

        // Build API URL — project key always required; personal key optional
        char* api_url = config->personal_api_key
            ? g_strdup_printf("%s/%s?api_key=%s&client_key=%s",
                              FANART_API_URL, mbid, FANART_PROJECT_KEY, config->personal_api_key)
            : g_strdup_printf("%s/%s?api_key=%s",
                              FANART_API_URL, mbid, FANART_PROJECT_KEY);

        // Fetch with retry
        SoupMessage* msg = soup_message_new("GET", api_url);
        g_free(api_url);

        if (!msg) {
            progress.errors++;
            progress.processed++;
            continue;
        }

        GError* error = NULL;
        GBytes* body = NULL;
        guint status = 0;
        int retries = 0;

        while (retries <= MAX_RETRIES) {
            body = soup_session_send_and_read(session, msg, cancellable, &error);
            status = soup_message_get_status(msg);

            if (error) {
                g_debug("artist_art: request failed for %s: %s", mbid, error->message);
                g_clear_error(&error);
                if (body) { g_bytes_unref(body); body = NULL; }
                progress.errors++;
                break;
            }

            if (status == 200) {
                rate_limiter_reset_backoff(&rl);
                break;
            } else if (status == 404) {
                // No images for this artist — write sentinel so pre-filter skips on re-index
                progress.no_images++;
                if (body) { g_bytes_unref(body); body = NULL; }

                char* artist_dir = g_strdup_printf("%s/artists/%s",
                    config->artwork_dir, mbid);
                g_mkdir_with_parents(artist_dir, 0755);
                char* marker = g_strdup_printf("%s/no-images", artist_dir);
                g_file_set_contents(marker, "", 0, NULL);
                g_free(marker);
                g_free(artist_dir);

                break;
            } else if (status == 401) {
                g_warning("Phase 7: fanart.tv returned 401 (unauthorized) — aborting phase");
                abort_phase = true;
                if (body) { g_bytes_unref(body); body = NULL; }
                break;
            } else if (status == 403) {
                g_warning("Phase 7: fanart.tv returned 403 (invalid API key) — aborting phase");
                abort_phase = true;
                if (body) { g_bytes_unref(body); body = NULL; }
                break;
            } else if (status == 429) {
                rate_limiter_backoff(&rl);
                if (body) { g_bytes_unref(body); body = NULL; }
                retries++;
                if (retries <= MAX_RETRIES) {
                    rate_limiter_wait(&rl);
                    // Re-create message for retry
                    g_object_unref(msg);
                    char* retry_url = config->personal_api_key
                        ? g_strdup_printf("%s/%s?api_key=%s&client_key=%s",
                                          FANART_API_URL, mbid, FANART_PROJECT_KEY, config->personal_api_key)
                        : g_strdup_printf("%s/%s?api_key=%s",
                                          FANART_API_URL, mbid, FANART_PROJECT_KEY);
                    msg = soup_message_new("GET", retry_url);
                    g_free(retry_url);
                    if (!msg) break;
                }
                continue;
            } else {
                // 5xx or other
                g_debug("artist_art: HTTP %u for %s — skipping", status, mbid);
                rate_limiter_backoff(&rl);
                if (body) { g_bytes_unref(body); body = NULL; }
                progress.errors++;
                break;
            }
        }

        if (msg) g_object_unref(msg);

        if (abort_phase) {
            if (body) g_bytes_unref(body);
            progress.processed++;
            break;
        }

        if (status == 200 && body) {
            gsize body_size;
            const char* body_data = g_bytes_get_data(body, &body_size);

            // Download three image types from the same API response
            const char* image_types[] = { "artistthumb", "musicbanner", "artistbackground" };
            const char* prefixes[]    = { "thumb",       "banner",      "background" };
            size_t total_downloaded_for_artist = 0;

            // Create artist directory keyed by MBID (globally stable across libraries)
            char* artist_dir = g_strdup_printf("%s/artists/%s",
                                                config->artwork_dir, mbid);
            bool dir_created = false;

            for (int t = 0; t < 3; t++) {
                char* urls[MAX_THUMBS] = {0};
                size_t url_count = parse_image_urls(body_data, body_size,
                                                     image_types[t], urls, MAX_THUMBS);

                for (size_t u = 0; u < url_count; u++) {
                    // Check cancellation between downloads
                    if (config->cancel_flag && atomic_load(config->cancel_flag)) {
                        g_cancellable_cancel(cancellable);
                        for (size_t f = u; f < url_count; f++) g_free(urls[f]);
                        goto images_done;
                    }

                    if (!dir_created) {
                        g_mkdir_with_parents(artist_dir, 0755);
                        dir_created = true;
                    }

                    char* path = g_strdup_printf("%s/%s_%zu.jpg", artist_dir, prefixes[t], u);
                    quadrature_result_t dl_res = download_and_save_image(session,
                                                                         urls[u], path,
                                                                         cancellable);
                    if (dl_res == QUADRATURE_OK) {
                        progress.downloaded++;
                        total_downloaded_for_artist++;
                    } else {
                        g_debug("artist_art: failed to download %s_%zu for artist %s",
                                prefixes[t], u, mbid);
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
                char* marker = g_strdup_printf("%s/no-images", artist_dir);
                g_file_set_contents(marker, "", 0, NULL);
                g_free(marker);
                progress.no_images++;
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
    if (cb) cb(&progress, user_data);

    // =========================================================================
    // Global artist atlas: flock() serialize, diff filesystem vs atlas, rewrite if dirty
    // =========================================================================

    if (config->atlas_path && config->atlas_lock_path) {
        int atlas_thumb = config->art_thumb_size > 0 ? config->art_thumb_size : 48;

        // Ensure atlas directory exists
        char* atlas_dir = g_path_get_dirname(config->atlas_path);
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
        artist_atlas_builder_t* atlas = NULL;
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
            if (config->cancel_flag && atomic_load(config->cancel_flag)) break;

            uint8_t uuid_bin[ARTIST_ATLAS_UUID_SIZE];
            if (!mbid_parse(mbids[i], uuid_bin)) continue;

            char* thumb_path = g_strdup_printf("%s/artists/%s/thumb_0.jpg",
                                                config->artwork_dir, mbids[i]);
            char* no_images_path = g_strdup_printf("%s/artists/%s/no-images",
                                                    config->artwork_dir, mbids[i]);

            bool has_thumb = g_file_test(thumb_path, G_FILE_TEST_EXISTS);
            bool has_no_images = g_file_test(no_images_path, G_FILE_TEST_EXISTS);

            if (has_thumb) {
                // Load and resize thumb, add to atlas
                VipsImage* img = NULL;
                if (vips_thumbnail(thumb_path, &img, atlas_thumb,
                                    "height", atlas_thumb,
                                    "crop", VIPS_INTERESTING_CENTRE,
                                    NULL) == 0) {
                    size_t pixel_size = 0;
                    void* pixel_data = vips_image_write_to_memory(img, &pixel_size);
                    if (pixel_data) {
                        size_t expected = (size_t)atlas_thumb * atlas_thumb * ARTWORK_ATLAS_CHANNELS;
                        if (pixel_size >= expected) {
                            if (artist_atlas_builder_add_art(atlas, uuid_bin,
                                    pixel_data, expected) == QUADRATURE_OK) {
                                dirty = true;
                            }
                        }
                        g_free(pixel_data);
                    }
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
              "%zu no images, %zu errors",
              progress.processed, progress.downloaded, cached_count,
              progress.no_images, progress.errors);

    // Cleanup
    g_object_unref(cancellable);
    g_object_unref(session);
    g_free(artists_dir);
    g_free(work_mbids);  // borrowed strings — freed via g_strfreev(mbids)
    g_free(artist_ids);
    g_strfreev(mbids);

    return abort_phase ? QUADRATURE_ERROR_INTERNAL : QUADRATURE_OK;
}
