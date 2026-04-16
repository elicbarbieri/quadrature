/**
 * Quadrature Database API
 *
 * SQLite-based persistence layer for the music library.
 * Provides:
 * - Track, artist, album storage and retrieval
 * - Multi-artist support via track_artists junction table
 * - Paginated queries for lazy loading
 * - Search with typed results
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
 * Domain Types
 * ============================================================================= */

/* Track info returned from queries (caller must free with db_track_free) */
typedef struct {
    int64_t id;
    char* title;
    char* artist;           /* Primary artist name */
    char* artist_display;   /* Formatted: "Artist A feat. Artist B" */
    char* album;
    char* path;             /* Track filename / relative path within album dir */
    char* album_path;       /* Album directory path (relative to library root) */
    char* album_musicbrainz_release_id; /* Album MBID (from JOIN); NULL if unresolved */
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
    char* musicbrainz_id;  /* NULL if not resolved; used for cross-library merging */
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
    char* path;            /* Album directory path (relative to library root) */
    char* musicbrainz_release_id;       /* MusicBrainz release MBID, or NULL */
    char* musicbrainz_release_group_id; /* MusicBrainz release group MBID, or NULL */
} db_album_t;

/* Track artist credit (from track_artists junction table) */
typedef struct {
    int64_t artist_id;
    char* name;          /* Canonical artist name */
    char* join_phrase;   /* Connector to next artist: " feat. ", " & ", "" for last */
    int position;        /* Display order; 0 = primary artist */
} db_track_artist_t;

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

/**
 * Get a single artist by ID with album/track counts and MusicBrainz ID.
 * Caller must free with db_artists_free(*out, 1). *out is NULL if not found.
 */
quadrature_result_t db_get_artist_by_id(quadrature_db_t* db, int64_t artist_id,
                                         db_artist_t** out);

/**
 * Get a single album by ID.
 * Caller must free with db_albums_free(*out, 1). *out is NULL if not found.
 */
quadrature_result_t db_get_album_by_id(quadrature_db_t* db, int64_t album_id,
                                        db_album_t** out);

quadrature_result_t db_get_albums_by_artist(quadrature_db_t* db, int64_t artist_id, db_album_t** out, size_t* count);
quadrature_result_t db_get_tracks_by_album(quadrature_db_t* db, int64_t album_id, db_track_t** out, size_t* count);

/* -----------------------------------------------------------------------------
 * Warming Iterators — no JOINs, no aggregates, maximum throughput.
 * Entities loaded in earlier phases; derived fields (counts, genres) computed
 * in C during Phase 4. Callbacks receive borrowed pointers valid only during
 * the callback. Return false to stop iteration (e.g. cancellation).
 * ----------------------------------------------------------------------------- */

/** Stream all artists that have at least one track. */
typedef bool (*db_artist_iter_cb)(const db_artist_t *artist, void *user_data);
quadrature_result_t db_iter_all_artists(quadrature_db_t *db,
                                         db_artist_iter_cb cb, void *user_data);

/** Stream all albums ordered by rowid. */
typedef bool (*db_album_iter_cb)(const db_album_t *album, void *user_data);
quadrature_result_t db_iter_all_albums(quadrature_db_t *db,
                                        db_album_iter_cb cb, void *user_data);

/** Track row from the tracks table only (no JOINs). */
typedef struct {
    int64_t id;
    int64_t album_id;
    const char *title;
    const char *path;
    const char *genre;
    const char *artist_display;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
} db_track_lean_t;

/** Stream all tracks ordered by (album_id, disc_num, track_num). */
typedef bool (*db_track_iter_cb)(const db_track_lean_t *track, void *user_data);
quadrature_result_t db_iter_all_tracks(quadrature_db_t *db,
                                        db_track_iter_cb cb, void *user_data);

/** Stream all track-artist associations ordered by (track_id, position). */
typedef bool (*db_track_artist_iter_cb)(int64_t track_id, int64_t artist_id,
                                        const char *join_phrase, int position,
                                        void *user_data);
quadrature_result_t db_iter_all_track_artists(quadrature_db_t *db,
                                               db_track_artist_iter_cb cb,
                                               void *user_data);

/** Fetch max IDs for all three entity tables in a single query. */
quadrature_result_t db_get_max_ids(quadrature_db_t *db,
                                    int64_t *max_artist, int64_t *max_album,
                                    int64_t *max_track);

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
 * Returns only entity IDs -- caller resolves against in-memory cache.
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
 * Transaction Operations (for indexer)
 * ============================================================================= */

