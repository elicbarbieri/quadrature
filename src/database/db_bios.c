/**
 * Bios DB — Artist biographies (Wikipedia summaries).
 *
 * Per-library quadrature-bios.sqlite: standalone database holding artist
 * biographies fetched from Wikipedia via Wikidata (indexer Phase 8).
 *
 * Separated from quadrature-metadata.sqlite so that deleting the metadata DB
 * (to force MusicBrainz re-resolve) does not destroy expensive-to-refetch bios.
 *
 * On first open, auto-migrates existing bios from the metadata DB if present.
 */

#include "quadrature/metadata.h"
#include <sqlite3.h>
#include <glib.h>
#include <string.h>

// =============================================================================
// Internal Types
// =============================================================================

struct quadrature_bios_db {
    sqlite3 *db;
    sqlite3_stmt *stmt_upsert;
};

// =============================================================================
// Schema
// =============================================================================

static const char *BIOS_SCHEMA_SQL = "CREATE TABLE IF NOT EXISTS artist_bios ("
                                     "  artist_mbid  TEXT PRIMARY KEY,"
                                     "  bio_text     TEXT NOT NULL,"
                                     "  wiki_url     TEXT"
                                     ");";

// =============================================================================
// Pragma Application
// =============================================================================

static void
apply_bios_pragmas(sqlite3 *db)
{
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);
}

// =============================================================================
// Auto-migration from metadata DB
// =============================================================================

/**
 * One-time migration: if quadrature-metadata.sqlite exists and has artist_bios
 * rows, copy them into the new bios DB. Runs only when the bios DB is empty.
 */
static void
migrate_from_metadata_db(sqlite3 *bios_db, const char *library_root)
{
    /* Only migrate if bios DB is empty */
    sqlite3_stmt *count_stmt = NULL;
    sqlite3_prepare_v2(bios_db, "SELECT COUNT(*) FROM artist_bios", -1, &count_stmt, NULL);
    if (!count_stmt)
        return;

    int64_t existing = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW)
        existing = sqlite3_column_int64(count_stmt, 0);
    sqlite3_finalize(count_stmt);

    if (existing > 0)
        return; /* Already populated */

    /* Check if metadata DB exists */
    char *meta_path = g_build_filename(library_root, "quadrature-metadata.sqlite", NULL);
    if (!g_file_test(meta_path, G_FILE_TEST_EXISTS)) {
        g_free(meta_path);
        return;
    }

    /* ATTACH metadata DB and copy rows */
    char *attach_sql = g_strdup_printf("ATTACH DATABASE '%s' AS meta", meta_path);
    g_free(meta_path);

    char *err = NULL;
    int rc = sqlite3_exec(bios_db, attach_sql, NULL, NULL, &err);
    g_free(attach_sql);

    if (rc != SQLITE_OK) {
        g_debug("db_bios: could not attach metadata DB for migration: %s", err ? err : "unknown");
        sqlite3_free(err);
        return;
    }

    /* Check if artist_bios table exists in metadata DB */
    sqlite3_stmt *check_stmt = NULL;
    rc = sqlite3_prepare_v2(bios_db,
                            "SELECT COUNT(*) FROM meta.sqlite_master "
                            "WHERE type='table' AND name='artist_bios'",
                            -1,
                            &check_stmt,
                            NULL);

    bool has_table = false;
    if (rc == SQLITE_OK && sqlite3_step(check_stmt) == SQLITE_ROW)
        has_table = sqlite3_column_int(check_stmt, 0) > 0;
    if (check_stmt)
        sqlite3_finalize(check_stmt);

    if (has_table) {
        rc = sqlite3_exec(bios_db,
                          "INSERT OR IGNORE INTO artist_bios "
                          "SELECT artist_mbid, bio_text, wiki_url FROM meta.artist_bios",
                          NULL,
                          NULL,
                          &err);

        if (rc == SQLITE_OK) {
            int migrated = sqlite3_changes(bios_db);
            if (migrated > 0)
                g_message("db_bios: migrated %d artist bios from metadata DB", migrated);
        } else {
            g_debug("db_bios: migration query failed: %s", err ? err : "unknown");
            sqlite3_free(err);
        }
    }

    sqlite3_exec(bios_db, "DETACH DATABASE meta", NULL, NULL, NULL);
}

// =============================================================================
// Lazy Statement Cache
// =============================================================================

