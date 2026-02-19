#ifndef QUADRATURE_DB_INTERNAL_H
#define QUADRATURE_DB_INTERNAL_H

/**
 * Internal header for database implementation.
 * Contains the quadrature_db struct definition and shared declarations.
 */

#include "quadrature/database.h"
#include <sqlite3.h>
#include <pthread.h>
#include <stdatomic.h>

// =============================================================================
// Database Handle (opaque type definition)
// =============================================================================

struct quadrature_db {
    sqlite3* db;
    char* db_path;
    pthread_mutex_t lock;
    atomic_int cancel_flag;

    // Prepared statements for writes
    sqlite3_stmt* insert_artist;
    sqlite3_stmt* select_artist;
    sqlite3_stmt* upsert_track;
    sqlite3_stmt* insert_artist_fts;  // artists_fts: (rowid, name)
    sqlite3_stmt* update_album_fts;   // albums_fts by album_id (joins for artist name)
    sqlite3_stmt* insert_track_artist;
    sqlite3_stmt* delete_track_artists;

    // Cached statements for indexer hot paths
    sqlite3_stmt* select_track_by_path;
    sqlite3_stmt* select_album_by_path;
    sqlite3_stmt* update_album_by_id;
    sqlite3_stmt* insert_folder_album;
    sqlite3_stmt* update_track_artist_display; /* UPDATE tracks SET artist_display=? WHERE id=? */
    sqlite3_stmt* set_album_release_id;        /* UPDATE albums SET mb_release_id=?,mb_status=? WHERE id=? AND mb_status=? */
    sqlite3_stmt* sync_album_tracks_fts;       /* Bulk INSERT OR REPLACE INTO tracks_fts for all tracks in an album */
    /* MB artist statements -- used in db_get_or_create_artist_mb() */
    sqlite3_stmt* select_artist_by_mb_id;
    sqlite3_stmt* update_artist_sort_name;
    /* Step 2: exact name match, only for rows with NULL or same MBID */
    sqlite3_stmt* select_artist_by_name_nocase;
    sqlite3_stmt* update_artist_mb_data;
    sqlite3_stmt* insert_artist_mb;
    sqlite3_stmt* insert_artist_fts_replace;
    /* Step 3: normalized name match (strips spaces + hyphens, lowercased) */
    sqlite3_stmt* select_artist_normalized_no_mbid;
    /* Rename in-place: UPDATE artists SET name=?, musicbrainz_id=?, sort_name=? WHERE id=? */
    sqlite3_stmt* rename_artist_mb;
    /* Merge helpers: move track_artists from one artist to another */
    sqlite3_stmt* move_track_artists;          /* UPDATE OR IGNORE track_artists SET artist_id=? WHERE artist_id=? */
    sqlite3_stmt* delete_track_artists_artist_id; /* DELETE FROM track_artists WHERE artist_id=? */

    /* MB resolver hot-path statements (avoid prepare/finalize per call) */
    sqlite3_stmt* update_track_title;          /* UPDATE tracks SET title=? WHERE id=? */
    sqlite3_stmt* update_track_genre;          /* UPDATE tracks SET genre=? WHERE id=? */
    sqlite3_stmt* set_album_mb_status;         /* UPDATE albums SET mb_status=?,mb_resolved_at=? WHERE id=? */
    sqlite3_stmt* update_album_artist;         /* UPDATE albums SET artist_id=?,is_compilation=? WHERE id=? */
    sqlite3_stmt* update_album_mb;             /* UPDATE albums SET title=?,mb_release_id=?,... WHERE id=? */
    sqlite3_stmt* sync_album_fts;              /* INSERT OR REPLACE INTO albums_fts for single album */

    // Transaction state
    bool in_transaction;
    int txn_depth;  // 0 = no txn, 1+ = nested depth (for batch transactions)

    // Cached statements for merge_duplicate_artist
    sqlite3_stmt* select_artist_by_name_and_mbid; /* SELECT id FROM artists WHERE name=? COLLATE NOCASE AND musicbrainz_id=? */
    sqlite3_stmt* delete_artist_fts;              /* DELETE FROM artists_fts WHERE rowid=? */
    sqlite3_stmt* delete_artist;                  /* DELETE FROM artists WHERE id=? */
};

// =============================================================================
// Internal Functions (db.c)
// =============================================================================

// Lock helpers
void db_lock(quadrature_db_t* db);
void db_unlock(quadrature_db_t* db);

// Statement preparation
void db_prepare_stmts(quadrature_db_t* db);
void db_finalize_stmts(quadrature_db_t* db);

// =============================================================================
// Internal Functions (db_write.c)
// =============================================================================

// Get or create artist, returning ID
int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name);

#endif // QUADRATURE_DB_INTERNAL_H
