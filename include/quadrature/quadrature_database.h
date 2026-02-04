/**
 * Quadrature Database API
 *
 * SQLite-based persistence layer for the music library.
 * Provides:
 * - Track, artist, album storage and retrieval
 * - Multi-artist support via track_artists junction table
 * - Paginated queries for lazy loading
 * - Search with typed results
 * - Watch path management
 * - Indexer support (batch writes, delta detection)
 */

#ifndef QUADRATURE_DATABASE_H
#define QUADRATURE_DATABASE_H

#include "quadrature.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Opaque Database Handle
 * ============================================================================= */

typedef struct quadrature_db quadrature_db_t;

/* =============================================================================
 * Artist Role (for track_artists junction table)
 * ============================================================================= */

typedef enum {
    ARTIST_ROLE_PRIMARY = 0,     /* Track artist (from ARTIST/TPE1 tag) */
    ARTIST_ROLE_FEATURING = 1,   /* Featured/guest artist (parsed from "feat." pattern) */
} db_artist_role_t;

/* =============================================================================
 * Domain Types
 * ============================================================================= */

/* Track info returned from queries (caller must free with db_track_free) */
typedef struct {
    int64_t id;
    char* title;
    char* artist;           /* Primary artist name */
    char* artist_display;   /* Formatted: "Artist A feat. Artist B" */
    char* album;
    char* path;
    char* genre;
    int64_t album_id;
    int64_t artist_id;      /* Primary artist ID (position 0) */
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
} db_track_t;

/* Artist info for library browsing (caller must free with db_artists_free) */
typedef struct {
    int64_t id;
    char* name;
    size_t album_count;
    size_t track_count;
} db_artist_t;

/* Album info for library browsing (caller must free with db_albums_free) */
typedef struct {
    int64_t id;
    char* title;
    char* artist_name;
    int64_t artist_id;
    uint16_t year;
    size_t track_count;
    char* genres;          /* Comma-separated distinct genres, or NULL */
} db_album_t;

/* Track artist credit (from track_artists junction table) */
typedef struct {
    int64_t artist_id;
    char* name;
    db_artist_role_t role;
    int position;
} db_track_artist_t;

/* Watch path info */
typedef struct {
    int64_t id;
    char* path;
    bool enabled;
    int64_t last_scanned;
} db_watch_path_t;

/* =============================================================================
 * Indexer Error Types
 * ============================================================================= */

typedef struct {
    int64_t id;
    char* path;
    char* message;
    int64_t created_at;
} db_indexer_error_t;

void db_indexer_error_free(db_indexer_error_t* err);
void db_indexer_errors_free(db_indexer_error_t* errors, size_t count);

/* =============================================================================
 * Pagination Types
 * ============================================================================= */

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

/* Search filter options for genre/year multi-select (NULL = no extra filters) */
typedef struct {
    const char* const* genres; /* Array of genre strings to match (OR logic) */
    size_t genre_count;        /* 0 = no genre filter */
    uint16_t year_mask;        /* Bitmask: bit 0=2020s..bit 7=Pre-1960. 0 = no filter */
} db_search_opts_t;

typedef struct {
    size_t offset;
    size_t limit;
    db_sort_t sort;
    const char* search_text;          /* NULL = no text filter */
    const db_search_opts_t* filters;  /* NULL = no genre/year filter */
} db_page_opts_t;

/* Index item for batch writes (used by indexer) */
typedef struct {
    const char* path;
    const char* title;
    const char* album;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
    int64_t mtime;
    const char* genre;
    const char* metadata_json;    /* All extended metadata as JSON */
} db_index_item_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

quadrature_result_t db_open(const char* path, quadrature_db_t** out);
quadrature_result_t db_open_readonly(const char* path, quadrature_db_t** out);
quadrature_result_t db_open_memory(quadrature_db_t** out);
void db_close(quadrature_db_t* db);
const char* db_path(const quadrature_db_t* db);

/* =============================================================================
 * Read Operations
 * ============================================================================= */

quadrature_result_t db_get_track(quadrature_db_t* db, int64_t id, db_track_t** out);
void db_track_free(db_track_t* track);
void db_tracks_free(db_track_t* tracks, size_t count);

/* =============================================================================
 * Artist/Album Read Operations
 * ============================================================================= */

quadrature_result_t db_get_albums_by_artist(quadrature_db_t* db, int64_t artist_id, db_album_t** out, size_t* count);
quadrature_result_t db_get_tracks_by_album(quadrature_db_t* db, int64_t album_id, db_track_t** out, size_t* count);

