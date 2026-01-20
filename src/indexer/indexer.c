/**
 * Four-phase indexer implementation.
 *
 * Phase 1 - SCAN: Fast directory walk, builds work queue
 * Phase 2 - METADATA: Parallel extraction with GThreadPool
 * Phase 3 - ARTWORK: Parallel image processing
 * Phase 4 - FINALIZE: Batch DB updates
 */

#include "quadrature/indexer/indexer.h"
#include "quadrature/database/database.h"
#include "internal.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_PATHS 64
#define DEFAULT_ART_SIZE 300
#define ALBUM_MTIME_PAGE_SIZE 1000
#define PROGRESS_THROTTLE_US (100 * 1000)  // 100ms
#define DIR_SCAN_INITIAL_CAPACITY 64

// =============================================================================
// Directory Scanning
// =============================================================================

void dir_scan_result_init(dir_scan_result_t* result) {
    memset(result, 0, sizeof(*result));
}

void dir_scan_result_free(dir_scan_result_t* result) {
    if (!result) return;
    for (size_t i = 0; i < result->file_count; i++) {
        free(result->files[i]);
    }
    free(result->files);
    free(result->stats);
    for (size_t i = 0; i < result->subdir_count; i++) {
        free(result->subdirs[i]);
    }
    free(result->subdirs);
    memset(result, 0, sizeof(*result));
}

static void dir_scan_add_file(dir_scan_result_t* result, const char* path, const struct stat* st) {
    if (result->file_count >= result->file_capacity) {
        size_t new_cap = result->file_capacity ? result->file_capacity * 2 : DIR_SCAN_INITIAL_CAPACITY;
        result->files = realloc(result->files, new_cap * sizeof(char*));
        result->stats = realloc(result->stats, new_cap * sizeof(struct stat));
        result->file_capacity = new_cap;
    }
    result->files[result->file_count] = strdup(path);
    result->stats[result->file_count] = *st;
    result->file_count++;
}

static void dir_scan_add_subdir(dir_scan_result_t* result, const char* path) {
    if (result->subdir_count >= result->subdir_capacity) {
        size_t new_cap = result->subdir_capacity ? result->subdir_capacity * 2 : DIR_SCAN_INITIAL_CAPACITY;
        result->subdirs = realloc(result->subdirs, new_cap * sizeof(char*));
        result->subdir_capacity = new_cap;
    }
    result->subdirs[result->subdir_count++] = strdup(path);
}

void dir_scan_single_pass(const char* dir_path, dir_scan_result_t* result) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        g_debug("dir_scan: failed to open %s: %s", dir_path, strerror(errno));
        return;
    }

    char path_buf[INDEXER_PATH_MAX];
    struct dirent* ent;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, ent->d_name);

        // Determine file type - use d_type if available, fall back to stat()
        unsigned char dtype = ent->d_type;
        struct stat st;

        if (dtype == DT_UNKNOWN || dtype == DT_LNK) {
            // Filesystem doesn't provide d_type or it's a symlink - use stat()
            if (stat(path_buf, &st) != 0) {
                g_debug("dir_scan: stat failed for %s: %s", path_buf, strerror(errno));
                continue;
            }
            if (S_ISREG(st.st_mode)) {
                dtype = DT_REG;
            } else if (S_ISDIR(st.st_mode)) {
                dtype = DT_DIR;
            } else {
                continue;  // Skip other types (sockets, fifos, etc.)
            }
        }

        if (dtype == DT_REG) {
            if (is_audio_file(ent->d_name)) {
                // stat() if we haven't already
                if (ent->d_type != DT_UNKNOWN && ent->d_type != DT_LNK) {
                    if (stat(path_buf, &st) != 0) continue;
                }
                dir_scan_add_file(result, path_buf, &st);
            }
        } else if (dtype == DT_DIR) {
            dir_scan_add_subdir(result, path_buf);
        }
    }

    closedir(dir);
}

// =============================================================================
// Work Queue Types
// =============================================================================

typedef struct {
    char* dir_path;         // Album directory (parent for multi-disc)
    int64_t dir_mtime;      // Current mtime from stat()
    int64_t album_id;       // 0 if new album
    bool is_multi_disc;     // True if album has disc subdirectories
    char** disc_dirs;       // Array of disc subdirectory paths (sorted by disc number)
    uint16_t* disc_nums;    // Parallel array of disc numbers
    size_t disc_count;      // Number of disc directories
} album_work_item_t;

typedef struct {
    album_work_item_t* items;
    size_t count;
    size_t capacity;
} work_queue_t;

static void work_queue_init(work_queue_t* q) {
    q->items = NULL;
    q->count = 0;
    q->capacity = 0;
}

static void work_queue_push(work_queue_t* q, const char* dir_path, int64_t mtime, int64_t album_id) {
    if (q->count >= q->capacity) {
        q->capacity = q->capacity ? q->capacity * 2 : 64;
        q->items = g_realloc(q->items, q->capacity * sizeof(album_work_item_t));
    }
    q->items[q->count++] = (album_work_item_t){
        .dir_path = g_strdup(dir_path),
        .dir_mtime = mtime,
        .album_id = album_id,
        .is_multi_disc = false,
        .disc_dirs = NULL,
        .disc_nums = NULL,
        .disc_count = 0
    };
}

// Push a multi-disc album work item
// Takes ownership of disc_dirs and disc_nums arrays
static void work_queue_push_multi_disc(work_queue_t* q, const char* dir_path, int64_t mtime,
                                        int64_t album_id, char** disc_dirs, uint16_t* disc_nums,
                                        size_t disc_count) {
    if (q->count >= q->capacity) {
        q->capacity = q->capacity ? q->capacity * 2 : 64;
        q->items = g_realloc(q->items, q->capacity * sizeof(album_work_item_t));
    }
    q->items[q->count++] = (album_work_item_t){
        .dir_path = g_strdup(dir_path),
        .dir_mtime = mtime,
        .album_id = album_id,
        .is_multi_disc = true,
        .disc_dirs = disc_dirs,
        .disc_nums = disc_nums,
        .disc_count = disc_count
    };
}

