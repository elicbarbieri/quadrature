#ifndef QUADRATURE_INDEXER_H
#define QUADRATURE_INDEXER_H

#include "quadrature/core/types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Four-Phase Indexer API
 *
 * Phase 1 - SCAN: Fast directory walk, build work queue of changed albums
 * Phase 2 - METADATA: Parallel metadata extraction (FFmpeg)
 * Phase 3 - ARTWORK: Parallel image processing (atlas)
 * Phase 4 - FINALIZE: Batch DB updates (mtimes, error flags, checkpoint)
 *
 * Delta detection via albums.last_updated_at (no separate dir_mtime table).
 */

// Forward declarations
struct quadrature_db;
typedef struct quadrature_db quadrature_db_t;
typedef struct indexer indexer_t;

// =============================================================================
// Progress Information
// =============================================================================

/**
 * Indexing phases for progress display.
 */
typedef enum {
    INDEXER_PHASE_SCANNING,      // Fast directory walk, build work queue
    INDEXER_PHASE_METADATA,      // Parallel metadata extraction
    INDEXER_PHASE_ARTWORK,       // Parallel artwork processing
    INDEXER_PHASE_FINALIZE,      // Batch DB updates (mtimes, errors)
    INDEXER_PHASE_COMPLETE       // Done
} indexer_phase_t;

typedef struct {
    size_t files_total;
    size_t files_processed;
    size_t files_new;
    size_t files_unchanged;
    size_t files_deleted;
    size_t dirs_scanned;
    size_t error_count;       // Errors logged during this scan
    double progress;          // 0.0 to 1.0
    const char* current_path; // Currently processing (read-only, do not free)

    // Phase tracking
    indexer_phase_t phase;

    // Artwork progress
    size_t albums_total;         // Albums needing artwork
    size_t albums_processed;     // Albums with artwork done

    // Timing (set by indexer, used by UI for rate/ETA)
    int64_t phase_start_time;    // g_get_monotonic_time() when phase started
} indexer_progress_t;

// =============================================================================
// Event Types
// =============================================================================

typedef enum {
    INDEXER_STARTED,
    INDEXER_PROGRESS,
    INDEXER_COMPLETED,
    INDEXER_CANCELLED,
    INDEXER_ERROR
} indexer_event_t;

// =============================================================================
// Callback
// =============================================================================

typedef void (*indexer_callback_t)(indexer_event_t event,
                                   const indexer_progress_t* progress,
                                   void* user_data);

// =============================================================================
// Configuration
// =============================================================================

typedef struct {
    int thread_count;         // 0 = auto (num_cpus)
    bool process_artwork;
    int art_size;             // Thumbnail size, default 300
    const char* art_dir;      // NULL = auto (~/.local/share/quadrature/art)
    indexer_callback_t callback;
    void* user_data;
} indexer_config_t;

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t indexer_create(indexer_t** out, const indexer_config_t* config);
void indexer_destroy(indexer_t* indexer);

// =============================================================================
// Operations
// =============================================================================

/**
 * Start scanning paths. Non-blocking - returns immediately.
 *
 * @param indexer Indexer instance
 * @param db Database handle
 * @param paths Array of paths to scan
 * @param path_count Number of paths
 * @return QUADRATURE_OK on success
 */
quadrature_result_t indexer_scan(indexer_t* indexer,
                                 quadrature_db_t* db,
                                 const char** paths,
                                 size_t path_count);

/**
 * Cancel current scan.
 *
 * @param indexer Indexer instance
 */
void indexer_cancel(indexer_t* indexer);

/**
 * Check if indexer is running.
 *
 * @param indexer Indexer instance
 * @return true if running
 */
bool indexer_is_running(const indexer_t* indexer);

/**
 * Wait for current scan to complete.
 *
 * @param indexer Indexer instance
 */
void indexer_wait(indexer_t* indexer);

/**
 * Get current progress (thread-safe copy).
 *
 * @param indexer Indexer instance
 * @param progress Output progress struct
 */
void indexer_get_progress(indexer_t* indexer, indexer_progress_t* progress);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_INDEXER_H
