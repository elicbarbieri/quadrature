#include <glib.h>
#include "internal.h"

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
    // Note: release_type, label, catalog_number, barcode removed — that data
    // lives in the metadata DB (releases table). Never read/written here.
    "CREATE TABLE IF NOT EXISTS albums ("
    "  id INTEGER PRIMARY KEY,"
    "  title TEXT NOT NULL,"
    "  artist_id INTEGER REFERENCES artists(id),"
    "  path TEXT NOT NULL DEFAULT '',"
    "  year INTEGER,"
    "  is_compilation INTEGER DEFAULT 0,"
    "  last_updated_at INTEGER,"
    "  musicbrainz_release_id TEXT,"
    "  musicbrainz_release_group_id TEXT,"
    "  mb_status INTEGER DEFAULT 0,"
    "  mb_resolved_at INTEGER"
    ");"

    // Tracks
    "CREATE TABLE IF NOT EXISTS tracks ("
    "  id INTEGER PRIMARY KEY,"
    "  title TEXT NOT NULL,"
    "  album_id INTEGER REFERENCES albums(id),"
    "  path TEXT NOT NULL,"
    "  duration_ms INTEGER NOT NULL,"
    "  track_num INTEGER,"
    "  disc_num INTEGER NOT NULL DEFAULT 1,"
    "  mtime INTEGER NOT NULL DEFAULT 0,"
    "  year INTEGER DEFAULT 0,"
    "  genre TEXT,"
    "  artist_display TEXT,"
    "  UNIQUE(album_id, path)"
    ");"

    // Multi-artist junction table (WITHOUT ROWID: composite PK, small rows)
    "CREATE TABLE IF NOT EXISTS track_artists ("
    "  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
    "  artist_id INTEGER NOT NULL REFERENCES artists(id),"
    "  position INTEGER NOT NULL DEFAULT 0,"
    "  join_phrase TEXT NOT NULL DEFAULT '',"
    "  PRIMARY KEY (track_id, artist_id)"
    ") WITHOUT ROWID;"

    // Full-text search — multi-column standalone FTS5
    // BM25 weights configured separately via apply_fts_rank_config() after table creation.
    "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(title, artist, album);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS artists_fts USING fts5(name);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS albums_fts USING fts5(title, artist);"

    // Indexer errors
    "CREATE TABLE IF NOT EXISTS indexer_errors ("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL,"
    "  message TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),"
    "  scan_generation INTEGER NOT NULL DEFAULT 0"
    ");"

    // Indexes — albums
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_albums_path ON albums(path) WHERE path != '';"
    "CREATE INDEX IF NOT EXISTS idx_albums_artist_year_title ON albums(artist_id, year, title);"
    "CREATE INDEX IF NOT EXISTS idx_albums_mb_release ON albums(musicbrainz_release_id) WHERE musicbrainz_release_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_albums_mb_status ON albums(mb_status);"
    "CREATE INDEX IF NOT EXISTS idx_albums_year_title ON albums(year, title COLLATE NOCASE);"

    // Indexes — tracks (album_id, disc_num, track_num matches warming ORDER BY)
    "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id, disc_num, track_num);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_year ON tracks(year);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);"
    "CREATE INDEX IF NOT EXISTS idx_tracks_album_genre ON tracks(album_id, genre);"

    // Indexes — artists
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_artists_mbid ON artists(musicbrainz_id) WHERE musicbrainz_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_artists_name ON artists(name COLLATE NOCASE);"

    // Indexes — track_artists (covering: position+artist_id avoids table lookup)
    "CREATE INDEX IF NOT EXISTS idx_track_artists_artist ON track_artists(artist_id);"
    "CREATE INDEX IF NOT EXISTS idx_track_artists_track ON track_artists(track_id, position, artist_id);"

    // Indexes — errors
    "CREATE INDEX IF NOT EXISTS idx_errors_path ON indexer_errors(path);"
    "CREATE INDEX IF NOT EXISTS idx_errors_generation ON indexer_errors(scan_generation);";

// =============================================================================
// Helper Functions
// =============================================================================

