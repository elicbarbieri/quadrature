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
    bool readonly;          /* true = skip app-level mutex (WAL handles reader isolation) */
    atomic_int cancel_flag;

    // Prepared statements for writes
    sqlite3_stmt* insert_artist;
    sqlite3_stmt* select_artist;
    sqlite3_stmt* insert_artist_fts;  // artists_fts: (rowid, name)
    sqlite3_stmt* update_album_fts;   // albums_fts by album_id (joins for artist name)
    sqlite3_stmt* insert_track_artist;
    sqlite3_stmt* delete_track_artists;

    // Cached statements used by the reconciler
    sqlite3_stmt* select_album_by_path;
    sqlite3_stmt* insert_folder_album;
    sqlite3_stmt* sync_album_tracks_fts;       /* Bulk INSERT OR REPLACE INTO tracks_fts for all tracks in an album */
    /* MB artist statements -- used in db_get_or_create_artist() */
    sqlite3_stmt* select_artist_by_mb_id;
    sqlite3_stmt* update_artist_sort_name;
    /* Step 2: exact name match, only for rows with NULL or same MBID */
    sqlite3_stmt* select_artist_by_name_nocase;
    sqlite3_stmt* insert_artist_mb;
    sqlite3_stmt* insert_artist_fts_replace;
    /* Step 3: normalized name match (strips spaces + hyphens, lowercased) */
    sqlite3_stmt* select_artist_normalized_no_mbid;
    /* Rename in-place: UPDATE artists SET name=?, musicbrainz_id=?, sort_name=? WHERE id=? */
    sqlite3_stmt* rename_artist_mb;
    /* Merge helpers: move track_artists from one artist to another */
    sqlite3_stmt* move_track_artists;          /* UPDATE OR IGNORE track_artists SET artist_id=? WHERE artist_id=? */
    sqlite3_stmt* delete_track_artists_artist_id; /* DELETE FROM track_artists WHERE artist_id=? */

    // Cached read statements (pre-compiled for warming + merged-count hot paths)
    sqlite3_stmt* read_track_by_id;
    sqlite3_stmt* read_album_by_id;
    sqlite3_stmt* read_tracks_by_album;
    sqlite3_stmt* iter_all_artists;           /* SELECT id, name, musicbrainz_id FROM artists */
    sqlite3_stmt* iter_all_albums;            /* SELECT id, title, artist_id, year, path, mb_release_id FROM albums */
    sqlite3_stmt* iter_all_tracks;            /* SELECT id, title, path, ... FROM tracks (no JOINs) */
    sqlite3_stmt* iter_all_track_artists;     /* SELECT track_id, artist_id, join_phrase, position (no JOIN) */
    sqlite3_stmt* get_max_ids;               /* SELECT MAX(id) from each entity table */

    // Reconciler: batch loads (json_each over an id array) and canonical writes.
    // All used inside db_reconcile_albums. Fast path for mb_status-only updates
    // bypasses the reconciler entirely and uses update_album_mb_status.
    sqlite3_stmt* update_album_mb_status;            /* UPDATE albums SET mb_status=?, mb_resolved_at=? WHERE id=? */
    sqlite3_stmt* reconcile_load_albums_batch;       /* WHERE id IN (SELECT value FROM json_each(?)) */
    sqlite3_stmt* reconcile_load_tracks_batch;       /* WHERE album_id IN (SELECT value FROM json_each(?)) */
    sqlite3_stmt* reconcile_load_track_artists_batch;/* WHERE track_id IN (SELECT value FROM json_each(?)) */
    sqlite3_stmt* reconcile_update_album;            /* Canonical full-field UPDATE */
    sqlite3_stmt* reconcile_update_track;            /* Canonical full-field UPDATE */
    sqlite3_stmt* reconcile_insert_track;            /* Canonical INSERT */
    sqlite3_stmt* reconcile_delete_track_by_id;      /* DELETE FROM tracks WHERE id=? */

    // Transaction state
    bool in_transaction;
    int txn_depth;  // 0 = no txn, 1+ = nested depth (for batch transactions)

    // Cached statements for merge_duplicate_artist
    sqlite3_stmt* select_artist_by_name_and_mbid; /* SELECT id FROM artists WHERE name=? COLLATE NOCASE AND musicbrainz_id=? */
    sqlite3_stmt* delete_artist_fts;              /* DELETE FROM artists_fts WHERE rowid=? */
    sqlite3_stmt* delete_artist;                  /* DELETE FROM artists WHERE id=? */
};

// =============================================================================
// Shared SQL fragments for track queries
// =============================================================================

#define TRACK_SELECT_COLS \
    "t.id, t.title, a.name, al.title, t.path, t.duration_ms, t.track_num, " \
    "t.disc_num, t.year, t.album_id, ta.artist_id, t.genre, al.path, "       \
    "al.musicbrainz_release_id, t.artist_display"

#define TRACK_SELECT_FROM \
    " FROM tracks t"                                                            \
    " LEFT JOIN track_artists ta ON ta.track_id = t.id AND ta.position = 0"    \
    " LEFT JOIN artists a ON a.id = ta.artist_id"                              \
    " LEFT JOIN albums al ON t.album_id = al.id"

// =============================================================================
// Internal Functions (db.c)
// =============================================================================

// Lock helpers
void db_lock(quadrature_db_t* db);
void db_unlock(quadrature_db_t* db);

// Statement preparation
void db_prepare_stmts(quadrature_db_t* db);
void db_finalize_stmts(quadrature_db_t* db);

#endif // QUADRATURE_DB_INTERNAL_H
