/**
 * Six-phase indexer implementation.
 *
 * Phase 1 - SCAN:        Fast directory walk, builds work queue
 * Phase 2 - METADATA:    Parallel tag extraction (FFmpeg, no audio decode)
 * Phase 3 - FINALIZE:    Batch mtime flush + WAL checkpoint
 *                         LIBRARY_READY fires here — UI browsable
 * Phase 4 - ARTWORK:     Parallel image processing
 *                         ARTWORK_READY fires here — new atlas available
 * Phase 5 - FINGERPRINT: Parallel Chromaprint + AcoustID (producer)
 * Phase 6 - RESOLVE:     Batched MusicBrainz PG resolution (consumer)
 *                         Phases 5+6 run concurrently via producer-consumer queue
 */

#define G_LOG_DOMAIN "quadrature"

#include "quadrature/indexer.h"
#include "quadrature/database.h"
#include "internal.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <ctype.h>

#define DEFAULT_ART_SIZE 48
#define ALBUM_MTIME_PAGE_SIZE 1000
#define PROGRESS_THROTTLE_US (100 * 1000)  // 100ms
#define DIR_SCAN_INITIAL_CAPACITY 64

static inline int64_t profile_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

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

// Compute max file mtime from a directory scan result.
// Returns 0 if no files were scanned.
static int64_t scan_max_file_mtime(const dir_scan_result_t* scan) {
    int64_t max_mtime = 0;
    for (size_t i = 0; i < scan->file_count; i++) {
        if (scan->stats[i].st_mtime > max_mtime)
            max_mtime = scan->stats[i].st_mtime;
    }
    return max_mtime;
}

// Forward declaration — struct defined later in this file (before struct indexer usage)
typedef struct artist_cache artist_cache_t;

// =============================================================================
// Work Queue Types
// =============================================================================

