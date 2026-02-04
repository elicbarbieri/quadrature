#include <glib.h>
#include "db_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// =============================================================================
// Schema SQL
// =============================================================================

static const char* SCHEMA_SQL =
    // Artists
    "CREATE TABLE IF NOT EXISTS artists ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    "  musicbrainz_id TEXT,"
    "  sort_name TEXT"
    ");"

    // Albums
    "CREATE TABLE IF NOT EXISTS albums ("
    "  id INTEGER PRIMARY KEY,"
    "  title TEXT NOT NULL,"
    "  artist_id INTEGER REFERENCES artists(id),"
    "  album_artist_id INTEGER REFERENCES artists(id),"
    "  path TEXT NOT NULL DEFAULT '',"
    "  year INTEGER,"
    "  genres TEXT,"
    "  is_compilation INTEGER DEFAULT 0,"
    "  last_updated_at INTEGER,"
    "  musicbrainz_release_id TEXT,"
    "  musicbrainz_release_group_id TEXT,"
    "  release_type TEXT,"
    "  label TEXT,"
    "  barcode TEXT,"
    "  mb_status INTEGER DEFAULT 0,"
    "  mb_resolved_at INTEGER"
    ");"

    // Tracks
    "CREATE TABLE IF NOT EXISTS tracks ("
    "  id INTEGER PRIMARY KEY,"
    "  title TEXT NOT NULL,"
    "  album_id INTEGER REFERENCES albums(id),"
    "  path TEXT NOT NULL UNIQUE,"
    "  duration_ms INTEGER NOT NULL,"
    "  track_num INTEGER,"
    "  disc_num INTEGER NOT NULL DEFAULT 1,"
    "  mtime INTEGER NOT NULL DEFAULT 0,"
    "  year INTEGER DEFAULT 0,"
    "  genre TEXT,"
    "  metadata TEXT NOT NULL DEFAULT '{}',"
    "  artist_display TEXT,"
    "  musicbrainz_recording_id TEXT,"
    "  chromaprint TEXT,"
    "  chromaprint_duration INTEGER DEFAULT 0"
    ");"

    // Multi-artist junction table
    "CREATE TABLE IF NOT EXISTS track_artists ("
    "  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
    "  artist_id INTEGER NOT NULL REFERENCES artists(id),"
    "  role INTEGER NOT NULL DEFAULT 0,"      // 0=primary, 1=featuring
    "  position INTEGER NOT NULL DEFAULT 0,"  // display order
    "  PRIMARY KEY (track_id, artist_id)"
    ");"

    // Full-text search
    "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
    "  title, content='tracks', content_rowid='id'"
    ");"

    // Indexer errors
    "CREATE TABLE IF NOT EXISTS indexer_errors ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL,"
    "  message TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))"
    ");"

    // Watch paths
    "CREATE TABLE IF NOT EXISTS watch_paths ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  enabled INTEGER NOT NULL DEFAULT 1,"
    "  last_scanned INTEGER"
    ");"

    // Indexes
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_albums_path ON albums(path) WHERE path != '';"
    "CREATE INDEX IF NOT EXISTS idx_albums_artist_year_title ON albums(artist_id, year, title);"
    "CREATE INDEX IF NOT EXISTS idx_albums_mb_release ON albums(musicbrainz_release_id) WHERE musicbrainz_release_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_albums_mb_status ON albums(mb_status);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id, track_num);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_year ON tracks(year);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album_genre ON tracks(album_id, genre);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_mb_recording ON tracks(musicbrainz_recording_id) WHERE musicbrainz_recording_id IS NOT NULL;"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_artists_mbid ON artists(musicbrainz_id) WHERE musicbrainz_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_track_artists_artist ON track_artists(artist_id);"
    "CREATE INDEX IF NOT EXISTS idx_track_artists_track ON track_artists(track_id, artist_id, role, position);"
    "CREATE INDEX IF NOT EXISTS idx_errors_path ON indexer_errors(path);";

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