quadrature_result_t db_begin_transaction(quadrature_db_t* db);
quadrature_result_t db_commit(quadrature_db_t* db);
quadrature_result_t db_rollback(quadrature_db_t* db);

/**
 * Batch transaction: wraps an entire indexer phase in a single transaction.
 * db_begin_batch() starts the transaction and releases the lock so workers
 * can acquire it. Worker calls to db_begin_transaction/db_commit become
 * no-ops (just lock/unlock for serialization). db_commit_batch() does the
 * single COMMIT + fsync.
 */
quadrature_result_t db_begin_batch(quadrature_db_t* db);
quadrature_result_t db_commit_batch(quadrature_db_t* db);

/* =============================================================================
 * Write Operations (for indexer)
 * ============================================================================= */

quadrature_result_t db_upsert_track_with_album(quadrature_db_t* db, const db_index_item_t* item,
                                                int64_t album_id, int64_t* track_id_out);
/* Get or create artist by name, returns artist_id (0 on error) */
int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name);

/**
 * Iterate all artist (id, name) pairs — used by indexer to pre-load name cache.
 */
typedef void (*db_artist_name_iter_cb)(int64_t id, const char *name, void *user_data);
quadrature_result_t db_iter_artist_names(quadrature_db_t *db, db_artist_name_iter_cb cb, void *user_data);

/* =============================================================================
 * MusicBrainz Resolution Operations
 * ============================================================================= */

/* MB resolution status values */
#define MB_STATUS_NOT_ATTEMPTED  0   /* No MB work done; no release ID in DB */
#define MB_STATUS_HAS_RELEASE_ID 1   /* Release UUID found in Picard tags (Phase 2); not yet fetched from MB PG */
#define MB_STATUS_RESOLVED       2   /* Fully resolved: MB PG data fetched and written to SQLite */
#define MB_STATUS_NO_MATCH       3   /* Resolution attempted but no MB match found */
#define MB_STATUS_FAILED         4   /* Resolution attempted but errored */

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
 * Only stores fields needed for indexing/sorting: title, release IDs, year, status.
 * Display-only metadata (label, catalog, barcode, type, genres) goes to metadata DB.
 */
quadrature_result_t db_update_album_mb(quadrature_db_t* db, int64_t album_id,
    const char* title,
    const char* musicbrainz_release_id,
    const char* musicbrainz_release_group_id,
    uint16_t year,
    int mb_status);

/**
 * Update track title from MusicBrainz recording (called by Phase 4).
 * Track is identified by (disc_num, track_num) within the resolved album.
 */
quadrature_result_t db_update_track_title(quadrature_db_t* db, int64_t track_id,
    const char* title);

/**
 * Merge MusicBrainz genres into a track's existing genre field.
 * Splits both current genre and mb_genres on ';', deduplicates,
 * and writes the merged semicolon-separated string back to tracks.genre.
 */
quadrature_result_t db_merge_track_genres(quadrature_db_t* db,
    int64_t track_id, const char* mb_genres);

/**
 * Bulk-sync tracks_fts for all tracks in an album.
 * Executes one INSERT OR REPLACE INTO tracks_fts SELECT ... WHERE album_id=?
 * Call once per album after all tracks and artist_display values are committed.
 * Requires an active transaction.
 */
quadrature_result_t db_sync_album_fts(quadrature_db_t* db, int64_t album_id);

/**
 * Store MUSICBRAINZ_ALBUMID read from file tags during Phase 2.
 * Sets mb_status = MB_STATUS_HAS_RELEASE_ID so the resolver knows a release ID is ready.
 * Only updates if mb_status = MB_STATUS_NOT_ATTEMPTED so Phase 4 data is never clobbered.
 */
quadrature_result_t db_set_album_release_id_from_tags(quadrature_db_t* db,
    int64_t album_id, const char* musicbrainz_release_id);

/**
 * Store MUSICBRAINZ_RELEASEGROUPID read from file tags during Phase 2.
 * Only writes if the album's release_group_id is currently NULL/empty and
 * mb_status != RESOLVED (never overwrite Phase 6 data).
 */
quadrature_result_t db_set_album_release_group_id_from_tags(quadrature_db_t* db,
    int64_t album_id, const char* musicbrainz_release_group_id);

/**
 * Get musicbrainz_release_id for an album (stored by Phase 2 from file tags).
 * Returns NULL if not set. Caller must g_free().
 */
char* db_get_album_musicbrainz_release_id(quadrature_db_t* db, int64_t album_id);

