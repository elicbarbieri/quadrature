/**
 * MusicBrainz enrichment orchestration.
 *
 * Main enrichment flow:
 * 1. Discover albums to enrich (check for existing MUSICBRAINZ_TRACKID)
 * 2. Fingerprint tracks without MB tags
 * 3. Match to MusicBrainz releases via AcoustID
 * 4. Fetch release metadata
 * 5. Write tags to files
 * 6. Download/write artwork
 */

#include "internal.h"
#include <glib/gstdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Audio file extensions we process
static const char* AUDIO_EXTENSIONS[] = {
    ".flac", ".mp3", ".m4a", ".ogg", ".opus", ".wav", ".aiff", ".wma", NULL
};

// =============================================================================
// Helper Functions
// =============================================================================

static bool is_audio_file(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;

    for (int i = 0; AUDIO_EXTENSIONS[i]; i++) {
        if (g_ascii_strcasecmp(ext, AUDIO_EXTENSIONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void update_progress(mb_enrichment_ctx_t* ctx) {
    if (!ctx->callback) return;

    g_mutex_lock(&ctx->progress_mutex);
    mb_enrichment_progress_t progress_copy = ctx->progress;
    g_mutex_unlock(&ctx->progress_mutex);

    ctx->callback(&progress_copy, ctx->user_data);
}

static void set_phase(mb_enrichment_ctx_t* ctx, mb_enrichment_phase_t phase) {
    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.phase = phase;
    g_mutex_unlock(&ctx->progress_mutex);
    update_progress(ctx);
}

static void set_current_album(mb_enrichment_ctx_t* ctx, const char* album) {
    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.current_album = album;
    g_mutex_unlock(&ctx->progress_mutex);
}

// =============================================================================
// Album Discovery
// =============================================================================

typedef struct {
    char* path;                 // Album directory path
    GPtrArray* audio_files;     // Array of audio file paths
    bool needs_enrichment;      // True if any track lacks MB tags
} album_info_t;

static void album_info_free(album_info_t* info) {
    if (!info) return;
    g_free(info->path);
    if (info->audio_files) {
        g_ptr_array_free(info->audio_files, TRUE);
    }
    g_free(info);
}

static album_info_t* scan_album_directory(const char* dir_path, bool force) {
    DIR* dir = opendir(dir_path);
    if (!dir) return NULL;

    album_info_t* info = g_new0(album_info_t, 1);
    info->path = g_strdup(dir_path);
    info->audio_files = g_ptr_array_new_with_free_func(g_free);
    info->needs_enrichment = false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char* full_path = g_build_filename(dir_path, entry->d_name, NULL);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (is_audio_file(entry->d_name)) {
                g_ptr_array_add(info->audio_files, full_path);

                // Check if this track needs enrichment
                if (force || !mb_tags_exist(full_path)) {
                    info->needs_enrichment = true;
                }
            } else {
                g_free(full_path);
            }
        } else {
            g_free(full_path);
        }
    }

    closedir(dir);

    // No audio files? Not an album directory
    if (info->audio_files->len == 0) {
        album_info_free(info);
        return NULL;
    }

    return info;
}

static void discover_albums_recursive(const char* path, GPtrArray* albums,
                                       bool force, volatile bool* cancelled) {
    if (*cancelled) return;

    struct stat st;
    if (stat(path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        // Check if this directory is an album (contains audio files)
        album_info_t* info = scan_album_directory(path, force);
        if (info) {
            if (info->needs_enrichment) {
                g_ptr_array_add(albums, info);
            } else {
                album_info_free(info);
            }
        }

        // Recurse into subdirectories
        DIR* dir = opendir(path);
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                if (*cancelled) break;
                if (entry->d_name[0] == '.') continue;

                char* subpath = g_build_filename(path, entry->d_name, NULL);
                struct stat subst;
                if (stat(subpath, &subst) == 0 && S_ISDIR(subst.st_mode)) {
                    discover_albums_recursive(subpath, albums, force, cancelled);
                }
                g_free(subpath);
            }
            closedir(dir);
        }
    }
}

// =============================================================================
// Release Matching
// =============================================================================

// Find the best matching release for an album based on multiple track fingerprints
static char* find_best_release(mb_enrichment_ctx_t* ctx, album_info_t* album) {
    if (album->audio_files->len == 0) return NULL;

    // Fingerprint up to MB_MIN_TRACKS_FOR_MATCH tracks
    size_t tracks_to_check = MIN(album->audio_files->len, (size_t)MB_MIN_TRACKS_FOR_MATCH);

    // Count release occurrences
    GHashTable* release_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (size_t i = 0; i < tracks_to_check && !ctx->cancelled; i++) {
        const char* track_path = g_ptr_array_index(album->audio_files, i);

        // Skip if already has MB tags
        if (!ctx->options.force && mb_tags_exist(track_path)) {
            continue;
        }

        // Generate fingerprint
        mb_fingerprint_t fp;
        if (mb_fingerprint_generate(track_path, &fp) != QUADRATURE_OK) {
            continue;
        }

        // Lookup in AcoustID
        mb_acoustid_response_t response;
        if (mb_acoustid_lookup(ctx->http_client, ctx->options.acoustid_api_key,
                               &fp, &response) == QUADRATURE_OK) {
            // Count release IDs
            for (size_t j = 0; j < response.count; j++) {
                if (response.results[j].release_id) {
                    gpointer count = g_hash_table_lookup(release_counts,
                                                          response.results[j].release_id);
                    g_hash_table_insert(release_counts,
                                        g_strdup(response.results[j].release_id),
                                        GINT_TO_POINTER(GPOINTER_TO_INT(count) + 1));
                }
            }
            mb_acoustid_response_free(&response);
        }

        mb_fingerprint_free(&fp);
    }

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

    // Check confidence threshold
    if (best_count < 1) {
        g_free(best_release);
        return NULL;
    }

    return best_release;
}

// =============================================================================
// Album Enrichment
// =============================================================================

static quadrature_result_t enrich_album(mb_enrichment_ctx_t* ctx, album_info_t* album) {
    if (ctx->cancelled) return QUADRATURE_ERROR_CANCELLED;

    set_current_album(ctx, album->path);

    // Phase: Fingerprinting & Matching
    set_phase(ctx, MB_PHASE_FINGERPRINTING);

    char* release_id = find_best_release(ctx, album);
    if (!release_id) {
        g_warning("No MusicBrainz match for: %s", album->path);
        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return QUADRATURE_OK; // Continue with other albums
    }

    if (ctx->cancelled) {
        g_free(release_id);
        return QUADRATURE_ERROR_CANCELLED;
    }

    // Phase: Fetch metadata
    set_phase(ctx, MB_PHASE_MATCHING);

    mb_release_t release;
    quadrature_result_t result = mb_fetch_release(ctx->http_client, release_id, &release);
    if (result != QUADRATURE_OK) {
        g_warning("Failed to fetch release %s: %d", release_id, result);
        g_free(release_id);
        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_failed++;
        g_mutex_unlock(&ctx->progress_mutex);
        return QUADRATURE_OK;
    }
    g_free(release_id);

    if (ctx->cancelled) {
        mb_release_free(&release);
        return QUADRATURE_ERROR_CANCELLED;
    }

    // Phase: Tag tracks
    set_phase(ctx, MB_PHASE_TAGGING);

    // Match tracks to recordings by position
    for (guint i = 0; i < album->audio_files->len && !ctx->cancelled; i++) {
        const char* track_path = g_ptr_array_index(album->audio_files, i);

        // Skip if already tagged (unless force)
        if (!ctx->options.force && mb_tags_exist(track_path)) {
            continue;
        }

        // Find matching recording (by track number)
        // This is a simple position-based match; could be improved with fingerprint matching
        mb_recording_t* matching_recording = NULL;

        // Try to match by track number in filename or position
        for (size_t j = 0; j < release.recording_count; j++) {
            // Simple heuristic: match by array position
            if (j == i && j < release.recording_count) {
                matching_recording = &release.recordings[j];
                break;
            }
        }

        if (!matching_recording && release.recording_count > 0) {
            // Fall back to first available if no match
            size_t idx = i % release.recording_count;
            matching_recording = &release.recordings[idx];
        }

        if (matching_recording) {
            if (mb_write_tags(track_path, &release, matching_recording,
                              ctx->options.dry_run) == QUADRATURE_OK) {
                g_mutex_lock(&ctx->progress_mutex);
                ctx->progress.tracks_tagged++;
                g_mutex_unlock(&ctx->progress_mutex);
            }
        }
    }

    if (ctx->cancelled) {
        mb_release_free(&release);
        return QUADRATURE_ERROR_CANCELLED;
    }

    // Phase: Artwork
    if (ctx->options.download_artwork || ctx->options.embed_artwork) {
        set_phase(ctx, MB_PHASE_ARTWORK);

        mb_artwork_t artwork;
        if (mb_artwork_download(ctx->http_client, release.id, &artwork) == QUADRATURE_OK) {
            // Write cover.jpg to album folder
            if (ctx->options.download_artwork) {
                mb_artwork_write(album->path, &artwork, ctx->options.dry_run);
            }

            // Embed in audio files
            if (ctx->options.embed_artwork) {
                for (guint i = 0; i < album->audio_files->len && !ctx->cancelled; i++) {
                    const char* track_path = g_ptr_array_index(album->audio_files, i);
                    mb_embed_artwork(track_path, artwork.data, artwork.size,
                                     ctx->options.dry_run);
                }
            }

            mb_artwork_free(&artwork);
        }
    }

    mb_release_free(&release);

    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.albums_matched++;
    g_mutex_unlock(&ctx->progress_mutex);

    return QUADRATURE_OK;
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_enrichment_create(mb_enrichment_ctx_t** out,
                                          const mb_enrichment_options_t* options,
                                          mb_enrichment_progress_cb callback,
                                          void* user_data) {
    if (!out || !options) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (!options->acoustid_api_key) {
        g_warning("AcoustID API key is required for enrichment");
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    mb_enrichment_ctx_t* ctx = g_new0(mb_enrichment_ctx_t, 1);

    ctx->options = *options;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->cancelled = false;

    g_mutex_init(&ctx->progress_mutex);

    // Create HTTP client
    quadrature_result_t result = mb_http_client_create(&ctx->http_client);
    if (result != QUADRATURE_OK) {
        g_mutex_clear(&ctx->progress_mutex);
        g_free(ctx);
        return result;
    }

    *out = ctx;
    return QUADRATURE_OK;
}

void mb_enrichment_destroy(mb_enrichment_ctx_t* ctx) {
    if (!ctx) return;

    mb_http_client_destroy(ctx->http_client);
    g_mutex_clear(&ctx->progress_mutex);

    if (ctx->album_queue) {
        g_ptr_array_free(ctx->album_queue, TRUE);
    }

    g_free(ctx);
}

quadrature_result_t mb_enrichment_run(mb_enrichment_ctx_t* ctx, const char* path) {
    if (!ctx || !path) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Reset progress
    g_mutex_lock(&ctx->progress_mutex);
    memset(&ctx->progress, 0, sizeof(mb_enrichment_progress_t));
    ctx->progress.phase = MB_PHASE_DISCOVERING;
    g_mutex_unlock(&ctx->progress_mutex);

    update_progress(ctx);

    // Phase 1: Discover albums
    GPtrArray* albums = g_ptr_array_new_with_free_func((GDestroyNotify)album_info_free);
    discover_albums_recursive(path, albums, ctx->options.force, &ctx->cancelled);

    if (ctx->cancelled) {
        g_ptr_array_free(albums, TRUE);
        return QUADRATURE_ERROR_CANCELLED;
    }

    g_mutex_lock(&ctx->progress_mutex);
    ctx->progress.albums_total = albums->len;
    ctx->progress.albums_skipped = 0; // We only added albums that need enrichment
    g_mutex_unlock(&ctx->progress_mutex);

    update_progress(ctx);

    g_debug("Found %u albums to enrich", albums->len);

    // Phase 2-5: Process each album
    for (guint i = 0; i < albums->len && !ctx->cancelled; i++) {
        album_info_t* album = g_ptr_array_index(albums, i);

        enrich_album(ctx, album);

        g_mutex_lock(&ctx->progress_mutex);
        ctx->progress.albums_processed++;
        ctx->progress.progress = (double)ctx->progress.albums_processed / ctx->progress.albums_total;
        g_mutex_unlock(&ctx->progress_mutex);

        update_progress(ctx);
    }

    g_ptr_array_free(albums, TRUE);

    // Complete
    set_phase(ctx, MB_PHASE_COMPLETE);
    set_current_album(ctx, NULL);

    return ctx->cancelled ? QUADRATURE_ERROR_CANCELLED : QUADRATURE_OK;
}

void mb_enrichment_cancel(mb_enrichment_ctx_t* ctx) {
    if (ctx) {
        ctx->cancelled = true;
    }
}

bool mb_enrichment_is_running(const mb_enrichment_ctx_t* ctx) {
    if (!ctx) return false;

    g_mutex_lock((GMutex*)&ctx->progress_mutex);
    bool running = ctx->progress.phase != MB_PHASE_COMPLETE &&
                   ctx->progress.phase != MB_PHASE_DISCOVERING;
    g_mutex_unlock((GMutex*)&ctx->progress_mutex);

    return running;
}