static quadrature_result_t apply_pragmas(sqlite3* db) {
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-64000", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);  // 256MB mmap for reads
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
        "SELECT id FROM artists WHERE name=? COLLATE NOCASE",
        -1, &db->select_artist, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT INTO tracks(title,album_id,path,duration_ms,track_num,disc_num,mtime,year,genre,metadata) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "title=excluded.title, album_id=excluded.album_id, "
        "duration_ms=excluded.duration_ms, track_num=excluded.track_num, "
        "disc_num=excluded.disc_num, mtime=excluded.mtime, "
        "year=excluded.year, genre=excluded.genre, metadata=excluded.metadata",
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
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO track_artists(track_id,artist_id,role,position) "
        "VALUES(?,?,?,?)",
        -1, &db->insert_track_artist, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM track_artists WHERE track_id = ?",
        -1, &db->delete_track_artists, NULL);

    // Cached statements for indexer hot paths
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM tracks WHERE path = ?",
        -1, &db->select_track_by_path, NULL);
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM albums WHERE path = ?",
        -1, &db->select_album_by_path, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET title = ?, artist_id = ?, album_artist_id = ?, "
        "is_compilation = ?, year = ? WHERE id = ?",
        -1, &db->update_album_by_id, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT INTO albums(title, artist_id, path, year, is_compilation, album_artist_id) "
        "VALUES(?, ?, ?, ?, ?, ?)",
        -1, &db->insert_folder_album, NULL);
}

void db_finalize_stmts(quadrature_db_t* db) {
    if (db->insert_artist) { sqlite3_finalize(db->insert_artist); db->insert_artist = NULL; }
    if (db->select_artist) { sqlite3_finalize(db->select_artist); db->select_artist = NULL; }
    if (db->upsert_track) { sqlite3_finalize(db->upsert_track); db->upsert_track = NULL; }
    if (db->insert_fts) { sqlite3_finalize(db->insert_fts); db->insert_fts = NULL; }
    if (db->delete_track) { sqlite3_finalize(db->delete_track); db->delete_track = NULL; }
    if (db->delete_fts) { sqlite3_finalize(db->delete_fts); db->delete_fts = NULL; }
    if (db->insert_track_artist) { sqlite3_finalize(db->insert_track_artist); db->insert_track_artist = NULL; }
    if (db->delete_track_artists) { sqlite3_finalize(db->delete_track_artists); db->delete_track_artists = NULL; }
    if (db->select_track_by_path) { sqlite3_finalize(db->select_track_by_path); db->select_track_by_path = NULL; }
    if (db->select_album_by_path) { sqlite3_finalize(db->select_album_by_path); db->select_album_by_path = NULL; }
    if (db->update_album_by_id) { sqlite3_finalize(db->update_album_by_id); db->update_album_by_id = NULL; }
    if (db->insert_folder_album) { sqlite3_finalize(db->insert_folder_album); db->insert_folder_album = NULL; }
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

    *out = db;
    return QUADRATURE_OK;
}

quadrature_result_t db_open_memory(quadrature_db_t** out) {
    return db_open(NULL, out);
}

quadrature_result_t db_open_readonly(const char* path, quadrature_db_t** out) {
    if (!out || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    quadrature_db_t* db = calloc(1, sizeof(quadrature_db_t));
    if (!db) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    pthread_mutex_init(&db->lock, NULL);
    atomic_init(&db->cancel_flag, 0);

    /* Open read-write + create so we can fully participate in WAL mode
     * (access -shm file) and succeed even if the DB doesn't exist yet.
     * SQLITE_OPEN_READONLY cannot reliably read WAL pages because it may
     * fail to open/create the shared-memory file.
     * SQLITE_OPEN_CREATE ensures library_cache is never NULL at startup.
     * We use PRAGMA query_only = ON below to prevent any actual writes. */
    int rc = sqlite3_open_v2(path, &db->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        g_critical("Failed to open database read-only: %s", sqlite3_errmsg(db->db));
        pthread_mutex_destroy(&db->lock);
        free(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->db_path = strdup(path);

    /* Prevent writes while allowing full WAL read visibility.
     * journal_mode is inherited from the database file (already WAL from writer),
     * so no need to set it here. READWRITE + query_only gives us WAL read
     * access without the ability to modify data. */
    sqlite3_exec(db->db, "PRAGMA query_only = ON;", NULL, NULL, NULL);

    *out = db;
    return QUADRATURE_OK;
}

void db_close(quadrature_db_t* db) {
    if (!db) return;

    db_finalize_stmts(db);
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

quadrature_result_t db_get_track_mtime(quadrature_db_t* db, const char* path,
                                        int64_t* mtime) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT mtime FROM tracks WHERE path = ?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (mtime) *mtime = sqlite3_column_int64(stmt, 0);
        res = QUADRATURE_OK;
    }
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return res;
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