/* =============================================================================
 * Track Artist Operations (multi-artist via junction table)
 * ============================================================================= */

/**
 * Set all artist associations for a track (replaces any existing).
 * Called by the indexer after parsing artist names.
 */
quadrature_result_t db_set_track_artists(quadrature_db_t* db, int64_t track_id,
                                          const db_track_artist_t* artists, size_t count);

/**
 * Get all artists for a track, ordered by position.
 * Caller must free with db_track_artists_free().
 */
quadrature_result_t db_get_track_artists(quadrature_db_t* db, int64_t track_id,
                                          db_track_artist_t** out, size_t* count);
void db_track_artists_free(db_track_artist_t* artists, size_t count);

/* =============================================================================
 * "Appears On" Operations (artist featured on other artists' albums)
 * ============================================================================= */

/**
 * Get albums where artist appears on tracks but is NOT the album artist.
 * Queries through track_artists junction table.
 */
quadrature_result_t db_get_artist_appearances(quadrature_db_t* db, int64_t artist_id,
                                               db_album_t** out, size_t* count);

/**
 * Get tracks where artist appears on albums by other artists.
 * Sorted by album title, disc number, track number.
 */
quadrature_result_t db_get_artist_appearance_tracks(quadrature_db_t* db, int64_t artist_id,
                                                     db_track_t** out, size_t* count);
void db_artists_free(db_artist_t* artists, size_t count);
void db_albums_free(db_album_t* albums, size_t count);

/* =============================================================================
 * ID-Only Filtered Queries (for cache-resolved filtering)
 * ============================================================================= */

/**
 * Options for ID-only filtered queries.
 * Returns only entity IDs — caller resolves against in-memory cache.
 */
typedef struct {
    const char* search_text;           /* NULL = no text filter */
    const db_search_opts_t* filters;   /* NULL = no genre/year filter */
    db_sort_t sort;
} db_id_query_opts_t;

/**
 * Get filtered artist IDs. Caller must g_free(*out_ids).
 */
quadrature_result_t db_get_artist_ids_filtered(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count);

/**
 * Get filtered album IDs. Caller must g_free(*out_ids).
 */
quadrature_result_t db_get_album_ids_filtered(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count);

/**
 * Search track IDs via FTS. Caller must g_free(*out_ids).
 */
quadrature_result_t db_search_track_ids(quadrature_db_t* db,
    const char* query, const db_search_opts_t* opts, size_t limit,
    int64_t** out_ids, size_t* out_count);

/**
 * Get MAX(id) for a table. Returns 0 if table is empty.
 */
int64_t db_get_max_id(quadrature_db_t* db, const char* table_name);

/* =============================================================================
 * Paginated Queries (for lazy loading)
 * ============================================================================= */

quadrature_result_t db_get_artists_page(quadrature_db_t* db,
                                         const db_page_opts_t* opts,
                                         db_artist_t** out,
                                         size_t* out_count,
                                         size_t* total_count);

quadrature_result_t db_get_albums_page(quadrature_db_t* db,
                                        const db_page_opts_t* opts,
                                        db_album_t** out,
                                        size_t* out_count,
                                        size_t* total_count);

/* =============================================================================
 * Watch Path Operations
 * ============================================================================= */

quadrature_result_t db_add_watch_path(quadrature_db_t* db, const char* path);
quadrature_result_t db_remove_watch_path(quadrature_db_t* db, const char* path);
quadrature_result_t db_get_watch_paths(quadrature_db_t* db, db_watch_path_t** out, size_t* count);
void db_free_watch_paths(db_watch_path_t* paths, size_t count);
quadrature_result_t db_get_track_count_for_path(quadrature_db_t* db, const char* path, size_t* count);
quadrature_result_t db_set_watch_path_enabled(quadrature_db_t* db, const char* path, bool enabled);
quadrature_result_t db_update_watch_path_scanned(quadrature_db_t* db, const char* path, int64_t timestamp);

/* =============================================================================
 * Transaction Operations (for indexer)
 * ============================================================================= */

quadrature_result_t db_begin_transaction(quadrature_db_t* db);
quadrature_result_t db_commit(quadrature_db_t* db);
quadrature_result_t db_rollback(quadrature_db_t* db);

/* =============================================================================
 * Track State (for delta detection)
 * ============================================================================= */