// FTS5 rank= cannot appear in CREATE TABLE — it must be set via a special
// metadata insert into the FTS shadow config table after the table exists.
// This is a no-op if already set (idempotent).
static void apply_fts_rank_config(sqlite3* db) {
    sqlite3_exec(db,
        "INSERT OR REPLACE INTO tracks_fts(tracks_fts, rank) VALUES('rank', 'bm25(10, 5, 1)');",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "INSERT OR REPLACE INTO albums_fts(albums_fts, rank) VALUES('rank', 'bm25(5, 1)');",
        NULL, NULL, NULL);
}

static quadrature_result_t apply_schema(sqlite3* db) {
    char* err = NULL;

    int rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Schema creation failed: %s", err);
        sqlite3_free(err);
        return QUADRATURE_ERROR_INTERNAL;
    }

    apply_fts_rank_config(db);
    return QUADRATURE_OK;
}

static quadrature_result_t apply_pragmas(sqlite3* db) {
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-64000", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);  // 256MB mmap for reads
    sqlite3_exec(db, "PRAGMA wal_autocheckpoint=10000", NULL, NULL, NULL);  // defer checkpoint until ~40MB burst
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA optimize", NULL, NULL, NULL);  // update planner statistics
    return QUADRATURE_OK;
}

// =============================================================================
// Lock Helpers
// =============================================================================

void db_lock(quadrature_db_t* db) {
    if (!db->readonly)
        pthread_mutex_lock(&db->lock);
}