/**
 * Get album IDs eligible for MB resolution.
 * Includes: NOT_ATTEMPTED (0), HAS_RELEASE_ID (1), FAILED (4),
 * and NO_MATCH (3) albums older than retry_no_match_before timestamp.
 * Pass 0 for retry_no_match_before to skip NO_MATCH retry.
 * Caller must g_free(*album_ids).
 */
quadrature_result_t db_get_unresolved_albums(quadrature_db_t* db,
    int64_t retry_no_match_before,
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
    int64_t artist_id, bool is_compilation);

/** Begin/end a deferred read transaction (snapshot isolation for bulk reads). */
quadrature_result_t db_begin_read(quadrature_db_t *db);
quadrature_result_t db_end_read(quadrature_db_t *db);

/* WAL checkpoint for durability */
quadrature_result_t db_checkpoint(quadrature_db_t* db);

/**
 * Delete artists that have no entries in track_artists.
 * Handles the 1-Phase2-artist-to-many-MB-credits split case: when Phase 2
 * wrote a combined tag ("Artist A & Artist B") as one artist and Phase 4
 * resolved it to two separate artists, the combined row is correctly orphaned.
 * Also cleans artists_fts for deleted rows.
 * Called from the indexer finalize phase.
 */
quadrature_result_t db_prune_orphan_artists(quadrature_db_t* db);

/**
 * Reconcile the tracks of a single album against the current filesystem.
 *
 * Deletes every track row for `album_id` whose path is NOT present in
 * `current_paths`. If that leaves the album with zero surviving tracks,
 * the album row itself is deleted — so this function covers both the
 * "some files removed/renamed" case (Phase 2 per-album write) and the
 * "whole album directory gone" case (Phase 1 orphan sweep).
 *
 * To delete an entire album, pass `current_paths = NULL, count = 0`:
 * every track matches "not in current" and the album becomes empty,
 * triggering deletion of the album row.
 *
 * `track_artists` rows cascade via FK; `tracks_fts` and `albums_fts`
 * orphan rows are cleaned up automatically.
 *
 * PRECONDITION: must be called within an active transaction (a batch
 * opened by `db_begin_batch` or an explicit `db_begin_transaction`).
 *
 * Paths must match exactly what `db_upsert_track_with_album` stored (the
 * album-relative path).
 */
quadrature_result_t db_reconcile_album_tracks(quadrature_db_t* db,
    int64_t album_id, const char* const* current_paths, size_t current_path_count);

/* =============================================================================
 * Folder-Based Album Operations
 * ============================================================================= */

quadrature_result_t db_upsert_folder_album(quadrature_db_t* db,
                                            const char* folder_path,
                                            const char* title,
                                            int64_t artist_id,
                                            uint16_t year,
                                            int64_t* album_id_out);

/* =============================================================================
 * Indexer Error Operations
 * ============================================================================= */

/* Structured error codes for indexer_errors.error_code */
typedef enum {
    INDEXER_ERR_UNKNOWN          = 0,
    /* 1xx — file access */
    INDEXER_ERR_FILE_UNREADABLE  = 100,
    INDEXER_ERR_PERMISSION       = 101,
    /* 2xx — FFmpeg / metadata */
    INDEXER_ERR_FFMPEG_DECODE    = 200,
    INDEXER_ERR_FFMPEG_NO_STREAM = 201,
    INDEXER_ERR_TRACK_NUMBERING  = 202,
    /* 3xx — artwork */
    INDEXER_ERR_ARTWORK_MISSING  = 300,
    INDEXER_ERR_ARTWORK_CORRUPT  = 301,
    /* 4xx — MusicBrainz */
    INDEXER_ERR_MB_NO_MATCH      = 400,
    INDEXER_ERR_MB_PG_ERROR      = 401,
    /* 5xx — network (phases 7-8) */
    INDEXER_ERR_NETWORK          = 500,
} indexer_error_code_t;

/* Error severity for indexer_errors.severity */
typedef enum {
    INDEXER_SEV_WARN  = 1,
    INDEXER_SEV_ERROR = 2,
    INDEXER_SEV_FATAL = 3,
} indexer_error_severity_t;

quadrature_result_t db_log_error_ex(quadrature_db_t* db, const char* path,
                                    indexer_error_code_t error_code, int phase,
                                    indexer_error_severity_t severity,
                                    const char* message, int64_t scan_generation);

/* Convenience: logs with error_code=0, phase=0, severity=ERROR (backward compat) */
quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message,
                                int64_t scan_generation);
quadrature_result_t db_clear_errors_for_path(quadrature_db_t* db, const char* path_prefix);