quadrature_result_t db_get_track_mtime(quadrature_db_t* db, const char* path,
                                        int64_t* mtime);

/* =============================================================================
 * Write Operations (for indexer)
 * ============================================================================= */

quadrature_result_t db_upsert_track_with_album(quadrature_db_t* db, const db_index_item_t* item,
                                                int64_t album_id, int64_t* track_id_out);
quadrature_result_t db_delete_track(quadrature_db_t* db, const char* path);

/* Get or create artist by name, returns artist_id (0 on error) */
int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name);

/* =============================================================================
 * Fingerprint Cache Operations (for indexer + resolver)
 * ============================================================================= */

/**
 * Store a chromaprint fingerprint for a track.
 */
quadrature_result_t db_set_track_fingerprint(quadrature_db_t* db, int64_t track_id,
                                              const char* chromaprint, int duration);

/**
 * Read cached chromaprint fingerprint for a track.
 * Caller must g_free(*chromaprint_out).
 * Returns QUADRATURE_ERROR_FILE_NOT_FOUND if no fingerprint cached.
 */
quadrature_result_t db_get_track_fingerprint(quadrature_db_t* db, int64_t track_id,
                                              char** chromaprint_out, int* duration_out);

/* =============================================================================
 * MusicBrainz Resolution Operations
 * ============================================================================= */

/* MB resolution status values */
#define MB_STATUS_NOT_ATTEMPTED  0
#define MB_STATUS_RESOLVED       1
#define MB_STATUS_NO_MATCH       2
#define MB_STATUS_FAILED         3

/**
 * Get or create artist with MusicBrainz data.
 * Deduplicates by musicbrainz_id first, then by name.
 * Returns artist_id (0 on error).
 */
int64_t db_get_or_create_artist_mb(quadrature_db_t* db,
                                    const char* name,
                                    const char* sort_name,
                                    const char* musicbrainz_id);

/**
 * Update album with MusicBrainz release metadata.
 */
quadrature_result_t db_update_album_mb(quadrature_db_t* db, int64_t album_id,
    const char* musicbrainz_release_id,
    const char* musicbrainz_release_group_id,
    const char* release_type,
    const char* label,
    const char* barcode,
    uint16_t year,
    int mb_status);

/**
 * Update track with MusicBrainz recording metadata.
 */
quadrature_result_t db_update_track_mb(quadrature_db_t* db, int64_t track_id,
    const char* musicbrainz_recording_id,
    const char* title);

/**
 * Get album IDs with mb_status = MB_STATUS_NOT_ATTEMPTED.
 * Caller must g_free(*album_ids).
 */
quadrature_result_t db_get_unresolved_albums(quadrature_db_t* db,
    int64_t** album_ids, size_t* count);

/**
 * Set album MB resolution status.
 */
quadrature_result_t db_set_album_mb_status(quadrature_db_t* db,
    int64_t album_id, int status, int64_t resolved_at);

/**
 * Update album's artist and compilation fields.
 */
quadrature_result_t db_update_album_artist(quadrature_db_t* db, int64_t album_id,
    int64_t artist_id, int64_t album_artist_id, bool is_compilation);

/* WAL checkpoint for durability */
quadrature_result_t db_checkpoint(quadrature_db_t* db);

/* =============================================================================
 * Folder-Based Album Operations
 * ============================================================================= */

quadrature_result_t db_upsert_folder_album(quadrature_db_t* db,
                                            const char* folder_path,
                                            const char* title,
                                            int64_t artist_id,
                                            int64_t album_artist_id,
                                            bool is_compilation,
                                            uint16_t year,
                                            int64_t* album_id_out);

int64_t db_get_track_id_by_path(quadrature_db_t* db, const char* path);

/* =============================================================================
 * Indexer Error Operations
 * ============================================================================= */

quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message);
quadrature_result_t db_clear_errors_for_path(quadrature_db_t* db, const char* path_prefix);
quadrature_result_t db_get_error_count(quadrature_db_t* db, const char* path_prefix, size_t* count);
quadrature_result_t db_get_errors_page(quadrature_db_t* db, const char* path_prefix,
                                       size_t offset, size_t limit,
                                       db_indexer_error_t** out, size_t* count);

/* =============================================================================
 * Album Mtime Operations (for indexer delta detection and artwork cache)
 * ============================================================================= */

typedef struct {
    int64_t album_id;
    char* path;
    int64_t last_updated_at;  /* 0 if never processed */
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

#endif /* QUADRATURE_DATABASE_H */
