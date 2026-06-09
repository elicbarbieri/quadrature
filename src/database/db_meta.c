/**
 * Metadata DB — MusicBrainz recording relations storage.
 *
 * Per-library quadrature-metadata.sqlite: recordings bridge table,
 * link_types, artists, and recording_links. Written by Phase 4,
 * read on-demand by the UI. See quadrature/metadata.h for the public API.
 */

#include "quadrature/metadata.h"
#include "quadrature/database.h"
#include "internal.h"
#include <sqlite3.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>

// =============================================================================
// Internal Types
// =============================================================================

struct quadrature_meta_db {
    sqlite3 *db;

    /* When non-NULL, this handle is ATTACHed to a main quadrature_db
     * connection as schema "meta". All writes share the main connection's
     * transaction. Lifecycle owned by caller; we DETACH (not close) on
     * db_meta_close. */
    quadrature_db_t *attached_to;

    // Cached prepared statements (lazy-initialized on first use)
    sqlite3_stmt *stmt_upsert_release;
    sqlite3_stmt *stmt_upsert_recording;
    sqlite3_stmt *stmt_upsert_link_type;
    sqlite3_stmt *stmt_upsert_artist;
    sqlite3_stmt *stmt_insert_recording_link;
    sqlite3_stmt *stmt_delete_recording_links;
};

/* Returns "meta." if attached, "" otherwise. */
static inline const char *
meta_schema_prefix(const quadrature_meta_db_t *db)
{
    return db->attached_to ? "meta." : "";
}

// =============================================================================
// Schema
// =============================================================================

static const char *META_SCHEMA_SQL =
    /* Bridge: (release_mbid, disc_num, track_num) → recording_mbid */
    "CREATE TABLE IF NOT EXISTS recordings ("
    "  recording_mbid  TEXT PRIMARY KEY,"
    "  release_mbid    TEXT NOT NULL,"
    "  disc_num        INTEGER NOT NULL,"
    "  track_num       INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS link_types ("
    "  link_type_gid  TEXT PRIMARY KEY,"
    "  name           TEXT NOT NULL,"
    "  description    TEXT"
    ");"

    "CREATE TABLE IF NOT EXISTS artists ("
    "  artist_mbid  TEXT PRIMARY KEY,"
    "  name         TEXT NOT NULL,"
    "  sort_name    TEXT,"
    "  artist_type  TEXT"
    ");"

    /* One row per l_artist_recording relationship */
    "CREATE TABLE IF NOT EXISTS recording_links ("
    "  id              INTEGER PRIMARY KEY,"
    "  recording_mbid  TEXT NOT NULL REFERENCES recordings(recording_mbid),"
    "  artist_mbid     TEXT NOT NULL REFERENCES artists(artist_mbid),"
    "  link_type_gid   TEXT NOT NULL REFERENCES link_types(link_type_gid),"
    "  entity0_credit  TEXT,"
    "  attributes      TEXT"
    ");"

    /* 95% path: all links for one recording */
    "CREATE INDEX IF NOT EXISTS idx_rl_recording"
    "  ON recording_links(recording_mbid);"

    /* 5% path: all recordings for a given artist, optionally by link type */
    "CREATE INDEX IF NOT EXISTS idx_rl_artist"
    "  ON recording_links(artist_mbid, link_type_gid);"

    /* Bridge lookup */
    "CREATE INDEX IF NOT EXISTS idx_recordings_position"
    "  ON recordings(release_mbid, disc_num, track_num);"

    /* Release-level metadata (date, type, label, genres) */
    "CREATE TABLE IF NOT EXISTS releases ("
    "  release_mbid    TEXT PRIMARY KEY,"
    "  release_date    TEXT,"
    "  release_type    TEXT,"
    "  label           TEXT,"
    "  catalog_number  TEXT,"
    "  barcode         TEXT,"
    "  genres          TEXT"
    ");"

    ;

// =============================================================================
// Pragma Application
// =============================================================================

