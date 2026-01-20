#include <glib.h>
#include "db_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// =============================================================================
// Schema SQL (v3 - folder-based album indexing with error tracking)
// =============================================================================

static const char* SCHEMA_SQL =
    // Core music data
    "CREATE TABLE IF NOT EXISTS artists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE"
    ");"
    "CREATE TABLE IF NOT EXISTS albums ("
    "  id INTEGER PRIMARY KEY,"
    "  title TEXT NOT NULL,"
    "  artist_id INTEGER REFERENCES artists(id),"
    "  path TEXT NOT NULL DEFAULT '',"
    "  year INTEGER,"
    "  last_updated_at INTEGER,"
    "  is_compilation INTEGER DEFAULT 0,"
    "  album_artist_id INTEGER REFERENCES artists(id),"
    "  UNIQUE(title, artist_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_albums_path ON albums(path);"

    "CREATE TABLE IF NOT EXISTS tracks ("
    "  id INTEGER PRIMARY KEY,"
    "  title TEXT NOT NULL,"
    "  artist_id INTEGER REFERENCES artists(id),"
    "  album_id INTEGER REFERENCES albums(id),"
    "  path TEXT NOT NULL UNIQUE,"
    "  duration_ms INTEGER NOT NULL,"
    "  track_num INTEGER,"
    "  disc_num INTEGER NOT NULL DEFAULT 1,"
    "  mtime INTEGER NOT NULL DEFAULT 0,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  last_seen INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id, track_num);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_last_seen ON tracks(last_seen);"

    // Full-text search
    "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
    "  title, content='tracks', content_rowid='id'"
    ");"

    // Track extended metadata (for metadata popup)
    "CREATE TABLE IF NOT EXISTS track_metadata ("
    "  track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,"
    "  raw_json TEXT NOT NULL,"
    "  bitrate INTEGER,"
    "  sample_rate INTEGER,"
    "  channels INTEGER,"
    "  codec TEXT,"
    "  album_artist TEXT,"
    "  genre TEXT,"
    "  comment TEXT,"
    "  compilation INTEGER DEFAULT 0,"
    "  disc_total INTEGER,"
    "  track_total INTEGER,"
    "  has_embedded_art INTEGER DEFAULT 0"
    ");"

    // Indexer errors for user review (path-based, no FK constraints)
    "CREATE TABLE IF NOT EXISTS indexer_errors ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL,"
    "  message TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_errors_path ON indexer_errors(path);"

    // Watch paths
    "CREATE TABLE IF NOT EXISTS watch_paths ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  enabled INTEGER NOT NULL DEFAULT 1,"
    "  last_scanned INTEGER"
    ");"

    // Directory mtime cache (fast change detection)
    "CREATE TABLE IF NOT EXISTS dir_mtime ("
    "  path TEXT PRIMARY KEY,"
    "  mtime INTEGER NOT NULL"
    ");";

// =============================================================================
// Helper Functions
// =============================================================================

static quadrature_result_t apply_schema(sqlite3* db) {
    char* err = NULL;

    int rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Schema creation failed: %s", err);
        sqlite3_free(err);
        return QUADRATURE_ERROR_INTERNAL;
    }

    return QUADRATURE_OK;
}

// Helper to check if a column exists in a table
static bool column_exists(sqlite3* db, const char* table, const char* column) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM pragma_table_info('%s') WHERE name='%s'",
        table, column);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return exists;
}

// Helper to add a column if it doesn't exist
static void add_column_if_missing(sqlite3* db, const char* table,
                                   const char* column, const char* type_and_default) {
    if (column_exists(db, table, column)) return;

    char sql[512];
    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s",
             table, column, type_and_default);

    char* err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_debug("Migration note: %s", err ? err : "unknown");
        sqlite3_free(err);
    }
}