void db_unlock(quadrature_db_t* db) {
    if (!db->readonly)
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
        "INSERT INTO tracks(title,album_id,path,duration_ms,track_num,disc_num,mtime,year,genre) "
        "VALUES(?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(album_id, path) DO UPDATE SET "
        "title=CASE WHEN (SELECT mb_status FROM albums WHERE id=excluded.album_id)=1 THEN title ELSE excluded.title END, "
        "album_id=excluded.album_id, "
        "duration_ms=excluded.duration_ms, track_num=excluded.track_num, "
        "disc_num=excluded.disc_num, mtime=excluded.mtime, "
        "year=excluded.year, genre=excluded.genre",
        -1, &db->upsert_track, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO artists_fts(rowid, name) VALUES(?,?)",
        -1, &db->insert_artist_fts, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO albums_fts(rowid, title, artist) "
        "SELECT al.id, al.title, COALESCE(ar.name,'') "
        "FROM albums al LEFT JOIN artists ar ON al.artist_id = ar.id WHERE al.id = ?",
        -1, &db->update_album_fts, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO track_artists(track_id,artist_id,position,join_phrase) "
        "VALUES(?,?,?,?)",
        -1, &db->insert_track_artist, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM track_artists WHERE track_id = ?",
        -1, &db->delete_track_artists, NULL);

    // Cached statements for indexer hot paths
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM tracks WHERE album_id = ? AND path = ?",
        -1, &db->select_track_by_path, NULL);
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM albums WHERE path = ?",
        -1, &db->select_album_by_path, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET title = ?, artist_id = ?, is_compilation = ?, year = ? "
        "WHERE id = ? AND mb_status < 2",
        -1, &db->update_album_by_id, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT INTO albums(title, artist_id, path, year, is_compilation) "
        "VALUES(?, ?, ?, ?, ?)",
        -1, &db->insert_folder_album, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE tracks SET artist_display = ? WHERE id = ?",
        -1, &db->update_track_artist_display, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET musicbrainz_release_id = ?, mb_status = ? "
        "WHERE id = ? AND mb_status = ?",
        -1, &db->set_album_release_id, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO tracks_fts(rowid, title, artist, album) "
        "SELECT t.id, t.title, COALESCE(t.artist_display,''), COALESCE(al.title,'') "
        "FROM tracks t JOIN albums al ON t.album_id = al.id WHERE t.album_id = ?",
        -1, &db->sync_album_tracks_fts, NULL);
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists WHERE musicbrainz_id = ?",
        -1, &db->select_artist_by_mb_id, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE artists SET sort_name = ? WHERE id = ? "
        "AND (sort_name IS NULL OR sort_name = '')",
        -1, &db->update_artist_sort_name, NULL);
    /* Step 2: exact NOCASE match, skip rows already claimed by a different MBID */
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists WHERE name = ? COLLATE NOCASE "
        "AND (musicbrainz_id IS NULL OR musicbrainz_id = ?)",
        -1, &db->select_artist_by_name_nocase, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE artists SET musicbrainz_id = ?, sort_name = ? WHERE id = ?",
        -1, &db->update_artist_mb_data, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT INTO artists(name, musicbrainz_id, sort_name) VALUES(?, ?, ?)",
        -1, &db->insert_artist_mb, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO artists_fts(rowid, name) VALUES(?,?)",
        -1, &db->insert_artist_fts_replace, NULL);
    /* Step 3: normalized lookup — strips spaces and hyphens using SQLite REPLACE+LOWER */
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists "
        "WHERE REPLACE(REPLACE(LOWER(name), ' ', ''), '-', '') = ? "
        "AND musicbrainz_id IS NULL",
        -1, &db->select_artist_normalized_no_mbid, NULL);
    /* Rename in-place: set canonical MB name + MBID + sort_name */
    sqlite3_prepare_v2(db->db,
        "UPDATE artists SET name = ?, musicbrainz_id = ?, sort_name = ? WHERE id = ?",
        -1, &db->rename_artist_mb, NULL);
    /* Conflict merge: re-home track_artists from old artist to new */
    sqlite3_prepare_v2(db->db,
        "UPDATE OR IGNORE track_artists SET artist_id = ? WHERE artist_id = ?",
        -1, &db->move_track_artists, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM track_artists WHERE artist_id = ?",
        -1, &db->delete_track_artists_artist_id, NULL);

    /* merge_duplicate_artist cached statements */
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists WHERE name = ? COLLATE NOCASE AND musicbrainz_id = ?",
        -1, &db->select_artist_by_name_and_mbid, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM artists_fts WHERE rowid = ?",
        -1, &db->delete_artist_fts, NULL);
    sqlite3_prepare_v2(db->db,
        "DELETE FROM artists WHERE id = ?",
        -1, &db->delete_artist, NULL);

    /* MB resolver hot-path statements */
    sqlite3_prepare_v2(db->db,
        "UPDATE tracks SET title = ? WHERE id = ?",
        -1, &db->update_track_title, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE tracks SET genre = ? WHERE id = ?",
        -1, &db->update_track_genre, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET mb_status = ?, mb_resolved_at = ? WHERE id = ?",
        -1, &db->set_album_mb_status, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET artist_id = ?, is_compilation = ? WHERE id = ?",
        -1, &db->update_album_artist, NULL);
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET title = ?, musicbrainz_release_id = ?, "
        "musicbrainz_release_group_id = ?, "
        "year = CASE WHEN ? > 0 THEN ? ELSE year END, "
        "mb_status = ?, mb_resolved_at = ? WHERE id = ?",
        -1, &db->update_album_mb, NULL);
    sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO albums_fts(rowid, title, artist) "
        "SELECT al.id, al.title, COALESCE(ar.name,'') "
        "FROM albums al LEFT JOIN artists ar ON al.artist_id = ar.id WHERE al.id = ?",
        -1, &db->sync_album_fts, NULL);

    /* ── Cached read statements ────────────────────────────────────────── */

    sqlite3_prepare_v2(db->db,
        "SELECT t.id, t.title, a.name, al.title, t.path, t.duration_ms, t.track_num, "
        "       t.disc_num, t.year, t.album_id, ta.artist_id, t.genre, al.path, "
        "       t.artist_display "
        "FROM tracks t "
        "LEFT JOIN track_artists ta ON ta.track_id = t.id AND ta.position = 0 "
        "LEFT JOIN artists a ON a.id = ta.artist_id "
        "LEFT JOIN albums al ON t.album_id = al.id "
        "WHERE t.id = ?",
        -1, &db->read_track_by_id, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT a.id, a.name, a.musicbrainz_id, "
        "COUNT(DISTINCT al.id), COUNT(DISTINCT ta.track_id) "
        "FROM artists a "
        "LEFT JOIN albums al ON al.artist_id = a.id "
        "LEFT JOIN track_artists ta ON ta.artist_id = a.id "
        "WHERE a.id = ? "
        "GROUP BY a.id",
        -1, &db->read_artist_by_id, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT al.id, al.title, a.name, al.artist_id, al.year, "
        "  (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.id) AS track_count, "
        "  (SELECT GROUP_CONCAT(g, ';') FROM (SELECT DISTINCT LOWER(genre) AS g "
        "   FROM tracks WHERE album_id = al.id AND genre IS NOT NULL AND genre != '')) AS genres, "
        "  al.path, al.musicbrainz_release_id "
        "FROM albums al "
        "LEFT JOIN artists a ON al.artist_id = a.id "
        "WHERE al.id = ?",
        -1, &db->read_album_by_id, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT COUNT(*) FROM albums WHERE artist_id = ?",
        -1, &db->read_albums_by_artist_count, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT al.id, al.title, a.name, al.artist_id, al.year, "
        "  (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.id) AS track_count, "
        "  (SELECT GROUP_CONCAT(g, ';') FROM (SELECT DISTINCT LOWER(genre) AS g "
        "   FROM tracks WHERE album_id = al.id AND genre IS NOT NULL AND genre != '')) AS genres, "
        "  al.path, al.musicbrainz_release_id "
        "FROM albums al "
        "LEFT JOIN artists a ON al.artist_id = a.id "
        "WHERE al.artist_id = ? "
        "ORDER BY al.year, al.title COLLATE NOCASE",
        -1, &db->read_albums_by_artist, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT " TRACK_SELECT_COLS TRACK_SELECT_FROM
        " WHERE t.album_id = ?"
        " ORDER BY t.disc_num, t.track_num, t.title COLLATE NOCASE",
        -1, &db->read_tracks_by_album, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT COUNT(*) FROM track_artists WHERE track_id = ?",
        -1, &db->read_track_artists_count, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT ta.artist_id, a.name, ta.join_phrase, ta.position "
        "FROM track_artists ta "
        "LEFT JOIN artists a ON a.id = ta.artist_id "
        "WHERE ta.track_id = ? "
        "ORDER BY ta.position",
        -1, &db->read_track_artists, NULL);

    /* Warming iterators — no JOINs, no aggregates, maximum throughput.
     * Entities loaded in earlier phases; derived fields computed in C. */
    /* No WHERE EXISTS — orphan artists pruned by indexer finalize.
     * ORDER BY rowid = pure sequential B-tree scan. */
    sqlite3_prepare_v2(db->db,
        "SELECT a.id, a.name, a.musicbrainz_id "
        "FROM artists a "
        "ORDER BY a.id",
        -1, &db->iter_all_artists, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT al.id, al.title, al.artist_id, al.year, al.path, "
        "al.musicbrainz_release_id "
        "FROM albums al "
        "ORDER BY al.id",
        -1, &db->iter_all_albums, NULL);

    /* ORDER BY rowid = pure sequential table scan, no index overhead.
     * Album grouping and disc/track ordering done in C (Phase 4). */
    sqlite3_prepare_v2(db->db,
        "SELECT t.id, t.title, t.path, t.duration_ms, t.track_num, "
        "t.disc_num, t.year, t.album_id, t.genre, t.artist_display "
        "FROM tracks t "
        "ORDER BY t.id",
        -1, &db->iter_all_tracks, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT ta.track_id, ta.artist_id, ta.join_phrase, ta.position "
        "FROM track_artists ta "
        "ORDER BY ta.track_id, ta.position",
        -1, &db->iter_all_track_artists, NULL);

    sqlite3_prepare_v2(db->db,
        "SELECT (SELECT MAX(id) FROM artists),"
        "       (SELECT MAX(id) FROM albums),"
        "       (SELECT MAX(id) FROM tracks)",
        -1, &db->get_max_ids, NULL);
}

