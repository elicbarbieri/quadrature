#include <glib.h>
#include "internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// =============================================================================
// Schema Initialization
//
// Fresh implementation: a single initial schema defined in
// src/database/migrations/001_initial.c. When additional schema changes become
// necessary, introduce a proper versioned migration runner here.
// =============================================================================

#define DB_SCHEMA_VERSION 1

static quadrature_result_t
db_init_schema(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int current = 0;
    sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        current = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (current == DB_SCHEMA_VERSION)
        return QUADRATURE_OK;
    if (current > DB_SCHEMA_VERSION) {
        g_critical("Database schema v%d is newer than app v%d — downgrade not supported",
                   current,
                   DB_SCHEMA_VERSION);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    quadrature_result_t res = db_migration_001_initial(db);
    if (res != QUADRATURE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return res;
    }
    sqlite3_exec(db, "PRAGMA user_version = " G_STRINGIFY(DB_SCHEMA_VERSION), NULL, NULL, NULL);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return QUADRATURE_OK;
}

// =============================================================================
// Lock Helpers
// =============================================================================

void
db_lock(quadrature_db_t *db)
{
    if (!db->readonly)
        pthread_mutex_lock(&db->lock);
}

void
db_unlock(quadrature_db_t *db)
{
    if (!db->readonly)
        pthread_mutex_unlock(&db->lock);
}

// =============================================================================
// Statement Management
// =============================================================================

typedef struct {
    size_t offset; /* offsetof(quadrature_db_t, stmt_field) */
    const char *sql;
} stmt_entry_t;

#define STMT(field, sql_text) { offsetof(struct quadrature_db, field), sql_text }

static const stmt_entry_t PREPARED_STMTS[] = {
    /* ── Artist writes ─────────────────────────────────────────────────── */
    STMT(insert_artist, "INSERT OR IGNORE INTO artists(name) VALUES(?)"),
    STMT(select_artist, "SELECT id FROM artists WHERE name=? COLLATE NOCASE"),
    STMT(insert_artist_fts, "INSERT OR REPLACE INTO artists_fts(rowid, name) VALUES(?,?)"),
    STMT(update_album_fts,
         "INSERT OR REPLACE INTO albums_fts(rowid, title, artist) "
         "SELECT al.id, al.title, COALESCE(ar.name,'') "
         "FROM albums al LEFT JOIN artists ar ON al.artist_id = ar.id WHERE al.id = ?"),
    STMT(insert_track_artist,
         "INSERT OR REPLACE INTO track_artists(track_id,artist_id,position,join_phrase) "
         "VALUES(?,?,?,?)"),
    STMT(delete_track_artists, "DELETE FROM track_artists WHERE track_id = ?"),

    /* ── Reconciler statements ─────────────────────────────────────────── */
    STMT(select_album_by_path, "SELECT id FROM albums WHERE path = ?"),
    STMT(insert_folder_album,
         "INSERT INTO albums(title, artist_id, path, year, is_compilation) "
         "VALUES(?, ?, ?, ?, ?)"),
    STMT(sync_album_tracks_fts,
         "INSERT OR REPLACE INTO tracks_fts(rowid, title, artist, album) "
         "SELECT t.id, t.title, COALESCE(t.artist_display,''), COALESCE(al.title,'') "
         "FROM tracks t JOIN albums al ON t.album_id = al.id WHERE t.album_id = ?"),

    /* ── MB artist resolution (db_get_or_create_artist) ─────────────── */
    STMT(select_artist_by_mb_id, "SELECT id FROM artists WHERE musicbrainz_id = ?"),
    STMT(update_artist_sort_name,
         "UPDATE artists SET sort_name = ? WHERE id = ? "
         "AND (sort_name IS NULL OR sort_name = '')"),
    STMT(select_artist_by_name_nocase,
         "SELECT id FROM artists WHERE name = ? COLLATE NOCASE "
         "AND (musicbrainz_id IS NULL OR musicbrainz_id = ?)"),
    STMT(insert_artist_mb, "INSERT INTO artists(name, musicbrainz_id, sort_name) VALUES(?, ?, ?)"),
    STMT(insert_artist_fts_replace, "INSERT OR REPLACE INTO artists_fts(rowid, name) VALUES(?,?)"),
    STMT(select_artist_normalized_no_mbid,
         "SELECT id FROM artists "
         "WHERE REPLACE(REPLACE(LOWER(name), ' ', ''), '-', '') = ? "
         "AND musicbrainz_id IS NULL"),
    STMT(rename_artist_mb,
         "UPDATE artists SET name = ?, musicbrainz_id = ?, sort_name = ? WHERE id = ?"),
    STMT(move_track_artists,
         "UPDATE OR IGNORE track_artists SET artist_id = ? WHERE artist_id = ?"),
    STMT(delete_track_artists_artist_id, "DELETE FROM track_artists WHERE artist_id = ?"),

    /* ── Duplicate artist merge ────────────────────────────────────────── */
    STMT(select_artist_by_name_and_mbid,
         "SELECT id FROM artists WHERE name = ? COLLATE NOCASE AND musicbrainz_id = ?"),
    STMT(delete_artist_fts, "DELETE FROM artists_fts WHERE rowid = ?"),
    STMT(delete_artist, "DELETE FROM artists WHERE id = ?"),

    /* ── Cached reads ──────────────────────────────────────────────────── */
    STMT(read_track_by_id, "SELECT " TRACK_SELECT_COLS TRACK_SELECT_FROM " WHERE t.id = ?"),
    STMT(read_album_by_id,
         "SELECT al.id, al.title, a.name, al.artist_id, al.year, "
         "  (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.id) AS track_count, "
         "  (SELECT GROUP_CONCAT(g, ';') FROM (SELECT DISTINCT LOWER(genre) AS g "
         "   FROM tracks WHERE album_id = al.id AND genre IS NOT NULL AND genre != '')) AS genres, "
         "  al.path, al.musicbrainz_release_id "
         "FROM albums al "
         "LEFT JOIN artists a ON al.artist_id = a.id "
         "WHERE al.id = ?"),
    STMT(read_tracks_by_album,
         "SELECT " TRACK_SELECT_COLS TRACK_SELECT_FROM " WHERE t.album_id = ?"
         " ORDER BY t.disc_num, t.track_num, t.title COLLATE NOCASE"),
    /* ── Warming iterators (no JOINs, sequential rowid scan) ───────────── */
    STMT(iter_all_artists, "SELECT a.id, a.name, a.musicbrainz_id FROM artists a ORDER BY a.id"),
    STMT(iter_all_albums,
         "SELECT al.id, al.title, al.artist_id, al.year, al.path, "
         "al.musicbrainz_release_id, al.musicbrainz_release_group_id "
         "FROM albums al ORDER BY al.id"),
    STMT(iter_all_tracks,
         "SELECT t.id, t.title, t.path, t.duration_ms, t.track_num, "
         "t.disc_num, t.year, t.album_id, t.genre, t.artist_display "
         "FROM tracks t ORDER BY t.id"),
    STMT(iter_all_track_artists,
         "SELECT ta.track_id, ta.artist_id, ta.join_phrase, ta.position "
         "FROM track_artists ta ORDER BY ta.track_id, ta.position"),
    STMT(get_max_ids,
         "SELECT (SELECT MAX(id) FROM artists),"
         "       (SELECT MAX(id) FROM albums),"
         "       (SELECT MAX(id) FROM tracks)"),

    /* ── Reconciler: fast-path status UPDATE ───────────────────────────── */
    STMT(update_album_mb_status,
         "UPDATE albums SET mb_status = ?, mb_resolved_at = ? WHERE id = ?"),

    /* ── Reconciler: batch loads via json_each(?) ──────────────────────── */
    STMT(reconcile_load_albums_batch,
         "SELECT id, path, title, artist_id, is_compilation, year, "
         "musicbrainz_release_id, musicbrainz_release_group_id, "
         "mb_status, mb_resolved_at "
         "FROM albums "
         "WHERE id IN (SELECT value FROM json_each(?))"),
    STMT(reconcile_load_tracks_batch,
         "SELECT id, album_id, path, title, track_num, disc_num, "
         "duration_ms, year, genre, artist_display, mtime "
         "FROM tracks "
         "WHERE album_id IN (SELECT value FROM json_each(?))"),
    STMT(reconcile_load_track_artists_batch,
         "SELECT ta.track_id, ta.artist_id, a.name, ta.join_phrase, ta.position "
         "FROM track_artists ta LEFT JOIN artists a ON a.id = ta.artist_id "
         "WHERE ta.track_id IN (SELECT value FROM json_each(?)) "
         "ORDER BY ta.track_id, ta.position"),

    /* ── Reconciler: canonical full-field writes ───────────────────────── */
    STMT(reconcile_update_album,
         "UPDATE albums SET "
         "  title = ?, artist_id = ?, is_compilation = ?, year = ?, "
         "  musicbrainz_release_id = ?, musicbrainz_release_group_id = ?, "
         "  mb_status = ?, mb_resolved_at = ? "
         "WHERE id = ?"),
    STMT(reconcile_update_track,
         "UPDATE tracks SET "
         "  title = ?, track_num = ?, disc_num = ?, duration_ms = ?, "
         "  year = ?, mtime = ?, genre = ?, artist_display = ? "
         "WHERE id = ?"),
    STMT(reconcile_insert_track,
         "INSERT INTO tracks(title, album_id, path, duration_ms, track_num, "
         "disc_num, mtime, year, genre, artist_display) "
         "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
    STMT(reconcile_delete_track_by_id, "DELETE FROM tracks WHERE id = ?"),
};

static inline sqlite3_stmt **
stmt_slot(quadrature_db_t *db, size_t offset)
{
    return (sqlite3_stmt **)((char *)db + offset);
}

void
db_prepare_stmts(quadrature_db_t *db)
{
    if (db->insert_artist)
        return; /* Already prepared */
    for (size_t i = 0; i < G_N_ELEMENTS(PREPARED_STMTS); i++) {
        const stmt_entry_t *e = &PREPARED_STMTS[i];
        sqlite3_prepare_v2(db->db, e->sql, -1, stmt_slot(db, e->offset), NULL);
    }
}

void
db_finalize_stmts(quadrature_db_t *db)
{
    for (size_t i = 0; i < G_N_ELEMENTS(PREPARED_STMTS); i++) {
        sqlite3_stmt **slot = stmt_slot(db, PREPARED_STMTS[i].offset);
        if (*slot) {
            sqlite3_finalize(*slot);
            *slot = NULL;
        }
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

static quadrature_result_t
db_open_rw(const char *path, quadrature_db_t **out)
{
    quadrature_db_t *db = g_new0(quadrature_db_t, 1);

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
        g_free(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (path) {
        db->db_path = g_strdup(path);
    }

    sqlite3_exec(db->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA cache_size=-64000", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL); /* 256MB mmap for reads */
    sqlite3_exec(db->db,
                 "PRAGMA wal_autocheckpoint=10000",
                 NULL,
                 NULL,
                 NULL); /* defer checkpoint until ~40MB burst */
    sqlite3_busy_timeout(db->db, 5000);
    sqlite3_exec(db->db, "PRAGMA optimize", NULL, NULL, NULL);

    quadrature_result_t res = db_init_schema(db->db);
    if (res != QUADRATURE_OK) {
        sqlite3_close(db->db);
        pthread_mutex_destroy(&db->lock);
        g_free(db->db_path);
        g_free(db);
        return res;
    }

    *out = db;
    return QUADRATURE_OK;
}

static quadrature_result_t
db_open_ro(const char *path, quadrature_db_t **out)
{
    quadrature_db_t *db = g_new0(quadrature_db_t, 1);

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
        g_free(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->db_path = g_strdup(path);
    db->readonly
        = true; /* Single-threaded read-only: skip app-level mutex, WAL handles isolation */

    /* Prevent writes while allowing full WAL read visibility.
     * journal_mode is inherited from the database file (already WAL from writer),
     * so no need to set it here. READWRITE + query_only gives us WAL read
     * access without the ability to modify data. */
    sqlite3_exec(db->db, "PRAGMA query_only = ON;", NULL, NULL, NULL);

    /* Performance PRAGMAs for read-heavy warming workload.
     * journal_mode inherited from DB file (already WAL from writer). */
    sqlite3_exec(db->db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA cache_size = -64000;", NULL, NULL, NULL);   /* 64MB page cache */
    sqlite3_exec(db->db, "PRAGMA mmap_size = 268435456;", NULL, NULL, NULL); /* 256MB mmap */
    sqlite3_busy_timeout(db->db, 5000);

    db_prepare_stmts(db);

    *out = db;
    return QUADRATURE_OK;
}

quadrature_result_t
db_open(const char *path, bool readonly, quadrature_db_t **out)
{
    if (!out)
        return QUADRATURE_ERROR_INVALID_PARAM;
    if (readonly && !path)
        return QUADRATURE_ERROR_INVALID_PARAM;
    return readonly ? db_open_ro(path, out) : db_open_rw(path, out);
}

void
db_close(quadrature_db_t *db)
{
    if (!db)
        return;

    db_finalize_stmts(db);
    sqlite3_close(db->db);
    pthread_mutex_destroy(&db->lock);
    g_free(db->db_path);
    g_free(db);
}

const char *
db_path(const quadrature_db_t *db)
{
    return db ? db->db_path : NULL;
}

// =============================================================================
// Transaction Operations
// =============================================================================

quadrature_result_t
db_begin_transaction(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    // Inside a batch transaction: increment depth, keep lock for serialization
    if (db->txn_depth > 0) {
        db->txn_depth++;
        return QUADRATURE_OK;
    }

    db_prepare_stmts(db);

    char *err = NULL;
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

quadrature_result_t
db_commit(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction)
        return QUADRATURE_ERROR_INTERNAL;

    // Inside a batch transaction: decrement depth, release lock for next worker
    if (db->txn_depth > 1) {
        db->txn_depth--;
        db_unlock(db);
        return QUADRATURE_OK;
    }

    char *err = NULL;
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

quadrature_result_t
db_rollback(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction)
        return QUADRATURE_ERROR_INTERNAL;

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

quadrature_result_t
db_begin_batch(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    db_prepare_stmts(db);

    char *err = NULL;
    int rc = sqlite3_exec(db->db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("Failed to begin batch transaction: %s", err);
        sqlite3_free(err);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    db->in_transaction = true;
    db->txn_depth = 1;
    db_unlock(db); // Release lock so workers can acquire it
    return QUADRATURE_OK;
}

quadrature_result_t
db_commit_batch(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    char *err = NULL;
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

quadrature_result_t
db_begin_read(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    sqlite3_exec(db->db, "BEGIN", NULL, NULL, NULL);
    return QUADRATURE_OK;
}

quadrature_result_t
db_end_read(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    sqlite3_exec(db->db, "END", NULL, NULL, NULL);
    return QUADRATURE_OK;
}

// =============================================================================
// WAL Checkpoint
// =============================================================================

quadrature_result_t
db_checkpoint(quadrature_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_wal_checkpoint_v2(db->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    db_unlock(db);

    return QUADRATURE_OK;
}
