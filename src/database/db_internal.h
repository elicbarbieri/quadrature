#ifndef QUADRATURE_DB_INTERNAL_H
#define QUADRATURE_DB_INTERNAL_H

/**
 * Internal header for database implementation.
 * Contains the quadrature_db struct definition and shared declarations.
 */

#include "quadrature/quadrature_database.h"
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
    sqlite3_stmt* insert_fts;
    sqlite3_stmt* delete_track;
    sqlite3_stmt* delete_fts;
    sqlite3_stmt* insert_track_artist;
    sqlite3_stmt* delete_track_artists;

    // Cached statements for indexer hot paths
    sqlite3_stmt* select_track_by_path;
    sqlite3_stmt* select_album_by_path;
    sqlite3_stmt* update_album_by_id;
    sqlite3_stmt* insert_folder_album;

    // Transaction state
    bool in_transaction;
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