static void work_queue_free(work_queue_t* q) {
    for (size_t i = 0; i < q->count; i++) {
        g_free(q->items[i].dir_path);
        if (q->items[i].disc_dirs) {
            for (size_t j = 0; j < q->items[i].disc_count; j++) {
                g_free(q->items[i].disc_dirs[j]);
            }
            g_free(q->items[i].disc_dirs);
        }
        g_free(q->items[i].disc_nums);
    }
    g_free(q->items);
    q->items = NULL;
    q->count = q->capacity = 0;
}

// =============================================================================
// Processed Album Tracking (for artwork and finalize phases)
// =============================================================================

typedef struct {
    int64_t album_id;
    int64_t mtime;
    char* path;  // Directory path (owned, for artwork phase)
} processed_album_t;

static void processed_albums_free(processed_album_t* albums, size_t count) {
    if (!albums) return;
    for (size_t i = 0; i < count; i++) {
        g_free(albums[i].path);
    }
    g_free(albums);
}

// =============================================================================
// Indexer Structure
// =============================================================================

struct indexer {
    int thread_count;
    bool process_artwork;
    int art_size;

    indexer_callback_t callback;
    void* user_data;

    atomic_int running;
    atomic_int cancel_flag;

    pthread_t worker_thread;
    bool thread_started;

    quadrature_db_t* db;
    char* paths[MAX_PATHS];
    size_t path_count;
    int64_t scan_timestamp;

    // Progress counters (atomic for thread safety)
    atomic_size_t files_total;
    atomic_size_t files_processed;
    atomic_size_t files_new;
    atomic_size_t files_unchanged;
    atomic_size_t files_deleted;
    atomic_size_t dirs_scanned;
    atomic_size_t albums_total;
    atomic_size_t albums_processed;
    atomic_size_t error_count;

    // Progress throttling (per-instance, not global)
    int64_t last_progress_time;

    atomic_int phase;
    int64_t phase_start_time;

    char current_path[INDEXER_PATH_MAX];
    pthread_mutex_t lock;  // Protects current_path and phase_start_time
};

// =============================================================================
// Helpers
// =============================================================================

static void set_phase(indexer_t* idx, indexer_phase_t phase) {
    atomic_store(&idx->phase, phase);
    pthread_mutex_lock(&idx->lock);
    idx->phase_start_time = g_get_monotonic_time();
    pthread_mutex_unlock(&idx->lock);
}

static void set_current_path(indexer_t* idx, const char* path) {
    pthread_mutex_lock(&idx->lock);
    if (path) {
        strncpy(idx->current_path, path, INDEXER_PATH_MAX - 1);
        idx->current_path[INDEXER_PATH_MAX - 1] = '\0';
    } else {
        idx->current_path[0] = '\0';
    }
    pthread_mutex_unlock(&idx->lock);
}

static void notify_progress_throttled(indexer_t* idx) {
    if (!idx->callback) return;
    int64_t now = g_get_monotonic_time();
    if (now - idx->last_progress_time < PROGRESS_THROTTLE_US) return;
    idx->last_progress_time = now;

    indexer_progress_t p;
    indexer_get_progress(idx, &p);
    idx->callback(INDEXER_PROGRESS, &p, idx->user_data);
}

static void notify_event(indexer_t* idx, indexer_event_t event) {
    if (!idx->callback) return;
    indexer_progress_t p;
    indexer_get_progress(idx, &p);
    idx->callback(event, &p, idx->user_data);
}

// Helper to log indexer errors to the database
static void log_indexer_error(indexer_t* idx, const char* path, const char* fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    db_log_error(idx->db, path, msg);
    atomic_fetch_add(&idx->error_count, 1);
}

// =============================================================================
// Phase 1: SCAN - Fast directory walk
// =============================================================================

// Comparison function for sorting disc directories by disc number
typedef struct {
    char* path;
    uint16_t disc_num;
} disc_info_t;

static int compare_disc_info(const void* a, const void* b) {
    const disc_info_t* da = a;
    const disc_info_t* db = b;
    return (int)da->disc_num - (int)db->disc_num;
}

// Check if any image files exist in a directory (for artwork validation)
static bool has_artwork_in_dir(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) return false;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        const char* ext = strrchr(ent->d_name, '.');
        if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
                    strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".webp") == 0)) {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return false;
}

// Check if directory has subdirectories with audio files (too deep nesting)
static bool has_nested_audio(const char* dir_path) {
    dir_scan_result_t scan = {0};
    dir_scan_single_pass(dir_path, &scan);

    bool has_audio_subdirs = false;
    for (size_t i = 0; i < scan.subdir_count && !has_audio_subdirs; i++) {
        // Check if subdir has audio files
        dir_scan_result_t subscan = {0};
        dir_scan_single_pass(scan.subdirs[i], &subscan);
        if (subscan.file_count > 0) {
            has_audio_subdirs = true;
        }
        dir_scan_result_free(&subscan);
    }

    dir_scan_result_free(&scan);
    return has_audio_subdirs;
}