typedef struct {
    char* dir_path;         // Album directory (parent for multi-disc)
    int64_t dir_mtime;      // Max file mtime across album tracks
    int64_t album_id;       // 0 if new album
    bool mb_resolved;       // albums.mb_status == MB_STATUS_RESOLVED at scan time — cached, no point-query needed
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

static void work_queue_push(work_queue_t* q, const char* dir_path, int64_t mtime,
                             int64_t album_id, bool mb_resolved) {
    if (q->count >= q->capacity) {
        q->capacity = q->capacity ? q->capacity * 2 : 64;
        q->items = g_realloc(q->items, q->capacity * sizeof(album_work_item_t));
    }
    q->items[q->count++] = (album_work_item_t){
        .dir_path    = g_strdup(dir_path),
        .dir_mtime   = mtime,
        .album_id    = album_id,
        .mb_resolved = mb_resolved,
        .is_multi_disc = false,
        .disc_dirs   = NULL,
        .disc_nums   = NULL,
        .disc_count  = 0
    };
}

// Push a multi-disc album work item
// Takes ownership of disc_dirs and disc_nums arrays
static void work_queue_push_multi_disc(work_queue_t* q, const char* dir_path, int64_t mtime,
                                        int64_t album_id, bool mb_resolved,
                                        char** disc_dirs, uint16_t* disc_nums, size_t disc_count) {
    if (q->count >= q->capacity) {
        q->capacity = q->capacity ? q->capacity * 2 : 64;
        q->items = g_realloc(q->items, q->capacity * sizeof(album_work_item_t));
    }
    q->items[q->count++] = (album_work_item_t){
        .dir_path    = g_strdup(dir_path),
        .dir_mtime   = mtime,
        .album_id    = album_id,
        .mb_resolved = mb_resolved,
        .is_multi_disc = true,
        .disc_dirs   = disc_dirs,
        .disc_nums   = disc_nums,
        .disc_count  = disc_count
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
// Phase 2 Producer-Consumer: Free Functions
// =============================================================================

void extracted_track_free(extracted_track_t* track) {
    if (!track) return;
    g_free(track->rel_path);
    g_free(track->title);
    g_free(track->artist_tag);
    g_free(track->genre);
}

void metadata_result_free(metadata_result_t* result) {
    if (!result) return;
    for (size_t i = 0; i < result->track_count; i++) {
        extracted_track_free(&result->tracks[i]);
    }
    g_free(result->tracks);
    g_free(result->dir_path);
    g_free(result->album_rel_path);
    g_free(result->folder_name);
    g_free(result->album_artist);
    g_free(result->album_mb_release_id);
    g_free(result);
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

    quadrature_db_t* db;  /* Opened/closed by worker thread */
    int64_t scan_timestamp;
    int64_t scan_generation;  /* Error generation for mark-and-sweep persistence */

    // Progress counters (atomic for thread safety)
    atomic_size_t files_total;
    atomic_size_t files_processed;
    atomic_size_t files_new;
    atomic_size_t files_unchanged;
    atomic_size_t dirs_scanned;
    atomic_size_t albums_total;
    atomic_size_t albums_processed;
    atomic_size_t error_count;
    atomic_size_t fingerprint_total;
    atomic_size_t fingerprint_processed;
    atomic_int mb_pg_error;         /* MusicBrainz PG unreachable */
    atomic_int acoustid_error;      /* AcoustID index/PG unreachable */

    // Progress throttling (per-instance, not global)
    int64_t last_progress_time;

    atomic_int phase;
    int64_t phase_start_times[INDEXER_PHASE_COUNT];

    char current_path[INDEXER_PATH_MAX];
    pthread_mutex_t lock;  // Protects current_path and phase_start_times

    // Library root path (set at scan start, owned) — where music files live
    char* library_root;
    // Data root path (owned) — where DB + artwork live (NULL = same as library_root)
    char* data_root;

    // Path to atlas written during the last artwork phase (set after phase_artwork completes)
    char atlas_path[INDEXER_PATH_MAX];

    // MusicBrainz config (Phase 4)
    bool mb_resolve;
    char* pg_conninfo;

    // Solr search (Phase 4 text search)
    char* mb_solr_url;

    // AcoustID config (Phase 5)
    char* acoustid_pg_conninfo;
    char* acoustid_index_url;

    // Artist art config (Phase 7)
    char* fanart_api_key;
    char** other_artwork_dirs;      // artwork dirs from other libraries (heap-owned)
    size_t other_artwork_dirs_count;
    atomic_size_t artist_art_total;
    atomic_size_t artist_art_processed;
    atomic_size_t artist_art_downloaded;
    atomic_int fanart_error;

    // Artist bio config (Phase 8)
    atomic_size_t artist_bio_total;
    atomic_size_t artist_bio_processed;
    atomic_size_t artist_bio_fetched;

    // Artist name cache — pre-loaded before Phase 2 GThreadPool starts; NULL outside Phase 2
    artist_cache_t* artist_cache;
};

// =============================================================================
// Helpers
// =============================================================================

/** Get effective data root (where DB + artwork live). */
static const char* get_data_root(const indexer_t* idx) {
    return idx->data_root ? idx->data_root : idx->library_root;
}

static void set_phase(indexer_t* idx, indexer_phase_t phase) {
    atomic_store(&idx->phase, phase);
    pthread_mutex_lock(&idx->lock);
    idx->phase_start_times[phase] = g_get_monotonic_time();
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
void log_indexer_error(indexer_t* idx, const char* path, const char* fmt, ...) {
    if (!idx || !idx->db || !path || !fmt) return;
    
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    quadrature_result_t res = db_log_error(idx->db, path, msg, idx->scan_generation);
    if (res == QUADRATURE_OK && idx) {
        /* Note: error_count increment skipped in tests with mock indexer */
        if (idx->db && idx->scan_generation > 0) {
            atomic_fetch_add(&idx->error_count, 1);
        }
    }
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


/* Try to match disc suffix patterns at end of `name`:
 *   "... Disc N", "... CD N", "... (Disc N)"
 * On success: *prefix_len = length of prefix without suffix, *disc_num = N.
 */
static bool try_strip_disc_suffix(const char* name, size_t* prefix_len, uint16_t* disc_num) {
    size_t len = strlen(name);
    if (len == 0) return false;

    // Scan trailing digits
    size_t i = len;
    while (i > 0 && isdigit((unsigned char)name[i-1])) i--;
    if (i == len || i == 0) return false;
    int num = atoi(name + i);
    if (num <= 0 || num > 99) return false;

    size_t before_num = i;

    // Try "(Disc N)" or "(disc N)" — name ends with ")"
    if (len > 0 && name[len-1] == ')') {
        // Find opening "("
        size_t k = len - 1;
        while (k > 0 && name[k-1] != '(') k--;
        if (k == 0) return false;
        // k-1 is position of '('
        size_t open_paren = k - 1;
        // Skip space before '('
        size_t prefix = open_paren;
        while (prefix > 0 && name[prefix-1] == ' ') prefix--;
        if (prefix == 0) return false;
        // Verify content starts with "disc " or "cd "
        const char* inside = name + k;
        if (g_ascii_strncasecmp(inside, "disc ", 5) == 0 || g_ascii_strncasecmp(inside, "cd ", 3) == 0) {
            *prefix_len = prefix;
            *disc_num = (uint16_t)num;
            return true;
        }
        return false;
    }

    // Try patterns without parens: "... Disc N" or "... CD N"
    // before_num points to just before the digits
    size_t d = before_num;
    // Skip optional space before digits
    if (d > 0 && name[d-1] == ' ') d--;
    if (d == 0) return false;

    // Try " Disc" (5 chars)
    if (d >= 5 && g_ascii_strncasecmp(name + d - 5, " disc", 5) == 0) {
        size_t prefix = d - 5;
        while (prefix > 0 && name[prefix-1] == ' ') prefix--;
        if (prefix == 0) return false;
        *prefix_len = prefix;
        *disc_num = (uint16_t)num;
        return true;
    }
    // Try " CD" (3 chars)
    if (d >= 3 && g_ascii_strncasecmp(name + d - 3, " cd", 3) == 0) {
        size_t prefix = d - 3;
        while (prefix > 0 && name[prefix-1] == ' ') prefix--;
        if (prefix == 0) return false;
        *prefix_len = prefix;
        *disc_num = (uint16_t)num;
        return true;
    }

    return false;
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

        // Sort disc folders by disc number
        qsort(disc_infos, disc_info_count, sizeof(disc_info_t), compare_disc_info);

        // Count total files and compute max file mtime across all disc directories
        size_t total_files = 0;
        bool any_empty = false;
        int64_t max_file_mtime = 0;

        for (size_t i = 0; i < disc_info_count; i++) {
            dir_scan_result_t disc_scan = {0};
            dir_scan_single_pass(disc_infos[i].path, &disc_scan);

            if (disc_scan.file_count == 0) {
                any_empty = true;
            } else {
                total_files += disc_scan.file_count;
                int64_t disc_max = scan_max_file_mtime(&disc_scan);
                if (disc_max > max_file_mtime) max_file_mtime = disc_max;
            }

            dir_scan_result_free(&disc_scan);
        }

        // Mtime check: compare max file mtime (catches in-place metadata edits)
        gpointer value = (total_files > 0)
            ? g_hash_table_lookup(album_mtimes, dir_path) : NULL;
        bool needs_processing = true;

        if (value) {
            db_album_mtime_t* cached = value;
            if (cached->last_updated_at == max_file_mtime) {
                // Unchanged - skip, errors from previous scan persist
                atomic_fetch_add(&idx->files_unchanged, total_files);
                needs_processing = false;
            }
        }

        if (needs_processing && (total_files > 0 || any_empty)) {
            // Clear old errors before reprocessing this directory
            db_clear_errors_for_path(idx->db, dir_path);

            // Validation: Mixed content (audio files in parent alongside disc folders)
            if (scan.file_count > 0) {
                log_indexer_error(idx, dir_path,
                    "Album has both tracks and disc folders - move tracks into disc folders");
            }

            // Validation: Orphan disc folder (only one disc folder)
            if (disc_info_count == 1) {
                const char* disc_name = strrchr(disc_infos[0].path, '/');
                disc_name = disc_name ? disc_name + 1 : disc_infos[0].path;
                log_indexer_error(idx, dir_path,
                    "Single disc folder '%s' found - remove disc folder or add more discs", disc_name);
            }

            // Validation: Non-sequential disc numbers
            for (size_t i = 0; i < disc_info_count; i++) {
                if (disc_infos[i].disc_num != i + 1) {
                    log_indexer_error(idx, dir_path,
                        "Non-sequential disc folders (missing Disc %zu)", i + 1);
                    break;
                }
            }

            // Validation: Empty disc folders
            if (any_empty) {
                for (size_t i = 0; i < disc_info_count; i++) {
                    dir_scan_result_t disc_scan = {0};
                    dir_scan_single_pass(disc_infos[i].path, &disc_scan);
                    if (disc_scan.file_count == 0) {
                        const char* disc_name = strrchr(disc_infos[i].path, '/');
                        disc_name = disc_name ? disc_name + 1 : disc_infos[i].path;
                        log_indexer_error(idx, dir_path,
                            "Empty disc folder '%s' - add tracks or remove folder", disc_name);
                    }
                    dir_scan_result_free(&disc_scan);
                }
            }

            // Queue work item if all discs have tracks
            if (total_files > 0 && !any_empty) {
                char** disc_dirs = g_new(char*, disc_info_count);
                uint16_t* disc_nums = g_new(uint16_t, disc_info_count);
                for (size_t i = 0; i < disc_info_count; i++) {
                    disc_dirs[i] = g_strdup(disc_infos[i].path);
                    disc_nums[i] = disc_infos[i].disc_num;
                }

                db_album_mtime_t* cached = value;
                int64_t album_id = cached ? cached->album_id  : 0;
                bool mb_resolved = cached ? cached->mb_status == MB_STATUS_RESOLVED : false;
                work_queue_push_multi_disc(queue, dir_path, max_file_mtime, album_id,
                                           mb_resolved, disc_dirs, disc_nums, disc_info_count);
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
            // Compute max file mtime (catches in-place metadata edits, not just dir changes)
            int64_t max_file_mtime = scan_max_file_mtime(&scan);

            // Lookup in album mtimes hashmap
            gpointer value = g_hash_table_lookup(album_mtimes, dir_path);
            bool needs_processing = true;

            if (value) {
                db_album_mtime_t* cached = value;
                if (cached->last_updated_at == max_file_mtime) {
                    // Unchanged - skip
                    atomic_fetch_add(&idx->files_unchanged, scan.file_count);
                    needs_processing = false;
                }
            }

            if (needs_processing) {
                // Clear old errors before reprocessing this directory
                db_clear_errors_for_path(idx->db, dir_path);

                // Queue for metadata processing
                db_album_mtime_t* cached = value;
                int64_t album_id = cached ? cached->album_id  : 0;
                bool mb_resolved = cached ? cached->mb_status == MB_STATUS_RESOLVED : false;
                work_queue_push(queue, dir_path, max_file_mtime, album_id, mb_resolved);
                atomic_fetch_add(&idx->files_total, scan.file_count);
            }
        }

        // Sibling disc detection: group subdirs by common prefix differing only by disc suffix
        // e.g., "Album Disc 1" + "Album Disc 2" -> synthetic album "Album"
        if (depth == 0) {
            g_message("Root scan found %zu subdirs, %zu audio files", scan.subdir_count, scan.file_count);
        }

        GHashTable* sibling_groups = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)g_ptr_array_unref);
        GPtrArray* non_disc_subdirs = g_ptr_array_new();

        for (size_t i = 0; i < scan.subdir_count; i++) {
            const char* subdir_name = strrchr(scan.subdirs[i], '/');
            subdir_name = subdir_name ? subdir_name + 1 : scan.subdirs[i];

            // Skip already-identified disc folders (handled by is_multi_disc path above)
            if (is_disc_folder(subdir_name)) {
                g_ptr_array_add(non_disc_subdirs, scan.subdirs[i]);
                continue;
            }

            size_t prefix_len = 0;
            uint16_t dnum = 0;
            if (try_strip_disc_suffix(subdir_name, &prefix_len, &dnum)) {
                char* prefix_key = g_strndup(subdir_name, prefix_len);
                GPtrArray* group = g_hash_table_lookup(sibling_groups, prefix_key);
                if (!group) {
                    group = g_ptr_array_new();
                    g_hash_table_insert(sibling_groups, g_strdup(prefix_key), group);
                }
                g_ptr_array_add(group, GUINT_TO_POINTER((guint)dnum));
                g_ptr_array_add(group, scan.subdirs[i]);
                g_free(prefix_key);
            } else {
                g_ptr_array_add(non_disc_subdirs, scan.subdirs[i]);
            }
        }

        // Process sibling groups
        GHashTableIter sib_iter;
        gpointer sib_key, sib_val;
        g_hash_table_iter_init(&sib_iter, sibling_groups);
        while (g_hash_table_iter_next(&sib_iter, &sib_key, &sib_val)) {
            GPtrArray* group = sib_val;
            size_t disc_count = group->len / 2;
            if (disc_count < 2) {
                // Only 1 sibling — recurse normally
                const char* lone_path = g_ptr_array_index(group, 1);
                g_ptr_array_add(non_disc_subdirs, (gpointer)lone_path);
                continue;
            }

            // Collect (disc_num, path) pairs
            typedef struct { uint16_t disc_num; const char* path; } sib_disc_pair_t;
            sib_disc_pair_t* pairs = g_new(sib_disc_pair_t, disc_count);
            for (size_t si = 0; si < disc_count; si++) {
                pairs[si].disc_num = (uint16_t)GPOINTER_TO_UINT(g_ptr_array_index(group, si*2));
                pairs[si].path = g_ptr_array_index(group, si*2 + 1);
            }
            // Sort by disc_num (insertion sort for small arrays)
            for (size_t si = 1; si < disc_count; si++) {
                sib_disc_pair_t tmp = pairs[si];
                size_t j = si;
                while (j > 0 && pairs[j-1].disc_num > tmp.disc_num) {
                    pairs[j] = pairs[j-1];
                    j--;
                }
                pairs[j] = tmp;
            }

            // Compute synthetic album path: dir_path + "/" + prefix
            const char* prefix_str = (const char*)sib_key;
            char* synthetic_path = g_build_filename(dir_path, prefix_str, NULL);

            // Compute max file mtime across all sibling disc dirs
            int64_t max_mtime = 0;
            size_t total_files = 0;
            for (size_t si = 0; si < disc_count; si++) {
                dir_scan_result_t dscan = {0};
                dir_scan_single_pass(pairs[si].path, &dscan);
                total_files += dscan.file_count;
                int64_t disc_max = scan_max_file_mtime(&dscan);
                if (disc_max > max_mtime) max_mtime = disc_max;
                dir_scan_result_free(&dscan);
            }

            // Look up in album_mtimes using the absolute synthetic path as key
            gpointer existing_val = g_hash_table_lookup(album_mtimes, synthetic_path);
            db_album_mtime_t* sib_cached = existing_val;
            int64_t sib_album_id  = sib_cached ? sib_cached->album_id        : 0;
            int64_t cached_mtime  = sib_cached ? sib_cached->last_updated_at  : 0;
            bool sib_mb_resolved  = sib_cached ? sib_cached->mb_status == MB_STATUS_RESOLVED   : false;

            bool needs_processing = (cached_mtime != max_mtime);

            if (needs_processing) {
                // Clear old errors before reprocessing this directory
                db_clear_errors_for_path(idx->db, synthetic_path);

                char** disc_dirs = g_new(char*, disc_count);
                uint16_t* disc_nums = g_new(uint16_t, disc_count);
                for (size_t si = 0; si < disc_count; si++) {
                    disc_dirs[si] = g_strdup(pairs[si].path);
                    disc_nums[si] = pairs[si].disc_num;
                }

                work_queue_push_multi_disc(queue, synthetic_path, max_mtime, sib_album_id,
                                           sib_mb_resolved, disc_dirs, disc_nums, disc_count);
                atomic_fetch_add(&idx->files_total, total_files);
            } else {
                atomic_fetch_add(&idx->files_unchanged, total_files);
            }

            g_free(synthetic_path);
            g_free(pairs);
        }

        g_hash_table_destroy(sibling_groups);

        // Recurse into non-disc subdirs
        for (guint i = 0; i < non_disc_subdirs->len && !atomic_load(&idx->cancel_flag); i++) {
            const char* subdir = g_ptr_array_index(non_disc_subdirs, i);
            scan_directory_recursive(idx, subdir, album_mtimes, queue, depth + 1);
        }
        g_ptr_array_free(non_disc_subdirs, TRUE);
    }

    g_free(disc_infos);
    dir_scan_result_free(&scan);
}

static void phase_scan(indexer_t* idx, work_queue_t* queue) {
    set_phase(idx, INDEXER_PHASE_SCANNING);

    g_message("Scanning library root: %s", idx->library_root);

    // Initialize scan generation for error mark-and-sweep persistence
    idx->scan_generation = db_get_next_error_generation(idx->db);

    // Build hashmap of absolute path -> album_mtime from DB (paged)
    // Keys are owned (g_free) since we reconstruct absolute paths from relative DB paths.
    GHashTable* album_mtimes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
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
                // DB now stores relative paths; reconstruct absolute for lookup key
                char* abs_key = idx->library_root
                    ? g_build_filename(idx->library_root, page[i].path, NULL)
                    : g_strdup(page[i].path);
                g_hash_table_insert(album_mtimes, abs_key, &page[i]);
            }
        }

        offset += page_count;
        if (page_count < ALBUM_MTIME_PAGE_SIZE) break;
    }

    g_message("Loaded %u album mtimes from DB", g_hash_table_size(album_mtimes));

    // Walk the library root directory
    if (!atomic_load(&idx->cancel_flag)) {
        scan_directory_recursive(idx, idx->library_root, album_mtimes, queue, 0);
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
    GAsyncQueue* result_queue;  // Push metadata_result_t* here
    size_t result_index;        // Index into results array (for writer thread)
} metadata_work_t;

// =============================================================================
// Artist Name Cache (Phase 2) — eliminates repeated SELECT per artist
// =============================================================================

struct artist_cache {
    GHashTable* by_name;  // g_utf8_casefold(name) → gint64* artist_id; owns both
};

static void artist_cache_preload_cb(int64_t id, const char* name, void* user_data) {
    artist_cache_t* c = user_data;
    gint64* v = g_new(gint64, 1);
    *v = id;
    g_hash_table_insert(c->by_name, g_utf8_casefold(name, -1), v);
}

static artist_cache_t* artist_cache_new(quadrature_db_t* db) {
    artist_cache_t* c = g_new0(artist_cache_t, 1);
    c->by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    db_iter_artist_names(db, artist_cache_preload_cb, c);
    return c;
}

static void artist_cache_free(artist_cache_t* c) {
    if (!c) return;
    g_hash_table_destroy(c->by_name);
    g_free(c);
}

// Single-threaded: only called from the DB writer thread.
static int64_t get_or_create_artist_cached(artist_cache_t* cache,
                                            quadrature_db_t* db,
                                            const char* name) {
    if (!name || !*name) name = "Unknown Artist";
    char* key = g_utf8_casefold(name, -1);

    gint64* hit = g_hash_table_lookup(cache->by_name, key);
    if (hit) {
        int64_t id = *hit;
        g_free(key);
        return id;
    }
    g_free(key);

    int64_t id = db_get_or_create_artist(db, name);
    if (id > 0) {
        char* k2 = g_utf8_casefold(name, -1);
        gint64* v = g_new(gint64, 1);
        *v = id;
        g_hash_table_insert(cache->by_name, k2, v);
    }
    return id;
}

// =============================================================================
// Phase 2 worker helpers
// =============================================================================

/**
 * Transform a delimiter-separated artist tag into feat. format.
 * First segment = primary artist, remaining segments joined with " & ".
 *
 * Examples (delim='/'):
 *   "Odesza/Charlie Houston" → "Odesza feat. Charlie Houston"
 *   "Odesza/Mansionair/Naomi Wild" → "Odesza feat. Mansionair & Naomi Wild"
 * Examples (delim=';'):
 *   "Sampha;Fred again.." → "Sampha feat. Fred again.."
 */
static void apply_artist_delimiter(char** artist_tag, char delim) {
    const char* tag = *artist_tag;
    const char* first_sep = strchr(tag, delim);
    if (!first_sep) return;

    GString* result = g_string_sized_new(strlen(tag) + 8);
    g_string_append_len(result, tag, (gssize)(first_sep - tag));
    g_string_append(result, " feat. ");

    // Join remaining segments with " & "
    const char* rest = first_sep + 1;
    while (*rest) {
        const char* next = strchr(rest, delim);
        if (next) {
            g_string_append_len(result, rest, (gssize)(next - rest));
            g_string_append(result, " & ");
            rest = next + 1;
        } else {
            g_string_append(result, rest);
            break;
        }
    }

    g_free(*artist_tag);
    *artist_tag = g_string_free(result, FALSE);
}

/**
 * Strip featuring artists from track title and merge into artist_tag.
 * Cleans title "(feat. X)" → title without it, artist_tag gains " feat. X".
 * Skips merging if the first featuring artist already appears in artist_tag
 * (handles ARTIST tags that use "/" delimiters for the same credits).
 */
static void apply_title_featuring(char** title, char** artist_tag) {
    if (!*title) return;

    char* clean = NULL;
    char* feat = NULL;
    if (!title_extract_featuring(*title, &clean, &feat)) return;

    g_free(*title);
    *title = clean;

    if (!feat || !*artist_tag) {
        g_free(feat);
        return;
    }

    // Check if the first featuring artist is already in the artist tag
    // (covers both "feat." and "/" delimiter conventions)
    const char* ampersand = strstr(feat, " & ");
    size_t check_len = ampersand ? (size_t)(ampersand - feat) : strlen(feat);
    char* check_name = g_strndup(feat, check_len);

    bool already_present = (strcasestr(*artist_tag, check_name) != NULL);
    g_free(check_name);

    if (!already_present) {
        char* merged = g_strdup_printf("%s feat. %s", *artist_tag, feat);
        g_free(*artist_tag);
        *artist_tag = merged;
    }

    g_free(feat);
}

// Process a single-disc album
/*
 * Write Phase 2 (file-tag) artist credits for a track.
 * Skipped entirely if the track was already resolved by MusicBrainz (Phase 4).
 * Splits the raw artist tag on " feat. ", " ft. ", and " & " delimiters.
 * Logs a warning for tags containing " with " or ", " which may need manual review.
 */
static void write_phase2_track_artists(indexer_t* idx, bool mb_resolved,
                                        int64_t track_id, const char* artist_tag,
                                        const char* file_path) {
    if (mb_resolved) return;

    /* Warn about ambiguous delimiters we don't auto-split */
    if (strcasestr(artist_tag, " with "))
        log_indexer_error(idx, file_path,
            "Artist tag may contain unsplit credits (' with '): %s", artist_tag);
    if (strstr(artist_tag, ", "))
        log_indexer_error(idx, file_path,
            "Artist tag may contain unsplit credits (', '): %s", artist_tag);

    artist_credit_t* credits = NULL;
    size_t count = parse_artist_tag(artist_tag, &credits);

    db_track_artist_t* ta = g_new(db_track_artist_t, count);
    size_t written = 0;
    for (size_t k = 0; k < count; k++) {
        int64_t aid = get_or_create_artist_cached(idx->artist_cache, idx->db, credits[k].name);
        if (aid <= 0) continue;
        ta[written++] = (db_track_artist_t){
            .artist_id   = aid,
            .name        = credits[k].name,       /* borrowed; freed by artist_credits_free below */
            .position    = (int)k,
            .join_phrase = credits[k].join_phrase, /* borrowed; same lifetime */
        };
    }
    if (written > 0)
        db_set_track_artists(idx->db, track_id, ta, written);

    g_free(ta);
    artist_credits_free(credits, count);
}

static void metadata_worker_single_disc(metadata_work_t* work, indexer_t* idx) {
    const char* dir_path = work->item->dir_path;

    // Scan files in directory
    dir_scan_result_t scan = {0};
    dir_scan_single_pass(dir_path, &scan);

    if (scan.file_count == 0) {
        dir_scan_result_free(&scan);
        return;
    }

    // Extract metadata from each file via FFmpeg
    index_item_t* items = g_new0(index_item_t, scan.file_count);
    size_t valid_count = 0;
    const char* first_artist = NULL;
    const char* first_album_artist = NULL;
    const char* album_mb_release_id = NULL;

    for (size_t i = 0; i < scan.file_count && !atomic_load(&idx->cancel_flag); i++) {
        quadrature_result_t res = extract_audio_metadata(scan.files[i], &items[i]);
        if (res == QUADRATURE_OK) {
            if (!first_artist && items[i].artist)
                first_artist = items[i].artist;
            if (!first_album_artist && items[i].album_artist)
                first_album_artist = items[i].album_artist;
            if (!album_mb_release_id && items[i].mb_release_id)
                album_mb_release_id = items[i].mb_release_id;
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
        for (size_t i = 0; i < scan.file_count; i++) index_item_free(&items[i]);
        g_free(items);
        dir_scan_result_free(&scan);
        return;
    }

    // Build metadata_result_t for the writer thread
    const char* folder_name = strrchr(dir_path, '/');
    folder_name = folder_name ? folder_name + 1 : dir_path;
    const char* album_artist = first_album_artist ? first_album_artist
                             : first_artist ? first_artist
                             : "Unknown Artist";

    const char* rel_path = dir_path;
    if (idx->library_root && g_str_has_prefix(dir_path, idx->library_root)) {
        rel_path = dir_path + strlen(idx->library_root);
        while (*rel_path == '/') rel_path++;
    }

    // Detect if '/' or ';' is an artist delimiter for this album
    const char** artist_tags = g_new(const char*, scan.file_count);
    for (size_t i = 0; i < scan.file_count; i++)
        artist_tags[i] = items[i].artist;
    char delim = detect_artist_delimiter(artist_tags, scan.file_count);
    g_free(artist_tags);

    // Build extracted tracks array (only valid tracks)
    extracted_track_t* tracks = g_new0(extracted_track_t, valid_count);
    size_t track_idx = 0;
    for (size_t i = 0; i < scan.file_count; i++) {
        if (!items[i].path) continue;

        const char* track_rel = scan.files[i];
        if (g_str_has_prefix(scan.files[i], dir_path)) {
            track_rel = scan.files[i] + strlen(dir_path);
            while (*track_rel == '/') track_rel++;
        }

        tracks[track_idx] = (extracted_track_t){
            .rel_path    = g_strdup(track_rel),
            .title       = g_strdup(items[i].title),
            .artist_tag  = g_strdup(items[i].artist ? items[i].artist : album_artist),
            .genre       = g_strdup(items[i].genre),
            .duration_ms = items[i].duration_ms,
            .track_num   = items[i].track_num,
            .disc_num    = items[i].disc_num > 0 ? items[i].disc_num : 1,
            .year        = items[i].year,
            .mtime       = scan.stats[i].st_mtime,
        };
        if (delim)
            apply_artist_delimiter(&tracks[track_idx].artist_tag, delim);
        apply_title_featuring(&tracks[track_idx].title, &tracks[track_idx].artist_tag);
        track_idx++;
    }

    metadata_result_t* result = g_new0(metadata_result_t, 1);
    result->dir_path            = g_strdup(dir_path);
    result->album_rel_path      = g_strdup(rel_path);
    result->folder_name         = g_strdup(folder_name);
    result->album_artist        = g_strdup(album_artist);
    result->album_mb_release_id = g_strdup(album_mb_release_id);
    result->dir_mtime           = work->item->dir_mtime;
    result->mb_resolved         = work->item->mb_resolved;
    result->tracks              = tracks;
    result->track_count         = track_idx;
    result->result_index        = work->result_index;

    g_async_queue_push(work->result_queue, result);

    // Cleanup extraction data
    for (size_t i = 0; i < scan.file_count; i++) index_item_free(&items[i]);
    g_free(items);
    dir_scan_result_free(&scan);
}

// Process a multi-disc album (FFmpeg extraction only, pushes to queue)
static void metadata_worker_multi_disc(metadata_work_t* work, indexer_t* idx) {
    const char* album_dir = work->item->dir_path;

    // Collect all tracks from all disc directories
    GPtrArray* all_scans = g_ptr_array_new();
    GPtrArray* all_items = g_ptr_array_new();

    size_t valid_count = 0;
    const char* first_artist = NULL;
    const char* first_album_artist = NULL;
    const char* album_mb_release_id = NULL;

    for (size_t d = 0; d < work->item->disc_count && !atomic_load(&idx->cancel_flag); d++) {
        const char* disc_dir = work->item->disc_dirs[d];
        uint16_t disc_num = work->item->disc_nums[d];

        dir_scan_result_t* scan = g_new0(dir_scan_result_t, 1);
        dir_scan_single_pass(disc_dir, scan);

        if (scan->file_count == 0) {
            g_free(scan);
            continue;
        }

        index_item_t* disc_items = g_new0(index_item_t, scan->file_count);

        for (size_t i = 0; i < scan->file_count && !atomic_load(&idx->cancel_flag); i++) {
            quadrature_result_t res = extract_audio_metadata(scan->files[i], &disc_items[i]);
            if (res == QUADRATURE_OK) {
                disc_items[i].disc_num = disc_num;
                if (!first_artist && disc_items[i].artist)
                    first_artist = disc_items[i].artist;
                if (!first_album_artist && disc_items[i].album_artist)
                    first_album_artist = disc_items[i].album_artist;
                if (!album_mb_release_id && disc_items[i].mb_release_id)
                    album_mb_release_id = disc_items[i].mb_release_id;
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
        g_ptr_array_add(all_items, disc_items);
    }

    if (valid_count == 0 || atomic_load(&idx->cancel_flag)) {
        for (guint s = 0; s < all_scans->len; s++) {
            dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
            index_item_t* items = g_ptr_array_index(all_items, s);
            for (size_t i = 0; i < scan->file_count; i++) index_item_free(&items[i]);
            g_free(items);
            dir_scan_result_free(scan);
            g_free(scan);
        }
        g_ptr_array_free(all_scans, TRUE);
        g_ptr_array_free(all_items, TRUE);
        return;
    }

    // Build metadata_result_t
    const char* folder_name = strrchr(album_dir, '/');
    folder_name = folder_name ? folder_name + 1 : album_dir;
    const char* album_artist = first_album_artist ? first_album_artist
                             : first_artist ? first_artist
                             : "Unknown Artist";

    const char* rel_path = album_dir;
    if (idx->library_root && g_str_has_prefix(album_dir, idx->library_root)) {
        rel_path = album_dir + strlen(idx->library_root);
        while (*rel_path == '/') rel_path++;
    }

    // Detect if '/' or ';' is an artist delimiter across all discs
    GPtrArray* all_artist_tags = g_ptr_array_new();
    for (guint s = 0; s < all_items->len; s++) {
        dir_scan_result_t* sc = g_ptr_array_index(all_scans, s);
        index_item_t* it = g_ptr_array_index(all_items, s);
        for (size_t i = 0; i < sc->file_count; i++)
            g_ptr_array_add(all_artist_tags, (gpointer)it[i].artist);
    }
    char delim = detect_artist_delimiter(
        (const char* const*)all_artist_tags->pdata, all_artist_tags->len);
    g_ptr_array_free(all_artist_tags, TRUE);

    // Build extracted tracks array from all discs
    extracted_track_t* tracks = g_new0(extracted_track_t, valid_count);
    size_t track_idx = 0;

    for (guint s = 0; s < all_scans->len; s++) {
        dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
        index_item_t* items = g_ptr_array_index(all_items, s);

        for (size_t i = 0; i < scan->file_count; i++) {
            if (!items[i].path) continue;

            // Compute track path relative to album_dir
            char track_path_buf[INDEXER_PATH_MAX];
            const char* track_rel = scan->files[i];
            if (g_str_has_prefix(scan->files[i], album_dir)) {
                track_rel = scan->files[i] + strlen(album_dir);
                while (*track_rel == '/') track_rel++;
            } else {
                char* parent = g_path_get_dirname(album_dir);
                if (g_str_has_prefix(scan->files[i], parent)) {
                    const char* from_parent = scan->files[i] + strlen(parent);
                    while (*from_parent == '/') from_parent++;
                    snprintf(track_path_buf, sizeof(track_path_buf), "../%s", from_parent);
                    track_rel = track_path_buf;
                }
                g_free(parent);
            }

            tracks[track_idx] = (extracted_track_t){
                .rel_path    = g_strdup(track_rel),
                .title       = g_strdup(items[i].title),
                .artist_tag  = g_strdup(items[i].artist ? items[i].artist : album_artist),
                .genre       = g_strdup(items[i].genre),
                .duration_ms = items[i].duration_ms,
                .track_num   = items[i].track_num,
                .disc_num    = items[i].disc_num > 0 ? items[i].disc_num : 1,
                .year        = items[i].year,
                .mtime       = scan->stats[i].st_mtime,
            };
            if (delim)
                apply_artist_delimiter(&tracks[track_idx].artist_tag, delim);
            apply_title_featuring(&tracks[track_idx].title, &tracks[track_idx].artist_tag);
            track_idx++;
        }
    }

    metadata_result_t* result = g_new0(metadata_result_t, 1);
    result->dir_path            = g_strdup(album_dir);
    result->album_rel_path      = g_strdup(rel_path);
    result->folder_name         = g_strdup(folder_name);
    result->album_artist        = g_strdup(album_artist);
    result->album_mb_release_id = g_strdup(album_mb_release_id);
    result->dir_mtime           = work->item->dir_mtime;
    result->mb_resolved         = work->item->mb_resolved;
    result->tracks              = tracks;
    result->track_count         = track_idx;
    result->result_index        = work->result_index;

    g_async_queue_push(work->result_queue, result);

    // Cleanup extraction data
    for (guint s = 0; s < all_scans->len; s++) {
        dir_scan_result_t* scan = g_ptr_array_index(all_scans, s);
        index_item_t* items = g_ptr_array_index(all_items, s);
        for (size_t i = 0; i < scan->file_count; i++) index_item_free(&items[i]);
        g_free(items);
        dir_scan_result_free(scan);
        g_free(scan);
    }
    g_ptr_array_free(all_scans, TRUE);
    g_ptr_array_free(all_items, TRUE);
}

static void metadata_worker(gpointer data, gpointer user_data) {
    metadata_work_t* work = data;
    indexer_t* idx = user_data;

    if (atomic_load(&idx->cancel_flag)) return;

    if (work->item->is_multi_disc) {
        metadata_worker_multi_disc(work, idx);
    } else {
        metadata_worker_single_disc(work, idx);
    }
}

// =============================================================================
// Phase 2 DB Writer Thread (single-threaded SQLite consumer)
// =============================================================================

#define WRITER_BATCH_SIZE 50

typedef struct {
    indexer_t* idx;
    GAsyncQueue* queue;
    processed_album_t* results;
    artist_cache_t* artist_cache;
    size_t albums_written;
} metadata_writer_ctx_t;

/* Removed: check_album_track_gaps() moved to library_validation.c as validate_album_track_numbering() */

/**
 * Write a single metadata_result_t to the database.
 * Returns true on success, false on failure (album skipped).
 */
static bool write_album_to_db(metadata_writer_ctx_t* ctx, metadata_result_t* mr) {
    indexer_t* idx = ctx->idx;
    quadrature_db_t* db = idx->db;

    int64_t artist_id = get_or_create_artist_cached(
        ctx->artist_cache, db, mr->album_artist);
    if (artist_id < 0) {
        log_indexer_error(idx, mr->dir_path,
            "Failed to create artist: %s", mr->album_artist);
        return false;
    }

    int64_t album_id = 0;
    quadrature_result_t res = db_upsert_folder_album(db,
        mr->album_rel_path, mr->folder_name, artist_id, 0, &album_id);
    if (res != QUADRATURE_OK || album_id <= 0) {
        log_indexer_error(idx, mr->dir_path,
            "Failed to create album: %s", mr->folder_name);
        return false;
    }

    if (mr->album_mb_release_id)
        db_set_album_release_id_from_tags(db, album_id, mr->album_mb_release_id);

    for (size_t t = 0; t < mr->track_count; t++) {
        extracted_track_t* tr = &mr->tracks[t];

        db_index_item_t db_item = {
            .path        = tr->rel_path,
            .title       = tr->title,
            .album       = mr->folder_name,
            .duration_ms = tr->duration_ms,
            .track_num   = tr->track_num,
            .disc_num    = tr->disc_num,
            .year        = tr->year,
            .mtime       = tr->mtime,
            .genre       = tr->genre,
        };

        int64_t track_id = 0;
        if (db_upsert_track_with_album(db, &db_item, album_id, &track_id) == QUADRATURE_OK) {
            atomic_fetch_add(&idx->files_new, 1);
            if (track_id > 0) {
                write_phase2_track_artists(idx, mr->mb_resolved,
                    track_id, tr->artist_tag, mr->dir_path);
            }
        } else {
            log_indexer_error(idx, mr->dir_path,
                "Failed to save track: %s", tr->title ? tr->title : "(unknown)");
        }
    }

    db_sync_album_fts(db, album_id);

    /* ── Validate track numbering (supports both per-disc and continuous patterns) ── */
    validate_album_track_numbering(idx, mr);

    // Populate result for artwork/finalize phases
    ctx->results[mr->result_index].album_id = album_id;
    ctx->results[mr->result_index].mtime = mr->dir_mtime;
    ctx->results[mr->result_index].path = g_strdup(mr->dir_path);
    return true;
}

static gpointer metadata_db_writer_thread(gpointer data) {
    metadata_writer_ctx_t* ctx = data;
    indexer_t* idx = ctx->idx;

    // Accumulate items into batches for transaction efficiency
    GPtrArray* batch = g_ptr_array_new();
    size_t albums_written = 0;

    // Start first batch transaction
    db_begin_batch(idx->db);

    for (;;) {
        // Pop from queue with 100ms timeout to allow cancel checks
        metadata_result_t* item = g_async_queue_timeout_pop(ctx->queue, 100 * 1000);

        if (item == NULL) {
            if (atomic_load(&idx->cancel_flag)) break;
            continue;
        }

        // Sentinel means all producers are done
        if (item == (void*)0x1) break;

        g_ptr_array_add(batch, item);

        // Process batch when full or queue is empty
        if (batch->len < WRITER_BATCH_SIZE && g_async_queue_length(ctx->queue) > 0)
            continue;

        // Write all items in this batch
        for (guint b = 0; b < batch->len; b++) {
            metadata_result_t* mr = g_ptr_array_index(batch, b);
            if (atomic_load(&idx->cancel_flag)) {
                metadata_result_free(mr);
                continue;
            }
            if (write_album_to_db(ctx, mr))
                albums_written++;
            metadata_result_free(mr);
        }

        // Rotate batch transaction: commit current, start new
        db_commit_batch(idx->db);
        db_begin_batch(idx->db);

        g_ptr_array_set_size(batch, 0);
        notify_progress_throttled(idx);
    }

    // Write any remaining items in partial batch
    for (guint b = 0; b < batch->len; b++) {
        metadata_result_t* mr = g_ptr_array_index(batch, b);
        if (atomic_load(&idx->cancel_flag)) {
            metadata_result_free(mr);
            continue;
        }
        if (write_album_to_db(ctx, mr))
            albums_written++;
        metadata_result_free(mr);
    }

    // Final commit
    db_commit_batch(idx->db);

    // Drain any remaining items on cancellation
    if (atomic_load(&idx->cancel_flag)) {
        metadata_result_t* leftover;
        while ((leftover = g_async_queue_try_pop(ctx->queue)) != NULL) {
            if (leftover != (void*)0x1)
                metadata_result_free(leftover);
        }
    }

    g_ptr_array_free(batch, TRUE);
    ctx->albums_written = albums_written;
    return NULL;
}

// =============================================================================
// Phase 2: METADATA - Producer-consumer orchestration
// =============================================================================

static size_t phase_metadata(indexer_t* idx, work_queue_t* queue, processed_album_t** results_out) {
    set_phase(idx, INDEXER_PHASE_METADATA);

    if (queue->count == 0) {
        *results_out = NULL;
        return 0;
    }

    // Allocate results array (one slot per queued album)
    processed_album_t* results = g_new0(processed_album_t, queue->count);
    metadata_work_t* work_items = g_new0(metadata_work_t, queue->count);

    // Create the async queue for producer-consumer communication
    GAsyncQueue* result_queue = g_async_queue_new();

    // Pre-load artist name cache (owned by writer thread)
    idx->artist_cache = artist_cache_new(idx->db);

    // Start DB writer thread
    metadata_writer_ctx_t writer_ctx = {
        .idx = idx,
        .queue = result_queue,
        .results = results,
        .artist_cache = idx->artist_cache,
        .albums_written = 0,
    };
    GThread* writer_thread = g_thread_new("metadata-writer",
        metadata_db_writer_thread, &writer_ctx);

    // Create FFmpeg worker thread pool (producers)
    GThreadPool* pool = g_thread_pool_new(metadata_worker, idx, idx->thread_count, FALSE, NULL);
    if (!pool) {
        // Signal writer to stop, join, and clean up
        g_async_queue_push(result_queue, (void*)0x1);
        g_thread_join(writer_thread);
        g_async_queue_unref(result_queue);
        artist_cache_free(idx->artist_cache);
        idx->artist_cache = NULL;
        g_free(results);
        g_free(work_items);
        *results_out = NULL;
        return 0;
    }

    // Queue all work items for FFmpeg extraction
    for (size_t i = 0; i < queue->count; i++) {
        work_items[i] = (metadata_work_t){
            .idx = idx,
            .item = &queue->items[i],
            .result_queue = result_queue,
            .result_index = i,
        };
        g_thread_pool_push(pool, &work_items[i], NULL);
    }

    // Wait for all FFmpeg workers to finish
    while (!atomic_load(&idx->cancel_flag)) {
        bool queue_empty = g_thread_pool_unprocessed(pool) == 0;
        bool all_done = atomic_load(&idx->files_processed) >= atomic_load(&idx->files_total);
        if (queue_empty && all_done) break;
        notify_progress_throttled(idx);
        g_usleep(50000);
    }

    g_thread_pool_free(pool, FALSE, TRUE);

    // Push sentinel to signal writer thread that all producers are done
    g_async_queue_push(result_queue, (void*)0x1);

    // Wait for writer thread to drain the queue and finish DB writes
    g_thread_join(writer_thread);

    g_async_queue_unref(result_queue);
    artist_cache_free(idx->artist_cache);
    idx->artist_cache = NULL;

    // Compact results: move successful entries (album_id > 0) to front
    size_t success_count = 0;
    for (size_t i = 0; i < queue->count; i++) {
        if (results[i].album_id > 0) {
            if (success_count != i) {
                results[success_count] = results[i];
                // Zero out moved-from slot to prevent double-free
                results[i] = (processed_album_t){0};
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

    bool used_fallback = false;
    quadrature_result_t res = artwork_atlas_process_album(builder, work->album_id, work->path,
                                                           &used_fallback);
    if (work->indexer) {
        if (used_fallback) {
            log_indexer_error(work->indexer, work->path,
                "No album art found. Add cover.jpg, cover.png, folder.jpg, or front.jpg");
        } else if (res != QUADRATURE_OK && res != QUADRATURE_ERROR_CANCELLED) {
            log_indexer_error(work->indexer, work->path,
                "Failed to process album artwork");
        }
    }

    g_free(work->path);
    g_free(work);
}

/* g_ptr_array_sort passes char** — dereference before comparing. */
static gint cmp_str_ptr(gconstpointer a, gconstpointer b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Keep only the N most recent atlas files matching "{size}px-artwork-*.atlas". */
static void rotate_atlas_files(const char *artwork_dir, int thumb_size, int keep_count) {
    GDir *dir = g_dir_open(artwork_dir, 0, NULL);
    if (!dir) return;

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%dpx-artwork-", thumb_size);

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_prefix(name, prefix) && g_str_has_suffix(name, ".atlas"))
            g_ptr_array_add(paths, g_build_filename(artwork_dir, name, NULL));
    }
    g_dir_close(dir);

    if ((int)paths->len <= keep_count) { g_ptr_array_unref(paths); return; }

    /* Ascending lexicographic sort = oldest first (timestamps are fixed-width) */
    g_ptr_array_sort(paths, cmp_str_ptr);

    for (guint i = 0; i < paths->len - (guint)keep_count; i++) {
        const char *p = g_ptr_array_index(paths, i);
        if (g_remove(p) == 0)
            g_message("rotate_atlas_files: removed old atlas %s", p);
        else
            g_warning("rotate_atlas_files: failed to remove %s", p);
    }
    g_ptr_array_unref(paths);
}

/* Find path of the newest timestamped atlas for this library/size. Caller must g_free(). */
static char *find_latest_existing_atlas(const char *artwork_dir, int thumb_size) {
    GDir *dir = g_dir_open(artwork_dir, 0, NULL);
    if (!dir) return NULL;

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%dpx-artwork-", thumb_size);

    char *latest = NULL;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, prefix) || !g_str_has_suffix(name, ".atlas")) continue;
        char *full = g_build_filename(artwork_dir, name, NULL);
        if (!latest || strcmp(full, latest) > 0) { g_free(latest); latest = full; }
        else g_free(full);
    }
    g_dir_close(dir);
    return latest;
}

static void phase_artwork(indexer_t* idx, processed_album_t* albums, size_t album_count) {
    if (!idx->process_artwork) return;

    g_assert(idx->library_root && idx->library_root[0]);
    int64_t wall_start = profile_now_ns();

    const char* art_root = get_data_root(idx);
    char artwork_dir[INDEXER_PATH_MAX];
    snprintf(artwork_dir, sizeof(artwork_dir), "%s/artwork", art_root);
    g_mkdir_with_parents(artwork_dir, 0755);

    /* Find latest existing atlas for entry preservation */
    char *existing_path = find_latest_existing_atlas(artwork_dir, idx->art_size);

    /* Generate a new timestamped atlas path for this run */
    gint64 ts = g_get_real_time() / G_USEC_PER_SEC;
    char atlas_path[INDEXER_PATH_MAX];
    g_snprintf(atlas_path, sizeof(atlas_path), "%s/%dpx-artwork-%" G_GINT64_FORMAT ".atlas",
               artwork_dir, idx->art_size, ts);

    // Open existing atlas to preserve unchanged entries
    artwork_atlas_reader_t* existing_atlas = artwork_atlas_reader_open(existing_path);
    g_free(existing_path);
    size_t existing_count = existing_atlas ? artwork_atlas_reader_get_count(existing_atlas) : 0;

    // Build hash set of album_ids already in the existing atlas (O(n), no file I/O)
    GHashTable* atlas_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    for (size_t i = 0; i < existing_count; i++) {
        int64_t aid = artwork_atlas_reader_get_album_id_at(existing_atlas, i);
        if (aid >= 0) {
            gint64* key = g_new(gint64, 1);
            *key = aid;
            g_hash_table_add(atlas_ids, key);
        }
    }

    // Build hash set of album_ids being re-processed this scan
    GHashTable* reprocessed_ids = g_hash_table_new(g_int64_hash, g_int64_equal);
    for (size_t i = 0; i < album_count; i++) {
        g_hash_table_add(reprocessed_ids, &albums[i].album_id);
    }

    // Find albums in the DB with no existing atlas entry — these need artwork processing
    // even though their metadata was unchanged (e.g. artwork failed on first index,
    // or library was indexed before atlas support was added).
    GArray*    no_atlas_ids   = g_array_new(FALSE, FALSE, sizeof(int64_t));
    GPtrArray* no_atlas_paths = g_ptr_array_new_with_free_func(g_free);
    {
        size_t offset = 0;
        while (!atomic_load(&idx->cancel_flag)) {
            db_album_mtime_t* page = NULL;
            size_t page_count = 0;
            if (db_get_album_mtimes_page(idx->db, offset, ALBUM_MTIME_PAGE_SIZE,
                                         &page, &page_count) != QUADRATURE_OK)
                break;
            if (page_count == 0) { db_free_album_mtimes(page, 0); break; }

            for (size_t i = 0; i < page_count; i++) {
                if (!page[i].path || !page[i].path[0]) continue;
                if (g_hash_table_contains(reprocessed_ids, &page[i].album_id)) continue;
                if (g_hash_table_contains(atlas_ids, &page[i].album_id)) continue;
                /* Album has no atlas entry and wasn't re-processed — queue for artwork */
                g_array_append_val(no_atlas_ids, page[i].album_id);
                g_ptr_array_add(no_atlas_paths,
                    g_build_filename(idx->library_root, page[i].path, NULL));
            }

            db_free_album_mtimes(page, page_count);
            offset += page_count;
            if (page_count < ALBUM_MTIME_PAGE_SIZE) break;
        }
    }

    g_hash_table_destroy(atlas_ids);
    g_hash_table_destroy(reprocessed_ids);

    size_t total_to_process = album_count + no_atlas_ids->len;

    // Nothing to process — existing atlas (if any) is already up to date
    if (total_to_process == 0) {
        if (existing_atlas) artwork_atlas_reader_close(existing_atlas);
        g_array_free(no_atlas_ids, TRUE);
        g_ptr_array_free(no_atlas_paths, TRUE);
        return;
    }

    if (total_to_process > 0) {
        set_phase(idx, INDEXER_PHASE_ARTWORK);
        atomic_store(&idx->albums_total, total_to_process);
        atomic_store(&idx->albums_processed, 0);
    }

    artwork_atlas_builder_t* builder = NULL;
    if (artwork_atlas_builder_create(atlas_path, idx->art_size, &builder) != QUADRATURE_OK) {
        if (existing_atlas) artwork_atlas_reader_close(existing_atlas);
        g_array_free(no_atlas_ids, TRUE);
        g_ptr_array_free(no_atlas_paths, TRUE);
        return;
    }

    // Copy unchanged entries from existing atlas (already fully loaded into memory by reader)
    int64_t preserve_start = profile_now_ns();
    size_t preserved_count = 0;
    if (existing_atlas && existing_count > 0) {
        uint32_t pixel_stride = artwork_atlas_reader_get_pixel_stride(existing_atlas);
        for (size_t i = 0; i < existing_count && !atomic_load(&idx->cancel_flag); i++) {
            int64_t album_id = artwork_atlas_reader_get_album_id_at(existing_atlas, i);
            if (album_id < 0) continue;

            const uint8_t* pixels = artwork_atlas_reader_get_pixels_at(existing_atlas, i);
            if (pixels) {
                artwork_atlas_add_cached_pixels(builder, album_id, pixels, pixel_stride);
                preserved_count++;
            }
        }
    }
    int64_t preserve_ns = profile_now_ns() - preserve_start;

    if (existing_atlas) artwork_atlas_reader_close(existing_atlas);

    // Process changed albums AND albums missing from atlas in parallel
    int64_t pool_start = profile_now_ns();
    if (total_to_process > 0) {
        GThreadPool* pool = g_thread_pool_new(artwork_worker, builder, idx->thread_count, FALSE, NULL);
        if (!pool) {
            g_array_free(no_atlas_ids, TRUE);
            g_ptr_array_free(no_atlas_paths, TRUE);
            artwork_atlas_builder_destroy(builder);
            return;
        }

        // Queue artwork work (paths carried from metadata phase)
        for (size_t i = 0; i < album_count && !atomic_load(&idx->cancel_flag); i++) {
            if (albums[i].path) {
                artwork_work_t* work = g_new(artwork_work_t, 1);
                work->album_id = albums[i].album_id;
                work->path = g_strdup(albums[i].path);
                work->indexer = idx;
                g_thread_pool_push(pool, work, NULL);
            }
        }

        // Queue albums with no existing atlas entry
        for (guint i = 0; i < no_atlas_ids->len && !atomic_load(&idx->cancel_flag); i++) {
            artwork_work_t* work = g_new(artwork_work_t, 1);
            work->album_id = g_array_index(no_atlas_ids, int64_t, i);
            work->path = g_strdup(g_ptr_array_index(no_atlas_paths, i));
            work->indexer = idx;
            g_thread_pool_push(pool, work, NULL);
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
    int64_t pool_ns = profile_now_ns() - pool_start;

    g_array_free(no_atlas_ids, TRUE);
    g_ptr_array_free(no_atlas_paths, TRUE);

    int64_t finish_start = profile_now_ns();
    if (!atomic_load(&idx->cancel_flag)) {
        if (artwork_atlas_builder_finish(builder) == QUADRATURE_OK) {
            /* Rotate: keep only the 3 most recent atlas files */
            rotate_atlas_files(artwork_dir, idx->art_size, 3);

            /* Store path for progress callback (read under lock in indexer_get_progress) */
            pthread_mutex_lock(&idx->lock);
            strncpy(idx->atlas_path, atlas_path, INDEXER_PATH_MAX - 1);
            idx->atlas_path[INDEXER_PATH_MAX - 1] = '\0';
            pthread_mutex_unlock(&idx->lock);
        }
    }
    int64_t finish_ns = profile_now_ns() - finish_start;

    // =========================================================================
    // Profile summary
    // =========================================================================
    int64_t wall_ns = profile_now_ns() - wall_start;
    size_t new_count = total_to_process;
    artwork_atlas_profile_t prof;
    artwork_atlas_builder_get_profile(builder, &prof);

    if (new_count > 0 || preserved_count > 0) {
        double wall_ms = (double)wall_ns / 1e6;
        double n = (double)new_count;

        g_info("=== Artwork Atlas Profile (%zu new + %zu preserved = %zu total) ===",
               new_count, preserved_count, new_count + preserved_count);
        g_info("  Phase-level:  preserve=%.0fms  pool=%.0fms  finish=%.0fms  wall=%.0fms",
               (double)preserve_ns / 1e6, (double)pool_ns / 1e6,
               (double)finish_ns / 1e6, wall_ms);
        if (n > 0) {
            g_info("  Per-album avg (%.2fms total):",
                   (double)pool_ns / 1e6 / n);
            g_info("    find=%.2fms  resize=%.2fms  fallbacks=%zu",
                   (double)prof.find_ns / 1e6 / n,
                   (double)prof.resize_ns / 1e6 / n,
                   prof.fallback_count);
            g_info("  Throughput: %.1f albums/sec (pool), %.1f albums/sec (wall)",
                   (double)pool_ns > 0 ? n / ((double)pool_ns / 1e9) : 0.0,
                   wall_ms > 0 ? n / (wall_ms / 1000.0) : 0.0);
        }
    }

    artwork_atlas_builder_destroy(builder);
}

// =============================================================================
// Phase 3: FINALIZE - Batch DB updates (runs before LIBRARY_READY)
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

    // Prune orphan artists left behind by artist name normalization/splitting
    db_prune_orphan_artists(idx->db);

    // WAL checkpoint
    db_checkpoint(idx->db);
}

// =============================================================================
// Worker Thread
// =============================================================================

/* Bridge callback: receives mb_resolver_progress_t events and writes them into
 * the indexer's shared atomics so the existing throttled-notify path picks them up. */
static void on_mb_resolver_progress(const mb_resolver_progress_t* p, void* user_data) {
    indexer_t* idx = user_data;
    atomic_store(&idx->albums_total,          p->albums_total);
    atomic_store(&idx->albums_processed,      p->albums_processed);
    atomic_store(&idx->fingerprint_total,     p->fingerprint_total);
    atomic_store(&idx->fingerprint_processed, p->fingerprint_processed);
    if (p->acoustid_error)
        atomic_store(&idx->acoustid_error, 1);

    /* Transition indexer phase based on resolver phase */
    if (p->phase == MB_RESOLVE_FINGERPRINTING || p->phase == MB_RESOLVE_MATCHING) {
        indexer_phase_t cur = atomic_load(&idx->phase);
        if (cur != INDEXER_PHASE_FINGERPRINT)
            set_phase(idx, INDEXER_PHASE_FINGERPRINT);
    } else if (p->phase == MB_RESOLVE_FETCHING || p->phase == MB_RESOLVE_WRITING) {
        indexer_phase_t cur = atomic_load(&idx->phase);
        if (cur != INDEXER_PHASE_RESOLVE)
            set_phase(idx, INDEXER_PHASE_RESOLVE);
    }

    notify_progress_throttled(idx);
}

static void on_artist_art_progress(const artist_art_progress_t* p, void* user_data) {
    indexer_t* idx = user_data;
    atomic_store(&idx->artist_art_total, p->total);
    atomic_store(&idx->artist_art_processed, p->processed);
    atomic_store(&idx->artist_art_downloaded, p->downloaded);
    notify_progress_throttled(idx);
}

static void on_artist_bio_progress(const artist_bio_progress_t* p, void* user_data) {
    indexer_t* idx = user_data;
    atomic_store(&idx->artist_bio_total, p->total);
    atomic_store(&idx->artist_bio_processed, p->processed);
    atomic_store(&idx->artist_bio_fetched, p->fetched);
    notify_progress_throttled(idx);
}

// =============================================================================
// Disk Space Pre-flight
// =============================================================================

/* Bytes per audio file estimate for quadrature data (DB + artwork + WAL).
 *
 * Breakdown:
 *   SQLite rows (tracks, albums, artists, indices):  ~2 KB/track
 *   Album atlas (48²×4 per album, ~12 tracks/album): ~0.8 KB/track
 *   Artist art  (~300 KB/artist, ~50 tracks/artist):  ~6 KB/track
 *   WAL headroom during writes:                       ~2 KB/track
 *   Safety margin:                                    ~4 KB/track
 *                                                    ─────────────
 *   Total:                                           ~15 KB/track
 */
#define BYTES_PER_TRACK_ESTIMATE (15 * 1024)

/* Absolute minimum free space required regardless of library size.
 * Covers SQLite overhead, WAL, and the atlas file itself. */
#define MIN_SPACE_BYTES (10 * 1024 * 1024)  /* 10 MB */

/**
 * Quick recursive count of audio files under a directory.
 * Does NOT parse metadata — just stat() + extension check.
 */
static size_t count_audio_files(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) return 0;

    size_t count = 0;
    struct dirent* ent;
    char path_buf[PATH_MAX];

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(path_buf, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            count += count_audio_files(path_buf);
        } else if (S_ISREG(st.st_mode) && is_audio_file(ent->d_name)) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/**
 * Estimate space needed and check available disk space.
 *
 * For fresh libraries: estimates from audio file count.
 * For rescans: uses existing DB size + estimates delta growth.
 *
 * Returns QUADRATURE_OK if sufficient space, QUADRATURE_ERROR_INTERNAL if not.
 * On failure, writes a human-readable message to msg_buf.
 */
static quadrature_result_t check_disk_space(const char* data_root,
                                              const char* music_root,
                                              quadrature_db_t* db,
                                              char* msg_buf, size_t msg_size) {
    struct statvfs vfs;
    if (statvfs(data_root, &vfs) != 0) {
        snprintf(msg_buf, msg_size, "Cannot stat filesystem at %s", data_root);
        return QUADRATURE_ERROR_INTERNAL;
    }

    uint64_t available = (uint64_t)vfs.f_bavail * vfs.f_frsize;

    /* Count audio files on disk */
    size_t disk_files = count_audio_files(music_root);

    /* For rescan: subtract already-indexed tracks to get delta */
    size_t existing_tracks = 0;
    uint64_t existing_db_size = 0;
    if (db) {
        db_get_total_track_count(db, &existing_tracks);
        char* db_path = g_build_filename(data_root, "quadrature.sqlite", NULL);
        struct stat db_stat;
        if (stat(db_path, &db_stat) == 0)
            existing_db_size = (uint64_t)db_stat.st_size;
        g_free(db_path);
    }

    size_t new_files = disk_files > existing_tracks ? disk_files - existing_tracks : 0;

    /* Space estimate: existing DB + WAL headroom + new file growth */
    uint64_t estimated = existing_db_size             /* current DB footprint */
                       + existing_db_size             /* WAL can double DB during writes */
                       + (uint64_t)new_files * BYTES_PER_TRACK_ESTIMATE;

    if (estimated < MIN_SPACE_BYTES)
        estimated = MIN_SPACE_BYTES;

    if (available < estimated) {
        double avail_mb = (double)available / (1024.0 * 1024.0);
        double need_mb  = (double)estimated / (1024.0 * 1024.0);
        snprintf(msg_buf, msg_size,
                 "Insufficient disk space: %.0f MB available, ~%.0f MB needed "
                 "(%zu new tracks detected)",
                 avail_mb, need_mb, new_files);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_message("Disk space check: %.0f MB available, ~%.0f MB estimated "
              "(%zu files on disk, %zu existing, %zu new)",
              (double)available / (1024.0 * 1024.0),
              (double)estimated / (1024.0 * 1024.0),
              disk_files, existing_tracks, new_files);

    return QUADRATURE_OK;
}

static void* indexer_worker(void* arg) {
    indexer_t* idx = arg;
    idx->scan_timestamp = time(NULL);

    // Open per-library database (in data_root, not library_root)
    const char* dr = get_data_root(idx);
    char* db_path = g_build_filename(dr, "quadrature.sqlite", NULL);
    if (db_open(db_path, &idx->db) != QUADRATURE_OK) {
        g_critical("indexer_worker: failed to open DB at %s", db_path);
        g_free(db_path);
        notify_event(idx, INDEXER_ERROR);
        atomic_store(&idx->running, 0);
        return NULL;
    }
    g_free(db_path);

    // Disk space pre-flight
    char space_msg[256];
    if (check_disk_space(dr, idx->library_root, idx->db, space_msg, sizeof(space_msg))
            != QUADRATURE_OK) {
        g_warning("indexer_worker: %s", space_msg);
        db_close(idx->db);
        idx->db = NULL;
        notify_event(idx, INDEXER_ERROR);
        atomic_store(&idx->running, 0);
        return NULL;
    }

    // Declare all resources at top for proper cleanup on cancel
    work_queue_t queue;
    work_queue_init(&queue);
    processed_album_t* processed = NULL;
    size_t processed_count = 0;

    notify_event(idx, INDEXER_STARTED);
    g_message("Indexing library: %s", idx->library_root);

    // Phase 1: SCAN
    phase_scan(idx, &queue);
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // Phase 2: METADATA
    processed_count = phase_metadata(idx, &queue, &processed);
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // Phase 3: FINALIZE (mtime batch flush + WAL checkpoint — before LIBRARY_READY for durability)
    phase_finalize(idx, processed, processed_count);

    // Prune orphan errors for directories that no longer exist
    db_prune_orphan_errors(idx->db, idx->library_root);

    // Phases 1-3 done: metadata usable, UI browsable (artwork may still be placeholder)
    notify_event(idx, INDEXER_LIBRARY_UPDATED);

    // Phase 4: ARTWORK (runs after LIBRARY_UPDATED so UI is immediately browsable)
    phase_artwork(idx, processed, processed_count);
    if (atomic_load(&idx->cancel_flag)) goto cancelled;
    notify_event(idx, INDEXER_ARTWORK_UPDATED);

    // Phase 5+6: FINGERPRINT + RESOLVE (concurrent, background, non-blocking for UI)
    if (!idx->mb_resolve) {
        g_message("Phase 4+5: MusicBrainz resolve disabled (enable in settings)");
    } else if (!idx->pg_conninfo || !idx->pg_conninfo[0]) {
        g_warning("Phase 4+5: MusicBrainz resolve enabled but pg_conninfo is empty — skipping");
    } else {
        set_phase(idx, INDEXER_PHASE_FINGERPRINT);
        /* Reset artwork counters so stale Phase 3 totals don't appear as initial resolve state */
        atomic_store(&idx->albums_total, 0);
        atomic_store(&idx->albums_processed, 0);
        atomic_store(&idx->fingerprint_total, 0);
        atomic_store(&idx->fingerprint_processed, 0);
        /* Force-notify so UI sees phase change before mb_resolver_create blocks on PG.
         * Reset throttle timer to bypass the 100ms gate — this is a one-time transition. */
        idx->last_progress_time = 0;
        notify_progress_throttled(idx);
        g_message("Phase 4+5: starting MusicBrainz resolve");
        mb_resolver_options_t opts = {
            .pg_conninfo = idx->pg_conninfo,
            .acoustid_pg_conninfo = idx->acoustid_pg_conninfo,
            .acoustid_index_url = idx->acoustid_index_url,
            .mb_solr_url = idx->mb_solr_url,
            .library_root = get_data_root(idx),
        };
        mb_resolver_t* resolver = NULL;
        quadrature_result_t res = mb_resolver_create(&resolver, idx->db, &opts,
                                                      on_mb_resolver_progress, idx);
        if (res != QUADRATURE_OK) {
            g_warning("Phase 4: mb_resolver_create failed (err=%d) — check PG connection", res);
            atomic_store(&idx->mb_pg_error, 1);
        } else {
            res = mb_resolver_run(resolver);
            if (res != QUADRATURE_OK) {
                g_warning("Phase 4: mb_resolver_run returned error %d", res);
            }
            mb_resolver_destroy(resolver);
        }
        // MB resolve may orphan Phase 2 artists that were replaced by corrected
        // MusicBrainz entries — prune them before artist art/bio phases run.
        db_prune_orphan_artists(idx->db);

        // MB enrichment now in DB — reload cache so UI reflects resolved metadata
        notify_event(idx, INDEXER_LIBRARY_UPDATED);
    }
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // =========================================================================
    // Phase 7: Artist Art (fanart.tv)
    // =========================================================================

    {
        set_phase(idx, INDEXER_PHASE_ARTIST_ART);
        atomic_store(&idx->artist_art_total, 0);
        atomic_store(&idx->artist_art_processed, 0);
        atomic_store(&idx->artist_art_downloaded, 0);
        atomic_store(&idx->fanart_error, 0);
        idx->last_progress_time = 0;
        notify_progress_throttled(idx);

        char* artwork_dir = g_strdup_printf("%s/artwork", get_data_root(idx));
        char* atlas_dir = g_build_filename(g_get_user_data_dir(), "quadrature", "atlas", NULL);
        char* atlas_path = g_build_filename(atlas_dir, "artists.atlas", NULL);
        char* atlas_lock_path = g_build_filename(atlas_dir, "artists.atlas.lock", NULL);

        artist_art_config_t art_config = {
            .personal_api_key = idx->fanart_api_key,
            .artwork_dir = artwork_dir,
            .db = idx->db,
            .cancel_flag = &idx->cancel_flag,
            .rate_limit_ms = 500,
            .art_thumb_size = idx->art_size,
            .atlas_path = atlas_path,
            .atlas_lock_path = atlas_lock_path,
            .other_artwork_dirs = (const char* const*)idx->other_artwork_dirs,
            .other_artwork_dirs_count = idx->other_artwork_dirs_count,
        };

        g_message("Phase 7: starting artist art fetch (fanart.tv)");
        quadrature_result_t art_res = artist_art_fetch_all(&art_config,
            on_artist_art_progress, idx);

        if (art_res != QUADRATURE_OK) {
            g_warning("Phase 7: artist_art_fetch_all returned error %d", art_res);
            atomic_store(&idx->fanart_error, 1);
        }
        g_free(artwork_dir);
        g_free(atlas_path);
        g_free(atlas_lock_path);
        g_free(atlas_dir);
        // Artist atlas updated — reload so new artist thumbnails appear
        notify_event(idx, INDEXER_ARTWORK_UPDATED);
    }
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    // =========================================================================
    // Phase 8: Artist Bios (Wikipedia via Wikidata)
    // =========================================================================

    {
        set_phase(idx, INDEXER_PHASE_ARTIST_BIO);
        atomic_store(&idx->artist_bio_total, 0);
        atomic_store(&idx->artist_bio_processed, 0);
        atomic_store(&idx->artist_bio_fetched, 0);
        idx->last_progress_time = 0;
        notify_progress_throttled(idx);

        artist_bio_config_t bio_config = {
            .db = idx->db,
            .library_root = get_data_root(idx),
            .cancel_flag = &idx->cancel_flag,
            .rate_limit_ms = 250,
        };

        g_message("Phase 8: starting artist bio fetch (Wikipedia)");
        quadrature_result_t bio_res = artist_bio_fetch_all(&bio_config,
            on_artist_bio_progress, idx);

        if (bio_res != QUADRATURE_OK)
            g_warning("Phase 8: artist_bio_fetch_all returned error %d", bio_res);
    }
    if (atomic_load(&idx->cancel_flag)) goto cancelled;

    set_phase(idx, INDEXER_PHASE_COMPLETE);
    size_t errors = atomic_load(&idx->error_count);
    g_message("Complete: %zu new, %zu unchanged, %zu errors",
              atomic_load(&idx->files_new),
              atomic_load(&idx->files_unchanged),
              errors);
    notify_event(idx, INDEXER_COMPLETED);

    processed_albums_free(processed, processed_count);
    work_queue_free(&queue);
    db_close(idx->db);
    idx->db = NULL;
    atomic_store(&idx->running, 0);
    return NULL;

cancelled:
    notify_event(idx, INDEXER_CANCELLED);
    processed_albums_free(processed, processed_count);
    work_queue_free(&queue);
    db_close(idx->db);
    idx->db = NULL;
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
    idx->thread_count = CLAMP(threads, 4, 64);
    idx->process_artwork = config ? config->process_artwork : true;
    idx->art_size = (config && config->art_size > 0) ? config->art_size : DEFAULT_ART_SIZE;

    if (config) {
        idx->callback = config->callback;
        idx->user_data = config->user_data;
        idx->mb_resolve = config->mb_resolve;
        idx->pg_conninfo = config->pg_conninfo ? strdup(config->pg_conninfo) : NULL;
        idx->mb_solr_url = config->mb_solr_url ? strdup(config->mb_solr_url) : NULL;
        idx->acoustid_pg_conninfo = config->acoustid_pg_conninfo ? strdup(config->acoustid_pg_conninfo) : NULL;
        idx->acoustid_index_url = config->acoustid_index_url ? strdup(config->acoustid_index_url) : NULL;
        idx->fanart_api_key = config->fanart_api_key ? strdup(config->fanart_api_key) : NULL;
        if (config->other_library_roots && config->other_library_roots_count > 0) {
            idx->other_artwork_dirs_count = config->other_library_roots_count;
            idx->other_artwork_dirs = calloc(idx->other_artwork_dirs_count, sizeof(char*));
            for (size_t i = 0; i < idx->other_artwork_dirs_count; i++) {
                idx->other_artwork_dirs[i] = g_strdup_printf("%s/artwork",
                    config->other_library_roots[i]);
            }
        }
    }

    pthread_mutex_init(&idx->lock, NULL);
    *out = idx;
    return QUADRATURE_OK;
}

void indexer_destroy(indexer_t* idx) {
    if (!idx) return;
    indexer_cancel(idx);
    indexer_wait(idx);
    free(idx->library_root);
    free(idx->data_root);
    free(idx->pg_conninfo);
    free(idx->mb_solr_url);
    free(idx->acoustid_pg_conninfo);
    free(idx->acoustid_index_url);
    free(idx->fanart_api_key);
    for (size_t i = 0; i < idx->other_artwork_dirs_count; i++)
        free(idx->other_artwork_dirs[i]);
    free(idx->other_artwork_dirs);
    pthread_mutex_destroy(&idx->lock);
    free(idx);
}

quadrature_result_t indexer_scan(indexer_t* idx, const char* library_root,
                                 const char* data_root) {
    if (!idx || !library_root || !library_root[0]) return QUADRATURE_ERROR_INVALID_PARAM;
    if (atomic_load(&idx->running)) return QUADRATURE_ERROR_DEVICE_BUSY;

    free(idx->library_root);
    idx->library_root = strdup(library_root);
    free(idx->data_root);
    idx->data_root = (data_root && data_root[0] && strcmp(data_root, library_root) != 0)
                     ? strdup(data_root) : NULL;

    // Reset all counters
    atomic_store(&idx->cancel_flag, 0);
    atomic_store(&idx->files_total, 0);
    atomic_store(&idx->files_processed, 0);
    atomic_store(&idx->files_new, 0);
    atomic_store(&idx->files_unchanged, 0);
    atomic_store(&idx->dirs_scanned, 0);
    atomic_store(&idx->error_count, 0);
    atomic_store(&idx->phase, INDEXER_PHASE_SCANNING);
    atomic_store(&idx->albums_total, 0);
    atomic_store(&idx->albums_processed, 0);
    atomic_store(&idx->fingerprint_total, 0);
    atomic_store(&idx->fingerprint_processed, 0);
    atomic_store(&idx->mb_pg_error, 0);
    atomic_store(&idx->acoustid_error, 0);
    atomic_store(&idx->artist_art_total, 0);
    atomic_store(&idx->artist_art_processed, 0);
    atomic_store(&idx->artist_art_downloaded, 0);
    atomic_store(&idx->fanart_error, 0);
    atomic_store(&idx->artist_bio_total, 0);
    atomic_store(&idx->artist_bio_processed, 0);
    atomic_store(&idx->artist_bio_fetched, 0);
    idx->current_path[0] = '\0';
    idx->atlas_path[0] = '\0';
    memset(idx->phase_start_times, 0, sizeof(idx->phase_start_times));
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
    p->dirs_scanned = atomic_load(&idx->dirs_scanned);
    p->error_count = atomic_load(&idx->error_count);
    p->progress = p->files_total ? (double)p->files_processed / p->files_total : 0.0;
    p->phase = atomic_load(&idx->phase);
    p->albums_total = atomic_load(&idx->albums_total);
    p->albums_processed = atomic_load(&idx->albums_processed);
    p->fingerprint_total = atomic_load(&idx->fingerprint_total);
    p->fingerprint_processed = atomic_load(&idx->fingerprint_processed);
    p->artist_art_total = atomic_load(&idx->artist_art_total);
    p->artist_art_processed = atomic_load(&idx->artist_art_processed);
    p->artist_art_downloaded = atomic_load(&idx->artist_art_downloaded);
    p->artist_bio_total = atomic_load(&idx->artist_bio_total);
    p->artist_bio_processed = atomic_load(&idx->artist_bio_processed);
    p->artist_bio_fetched = atomic_load(&idx->artist_bio_fetched);
    p->mb_pg_error = atomic_load(&idx->mb_pg_error);
    p->acoustid_error = atomic_load(&idx->acoustid_error);
    p->fanart_error = atomic_load(&idx->fanart_error);

    pthread_mutex_lock(&idx->lock);
    memcpy(p->phase_start_times, idx->phase_start_times, sizeof(p->phase_start_times));
    p->current_path = idx->current_path;
    strncpy(p->atlas_path, idx->atlas_path, sizeof(p->atlas_path) - 1);
    p->atlas_path[sizeof(p->atlas_path) - 1] = '\0';
    pthread_mutex_unlock(&idx->lock);
}
