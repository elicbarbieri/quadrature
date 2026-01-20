#ifndef QUADRATURE_DATABASE_H
#define QUADRATURE_DATABASE_H

#include "quadrature/core/types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Opaque Database Handle
// =============================================================================

typedef struct quadrature_db quadrature_db_t;

// =============================================================================
// Domain Types
// =============================================================================

// Track info returned from queries (caller must free with db_track_free)
typedef struct {
    int64_t id;
    char* title;
    char* artist;
    char* album;
    char* path;
    int64_t album_id;     // For grouping/lookup
    int64_t artist_id;    // Artist ID for navigation
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;    // Disc number (default 1)
    uint16_t year;
} db_track_t;

// Artist info for library browsing (caller must free with db_artists_free)
typedef struct {
    int64_t id;
    char* name;
    size_t album_count;
    size_t track_count;
} db_artist_t;

// Album info for library browsing (caller must free with db_albums_free)
typedef struct {
    int64_t id;
    char* title;
    char* artist_name;
    int64_t artist_id;
    uint16_t year;
    size_t track_count;
} db_album_t;

// Watch path info
typedef struct {
    int64_t id;
    char* path;
    bool enabled;
    int64_t last_scanned;
} db_watch_path_t;

// =============================================================================
// Extended Metadata Types (for metadata popup)
// =============================================================================

typedef struct {
    int64_t track_id;
    char* raw_json;          // All tags as JSON
    int32_t bitrate;         // kbps
    int32_t sample_rate;     // Hz
    int32_t channels;
    char* codec;             // e.g., "FLAC", "MP3", "AAC"
    char* album_artist;
    char* genre;
    char* comment;
    bool compilation;
    int16_t disc_total;
    int16_t track_total;
    bool has_embedded_art;
} db_track_metadata_t;

void db_track_metadata_free(db_track_metadata_t* meta);

// =============================================================================
// Indexer Error Types (simplified path-based)
// =============================================================================

typedef struct {
    int64_t id;
    char* path;
    char* message;
    int64_t created_at;
} db_indexer_error_t;

void db_indexer_error_free(db_indexer_error_t* err);
void db_indexer_errors_free(db_indexer_error_t* errors, size_t count);

// =============================================================================
// Pagination Types
// =============================================================================

typedef enum {
    DB_SORT_NAME_ASC,
    DB_SORT_NAME_DESC,
    DB_SORT_YEAR_ASC,
    DB_SORT_YEAR_DESC,
    DB_SORT_RECENT,
    DB_SORT_ARTIST_ASC,
    DB_SORT_ARTIST_DESC,
    DB_SORT_ADDED_ASC,
    DB_SORT_ADDED_DESC,
    DB_SORT_ALBUM_ASC,
    DB_SORT_ALBUM_DESC,
    DB_SORT_DURATION_ASC,
    DB_SORT_DURATION_DESC,
    DB_SORT_TRACK_NUM_ASC,
    DB_SORT_DISC_NUM_ASC,
} db_sort_t;

// Search filter type
typedef enum {
    DB_SEARCH_ALL,
    DB_SEARCH_ARTISTS,
    DB_SEARCH_ALBUMS,
    DB_SEARCH_TRACKS,
} db_search_type_t;

// Typed search results (caller must free with db_search_results_free)
typedef struct {
    db_artist_t* artists;
    size_t artist_count;
    db_album_t* albums;
    size_t album_count;
    db_track_t* tracks;
    size_t track_count;
} db_search_results_t;

typedef struct {
    size_t offset;
    size_t limit;
    db_sort_t sort;
} db_page_opts_t;

// Index item for batch writes (used by indexer)
typedef struct {
    const char* path;
    const char* title;
    const char* artist;
    const char* album;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;    // Disc number (default 1)
    uint16_t year;
    int64_t mtime;
    int64_t size;
} db_index_item_t;

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t db_open(const char* path, quadrature_db_t** out);
quadrature_result_t db_open_memory(quadrature_db_t** out);
void db_close(quadrature_db_t* db);
const char* db_path(const quadrature_db_t* db);

// =============================================================================
// Read Operations
// =============================================================================

quadrature_result_t db_search_typed(quadrature_db_t* db, const char* query,
                                     db_search_type_t type, size_t limit,
                                     db_search_results_t* out);
void db_search_results_free(db_search_results_t* results);
quadrature_result_t db_get_track(quadrature_db_t* db, int64_t id, db_track_t** out);
quadrature_result_t db_get_track_count(quadrature_db_t* db, size_t* count);
void db_track_free(db_track_t* track);
void db_tracks_free(db_track_t* tracks, size_t count);

// =============================================================================
// Artist/Album Read Operations
// =============================================================================

quadrature_result_t db_get_albums_by_artist(quadrature_db_t* db, int64_t artist_id, db_album_t** out, size_t* count);
quadrature_result_t db_get_tracks_by_album(quadrature_db_t* db, int64_t album_id, db_track_t** out, size_t* count);
quadrature_result_t db_get_artist_count(quadrature_db_t* db, size_t* count);
quadrature_result_t db_get_album_count(quadrature_db_t* db, size_t* count);
void db_artists_free(db_artist_t* artists, size_t count);
void db_albums_free(db_album_t* albums, size_t count);