static sqlite3_stmt *
bios_get_stmt(quadrature_bios_db_t *db, sqlite3_stmt **slot, const char *sql)
{
    if (!*slot) {
        int rc = sqlite3_prepare_v2(db->db, sql, -1, slot, NULL);
        if (rc != SQLITE_OK) {
            g_critical("bios_get_stmt: prepare failed: %s", sqlite3_errmsg(db->db));
            return NULL;
        }
    }
    return *slot;
}

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t
db_bios_open(const char *library_root, quadrature_bios_db_t **out)
{
    if (!library_root || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    char *path = g_build_filename(library_root, "quadrature-bios.sqlite", NULL);

    sqlite3 *raw = NULL;
    int rc = sqlite3_open(path, &raw);
    g_free(path);

    if (rc != SQLITE_OK) {
        g_warning("db_bios_open: failed to open bios DB: %s", sqlite3_errmsg(raw));
        sqlite3_close(raw);
        return QUADRATURE_ERROR_INTERNAL;
    }

    apply_bios_pragmas(raw);

    char *err = NULL;
    rc = sqlite3_exec(raw, BIOS_SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("db_bios_open: schema creation failed: %s", err);
        sqlite3_free(err);
        sqlite3_close(raw);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Auto-migrate from metadata DB on first open */
    migrate_from_metadata_db(raw, library_root);

    quadrature_bios_db_t *db = g_new0(quadrature_bios_db_t, 1);
    db->db = raw;
    *out = db;
    return QUADRATURE_OK;
}

quadrature_result_t
db_bios_open_readonly(const char *library_root, quadrature_bios_db_t **out)
{
    if (!library_root || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    char *path = g_build_filename(library_root, "quadrature-bios.sqlite", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    sqlite3 *raw = NULL;
    int rc = sqlite3_open_v2(path, &raw, SQLITE_OPEN_READWRITE, NULL);
    g_free(path);

    if (rc != SQLITE_OK) {
        g_warning("db_bios_open_readonly: failed: %s", sqlite3_errmsg(raw));
        sqlite3_close(raw);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_exec(raw, "PRAGMA query_only = ON;", NULL, NULL, NULL);
    sqlite3_busy_timeout(raw, 2000);

    quadrature_bios_db_t *db = g_new0(quadrature_bios_db_t, 1);
    db->db = raw;
    *out = db;
    return QUADRATURE_OK;
}

void
db_bios_close(quadrature_bios_db_t *db)
{
    if (!db)
        return;
    if (db->stmt_upsert) {
        sqlite3_finalize(db->stmt_upsert);
        db->stmt_upsert = NULL;
    }
    sqlite3_close(db->db);
    g_free(db);
}

// =============================================================================
// Transactions
// =============================================================================

quadrature_result_t
db_bios_begin(quadrature_bios_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    char *err = NULL;
    int rc = sqlite3_exec(db->db, "BEGIN IMMEDIATE", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("db_bios_begin: %s", err);
        sqlite3_free(err);
        return QUADRATURE_ERROR_INTERNAL;
    }
    return QUADRATURE_OK;
}

quadrature_result_t
db_bios_commit(quadrature_bios_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    char *err = NULL;
    int rc = sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        g_critical("db_bios_commit: %s", err);
        sqlite3_free(err);
        sqlite3_exec(db->db, "ROLLBACK", NULL, NULL, NULL);
        return QUADRATURE_ERROR_INTERNAL;
    }
    return QUADRATURE_OK;
}

quadrature_result_t
db_bios_checkpoint(quadrature_bios_db_t *db)
{
    if (!db)
        return QUADRATURE_ERROR_INVALID_PARAM;
    sqlite3_wal_checkpoint_v2(db->db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    return QUADRATURE_OK;
}

// =============================================================================
// Write Operations
// =============================================================================

quadrature_result_t
db_bios_upsert(quadrature_bios_db_t *db,
               const char *artist_mbid,
               const char *bio_text,
               const char *wiki_url)
{
    if (!db || !artist_mbid || !bio_text)
        return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt *stmt
        = bios_get_stmt(db,
                        &db->stmt_upsert,
                        "INSERT OR REPLACE INTO artist_bios(artist_mbid, bio_text, wiki_url)"
                        " VALUES(?,?,?)");
    if (!stmt)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, artist_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, bio_text, -1, SQLITE_STATIC);
    if (wiki_url)
        sqlite3_bind_text(stmt, 3, wiki_url, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// Read Operations
// =============================================================================

quadrature_result_t
db_bios_get(quadrature_bios_db_t *db,
            const char *artist_mbid,
            char **bio_text_out,
            char **wiki_url_out)
{
    if (!db || !artist_mbid || !bio_text_out || !wiki_url_out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *bio_text_out = NULL;
    *wiki_url_out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
                                "SELECT bio_text, wiki_url FROM artist_bios WHERE artist_mbid = ?",
                                -1,
                                &stmt,
                                NULL);
    if (rc != SQLITE_OK)
        return QUADRATURE_ERROR_FILE_NOT_FOUND;

    sqlite3_bind_text(stmt, 1, artist_mbid, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    const char *v;
    v = (const char *)sqlite3_column_text(stmt, 0);
    *bio_text_out = v ? g_strdup(v) : NULL;
    v = (const char *)sqlite3_column_text(stmt, 1);
    *wiki_url_out = v ? g_strdup(v) : NULL;

    sqlite3_finalize(stmt);
    return QUADRATURE_OK;
}

quadrature_result_t
db_bios_exists(quadrature_bios_db_t *db, const char *artist_mbid, bool *exists_out)
{
    if (!db || !artist_mbid || !exists_out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *exists_out = false;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db->db, "SELECT 1 FROM artist_bios WHERE artist_mbid = ? LIMIT 1", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_text(stmt, 1, artist_mbid, -1, SQLITE_STATIC);

    *exists_out = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return QUADRATURE_OK;
}
