/**
 * Quadrature Database Write API
 *
 * Write-only operations: artist creation, MusicBrainz resolution writes,
 * indexer error logging, album mtime batch writes, orphan pruning, WAL
 * checkpoint.
 *
 * Reads live in <quadrature/database.h>. The reconciler is the sole writer
 * for album/track state; see <quadrature/indexer.h> for
 * `db_reconcile_album` / `db_delete_album` / `db_create_or_get_album_by_path`.
 */

#ifndef QUADRATURE_DB_WRITE_H
#define QUADRATURE_DB_WRITE_H

#include "database.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Artist Writes
 * ============================================================================= */

/** Get or create artist by name. Returns artist_id (0 on error). */
int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name);

/**
 * Get or create artist with MusicBrainz data.
 * Deduplicates by musicbrainz_id first, then by name. Returns 0 on error.
 */
int64_t db_get_or_create_artist_mb(quadrature_db_t* db,
                                    const char* name,
                                    const char* sort_name,
                                    const char* musicbrainz_id);

/**
 * Delete artists with no entries in track_artists. Handles the
 * 1-Phase2-artist-to-many-MB-credits split case. Called from finalize.
 */
quadrature_result_t db_prune_orphan_artists(quadrature_db_t* db);

/* =============================================================================
 * MusicBrainz Resolution Writes
 * ============================================================================= */

quadrature_result_t db_set_album_mb_status(quadrature_db_t* db,
    int64_t album_id, int status, int64_t resolved_at);

/* =============================================================================
 * Indexer Error Logging
 * ============================================================================= */

quadrature_result_t db_log_error_ex(quadrature_db_t* db, const char* path,
                                    indexer_error_code_t error_code, int phase,
                                    indexer_error_severity_t severity,
                                    const char* message, int64_t scan_generation);

/** Convenience: logs with error_code=0, phase=0, severity=ERROR. */
quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message,
                                int64_t scan_generation);

quadrature_result_t db_clear_errors_for_path(quadrature_db_t* db, const char* path_prefix);

/**
 * Prune orphan errors whose paths don't correspond to any known album.
 */
quadrature_result_t db_prune_orphan_errors(quadrature_db_t* db, const char* library_root);

/* =============================================================================
 * Album Mtime Batch Writes (indexer delta detection)
 * ============================================================================= */

quadrature_result_t db_set_album_mtimes_batch(quadrature_db_t* db,
                                               const int64_t* album_ids,
                                               const int64_t* mtimes,
                                               const int64_t* sizes,
                                               size_t count);

/* =============================================================================
 * WAL Checkpoint
 * ============================================================================= */

quadrature_result_t db_checkpoint(quadrature_db_t* db);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_DB_WRITE_H */