static void scan_directory_recursive(indexer_t* idx, const char* dir_path,
                                      GHashTable* album_mtimes, work_queue_t* queue, int depth) {
    if (depth > 32 || atomic_load(&idx->cancel_flag)) return;

    struct stat dir_stat;
    if (stat(dir_path, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) return;

    set_current_path(idx, dir_path);
    atomic_fetch_add(&idx->dirs_scanned, 1);
    notify_progress_throttled(idx);

    // Single-pass directory scan
    dir_scan_result_t scan = {0};
    dir_scan_single_pass(dir_path, &scan);

    // Check for disc subdirectories
    disc_info_t* disc_infos = NULL;
    size_t disc_info_count = 0;
    size_t disc_info_capacity = 0;
    for (size_t i = 0; i < scan.subdir_count; i++) {
        const char* subdir_name = strrchr(scan.subdirs[i], '/');
        subdir_name = subdir_name ? subdir_name + 1 : scan.subdirs[i];

        uint16_t disc_num = get_disc_number_from_folder(subdir_name);
        if (disc_num > 0) {
            // It's a disc folder
            if (disc_info_count >= disc_info_capacity) {
                disc_info_capacity = disc_info_capacity ? disc_info_capacity * 2 : 8;
                disc_infos = g_realloc(disc_infos, disc_info_capacity * sizeof(disc_info_t));
            }
            disc_infos[disc_info_count].path = scan.subdirs[i];
            disc_infos[disc_info_count].disc_num = disc_num;
            disc_info_count++;
        }
    }

    bool is_multi_disc = disc_info_count > 0;

    if (is_multi_disc) {
        // ============================================================
        // Multi-disc album handling
        // ============================================================

        // Validation: Mixed content (audio files in parent alongside disc folders)
        if (scan.file_count > 0) {
            log_indexer_error(idx, dir_path,
                "Album has both tracks and disc folders - move tracks into disc folders");
            // Process as multi-disc anyway, ignore loose tracks
        }

        // Validation: Orphan disc folder (only one disc folder)
        if (disc_info_count == 1) {
            const char* disc_name = strrchr(disc_infos[0].path, '/');
            disc_name = disc_name ? disc_name + 1 : disc_infos[0].path;
            log_indexer_error(idx, dir_path,
                "Single disc folder '%s' found - remove disc folder or add more discs", disc_name);
        }

        // Sort disc folders by disc number
        qsort(disc_infos, disc_info_count, sizeof(disc_info_t), compare_disc_info);

        // Validation: Non-sequential disc numbers
        for (size_t i = 0; i < disc_info_count; i++) {
            if (disc_infos[i].disc_num != i + 1) {
                log_indexer_error(idx, dir_path,
                    "Non-sequential disc folders (missing Disc %zu)", i + 1);
                break;
            }
        }

        // Count total files across all disc directories and validate each disc
        size_t total_files = 0;
        bool any_empty = false;

        for (size_t i = 0; i < disc_info_count; i++) {
            dir_scan_result_t disc_scan = {0};
            dir_scan_single_pass(disc_infos[i].path, &disc_scan);

            // Validation: Empty disc folder
            if (disc_scan.file_count == 0) {
                const char* disc_name = strrchr(disc_infos[i].path, '/');
                disc_name = disc_name ? disc_name + 1 : disc_infos[i].path;
                log_indexer_error(idx, dir_path,
                    "Empty disc folder '%s' - add tracks or remove folder", disc_name);
                any_empty = true;
            } else {
                total_files += disc_scan.file_count;
            }

            // Validation: Artwork in disc folder
            if (has_artwork_in_dir(disc_infos[i].path)) {
                log_indexer_error(idx, dir_path,
                    "Artwork found in disc folder - move to album root");
            }

            // Validation: Too deep nesting
            if (has_nested_audio(disc_infos[i].path)) {
                log_indexer_error(idx, dir_path,
                    "Tracks found more than 1 level deep - flatten structure");
            }

            dir_scan_result_free(&disc_scan);
        }

        // Queue multi-disc album if it has tracks
        if (total_files > 0 && !any_empty) {
            // Lookup in album mtimes hashmap using parent path
            gpointer value = g_hash_table_lookup(album_mtimes, dir_path);
            bool needs_processing = true;

            if (value) {
                db_album_mtime_t* cached = value;
                if (cached->last_updated_at == dir_stat.st_mtime) {
                    // Unchanged - mark tracks seen and skip
                    for (size_t i = 0; i < disc_info_count; i++) {
                        db_mark_tracks_seen(idx->db, disc_infos[i].path, idx->scan_timestamp);
                    }
                    atomic_fetch_add(&idx->files_unchanged, total_files);
                    needs_processing = false;
                }
            }

            if (needs_processing) {
                // Build disc_dirs and disc_nums arrays for work item
                char** disc_dirs = g_new(char*, disc_info_count);
                uint16_t* disc_nums = g_new(uint16_t, disc_info_count);
                for (size_t i = 0; i < disc_info_count; i++) {
                    disc_dirs[i] = g_strdup(disc_infos[i].path);
                    disc_nums[i] = disc_infos[i].disc_num;
                }

                int64_t album_id = value ? ((db_album_mtime_t*)value)->album_id : 0;
                work_queue_push_multi_disc(queue, dir_path, dir_stat.st_mtime, album_id,
                                           disc_dirs, disc_nums, disc_info_count);
                atomic_fetch_add(&idx->files_total, total_files);
            }
        }

        // Recurse into non-disc subdirectories only
        for (size_t i = 0; i < scan.subdir_count && !atomic_load(&idx->cancel_flag); i++) {
            const char* subdir_name = strrchr(scan.subdirs[i], '/');
            subdir_name = subdir_name ? subdir_name + 1 : scan.subdirs[i];
            if (!is_disc_folder(subdir_name)) {
                scan_directory_recursive(idx, scan.subdirs[i], album_mtimes, queue, depth + 1);
            }
        }
    } else {
        // ============================================================
        // Single-disc album handling (original logic)
        // ============================================================

        // Check if this directory has audio files (is an album)
        if (scan.file_count > 0) {
            // Lookup in album mtimes hashmap
            gpointer value = g_hash_table_lookup(album_mtimes, dir_path);
            bool needs_processing = true;

            if (value) {
                db_album_mtime_t* cached = value;
                if (cached->last_updated_at == dir_stat.st_mtime) {
                    // Unchanged - mark tracks seen and skip
                    db_mark_tracks_seen(idx->db, dir_path, idx->scan_timestamp);
                    atomic_fetch_add(&idx->files_unchanged, scan.file_count);
                    needs_processing = false;
                }
            }

            if (needs_processing) {
                // Queue for metadata processing
                int64_t album_id = value ? ((db_album_mtime_t*)value)->album_id : 0;
                work_queue_push(queue, dir_path, dir_stat.st_mtime, album_id);
                atomic_fetch_add(&idx->files_total, scan.file_count);
            }
        }

        // Recurse into subdirectories
        if (depth == 0) {
            g_message("Root scan found %zu subdirs, %zu audio files", scan.subdir_count, scan.file_count);
        }
        for (size_t i = 0; i < scan.subdir_count && !atomic_load(&idx->cancel_flag); i++) {
            scan_directory_recursive(idx, scan.subdirs[i], album_mtimes, queue, depth + 1);
        }
    }

    g_free(disc_infos);
    dir_scan_result_free(&scan);
}

static void phase_scan(indexer_t* idx, work_queue_t* queue) {
    set_phase(idx, INDEXER_PHASE_SCANNING);

    // Log what paths we're scanning
    for (size_t i = 0; i < idx->path_count; i++) {
        g_message("Scanning watch path [%zu]: %s", i, idx->paths[i]);
    }

    // Clear existing errors for paths being scanned
    for (size_t i = 0; i < idx->path_count; i++) {
        db_clear_errors_for_path(idx->db, idx->paths[i]);
    }

    // Build hashmap of path -> album_mtime from DB (paged)
    GHashTable* album_mtimes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    GPtrArray* all_pages = g_ptr_array_new();  // Keep pages alive

    size_t offset = 0;
    while (!atomic_load(&idx->cancel_flag)) {
        db_album_mtime_t* page = NULL;
        size_t page_count = 0;

        if (db_get_album_mtimes_page(idx->db, offset, ALBUM_MTIME_PAGE_SIZE, &page, &page_count) != QUADRATURE_OK) {
            break;
        }
        if (page_count == 0) {
            db_free_album_mtimes(page, 0);
            break;
        }

        g_ptr_array_add(all_pages, page);
        for (size_t i = 0; i < page_count; i++) {
            if (page[i].path) {
                g_hash_table_insert(album_mtimes, page[i].path, &page[i]);
            }
        }

        offset += page_count;
        if (page_count < ALBUM_MTIME_PAGE_SIZE) break;
    }

    g_message("Loaded %u album mtimes from DB", g_hash_table_size(album_mtimes));

    // Walk directories
    for (size_t i = 0; i < idx->path_count && !atomic_load(&idx->cancel_flag); i++) {
        scan_directory_recursive(idx, idx->paths[i], album_mtimes, queue, 0);
    }

    g_message("Scan complete: %zu albums queued, %zu directories visited",
              queue->count, (size_t)atomic_load(&idx->dirs_scanned));

    // Cleanup
    g_hash_table_destroy(album_mtimes);
    for (guint i = 0; i < all_pages->len; i++) {
        db_album_mtime_t* page = g_ptr_array_index(all_pages, i);
        // Count items in page for proper free
        size_t count = 0;
        while (count < ALBUM_MTIME_PAGE_SIZE && page[count].path) count++;
        db_free_album_mtimes(page, count);
    }
    g_ptr_array_free(all_pages, TRUE);
}

// =============================================================================
// Phase 2: METADATA - Parallel extraction
// =============================================================================

typedef struct {
    indexer_t* idx;
    album_work_item_t* item;
    processed_album_t* result;  // Output: album_id and mtime on success
    bool success;
} metadata_work_t;

// Process a single-disc album
static void metadata_worker_single_disc(metadata_work_t* work, indexer_t* idx) {
    const char* dir_path = work->item->dir_path;

    // Scan files in directory
    dir_scan_result_t scan = {0};
    dir_scan_single_pass(dir_path, &scan);

    if (scan.file_count == 0) {
        dir_scan_result_free(&scan);
        work->success = false;
        return;
    }

    // Extract metadata and build folder album context
    folder_album_context_t* album_ctx = folder_album_context_new(dir_path);
    extended_metadata_t* all_meta = g_new0(extended_metadata_t, scan.file_count);

    size_t valid_count = 0;
    for (size_t i = 0; i < scan.file_count && !atomic_load(&idx->cancel_flag); i++) {
        quadrature_result_t res = extract_extended_metadata(scan.files[i], &all_meta[i]);
        if (res == QUADRATURE_OK) {
            folder_album_context_add_track(album_ctx, &all_meta[i]);
            valid_count++;
        } else {
            const char* filename = strrchr(scan.files[i], '/');
            filename = filename ? filename + 1 : scan.files[i];

            if (res == QUADRATURE_ERROR_FILE_NOT_FOUND) {
                log_indexer_error(idx, scan.files[i],
                    "Cannot read '%s' - file may be corrupted or inaccessible", filename);
            } else {
                log_indexer_error(idx, scan.files[i],
                    "Unsupported audio format: %s", filename);
            }
        }
        atomic_fetch_add(&idx->files_processed, 1);
    }

    if (valid_count == 0 || atomic_load(&idx->cancel_flag)) {
        for (size_t i = 0; i < scan.file_count; i++) extended_metadata_free(&all_meta[i]);
        g_free(all_meta);
        folder_album_context_free(album_ctx);
        dir_scan_result_free(&scan);
        work->success = false;
        return;
    }

    folder_album_finalize(album_ctx);

    // Write to database (sequential per album)
    int64_t artist_id = db_get_or_create_artist(idx->db, folder_album_get_artist(album_ctx));
    if (artist_id < 0) {
        log_indexer_error(idx, dir_path, "Failed to create artist: %s", folder_album_get_artist(album_ctx));
        for (size_t i = 0; i < scan.file_count; i++) extended_metadata_free(&all_meta[i]);
        g_free(all_meta);
        folder_album_context_free(album_ctx);
        dir_scan_result_free(&scan);
        work->success = false;
        return;
    }

    int64_t album_artist_id = 0;
    if (folder_album_is_compilation(album_ctx)) {
        album_artist_id = db_get_or_create_artist(idx->db, "Various Artists");
    }

    int64_t album_id = 0;
    quadrature_result_t album_res = db_upsert_folder_album(idx->db,
        folder_album_get_directory_path(album_ctx),
        folder_album_get_title(album_ctx),
        artist_id, album_artist_id,
        folder_album_is_compilation(album_ctx),
        folder_album_get_year(album_ctx),
        &album_id);

    if (album_res != QUADRATURE_OK || album_id <= 0) {
        log_indexer_error(idx, dir_path, "Failed to create album: %s", folder_album_get_title(album_ctx));
        for (size_t i = 0; i < scan.file_count; i++) extended_metadata_free(&all_meta[i]);
        g_free(all_meta);
        folder_album_context_free(album_ctx);
        dir_scan_result_free(&scan);
        work->success = false;
        return;
    }

    // Insert tracks using the album_id from db_upsert_folder_album
    db_begin_transaction(idx->db);
    for (size_t i = 0; i < scan.file_count; i++) {
        if (!all_meta[i].path) continue;

        db_index_item_t db_item = {
            .path = all_meta[i].path,
            .title = all_meta[i].title,
            .artist = all_meta[i].artist,
            .album = folder_album_get_title(album_ctx),
            .duration_ms = all_meta[i].duration_ms,
            .track_num = all_meta[i].track_num,
            .disc_num = all_meta[i].disc_num,  // Use metadata disc_num (default 1)
            .year = all_meta[i].year > 0 ? all_meta[i].year : folder_album_get_year(album_ctx),
            .mtime = scan.stats[i].st_mtime,
            .size = scan.stats[i].st_size
        };

        if (db_upsert_track_with_album(idx->db, &db_item, album_id, idx->scan_timestamp) == QUADRATURE_OK) {
            atomic_fetch_add(&idx->files_new, 1);
        } else {
            log_indexer_error(idx, db_item.path, "Failed to save track: %s", db_item.title ? db_item.title : "(unknown)");
        }
    }
    db_commit(idx->db);

    // Record result for artwork and finalize phases
    work->result->album_id = album_id;
    work->result->mtime = work->item->dir_mtime;
    work->result->path = g_strdup(dir_path);
    work->success = true;

    // Cleanup
    for (size_t i = 0; i < scan.file_count; i++) extended_metadata_free(&all_meta[i]);
    g_free(all_meta);
    folder_album_context_free(album_ctx);
    dir_scan_result_free(&scan);
}

// Process a multi-disc album
static void metadata_worker_multi_disc(metadata_work_t* work, indexer_t* idx) {
    const char* album_dir = work->item->dir_path;

    // Create folder album context using the parent (album) directory
    folder_album_context_t* album_ctx = folder_album_context_new(album_dir);

    // Collect all tracks from all disc directories
    GPtrArray* all_scans = g_ptr_array_new();       // Array of dir_scan_result_t*
    GPtrArray* all_metadata = g_ptr_array_new();    // Array of extended_metadata_t*
    GArray* disc_numbers = g_array_new(FALSE, FALSE, sizeof(uint16_t));  // Disc number for each scan

    size_t valid_count = 0;

    for (size_t d = 0; d < work->item->disc_count && !atomic_load(&idx->cancel_flag); d++) {
        const char* disc_dir = work->item->disc_dirs[d];
        uint16_t disc_num = work->item->disc_nums[d];

        dir_scan_result_t* scan = g_new0(dir_scan_result_t, 1);
        dir_scan_single_pass(disc_dir, scan);

        if (scan->file_count == 0) {
            g_free(scan);
            continue;
        }

        extended_metadata_t* disc_meta = g_new0(extended_metadata_t, scan->file_count);

        for (size_t i = 0; i < scan->file_count && !atomic_load(&idx->cancel_flag); i++) {
            quadrature_result_t res = extract_extended_metadata(scan->files[i], &disc_meta[i]);
            if (res == QUADRATURE_OK) {
                // Override disc_num from folder name (more reliable than embedded metadata)
                disc_meta[i].disc_num = disc_num;
                folder_album_context_add_track(album_ctx, &disc_meta[i]);
                valid_count++;
            } else {
                const char* filename = strrchr(scan->files[i], '/');
                filename = filename ? filename + 1 : scan->files[i];

                if (res == QUADRATURE_ERROR_FILE_NOT_FOUND) {
                    log_indexer_error(idx, scan->files[i],
                        "Cannot read '%s' - file may be corrupted or inaccessible", filename);
                } else {
                    log_indexer_error(idx, scan->files[i],
                        "Unsupported audio format: %s", filename);
                }
            }
            atomic_fetch_add(&idx->files_processed, 1);
        }

        g_ptr_array_add(all_scans, scan);
        g_ptr_array_add(all_metadata, disc_meta);
        g_array_append_val(disc_numbers, disc_num);
    }

    if (valid_count == 0 || atomic_load(&idx->cancel_flag)) {
        // Cleanup and fail
        for (guint s = 0; s < all_scans->len; s++) {
            dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
            extended_metadata_t* meta = g_ptr_array_index(all_metadata, s);
            for (size_t i = 0; i < scan->file_count; i++) extended_metadata_free(&meta[i]);
            g_free(meta);
            dir_scan_result_free(scan);
            g_free(scan);
        }
        g_ptr_array_free(all_scans, TRUE);
        g_ptr_array_free(all_metadata, TRUE);
        g_array_free(disc_numbers, TRUE);
        folder_album_context_free(album_ctx);
        work->success = false;
        return;
    }

    folder_album_finalize(album_ctx);

    // Create album record using parent directory path
    int64_t artist_id = db_get_or_create_artist(idx->db, folder_album_get_artist(album_ctx));
    if (artist_id < 0) {
        log_indexer_error(idx, album_dir, "Failed to create artist: %s", folder_album_get_artist(album_ctx));
        goto cleanup_fail;
    }

    int64_t album_artist_id = 0;
    if (folder_album_is_compilation(album_ctx)) {
        album_artist_id = db_get_or_create_artist(idx->db, "Various Artists");
    }

    int64_t album_id = 0;
    quadrature_result_t album_res = db_upsert_folder_album(idx->db,
        album_dir,  // Use parent directory as album path
        folder_album_get_title(album_ctx),
        artist_id, album_artist_id,
        folder_album_is_compilation(album_ctx),
        folder_album_get_year(album_ctx),
        &album_id);

    if (album_res != QUADRATURE_OK || album_id <= 0) {
        log_indexer_error(idx, album_dir, "Failed to create album: %s", folder_album_get_title(album_ctx));
        goto cleanup_fail;
    }

    // Insert tracks from all discs using the album_id from db_upsert_folder_album
    db_begin_transaction(idx->db);
    for (guint s = 0; s < all_scans->len; s++) {
        dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
        extended_metadata_t* meta = g_ptr_array_index(all_metadata, s);

        for (size_t i = 0; i < scan->file_count; i++) {
            if (!meta[i].path) continue;

            db_index_item_t db_item = {
                .path = meta[i].path,
                .title = meta[i].title,
                .artist = meta[i].artist,
                .album = folder_album_get_title(album_ctx),
                .duration_ms = meta[i].duration_ms,
                .track_num = meta[i].track_num,
                .disc_num = meta[i].disc_num,  // Set from folder name
                .year = meta[i].year > 0 ? meta[i].year : folder_album_get_year(album_ctx),
                .mtime = scan->stats[i].st_mtime,
                .size = scan->stats[i].st_size
            };

            if (db_upsert_track_with_album(idx->db, &db_item, album_id, idx->scan_timestamp) == QUADRATURE_OK) {
                atomic_fetch_add(&idx->files_new, 1);
            } else {
                log_indexer_error(idx, db_item.path, "Failed to save track: %s", db_item.title ? db_item.title : "(unknown)");
            }
        }
    }
    db_commit(idx->db);

    // Record result for artwork and finalize phases
    // Use parent directory for artwork lookup
    work->result->album_id = album_id;
    work->result->mtime = work->item->dir_mtime;
    work->result->path = g_strdup(album_dir);
    work->success = true;

    // Cleanup success path
    for (guint s = 0; s < all_scans->len; s++) {
        dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
        extended_metadata_t* meta = g_ptr_array_index(all_metadata, s);
        for (size_t i = 0; i < scan->file_count; i++) extended_metadata_free(&meta[i]);
        g_free(meta);
        dir_scan_result_free(scan);
        g_free(scan);
    }
    g_ptr_array_free(all_scans, TRUE);
    g_ptr_array_free(all_metadata, TRUE);
    g_array_free(disc_numbers, TRUE);
    folder_album_context_free(album_ctx);
    return;

cleanup_fail:
    for (guint s = 0; s < all_scans->len; s++) {
        dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
        extended_metadata_t* meta = g_ptr_array_index(all_metadata, s);
        for (size_t i = 0; i < scan->file_count; i++) extended_metadata_free(&meta[i]);
        g_free(meta);
        dir_scan_result_free(scan);
        g_free(scan);
    }
    g_ptr_array_free(all_scans, TRUE);
    g_ptr_array_free(all_metadata, TRUE);
    g_array_free(disc_numbers, TRUE);
    folder_album_context_free(album_ctx);
    work->success = false;
}

static void metadata_worker(gpointer data, gpointer user_data) {
    metadata_work_t* work = data;
    indexer_t* idx = user_data;

    if (atomic_load(&idx->cancel_flag)) {
        work->success = false;
        return;
    }

    if (work->item->is_multi_disc) {
        metadata_worker_multi_disc(work, idx);
    } else {
        metadata_worker_single_disc(work, idx);
    }
}

static size_t phase_metadata(indexer_t* idx, work_queue_t* queue, processed_album_t** results_out) {
    set_phase(idx, INDEXER_PHASE_METADATA);

    if (queue->count == 0) {
        *results_out = NULL;
        return 0;
    }

    // Allocate results array
    processed_album_t* results = g_new0(processed_album_t, queue->count);
    metadata_work_t* work_items = g_new0(metadata_work_t, queue->count);

    // Create thread pool
    GThreadPool* pool = g_thread_pool_new(metadata_worker, idx, idx->thread_count, FALSE, NULL);
    if (!pool) {
        g_free(results);
        g_free(work_items);
        *results_out = NULL;
        return 0;
    }

    // Queue all work
    for (size_t i = 0; i < queue->count; i++) {
        work_items[i] = (metadata_work_t){
            .idx = idx,
            .item = &queue->items[i],
            .result = &results[i],
            .success = false
        };
        g_thread_pool_push(pool, &work_items[i], NULL);
    }

    // Wait with progress updates
    while (!atomic_load(&idx->cancel_flag)) {
        if (g_thread_pool_unprocessed(pool) == 0) break;
        notify_progress_throttled(idx);
        g_usleep(50000);
    }

    g_thread_pool_free(pool, FALSE, TRUE);

    // Count successful results
    size_t success_count = 0;
    for (size_t i = 0; i < queue->count; i++) {
        if (work_items[i].success) {
            if (success_count != i) {
                results[success_count] = results[i];
            }
            success_count++;
        }
    }

    g_free(work_items);
    *results_out = results;
    return success_count;
}

// =============================================================================
// Phase 3: ARTWORK - Parallel image processing
// =============================================================================

typedef struct {
    int64_t album_id;
    char* path;
    indexer_t* indexer;  // For error logging
} artwork_work_t;

static void artwork_worker(gpointer data, gpointer user_data) {
    artwork_work_t* work = data;
    artwork_atlas_builder_t* builder = user_data;

    quadrature_result_t res = artwork_atlas_process_album(builder, work->album_id, work->path);
    if (res != QUADRATURE_OK && work->indexer) {
        log_indexer_error(work->indexer, work->path,
            "No album art found. Add cover.jpg, cover.png, folder.jpg, or front.jpg");
    }

    g_free(work->path);
    g_free(work);
}

static void phase_artwork(indexer_t* idx, processed_album_t* albums, size_t album_count) {
    if (!idx->process_artwork) return;

    char atlas_path[INDEXER_PATH_MAX];
    snprintf(atlas_path, sizeof(atlas_path), "%s/quadrature/artwork.atlas", g_get_user_data_dir());

    // Open existing atlas to preserve unchanged entries
    artwork_atlas_reader_t* existing_atlas = artwork_atlas_reader_open(atlas_path);
    size_t existing_count = existing_atlas ? artwork_atlas_reader_get_count(existing_atlas) : 0;

    // If no changes and no existing atlas, nothing to do
    if (album_count == 0 && existing_count == 0) {
        if (existing_atlas) artwork_atlas_reader_close(existing_atlas);
        return;
    }

    set_phase(idx, INDEXER_PHASE_ARTWORK);
    atomic_store(&idx->albums_total, album_count);
    atomic_store(&idx->albums_processed, 0);

    artwork_atlas_builder_t* builder = NULL;
    if (artwork_atlas_builder_create(atlas_path, idx->art_size, &builder) != QUADRATURE_OK) {
        if (existing_atlas) artwork_atlas_reader_close(existing_atlas);
        return;
    }

    // Build hash set of album_ids being re-processed
    GHashTable* reprocessed_ids = g_hash_table_new(g_int64_hash, g_int64_equal);
    for (size_t i = 0; i < album_count; i++) {
        g_hash_table_add(reprocessed_ids, &albums[i].album_id);
    }

    // Copy unchanged entries from existing atlas
    size_t preserved_count = 0;
    if (existing_atlas && existing_count > 0) {
        for (size_t i = 0; i < existing_count && !atomic_load(&idx->cancel_flag); i++) {
            int64_t album_id = artwork_atlas_reader_get_album_id_at(existing_atlas, i);
            if (album_id < 0) continue;

            // Skip if this album is being re-processed
            if (g_hash_table_contains(reprocessed_ids, &album_id)) continue;

            // Copy existing PNG data to new builder
            size_t png_size = 0;
            uint8_t* png_data = artwork_atlas_reader_get_png_at(existing_atlas, i, &png_size);
            if (png_data && png_size > 0) {
                artwork_atlas_add_cached_png(builder, album_id, png_data, png_size);
                preserved_count++;
            }
            g_free(png_data);
        }
        g_message("Preserved %zu unchanged album artworks from existing atlas", preserved_count);
    }

    g_hash_table_destroy(reprocessed_ids);
    if (existing_atlas) artwork_atlas_reader_close(existing_atlas);

    // Process changed albums in parallel
    if (album_count > 0) {
        GThreadPool* pool = g_thread_pool_new(artwork_worker, builder, idx->thread_count, FALSE, NULL);
        if (!pool) {
            artwork_atlas_builder_destroy(builder);
            return;
        }

        // Queue artwork work (paths carried from metadata phase)
        for (size_t i = 0; i < album_count && !atomic_load(&idx->cancel_flag); i++) {
            if (albums[i].path) {
                artwork_work_t* work = g_new(artwork_work_t, 1);
                work->album_id = albums[i].album_id;
                work->path = g_strdup(albums[i].path);
                work->indexer = idx;  // For error logging
                g_thread_pool_push(pool, work, NULL);
            }
        }

        // Wait with progress updates
        while (!atomic_load(&idx->cancel_flag)) {
            size_t processed, errors;
            artwork_atlas_builder_get_progress(builder, &processed, &errors);
            // Subtract preserved count since those aren't "processed" in this run
            atomic_store(&idx->albums_processed, processed > preserved_count ? processed - preserved_count : 0);

            if (g_thread_pool_unprocessed(pool) == 0) break;
            notify_progress_throttled(idx);
            g_usleep(100000);
        }

        g_thread_pool_free(pool, FALSE, TRUE);
    }

    if (!atomic_load(&idx->cancel_flag)) {
        artwork_atlas_builder_finish(builder);
    }

    artwork_atlas_builder_destroy(builder);
}

// =============================================================================
// Phase 4: FINALIZE - Batch DB updates
// =============================================================================

static void phase_finalize(indexer_t* idx, processed_album_t* albums, size_t album_count) {
    set_phase(idx, INDEXER_PHASE_FINALIZE);

    // Batch update album mtimes
    if (album_count > 0) {
        int64_t* ids = g_new(int64_t, album_count);
        int64_t* mtimes = g_new(int64_t, album_count);

        for (size_t i = 0; i < album_count; i++) {
            ids[i] = albums[i].album_id;
            mtimes[i] = albums[i].mtime;
        }

        db_set_album_mtimes_batch(idx->db, ids, mtimes, album_count);

        g_free(ids);
        g_free(mtimes);
    }

    // Delete tracks not seen in this scan
    size_t before, after;
    db_get_track_count(idx->db, &before);
    db_delete_unseen_tracks(idx->db, idx->scan_timestamp);
    db_get_track_count(idx->db, &after);
    if (before > after) {
        atomic_store(&idx->files_deleted, before - after);
    }

    // Update watch paths
    for (size_t i = 0; i < idx->path_count; i++) {
        db_update_watch_path_scanned(idx->db, idx->paths[i], idx->scan_timestamp);
    }

    // WAL checkpoint
    db_checkpoint(idx->db);
}

// =============================================================================
// Worker Thread
// =============================================================================

static void* indexer_worker(void* arg) {
    indexer_t* idx = arg;
    idx->scan_timestamp = time(NULL);

    // Declare all resources at top for proper cleanup on cancel
    work_queue_t queue;
    work_queue_init(&queue);
    processed_album_t* processed = NULL;
    size_t processed_count = 0;

    notify_event(idx, INDEXER_STARTED);
    g_message("Indexing %zu paths", idx->path_count);

    // Phase 1: SCAN
    phase_scan(idx, &queue);
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // Phase 2: METADATA
    processed_count = phase_metadata(idx, &queue, &processed);
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // Phase 3: ARTWORK
    phase_artwork(idx, processed, processed_count);
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // Phase 4: FINALIZE
    phase_finalize(idx, processed, processed_count);

    set_phase(idx, INDEXER_PHASE_COMPLETE);
    size_t errors = atomic_load(&idx->error_count);
    g_message("Complete: %zu new, %zu unchanged, %zu deleted, %zu errors",
              atomic_load(&idx->files_new),
              atomic_load(&idx->files_unchanged),
              atomic_load(&idx->files_deleted),
              errors);
    notify_event(idx, INDEXER_COMPLETED);

    processed_albums_free(processed, processed_count);
    work_queue_free(&queue);
    atomic_store(&idx->running, 0);
    return NULL;

cancelled:
    notify_event(idx, INDEXER_CANCELLED);
    processed_albums_free(processed, processed_count);
    work_queue_free(&queue);
    atomic_store(&idx->running, 0);
    return NULL;
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t indexer_create(indexer_t** out, const indexer_config_t* config) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    indexer_t* idx = calloc(1, sizeof(indexer_t));
    if (!idx) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    int threads = (config && config->thread_count > 0) ? config->thread_count : (int)g_get_num_processors();
    idx->thread_count = CLAMP(threads, 4, 16);
    idx->process_artwork = config ? config->process_artwork : true;
    idx->art_size = (config && config->art_size > 0) ? config->art_size : DEFAULT_ART_SIZE;

    if (config) {
        idx->callback = config->callback;
        idx->user_data = config->user_data;
    }

    pthread_mutex_init(&idx->lock, NULL);
    *out = idx;
    return QUADRATURE_OK;
}

void indexer_destroy(indexer_t* idx) {
    if (!idx) return;
    indexer_cancel(idx);
    indexer_wait(idx);
    for (size_t i = 0; i < idx->path_count; i++) free(idx->paths[i]);
    pthread_mutex_destroy(&idx->lock);
    free(idx);
}

quadrature_result_t indexer_scan(indexer_t* idx, quadrature_db_t* db,
                                 const char** paths, size_t path_count) {
    if (!idx || !db || !paths || path_count == 0) return QUADRATURE_ERROR_INVALID_PARAM;
    if (atomic_load(&idx->running)) return QUADRATURE_ERROR_DEVICE_BUSY;

    for (size_t i = 0; i < idx->path_count; i++) free(idx->paths[i]);
    idx->path_count = 0;

    for (size_t i = 0; i < path_count && i < MAX_PATHS; i++) {
        idx->paths[i] = strdup(paths[i]);
    }
    idx->path_count = (path_count < MAX_PATHS) ? path_count : MAX_PATHS;
    idx->db = db;

    // Reset all counters
    atomic_store(&idx->cancel_flag, 0);
    atomic_store(&idx->files_total, 0);
    atomic_store(&idx->files_processed, 0);
    atomic_store(&idx->files_new, 0);
    atomic_store(&idx->files_unchanged, 0);
    atomic_store(&idx->files_deleted, 0);
    atomic_store(&idx->dirs_scanned, 0);
    atomic_store(&idx->error_count, 0);
    atomic_store(&idx->phase, INDEXER_PHASE_SCANNING);
    atomic_store(&idx->albums_total, 0);
    atomic_store(&idx->albums_processed, 0);
    idx->current_path[0] = '\0';
    idx->phase_start_time = 0;
    idx->last_progress_time = 0;

    atomic_store(&idx->running, 1);
    if (pthread_create(&idx->worker_thread, NULL, indexer_worker, idx) != 0) {
        atomic_store(&idx->running, 0);
        return QUADRATURE_ERROR_INTERNAL;
    }
    idx->thread_started = true;
    return QUADRATURE_OK;
}

void indexer_cancel(indexer_t* idx) {
    if (idx) atomic_store(&idx->cancel_flag, 1);
}

bool indexer_is_running(const indexer_t* idx) {
    return idx && atomic_load(&idx->running);
}

void indexer_wait(indexer_t* idx) {
    if (idx && idx->thread_started) {
        pthread_join(idx->worker_thread, NULL);
        idx->thread_started = false;
    }
}

void indexer_get_progress(indexer_t* idx, indexer_progress_t* p) {
    if (!idx || !p) return;

    p->files_total = atomic_load(&idx->files_total);
    p->files_processed = atomic_load(&idx->files_processed);
    p->files_new = atomic_load(&idx->files_new);
    p->files_unchanged = atomic_load(&idx->files_unchanged);
    p->files_deleted = atomic_load(&idx->files_deleted);
    p->dirs_scanned = atomic_load(&idx->dirs_scanned);
    p->error_count = atomic_load(&idx->error_count);
    p->progress = p->files_total ? (double)p->files_processed / p->files_total : 0.0;
    p->phase = atomic_load(&idx->phase);
    p->albums_total = atomic_load(&idx->albums_total);
    p->albums_processed = atomic_load(&idx->albums_processed);

    pthread_mutex_lock(&idx->lock);
    p->phase_start_time = idx->phase_start_time;
    p->current_path = idx->current_path;
    pthread_mutex_unlock(&idx->lock);
}