static void
apply_meta_pragmas(sqlite3 *db)
{
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-131072", NULL, NULL, NULL);  /* 128MB page cache */
    sqlite3_exec(db, "PRAGMA mmap_size=134217728", NULL, NULL, NULL); /* 128MB mmap */
    /* FK enforcement skipped: we control insertion order (link_types + artists
     * before recording_links), so FK violations are structurally impossible.
     * Eliminates 3 FK index lookups per recording_link INSERT. */
    sqlite3_exec(db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);
}

// =============================================================================
// Lazy Statement Cache
// =============================================================================

/* Returns a cached prepared statement, preparing it on first use.
 * sql_fmt must contain a single %s placeholder for the schema prefix
 * (expands to "meta." when attached, "" otherwise). */
static sqlite3_stmt *
meta_get_stmt(quadrature_meta_db_t *db, sqlite3_stmt **slot, const char *sql_fmt)
{
    if (!*slot) {
        char *sql = sqlite3_mprintf(sql_fmt, meta_schema_prefix(db));
        int rc = sqlite3_prepare_v2(db->db, sql, -1, slot, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) {
            g_critical("meta_get_stmt: prepare failed: %s", sqlite3_errmsg(db->db));
            return NULL;
        }
    }
    return *slot;
}

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t
db_meta_open(const char *library_root, quadrature_meta_db_t **out)
{
    if (!library_root || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    char *path = g_build_filename(library_root, "quadrature-metadata.sqlite", NULL);

    sqlite3 *raw = NULL;
    int rc = sqlite3_open(path, &raw);
    g_free(path);

    if (rc != SQLITE_OK) {
        g_warning("db_meta_open: failed to open metadata DB: %s", sqlite3_errmsg(raw));
        sqlite3_close(raw);
        return QUADRATURE_ERROR_INTERNAL;
    }

    apply_meta_pragmas(raw);

    char *err = NULL;
    rc = sqlite3_exec(raw, META_SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("db_meta_open: schema creation failed: %s", err);
        sqlite3_free(err);
        sqlite3_close(raw);
        return QUADRATURE_ERROR_INTERNAL;
    }

    quadrature_meta_db_t *db = g_new0(quadrature_meta_db_t, 1);
    db->db = raw;
    *out = db;
    return QUADRATURE_OK;
}

quadrature_result_t
db_meta_open_readonly(const char *library_root, quadrature_meta_db_t **out)
{
    if (!library_root || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    char *path = g_build_filename(library_root, "quadrature-metadata.sqlite", NULL);

    /* Check existence before opening — callers need QUADRATURE_ERROR_FILE_NOT_FOUND
     * to distinguish "Phase 4 never ran" from a genuine open error. */
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    /* Open READWRITE (no CREATE) + query_only to get full WAL read visibility. */
    sqlite3 *raw = NULL;
    int rc = sqlite3_open_v2(path, &raw, SQLITE_OPEN_READWRITE, NULL);
    g_free(path);

    if (rc != SQLITE_OK) {
        g_warning("db_meta_open_readonly: failed: %s", sqlite3_errmsg(raw));
        sqlite3_close(raw);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_exec(raw, "PRAGMA query_only = ON;", NULL, NULL, NULL);
    sqlite3_busy_timeout(raw, 2000);

    quadrature_meta_db_t *db = g_new0(quadrature_meta_db_t, 1);
    db->db = raw;
    *out = db;
    return QUADRATURE_OK;
}

quadrature_result_t
db_meta_open_attached(quadrature_db_t *main_db,
                      const char *library_root,
                      quadrature_meta_db_t **out)
{
    if (!main_db || !library_root || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    /* First, ensure the meta file exists with schema applied (standalone open
     * + close). This also runs the per-file pragmas. */
    {
        quadrature_meta_db_t *tmp = NULL;
        quadrature_result_t res = db_meta_open(library_root, &tmp);
        if (res != QUADRATURE_OK)
            return res;
        db_meta_close(tmp);
    }

    char *path = g_build_filename(library_root, "quadrature-metadata.sqlite", NULL);
    char *sql = sqlite3_mprintf("ATTACH DATABASE %Q AS meta", path);
    g_free(path);

    db_lock(main_db);
    /* Idempotent ATTACH: detach first if already attached (e.g. on re-scan). */
    sqlite3_exec(main_db->db, "DETACH DATABASE meta", NULL, NULL, NULL);
    char *err = NULL;
    int rc = sqlite3_exec(main_db->db, sql, NULL, NULL, &err);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
        g_critical("db_meta_open_attached: ATTACH failed: %s", err ? err : "?");
        sqlite3_free(err);
        db_unlock(main_db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    /* Meta-DB pragmas on the attached schema (connection-scoped pragmas like
     * cache_size/mmap_size are set on the main connection already). */
    sqlite3_exec(main_db->db, "PRAGMA meta.synchronous=OFF", NULL, NULL, NULL);
    db_unlock(main_db);

    quadrature_meta_db_t *db = g_new0(quadrature_meta_db_t, 1);
    db->db = main_db->db;
    db->attached_to = main_db;
    *out = db;
    return QUADRATURE_OK;
}

void
db_meta_close(quadrature_meta_db_t *db)
{
    if (!db)
        return;

    // Finalize all cached prepared statements
    sqlite3_stmt **stmts[] = {
        &db->stmt_upsert_release, &db->stmt_upsert_recording,      &db->stmt_upsert_link_type,
        &db->stmt_upsert_artist,  &db->stmt_insert_recording_link, &db->stmt_delete_recording_links,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(stmts); i++) {
        if (*stmts[i]) {
            sqlite3_finalize(*stmts[i]);
            *stmts[i] = NULL;
        }
    }

    if (db->attached_to) {
        /* Detach, but leave the main connection open. */
        db_lock(db->attached_to);
        sqlite3_exec(db->attached_to->db, "DETACH DATABASE meta", NULL, NULL, NULL);
        db_unlock(db->attached_to);
    } else {
        sqlite3_close(db->db);
    }
    g_free(db);
}

// =============================================================================
// Transactions
// =============================================================================

quadrature_result_t
db_meta_begin(quadrature_meta_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    /* Attached: main db owns the transaction — no-op. */
    if (db->attached_to)
        return QUADRATURE_OK;
    char *err = NULL;
    int rc = sqlite3_exec(db->db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("db_meta_begin: %s", err);
        sqlite3_free(err);
        return QUADRATURE_ERROR_INTERNAL;
    }
    return QUADRATURE_OK;
}

quadrature_result_t
db_meta_commit(quadrature_meta_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    if (db->attached_to)
        return QUADRATURE_OK;
    char *err = NULL;
    int rc = sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("db_meta_commit: %s", err);
        sqlite3_free(err);
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        return QUADRATURE_ERROR_INTERNAL;
    }
    return QUADRATURE_OK;
}

// =============================================================================
// Write Operations
// =============================================================================

quadrature_result_t
db_meta_upsert_recording(quadrature_meta_db_t *db,
                         const char *recording_mbid,
                         const char *release_mbid,
                         int disc_num,
                         int track_num)
{
    if (!db || !recording_mbid || !release_mbid)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt = meta_get_stmt(
        db,
        &db->stmt_upsert_recording,
        "INSERT OR REPLACE INTO %srecordings(recording_mbid, release_mbid, disc_num, track_num)"
        " VALUES(?,?,?,?)");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, recording_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, release_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, disc_num);
    sqlite3_bind_int(stmt, 4, track_num);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t
db_meta_upsert_link_type(quadrature_meta_db_t *db,
                         const char *link_type_gid,
                         const char *name,
                         const char *description)
{
    if (!db || !link_type_gid || !name)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt
        = meta_get_stmt(db,
                        &db->stmt_upsert_link_type,
                        "INSERT OR REPLACE INTO %slink_types(link_type_gid, name, description)"
                        " VALUES(?,?,?)");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, link_type_gid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    if (description)
        sqlite3_bind_text(stmt, 3, description, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t
db_meta_upsert_artist(quadrature_meta_db_t *db,
                      const char *artist_mbid,
                      const char *name,
                      const char *sort_name,
                      const char *artist_type)
{
    if (!db || !artist_mbid || !name)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt = meta_get_stmt(
        db,
        &db->stmt_upsert_artist,
        "INSERT OR REPLACE INTO %sartists(artist_mbid, name, sort_name, artist_type)"
        " VALUES(?,?,?,?)");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, artist_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    if (sort_name)
        sqlite3_bind_text(stmt, 3, sort_name, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    if (artist_type)
        sqlite3_bind_text(stmt, 4, artist_type, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t
db_meta_insert_recording_link(quadrature_meta_db_t *db,
                              const char *recording_mbid,
                              const char *artist_mbid,
                              const char *link_type_gid,
                              const char *entity0_credit,
                              const char *attributes)
{
    if (!db || !recording_mbid || !artist_mbid || !link_type_gid)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt
        = meta_get_stmt(db,
                        &db->stmt_insert_recording_link,
                        "INSERT INTO %srecording_links"
                        "(recording_mbid, artist_mbid, link_type_gid, entity0_credit, attributes)"
                        " VALUES(?,?,?,?,?)");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, recording_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, artist_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, link_type_gid, -1, SQLITE_STATIC);
    if (entity0_credit)
        sqlite3_bind_text(stmt, 4, entity0_credit, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    if (attributes)
        sqlite3_bind_text(stmt, 5, attributes, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t
db_meta_delete_recording_links(quadrature_meta_db_t *db, const char *recording_mbid)
{
    if (!db || !recording_mbid)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt = meta_get_stmt(db,
                                       &db->stmt_delete_recording_links,
                                       "DELETE FROM %srecording_links WHERE recording_mbid = ?");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, recording_mbid, -1, SQLITE_STATIC);

    sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return QUADRATURE_OK;
}

// =============================================================================
// Read Operations
// =============================================================================

quadrature_result_t
db_meta_get_recording_mbid(
    quadrature_meta_db_t *db, const char *release_mbid, int disc_num, int track_num, char **out)
{
    if (!db || !release_mbid || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db,
                       "SELECT recording_mbid FROM recordings"
                       " WHERE release_mbid=? AND disc_num=? AND track_num=?",
                       -1,
                       &stmt,
                       NULL);
    sqlite3_bind_text(stmt, 1, release_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, disc_num);
    sqlite3_bind_int(stmt, 3, track_num);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        *out = val ? g_strdup(val) : NULL;
        res = QUADRATURE_OK;
    }
    sqlite3_finalize(stmt);
    return res;
}

quadrature_result_t
db_meta_get_links(quadrature_meta_db_t *db,
                  const char *recording_mbid,
                  db_meta_link_t **out,
                  size_t *count)
{
    if (!db || !recording_mbid || !out || !count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db,
                       "SELECT lt.link_type_gid, lt.name,"
                       "       a.artist_mbid, a.name, a.sort_name, a.artist_type,"
                       "       rl.entity0_credit, rl.attributes"
                       " FROM recording_links rl"
                       " JOIN link_types lt ON lt.link_type_gid = rl.link_type_gid"
                       " JOIN artists    a  ON a.artist_mbid     = rl.artist_mbid"
                       " WHERE rl.recording_mbid = ?"
                       " ORDER BY lt.name, a.sort_name",
                       -1,
                       &stmt,
                       NULL);
    sqlite3_bind_text(stmt, 1, recording_mbid, -1, SQLITE_STATIC);

    GArray *rows = g_array_new(FALSE, TRUE, sizeof(db_meta_link_t));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_meta_link_t row = { 0 };
        const char *v;

        v = (const char *)sqlite3_column_text(stmt, 0);
        row.link_type_gid = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 1);
        row.link_type_name = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 2);
        row.artist_mbid = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 3);
        row.artist_name = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 4);
        row.artist_sort_name = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 5);
        row.artist_type = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 6);
        row.entity0_credit = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 7);
        row.attributes = v ? g_strdup(v) : NULL;

        g_array_append_val(rows, row);
    }
    sqlite3_finalize(stmt);

    *count = rows->len;
    *out = (db_meta_link_t *)g_array_free(rows, FALSE);
    return QUADRATURE_OK;
}

quadrature_result_t
db_meta_upsert_release(quadrature_meta_db_t *db,
                       const char *release_mbid,
                       const char *release_date,
                       const char *release_type,
                       const char *label,
                       const char *catalog_number,
                       const char *barcode,
                       const char *genres)
{
    if (!db || !release_mbid)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt = meta_get_stmt(
        db,
        &db->stmt_upsert_release,
        "INSERT OR REPLACE INTO %sreleases"
        "(release_mbid, release_date, release_type, label, catalog_number, barcode, genres)"
        " VALUES(?,?,?,?,?,?,?)");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    int p = 1;
    sqlite3_bind_text(stmt, p++, release_mbid, -1, SQLITE_STATIC);

    if (release_date)
        sqlite3_bind_text(stmt, p++, release_date, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, p++);
    if (release_type)
        sqlite3_bind_text(stmt, p++, release_type, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, p++);
    if (label)
        sqlite3_bind_text(stmt, p++, label, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, p++);
    if (catalog_number)
        sqlite3_bind_text(stmt, p++, catalog_number, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, p++);
    if (barcode)
        sqlite3_bind_text(stmt, p++, barcode, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, p++);
    if (genres)
        sqlite3_bind_text(stmt, p++, genres, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, p++);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t
db_meta_get_release(quadrature_meta_db_t *db, const char *release_mbid, db_meta_release_t **out)
{
    if (!db || !release_mbid || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db->db,
        "SELECT release_date, release_type, label, catalog_number, barcode, genres"
        " FROM releases WHERE release_mbid = ?",
        -1,
        &stmt,
        NULL);

    /* Table may not exist in older metadata DBs — treat as not found */
    if (rc != SQLITE_OK)
        return QUADRATURE_ERROR_FILE_NOT_FOUND;

    sqlite3_bind_text(stmt, 1, release_mbid, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    db_meta_release_t *rel = g_new0(db_meta_release_t, 1);
    const char *v;
    v = (const char *)sqlite3_column_text(stmt, 0);
    rel->release_date = v ? g_strdup(v) : NULL;
    v = (const char *)sqlite3_column_text(stmt, 1);
    rel->release_type = v ? g_strdup(v) : NULL;
    v = (const char *)sqlite3_column_text(stmt, 2);
    rel->label = v ? g_strdup(v) : NULL;
    v = (const char *)sqlite3_column_text(stmt, 3);
    rel->catalog_number = v ? g_strdup(v) : NULL;
    v = (const char *)sqlite3_column_text(stmt, 4);
    rel->barcode = v ? g_strdup(v) : NULL;
    v = (const char *)sqlite3_column_text(stmt, 5);
    rel->genres = v ? g_strdup(v) : NULL;

    sqlite3_finalize(stmt);
    *out = rel;
    return QUADRATURE_OK;
}

void
db_meta_release_free(db_meta_release_t *release)
{
    if (!release)
        return;
    g_free(release->release_date);
    g_free(release->release_type);
    g_free(release->label);
    g_free(release->catalog_number);
    g_free(release->barcode);
    g_free(release->genres);
    g_free(release);
}

void
db_meta_links_free(db_meta_link_t *links, size_t count)
{
    if (!links)
        return;
    for (size_t i = 0; i < count; i++) {
        g_free(links[i].link_type_gid);
        g_free(links[i].link_type_name);
        g_free(links[i].artist_mbid);
        g_free(links[i].artist_name);
        g_free(links[i].artist_sort_name);
        g_free(links[i].artist_type);
        g_free(links[i].entity0_credit);
        g_free(links[i].attributes);
    }
    g_free(links);
}

// =============================================================================
// Credit Bridge Queries (artist-centric)
// =============================================================================

quadrature_result_t
db_meta_get_credits_by_artist(quadrature_meta_db_t *db,
                              const char *artist_mbid,
                              const char *link_type_gid_filter,
                              db_meta_artist_credit_t **out,
                              size_t *count)
{
    if (!db || !artist_mbid || !out || !count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    sqlite3_stmt *stmt = NULL;
    if (link_type_gid_filter) {
        sqlite3_prepare_v2(db->db,
                           "SELECT r.release_mbid, r.disc_num, r.track_num,"
                           "       lt.name, rl.attributes"
                           " FROM recording_links rl"
                           " JOIN recordings r ON r.recording_mbid = rl.recording_mbid"
                           " JOIN link_types lt ON lt.link_type_gid = rl.link_type_gid"
                           " WHERE rl.artist_mbid = ? AND rl.link_type_gid = ?"
                           " ORDER BY r.release_mbid, r.disc_num, r.track_num",
                           -1,
                           &stmt,
                           NULL);
        sqlite3_bind_text(stmt, 1, artist_mbid, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, link_type_gid_filter, -1, SQLITE_STATIC);
    } else {
        sqlite3_prepare_v2(db->db,
                           "SELECT r.release_mbid, r.disc_num, r.track_num,"
                           "       lt.name, rl.attributes"
                           " FROM recording_links rl"
                           " JOIN recordings r ON r.recording_mbid = rl.recording_mbid"
                           " JOIN link_types lt ON lt.link_type_gid = rl.link_type_gid"
                           " WHERE rl.artist_mbid = ?"
                           " ORDER BY r.release_mbid, r.disc_num, r.track_num",
                           -1,
                           &stmt,
                           NULL);
        sqlite3_bind_text(stmt, 1, artist_mbid, -1, SQLITE_STATIC);
    }

    GArray *rows = g_array_new(FALSE, TRUE, sizeof(db_meta_artist_credit_t));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_meta_artist_credit_t row = { 0 };
        const char *v;

        v = (const char *)sqlite3_column_text(stmt, 0);
        row.release_mbid = v ? g_strdup(v) : NULL;
        row.disc_num = sqlite3_column_int(stmt, 1);
        row.track_num = sqlite3_column_int(stmt, 2);
        v = (const char *)sqlite3_column_text(stmt, 3);
        row.link_type_name = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 4);
        row.attributes = v ? g_strdup(v) : NULL;

        g_array_append_val(rows, row);
    }
    sqlite3_finalize(stmt);

    *count = rows->len;
    *out = (db_meta_artist_credit_t *)g_array_free(rows, FALSE);
    return QUADRATURE_OK;
}

void
db_meta_artist_credits_free(db_meta_artist_credit_t *credits, size_t count)
{
    if (!credits)
        return;
    for (size_t i = 0; i < count; i++) {
        g_free(credits[i].release_mbid);
        g_free(credits[i].link_type_name);
        g_free(credits[i].attributes);
    }
    g_free(credits);
}

quadrature_result_t
db_meta_search_artists(quadrature_meta_db_t *db,
                       const char *query,
                       size_t limit,
                       db_meta_artist_search_result_t **out,
                       size_t *count)
{
    if (!db || !query || !out || !count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    if (!query[0])
        return QUADRATURE_OK;

    char *pattern = g_strdup_printf("%%%s%%", query);

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db,
                       "SELECT artist_mbid, name, sort_name, artist_type"
                       " FROM artists"
                       " WHERE name LIKE ? COLLATE NOCASE"
                       " ORDER BY name COLLATE NOCASE"
                       " LIMIT ?",
                       -1,
                       &stmt,
                       NULL);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, limit > 0 ? (int64_t)limit : 50);

    GArray *rows = g_array_new(FALSE, TRUE, sizeof(db_meta_artist_search_result_t));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_meta_artist_search_result_t row = { 0 };
        const char *v;

        v = (const char *)sqlite3_column_text(stmt, 0);
        row.artist_mbid = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 1);
        row.name = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 2);
        row.sort_name = v ? g_strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 3);
        row.artist_type = v ? g_strdup(v) : NULL;

        g_array_append_val(rows, row);
    }
    sqlite3_finalize(stmt);
    g_free(pattern);

    *count = rows->len;
    *out = (db_meta_artist_search_result_t *)g_array_free(rows, FALSE);
    return QUADRATURE_OK;
}

void
db_meta_artist_search_results_free(db_meta_artist_search_result_t *results, size_t count)
{
    if (!results)
        return;
    for (size_t i = 0; i < count; i++) {
        g_free(results[i].artist_mbid);
        g_free(results[i].name);
        g_free(results[i].sort_name);
        g_free(results[i].artist_type);
    }
    g_free(results);
}

// =============================================================================
// Maintenance
// =============================================================================

quadrature_result_t
db_meta_checkpoint(quadrature_meta_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    sqlite3_wal_checkpoint_v2(db->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    return QUADRATURE_OK;
}