void db_finalize_stmts(quadrature_db_t* db) {
    sqlite3_stmt** stmts[] = {
        &db->insert_artist, &db->select_artist, &db->upsert_track,
        &db->insert_artist_fts, &db->update_album_fts,
        &db->insert_track_artist, &db->delete_track_artists,
        &db->select_track_by_path, &db->select_album_by_path,
        &db->update_album_by_id, &db->insert_folder_album,
        &db->update_track_artist_display, &db->set_album_release_id,
        &db->sync_album_tracks_fts, &db->select_artist_by_mb_id,
        &db->update_artist_sort_name, &db->select_artist_by_name_nocase,
        &db->update_artist_mb_data, &db->insert_artist_mb,
        &db->insert_artist_fts_replace, &db->select_artist_normalized_no_mbid,
        &db->rename_artist_mb, &db->move_track_artists,
        &db->delete_track_artists_artist_id,
        &db->update_track_title, &db->update_track_genre,
        &db->set_album_mb_status,
        &db->update_album_artist, &db->update_album_mb, &db->sync_album_fts,
        // merge_duplicate_artist cached statements
        &db->select_artist_by_name_and_mbid,
        &db->delete_artist_fts, &db->delete_artist,
        // cached read statements
        &db->read_track_by_id, &db->read_artist_by_id, &db->read_album_by_id,
        &db->read_albums_by_artist_count, &db->read_albums_by_artist,
        &db->read_tracks_by_album, &db->read_track_artists_count,
        &db->read_track_artists,
        &db->iter_all_artists, &db->iter_all_albums,
        &db->iter_all_tracks, &db->iter_all_track_artists,
        &db->get_max_ids,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(stmts); i++) {
        if (*stmts[i]) { sqlite3_finalize(*stmts[i]); *stmts[i] = NULL; }
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t db_open(const char* path, quadrature_db_t** out) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    quadrature_db_t* db = calloc(1, sizeof(quadrature_db_t));
    if (!db) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&db->lock, &mattr);
    pthread_mutexattr_destroy(&mattr);
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
    db->readonly = true;  /* Single-threaded read-only: skip app-level mutex, WAL handles isolation */

    /* Prevent writes while allowing full WAL read visibility.
     * journal_mode is inherited from the database file (already WAL from writer),
     * so no need to set it here. READWRITE + query_only gives us WAL read
     * access without the ability to modify data. */
    sqlite3_exec(db->db, "PRAGMA query_only = ON;", NULL, NULL, NULL);

    /* Performance PRAGMAs for read-heavy warming workload.
     * journal_mode inherited from DB file (already WAL from writer). */
    sqlite3_exec(db->db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA cache_size = -64000;", NULL, NULL, NULL);  /* 64MB page cache */
    sqlite3_exec(db->db, "PRAGMA mmap_size = 268435456;", NULL, NULL, NULL);  /* 256MB mmap */
    sqlite3_busy_timeout(db->db, 5000);

    db_prepare_stmts(db);

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

    // Inside a batch transaction: increment depth, keep lock for serialization
    if (db->txn_depth > 0) {
        db->txn_depth++;
        return QUADRATURE_OK;
    }

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
    db->txn_depth = 1;
    // Note: lock remains held during transaction
    return QUADRATURE_OK;
}

quadrature_result_t db_commit(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    // Inside a batch transaction: decrement depth, release lock for next worker
    if (db->txn_depth > 1) {
        db->txn_depth--;
        db_unlock(db);
        return QUADRATURE_OK;
    }

    char* err = NULL;
    int rc = sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Failed to commit transaction: %s", err);
        sqlite3_free(err);
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        db->in_transaction = false;
        db->txn_depth = 0;
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->in_transaction = false;
    db->txn_depth = 0;
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_rollback(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    // Inside a batch transaction: partial writes stay — album mtime won't be set,
    // so the album gets re-scanned next run (self-healing).
    if (db->txn_depth > 1) {
        db->txn_depth--;
        db_unlock(db);
        return QUADRATURE_OK;
    }

    sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
    db->in_transaction = false;
    db->txn_depth = 0;
    db_unlock(db);
    return QUADRATURE_OK;
}

// =============================================================================
// Batch Transaction Operations (for wrapping entire indexer phases)
// =============================================================================

quadrature_result_t db_begin_batch(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    db_prepare_stmts(db);

    char* err = NULL;
    int rc = sqlite3_exec(db->db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Failed to begin batch transaction: %s", err);
        sqlite3_free(err);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->in_transaction = true;
    db->txn_depth = 1;
    db_unlock(db);  // Release lock so workers can acquire it
    return QUADRATURE_OK;
}

quadrature_result_t db_commit_batch(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    char* err = NULL;
    int rc = sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Failed to commit batch transaction: %s", err);
        sqlite3_free(err);
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
    }

    db->in_transaction = false;
    db->txn_depth = 0;
    db_unlock(db);

    return (rc == SQLITE_OK) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// Read Transaction (snapshot isolation for bulk reads)
// =============================================================================

quadrature_result_t db_begin_read(quadrature_db_t *db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;
    sqlite3_exec(db->db, "BEGIN", NULL, NULL, NULL);
    return QUADRATURE_OK;
}

quadrature_result_t db_end_read(quadrature_db_t *db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;
    sqlite3_exec(db->db, "END", NULL, NULL, NULL);
    return QUADRATURE_OK;
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