// Apply migrations for existing databases
static quadrature_result_t apply_migrations(sqlite3* db) {
    // v2 migration: last_updated_at column
    add_column_if_missing(db, "albums", "last_updated_at", "INTEGER");

    // v3 migrations: folder-based album indexing
    add_column_if_missing(db, "albums", "is_compilation", "INTEGER DEFAULT 0");
    add_column_if_missing(db, "albums", "album_artist_id", "INTEGER REFERENCES artists(id)");

    // Create indexes if they don't exist (idempotent)
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_albums_path ON albums(path)",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_errors_path ON indexer_errors(path)",
                 NULL, NULL, NULL);

    // v4 migration: Migrate old FK-based indexer_errors table to path-based
    // Check if old schema exists (has track_id column)
    if (column_exists(db, "indexer_errors", "track_id")) {
        // Drop old table and recreate with new schema
        sqlite3_exec(db, "DROP TABLE IF EXISTS indexer_errors", NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS indexer_errors ("
            "  id INTEGER PRIMARY KEY,"
            "  path TEXT NOT NULL,"
            "  message TEXT NOT NULL,"
            "  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))"
            ")",
            NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_errors_path ON indexer_errors(path)",
                     NULL, NULL, NULL);
    }

    return QUADRATURE_OK;
}

static quadrature_result_t apply_pragmas(sqlite3* db) {
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-64000", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 5000);
    return QUADRATURE_OK;
}

// =============================================================================
// Lock Helpers
// =============================================================================

void db_lock(quadrature_db_t* db) {
    pthread_mutex_lock(&db->lock);
}

void db_unlock(quadrature_db_t* db) {
    pthread_mutex_unlock(&db->lock);
}

// =============================================================================
// Statement Management
// =============================================================================

void db_prepare_stmts(quadrature_db_t* db) {
    if (db->insert_artist) return;  // Already prepared

    sqlite3_prepare_v2(db->db,
        "INSERT OR IGNORE INTO artists(name) VALUES(?)",
        -1, &db->insert_artist, NULL);
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists WHERE name=?",
        -1, &db->select_artist, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR IGNORE INTO albums(title,artist_id,path,year) VALUES(?,?,?,?)",
        -1, &db->insert_album, NULL);
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM albums WHERE title=? AND artist_id=?",
        -1, &db->select_album, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT INTO tracks(title,artist_id,album_id,path,duration_ms,track_num,disc_num,mtime,size,last_seen) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "title=excluded.title, artist_id=excluded.artist_id, album_id=excluded.album_id, "
        "duration_ms=excluded.duration_ms, track_num=excluded.track_num, disc_num=excluded.disc_num, "
        "mtime=excluded.mtime, size=excluded.size, last_seen=excluded.last_seen",
        -1, &db->upsert_track, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO tracks_fts(rowid, title) VALUES(?, ?)",
        -1, &db->insert_fts, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM tracks WHERE path = ?",
        -1, &db->delete_track, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM tracks_fts WHERE rowid = ?",
        -1, &db->delete_fts, NULL);
}

