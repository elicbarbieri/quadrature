#ifndef INDEXER_CONTROLLER_H
#define INDEXER_CONTROLLER_H

#include <gtk/gtk.h>
#include "quadrature/indexer/indexer.h"

G_BEGIN_DECLS

/**
 * IndexerController
 *
 * GTK wrapper for the simplified indexer providing:
 * - GObject properties for binding (progress, running, status)
 * - GSignals for events (started, progress, completed)
 * - Easy integration with GTK widgets
 *
 * Uses the new simplified indexer API with two-level delta detection.
 */

#define INDEXER_TYPE_CONTROLLER (indexer_controller_get_type())
G_DECLARE_FINAL_TYPE(IndexerController, indexer_controller, INDEXER, CONTROLLER, GObject)

// Forward declaration
struct quadrature_db;
typedef struct quadrature_db quadrature_db_t;

/**
 * Create a new indexer controller.
 *
 * @param db Database handle
 * @return New controller instance
 */
IndexerController* indexer_controller_new(quadrature_db_t* db);

/**
 * Set number of worker threads.
 *
 * @param self Controller instance
 * @param thread_count Thread count (0 = auto)
 */
void indexer_controller_set_thread_count(IndexerController* self, int thread_count);

/**
 * Enable/disable artwork processing.
 *
 * @param self Controller instance
 * @param enable True to enable
 */
void indexer_controller_set_process_artwork(IndexerController* self, gboolean enable);

/**
 * Set art thumbnail size.
 *
 * @param self Controller instance
 * @param size Size in pixels
 */
void indexer_controller_set_art_size(IndexerController* self, int size);

/**
 * Start indexing specified paths.
 *
 * @param self Controller instance
 * @param paths Array of paths
 * @param path_count Number of paths
 * @return TRUE on success
 */
gboolean indexer_controller_start(IndexerController* self,
                                   const char** paths, gsize path_count);

/**
 * Start indexing all configured watch paths.
 *
 * @param self Controller instance
 * @return TRUE on success
 */
gboolean indexer_controller_start_all(IndexerController* self);

/**
 * Add a watch path and start indexing it.
 *
 * @param self Controller instance
 * @param path Path to add
 * @return TRUE on success
 */
gboolean indexer_controller_add_path(IndexerController* self, const char* path);

/**
 * Cancel indexing.
 *
 * @param self Controller instance
 */
void indexer_controller_cancel(IndexerController* self);

/**
 * Check if indexing is in progress.
 *
 * @param self Controller instance
 * @return TRUE if running
 */
gboolean indexer_controller_is_running(IndexerController* self);

/**
 * Get progress percentage (0.0 - 100.0).
 *
 * @param self Controller instance
 * @return Progress percentage
 */
double indexer_controller_get_progress(IndexerController* self);

/**
 * Get current item being processed.
 *
 * @param self Controller instance
 * @return Current item path or NULL
 */
const char* indexer_controller_get_current_item(IndexerController* self);

/**
 * Get full progress information.
 *
 * @param self Controller instance
 * @param progress Output progress struct
 */
void indexer_controller_get_progress_info(IndexerController* self,
                                           indexer_progress_t* progress);

/**
 * Get status string describing current state.
 *
 * @param self Controller instance
 * @return Status string (e.g., "Scanning", "Idle", "Complete")
 */
const char* indexer_controller_get_status(IndexerController* self);

G_END_DECLS

#endif // INDEXER_CONTROLLER_H
