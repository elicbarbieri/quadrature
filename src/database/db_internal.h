#ifndef QUADRATURE_DB_INTERNAL_H
#define QUADRATURE_DB_INTERNAL_H

/**
 * Internal header for database implementation.
 * Contains the quadrature_db struct definition and shared declarations.
 */

#include "quadrature/database/database.h"
#include <sqlite3.h>
#include <pthread.h>
#include <stdatomic.h>

// =============================================================================
// LRU Cache for Artist/Album ID Lookups
// =============================================================================

#define DB_CACHE_SIZE 1024

typedef struct {
    char* key;
    int64_t id;
    uint32_t access_count;
} db_cache_entry_t;

typedef struct {
    db_cache_entry_t entries[DB_CACHE_SIZE];
    pthread_mutex_t lock;
} db_cache_t;

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
    sqlite3_stmt* insert_album;
    sqlite3_stmt* select_album;
    sqlite3_stmt* upsert_track;
    sqlite3_stmt* insert_fts;
    sqlite3_stmt* delete_track;
    sqlite3_stmt* delete_fts;

    // LRU caches for artist/album IDs during batch indexing
    db_cache_t artist_cache;
    db_cache_t album_cache;

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

// Get or create artist/album, returning ID (uses cache)
int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name);
int64_t db_get_or_create_album(quadrature_db_t* db, const char* title, int64_t artist_id,
                               const char* path, int year);

// Cache operations
void db_cache_init(db_cache_t* cache);
void db_cache_destroy(db_cache_t* cache);
int64_t db_cache_get(db_cache_t* cache, const char* key);
void db_cache_put(db_cache_t* cache, const char* key, int64_t id);
void db_cache_clear(db_cache_t* cache);

#endif // QUADRATURE_DB_INTERNAL_H