void db_finalize_stmts(quadrature_db_t* db) {
    if (db->insert_artist) { sqlite3_finalize(db->insert_artist); db->insert_artist = NULL; }
    if (db->select_artist) { sqlite3_finalize(db->select_artist); db->select_artist = NULL; }
    if (db->insert_album) { sqlite3_finalize(db->insert_album); db->insert_album = NULL; }
    if (db->select_album) { sqlite3_finalize(db->select_album); db->select_album = NULL; }
    if (db->upsert_track) { sqlite3_finalize(db->upsert_track); db->upsert_track = NULL; }
    if (db->insert_fts) { sqlite3_finalize(db->insert_fts); db->insert_fts = NULL; }
    if (db->delete_track) { sqlite3_finalize(db->delete_track); db->delete_track = NULL; }
    if (db->delete_fts) { sqlite3_finalize(db->delete_fts); db->delete_fts = NULL; }
}

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t db_open(const char* path, quadrature_db_t** out) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    quadrature_db_t* db = calloc(1, sizeof(quadrature_db_t));
    if (!db) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    pthread_mutex_init(&db->lock, NULL);
    atomic_init(&db->cancel_flag, 0);
    db_cache_init(&db->artist_cache);
    db_cache_init(&db->album_cache);

    int rc = sqlite3_open(path ? path : ":memory:", &db->db);
    if (rc != SQLITE_OK) {
        g_critical("Failed to open database: %s", sqlite3_errmsg(db->db));
        pthread_mutex_destroy(&db->lock);
        free(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (path) {
        db->db_path = strdup(path);
    }

    quadrature_result_t res = apply_pragmas(db->db);
    if (res != QUADRATURE_OK) {
        sqlite3_close(db->db);
        pthread_mutex_destroy(&db->lock);
        free(db->db_path);
        free(db);
        return res;
    }

    res = apply_schema(db->db);
    if (res != QUADRATURE_OK) {
        sqlite3_close(db->db);
        pthread_mutex_destroy(&db->lock);
        free(db->db_path);
        free(db);
        return res;
    }

    // Apply migrations for existing databases
    res = apply_migrations(db->db);
    if (res != QUADRATURE_OK) {
        sqlite3_close(db->db);
        pthread_mutex_destroy(&db->lock);
        free(db->db_path);
        free(db);
        return res;
    }

    *out = db;
    return QUADRATURE_OK;
}

quadrature_result_t db_open_memory(quadrature_db_t** out) {
    return db_open(NULL, out);
}

void db_close(quadrature_db_t* db) {
    if (!db) return;

    db_finalize_stmts(db);
    db_cache_destroy(&db->artist_cache);
    db_cache_destroy(&db->album_cache);
    sqlite3_close(db->db);
    pthread_mutex_destroy(&db->lock);
    free(db->db_path);
    free(db);
}

const char* db_path(const quadrature_db_t* db) {
    return db ? db->db_path : NULL;
}

// =============================================================================
// Transaction Operations
// =============================================================================

quadrature_result_t db_begin_transaction(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    db_prepare_stmts(db);

    char* err = NULL;
    int rc = sqlite3_exec(db->db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Failed to begin transaction: %s", err);
        sqlite3_free(err);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->in_transaction = true;
    // Note: lock remains held during transaction
    return QUADRATURE_OK;
}

quadrature_result_t db_commit(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    char* err = NULL;
    int rc = sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Failed to commit transaction: %s", err);
        sqlite3_free(err);
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        db->in_transaction = false;
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->in_transaction = false;
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_rollback(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
    db->in_transaction = false;
    db_unlock(db);
    return QUADRATURE_OK;
}

// =============================================================================
// Track State Operations
// =============================================================================

quadrature_result_t db_get_track_state(quadrature_db_t* db, const char* path,
                                       int64_t* mtime, int64_t* size) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT mtime, size FROM tracks WHERE path = ?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (mtime) *mtime = sqlite3_column_int64(stmt, 0);
        if (size) *size = sqlite3_column_int64(stmt, 1);
        res = QUADRATURE_OK;
    }
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return res;
}

quadrature_result_t db_mark_tracks_seen(quadrature_db_t* db, const char* dir_path, int64_t timestamp) {
    if (!db || !dir_path) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE tracks SET last_seen = ? WHERE path LIKE ? || '%'",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, timestamp);
    sqlite3_bind_text(stmt, 2, dir_path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_delete_unseen_tracks(quadrature_db_t* db, int64_t older_than) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    // First delete from FTS
    sqlite3_exec(db->db,
        "DELETE FROM tracks_fts WHERE rowid IN "
        "(SELECT id FROM tracks WHERE last_seen > 0 AND last_seen < ?)",
        NULL, NULL, NULL);

    // Then delete from tracks
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "DELETE FROM tracks WHERE last_seen > 0 AND last_seen < ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, older_than);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// WAL Checkpoint
// =============================================================================

quadrature_result_t db_checkpoint(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_wal_checkpoint_v2(db->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    db_unlock(db);

    return QUADRATURE_OK;
}