/**
 * Get the next scan generation number (MAX(scan_generation) + 1).
 * Returns 1 for an empty indexer_errors table.
 */
int64_t db_get_next_error_generation(quadrature_db_t* db);

/**
 * Prune orphan errors whose paths don't correspond to any known album.
 * Cleans up errors for directories that have been removed from the library.
 */
quadrature_result_t db_prune_orphan_errors(quadrature_db_t* db, const char* library_root);
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
    int64_t last_updated_at;    /* 0 if never processed */
    int64_t last_updated_size;  /* file count + total bytes; 0 if never computed */
    int mb_status;              /* see MB_STATUS_* constants -- cached from Phase 1 scan */
} db_album_mtime_t;

quadrature_result_t db_get_album_mtimes_page(quadrature_db_t* db,
                                              size_t offset,
                                              size_t limit,
                                              db_album_mtime_t** out,
                                              size_t* count_out);
quadrature_result_t db_set_album_mtimes_batch(quadrature_db_t* db,
                                               const int64_t* album_ids,
                                               const int64_t* mtimes,
                                               const int64_t* sizes,
                                               size_t count);
void db_free_album_mtimes(db_album_mtime_t* albums, size_t count);

/* =============================================================================
 * Positional & MBID Bridge Queries (for credits navigation)
 * ============================================================================= */

/**
 * Find a track by its positional coordinates within a MusicBrainz release.
 * Used to resolve metadata DB credits to main DB track_ids.
 * Returns QUADRATURE_ERROR_FILE_NOT_FOUND if no matching track exists.
 */
quadrature_result_t db_get_track_by_position(
    quadrature_db_t *db, const char *release_mbid,
    int disc_num, int track_num, int64_t *track_id_out);

/**
 * Batch-resolve an array of positional coordinates to track_ids.
 * Prepares the lookup statement once and reuses it for all entries.
 * track_ids_out must be pre-allocated with `count` elements; unresolved
 * positions are set to 0. Returns QUADRATURE_OK on success (even if some
 * positions didn't resolve).
 */
typedef struct {
    const char *release_mbid;
    int disc_num;
    int track_num;
} db_track_position_t;

quadrature_result_t db_resolve_track_positions_batch(
    quadrature_db_t *db,
    const db_track_position_t *positions, size_t count,
    int64_t *track_ids_out);

/**
 * Find a main DB artist by MusicBrainz ID (bidirectional bridge).
 * Returns QUADRATURE_ERROR_FILE_NOT_FOUND if no artist has this MBID.
 */
quadrature_result_t db_get_artist_by_mbid(
    quadrature_db_t *db, const char *musicbrainz_id, int64_t *artist_id_out);

/* =============================================================================
 * Artist Art Queries
 * ============================================================================= */

/**
 * Get all artists that have a MusicBrainz ID assigned.
 * Used by the artist art phase to fetch images from fanart.tv.
 * Caller must g_free(*artist_ids) and g_strfreev(*mbids).
 */
quadrature_result_t db_get_artists_with_mbid(quadrature_db_t* db,
    int64_t** artist_ids, char*** mbids, size_t* count);

/**
 * Get all albums that have a MusicBrainz release group ID and whose artist
 * also has a MusicBrainz ID. Returns parallel arrays of album IDs, release
 * group IDs, and artist MBIDs. Used by the artist art phase to find albums
 * eligible for fanart.tv cover art.
 * Caller must g_free(*album_ids), g_strfreev(*release_group_ids),
 * and g_strfreev(*artist_mbids).
 */
quadrature_result_t db_get_albums_with_release_group_id(quadrature_db_t* db,
    int64_t** album_ids, char*** release_group_ids, char*** artist_mbids,
    size_t* count);

/* =============================================================================
 * Aggregate Queries
 * ============================================================================= */

/**
 * Get total number of tracks in the database.
 *
 * @param db Database handle
 * @param count Output count
 * @return QUADRATURE_OK on success
 */
quadrature_result_t db_get_total_track_count(quadrature_db_t* db, size_t* count);
quadrature_result_t db_get_total_album_count(quadrature_db_t* db, size_t* count);
quadrature_result_t db_get_total_artist_count(quadrature_db_t* db, size_t* count);

/**
 * Unix timestamp (seconds) of the most recently indexed album (MAX last_updated_at).
 * Returns 0 if the library has never been indexed.
 */
quadrature_result_t db_get_last_indexed_time(quadrature_db_t* db, int64_t* unix_time);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_DATABASE_H */