// =============================================================================
// Paginated Queries (for lazy loading)
// =============================================================================

// Returns page of artists + total count for pagination
quadrature_result_t db_get_artists_page(quadrature_db_t* db,
                                         const db_page_opts_t* opts,
                                         db_artist_t** out,
                                         size_t* out_count,
                                         size_t* total_count);

// Returns page of albums + total count for pagination
quadrature_result_t db_get_albums_page(quadrature_db_t* db,
                                        const db_page_opts_t* opts,
                                        db_album_t** out,
                                        size_t* out_count,
                                        size_t* total_count);

// Returns page of tracks + total count for pagination
quadrature_result_t db_get_tracks_page(quadrature_db_t* db,
                                        const db_page_opts_t* opts,
                                        db_track_t** out,
                                        size_t* out_count,
                                        size_t* total_count);

// =============================================================================
// Watch Path Operations
// =============================================================================

quadrature_result_t db_add_watch_path(quadrature_db_t* db, const char* path);
quadrature_result_t db_remove_watch_path(quadrature_db_t* db, const char* path);
quadrature_result_t db_get_watch_paths(quadrature_db_t* db, db_watch_path_t** out, size_t* count);
void db_free_watch_paths(db_watch_path_t* paths, size_t count);
quadrature_result_t db_get_track_count_for_path(quadrature_db_t* db, const char* path, size_t* count);
quadrature_result_t db_set_watch_path_enabled(quadrature_db_t* db, const char* path, bool enabled);
quadrature_result_t db_update_watch_path_scanned(quadrature_db_t* db, const char* path, int64_t timestamp);

// =============================================================================
// Transaction Operations (for indexer)
// =============================================================================

quadrature_result_t db_begin_transaction(quadrature_db_t* db);
quadrature_result_t db_commit(quadrature_db_t* db);
quadrature_result_t db_rollback(quadrature_db_t* db);

// =============================================================================
// Track State (for delta detection)
// =============================================================================

quadrature_result_t db_get_track_state(quadrature_db_t* db, const char* path,
                                       int64_t* mtime, int64_t* size);
quadrature_result_t db_mark_tracks_seen(quadrature_db_t* db, const char* dir_path, int64_t timestamp);
quadrature_result_t db_delete_unseen_tracks(quadrature_db_t* db, int64_t older_than);

// =============================================================================
// Write Operations (for indexer)
// =============================================================================

quadrature_result_t db_upsert_track(quadrature_db_t* db, const db_index_item_t* item, int64_t scan_time);
quadrature_result_t db_upsert_track_with_album(quadrature_db_t* db, const db_index_item_t* item,
                                                int64_t album_id, int64_t scan_time);
quadrature_result_t db_delete_track(quadrature_db_t* db, const char* path);

// Get or create artist by name, returns artist_id (0 on error)
int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name);

// WAL checkpoint for durability
quadrature_result_t db_checkpoint(quadrature_db_t* db);

// =============================================================================
// Folder-Based Album Operations
// =============================================================================

// Insert/update album using folder path as identifier
quadrature_result_t db_upsert_folder_album(quadrature_db_t* db,
                                            const char* folder_path,
                                            const char* title,
                                            int64_t artist_id,
                                            int64_t album_artist_id,
                                            bool is_compilation,
                                            uint16_t year,
                                            int64_t* album_id_out);

// Get track ID by path (returns 0 if not found)
int64_t db_get_track_id_by_path(quadrature_db_t* db, const char* path);

// =============================================================================
// Track Metadata Operations
// =============================================================================

// Store extended track metadata
quadrature_result_t db_insert_track_metadata(quadrature_db_t* db,
                                              int64_t track_id,
                                              const db_track_metadata_t* meta);

// Get extended track metadata
quadrature_result_t db_get_track_metadata(quadrature_db_t* db,
                                           int64_t track_id,
                                           db_track_metadata_t** out);

// =============================================================================
// Indexer Error Operations (simplified path-based)
// =============================================================================

// Log an error for a path
quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message);

// Clear errors for paths matching prefix (call before re-indexing a directory)
quadrature_result_t db_clear_errors_for_path(quadrature_db_t* db, const char* path_prefix);

// Get error count (path_prefix can be NULL for all)
quadrature_result_t db_get_error_count(quadrature_db_t* db, const char* path_prefix, size_t* count);

// Get errors paged (for UI, path_prefix can be NULL for all)
quadrature_result_t db_get_errors_page(quadrature_db_t* db, const char* path_prefix,
                                       size_t offset, size_t limit,
                                       db_indexer_error_t** out, size_t* count);

// =============================================================================
// Album Mtime Operations (paged - for indexer delta detection and artwork cache)
// =============================================================================

typedef struct {
    int64_t album_id;
    char* path;
    int64_t last_updated_at;  // 0 if never processed
} db_album_mtime_t;

quadrature_result_t db_get_album_mtimes_page(quadrature_db_t* db,
                                              size_t offset,
                                              size_t limit,
                                              db_album_mtime_t** out,
                                              size_t* count_out);
quadrature_result_t db_set_album_mtimes_batch(quadrature_db_t* db,
                                               const int64_t* album_ids,
                                               const int64_t* mtimes,
                                               size_t count);
void db_free_album_mtimes(db_album_mtime_t* albums, size_t count);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_DATABASE_H
