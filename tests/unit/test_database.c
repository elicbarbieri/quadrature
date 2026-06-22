#include <criterion/criterion.h>
#include "test_helpers.h"
#include "quadrature/indexer.h"
#include <pthread.h>

#define TEST_DB_FILE "test_database.db"

// Local helper: count tracks via direct SQL (db_get_track_count was removed)
static quadrature_result_t
test_get_track_count(quadrature_db_t *db, size_t *out)
{
    if (!db || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM tracks", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    } else {
        *out = 0;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

// ============================================================================
// Null Safety and Parameter Validation
// ============================================================================

Test(database, null_safety_and_validation)
{
    db_track_t *track = NULL;

    // --- Null database handle ---
    cr_assert_eq(db_open(NULL, false, NULL), QUADRATURE_ERROR_INVALID_PARAM);
    db_close(NULL); // Should not crash
    cr_assert_null(db_path(NULL));

    // Free functions accept null safely
    db_tracks_free(NULL, 1);

    // Operations with null database
    cr_assert_eq(db_get_track(NULL, 1, &track), QUADRATURE_ERROR_INVALID_PARAM);

    // --- Null parameters with valid database ---
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    cr_assert_eq(db_get_track(db, 1, NULL), QUADRATURE_ERROR_INVALID_PARAM);

    db_close(db);
}

// ============================================================================
// Lifecycle (Memory and File-based)
// ============================================================================

Test(database, lifecycle)
{
    quadrature_db_t *db = NULL;

    // --- In-memory database ---
    cr_assert_eq(db_open(NULL, false, &db), QUADRATURE_OK);
    cr_assert_not_null(db);
    cr_assert_null(db_path(db));
    db_close(db);

    // --- File-based database ---
    db = NULL;
    cr_assert_eq(db_open(TEST_DB_FILE, false, &db), QUADRATURE_OK);
    cr_assert_not_null(db);
    cr_assert_str_eq(db_path(db), TEST_DB_FILE);

    // Verify empty on creation
    size_t count = 999;
    test_get_track_count(db, &count);
    cr_assert_eq(count, 0);
    db_close(db);

    // File should exist
    cr_assert_eq(access(TEST_DB_FILE, F_OK), 0);

    // Reopen and verify persistence
    db = NULL;
    cr_assert_eq(db_open(TEST_DB_FILE, false, &db), QUADRATURE_OK);
    count = 999;
    test_get_track_count(db, &count);
    cr_assert_eq(count, 0);
    db_close(db);

    // Cleanup
    unlink(TEST_DB_FILE);
    unlink(TEST_DB_FILE "-wal");
    unlink(TEST_DB_FILE "-shm");
}

// ============================================================================
// Empty Database Operations
// ============================================================================

Test(database, empty_operations)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    size_t count = 999;
    db_track_t *track = NULL;

    // Track count is zero
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 0);

    // Get nonexistent track
    cr_assert_eq(db_get_track(db, 1, &track), QUADRATURE_ERROR_FILE_NOT_FOUND);
    cr_assert_eq(db_get_track(db, -1, &track), QUADRATURE_ERROR_FILE_NOT_FOUND);

    db_close(db);
}

// ============================================================================
// Transaction Rollback
// ============================================================================

Test(database, transaction_rollback)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t artist_id = test_goc_artist(db, "Test Artist", NULL, NULL);
    int64_t album_id = test_insert_album(db, "/music", "Album", artist_id, 2024);
    test_insert_track_full(
        db, album_id, "/music/committed.mp3", "Committed", 1, 1, 100000, NULL, NULL, NULL, 0);

    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 1);

    // Start a transaction, write a track via raw SQL, then rollback.
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    db_lock(db);
    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db->db,
                       "INSERT INTO tracks(title, album_id, path, duration_ms, track_num, "
                       "disc_num, mtime, year) VALUES('Should Not Exist', ?, "
                       "'/music/rolled_back.mp3', 100000, 2, 1, 2000, 2024)",
                       -1,
                       &ins,
                       NULL);
    sqlite3_bind_int64(ins, 1, album_id);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    db_unlock(db);
    cr_assert_eq(db_rollback(db), QUADRATURE_OK);

    // Count should still be 1
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 1);

    db_close(db);
}

// ============================================================================
// Artist Identity — In-Place Rename & Deduplication
// ============================================================================

/* Helper: count rows in artists table directly via SQL */
static size_t
count_artists(quadrature_db_t *db)
{
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM artists", -1, &stmt, NULL);
    sqlite3_step(stmt);
    size_t n = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return n;
}

/* Phase 2 creates "2 Mex" (space variant).  Phase 4 resolves "2Mex" via MB.
 * Expected: rename in-place — same artist_id returned, no new row. */
Test(database, artist_normalized_rename_preserves_id)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    /* Phase 2: create via simple name (no MBID) */
    int64_t id_phase2 = test_goc_artist(db, "2 Mex", NULL, NULL);
    cr_assert_gt(id_phase2, 0, "Phase 2 artist should be created");
    cr_assert_eq(count_artists(db), 1, "Should have exactly 1 artist");

    /* Phase 4: resolve via MB — name differs by space but normalizes the same */
    int64_t id_phase4 = test_goc_artist(db, "2Mex", "2Mex", "696c9bcb-1234-5678-abcd-000000000001");
    cr_assert_gt(id_phase4, 0, "MB artist should be found/created");
    cr_assert_eq(
        id_phase4, id_phase2, "Normalized rename should return original id (no orphan created)");
    cr_assert_eq(count_artists(db), 1, "Still exactly 1 artist after normalized rename");

    db_close(db);
}

/* Phase 4 called twice with the same MBID should return the same id. */
Test(database, artist_mb_lookup_by_mbid_is_idempotent)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t id1 = test_goc_artist(db, "Madlib", "Madlib", "some-mbid-aaa");
    int64_t id2 = test_goc_artist(db, "Madlib", "Madlib", "some-mbid-aaa");
    cr_assert_eq(id1, id2, "Same MBID should always return the same artist_id");
    cr_assert_eq(count_artists(db), 1, "One artist row for repeated MBID lookup");

    db_close(db);
}

/* Step 2: exact name match (case-insensitive) with no MBID should rename in-place. */
Test(database, artist_mb_exact_nocase_rename)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t id_phase2 = test_goc_artist(db, "ac/dc", NULL, NULL);
    cr_assert_gt(id_phase2, 0);

    int64_t id_mb = test_goc_artist(db, "AC/DC", "AC/DC", "mbid-acdc-001");
    cr_assert_eq(id_mb, id_phase2, "NOCASE rename should preserve artist_id");
    cr_assert_eq(count_artists(db), 1);

    db_close(db);
}

/* db_prune_orphan_artists must delete artists that have no track_artists rows. */
Test(database, prune_orphan_artists_removes_unreferenced)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    /* Create two artists */
    int64_t orphan_id = test_goc_artist(db, "OrphanArtist", NULL, NULL);
    int64_t linked_id = test_goc_artist(db, "LinkedArtist", NULL, NULL);
    cr_assert_gt(orphan_id, 0);
    cr_assert_gt(linked_id, 0);

    /* Create an album and a track linked to linked_id only */
    int64_t album_id = test_insert_album(db, "/music", "Album", linked_id, 2020);
    const int64_t ta[1] = { linked_id };
    const char *names[1] = { "LinkedArtist" };
    const char *joins[1] = { "" };
    test_insert_track_full(
        db, album_id, "/music/track.mp3", "Track", 1, 1, 60000, ta, names, joins, 1);

    cr_assert_eq(count_artists(db), 2, "Both artists exist before prune");

    quadrature_result_t res = db_prune_orphan_artists(db);
    cr_assert_eq(res, QUADRATURE_OK);

    cr_assert_eq(count_artists(db), 1, "Orphan artist removed after prune");

    /* Verify the right one survived */
    db_lock(db);
    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db->db, "SELECT id FROM artists LIMIT 1", -1, &chk, NULL);
    sqlite3_step(chk);
    int64_t survivor = sqlite3_column_int64(chk, 0);
    sqlite3_finalize(chk);
    db_unlock(db);
    cr_assert_eq(survivor, linked_id, "Linked artist should survive prune");

    db_close(db);
}

/* An artist referenced only via albums.artist_id (no track_artists) must survive prune. */
Test(database, prune_orphan_artists_preserves_album_artist)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t orphan_id = test_goc_artist(db, "TrueOrphan", NULL, NULL);
    int64_t album_artist_id = test_goc_artist(db, "AlbumOnlyArtist", NULL, NULL);
    cr_assert_gt(orphan_id, 0);
    cr_assert_gt(album_artist_id, 0);

    /* Create an album whose artist_id references album_artist_id — no track_artists rows */
    int64_t album_id = test_insert_album(db, "/music/ao", "Album", album_artist_id, 2024);
    (void)album_id;

    cr_assert_eq(count_artists(db), 2, "Both artists exist before prune");

    cr_assert_eq(db_prune_orphan_artists(db), QUADRATURE_OK);

    cr_assert_eq(count_artists(db), 1, "True orphan removed, album artist kept");

    /* Verify the album artist survived */
    db_lock(db);
    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db->db, "SELECT id FROM artists LIMIT 1", -1, &chk, NULL);
    sqlite3_step(chk);
    int64_t survivor = sqlite3_column_int64(chk, 0);
    sqlite3_finalize(chk);
    db_unlock(db);
    cr_assert_eq(survivor, album_artist_id, "Album-only artist should survive prune");

    db_close(db);
}

// MB-resolved re-index guard behavior is now tested against the reconciler
// directly in test_reconciler.c.

// ============================================================================
// Concurrent Read Safety
// ============================================================================

static void *
reader_thread(void *arg)
{
    quadrature_db_t *db = (quadrature_db_t *)arg;
    for (int i = 0; i < 100; i++) {
        size_t count;
        test_get_track_count(db, &count);
    }
    return NULL;
}

Test(database, concurrent_reads)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, reader_thread, db);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    db_close(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Migration system: version tracking, idempotency, downgrade rejection
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(database, fresh_db_version)
{
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(NULL, false, &db), QUADRATURE_OK);

    /* user_version must be set after migrations run (currently 1) */
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, "PRAGMA user_version", -1, &stmt, NULL);
    cr_assert_eq(sqlite3_step(stmt), SQLITE_ROW);
    int version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);

    cr_assert_geq(version, 1, "Fresh DB must have user_version >= 1");
    db_close(db);
}

Test(database, idempotent_reopen)
{
    const char *path = "test_migration_idempotent.db";
    unlink(path);

    /* Open, write data, close */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(path, false, &db), QUADRATURE_OK);
    int64_t artist_id = test_goc_artist(db, "Migration Test Artist", NULL, NULL);
    cr_assert_gt(artist_id, 0);
    db_close(db);

    /* Reopen — should succeed without re-running migrations */
    db = NULL;
    cr_assert_eq(db_open(path, false, &db), QUADRATURE_OK);

    /* Data intact */
    int64_t artist_id2 = test_goc_artist(db, "Migration Test Artist", NULL, NULL);
    cr_assert_eq(artist_id, artist_id2);

    /* Version still correct */
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, "PRAGMA user_version", -1, &stmt, NULL);
    cr_assert_eq(sqlite3_step(stmt), SQLITE_ROW);
    cr_assert_geq(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    db_unlock(db);

    db_close(db);
    unlink(path);
}

Test(database, rejects_newer_database)
{
    const char *path = "test_migration_downgrade.db";
    unlink(path);

    /* Create a DB with a future version */
    sqlite3 *raw = NULL;
    cr_assert_eq(sqlite3_open(path, &raw), SQLITE_OK);
    sqlite3_exec(raw, "PRAGMA user_version = 999", NULL, NULL, NULL);
    sqlite3_close(raw);

    /* db_open should reject it */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(path, false, &db), QUADRATURE_ERROR_INTERNAL);

    unlink(path);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Album mtime batch with sizes
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * db_reconcile_album_tracks: whole-album wipe cascade (tracks, track_artists, FTS)
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Prune each given album by deleting it (and all its tracks).
 *  Matches how Phase 1's orphan sweep invokes the API. */
static void
test_prune_albums(quadrature_db_t *db, const int64_t *ids, size_t count)
{
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    for (size_t i = 0; i < count; i++) {
        cr_assert_eq(db_delete_album(db, ids[i]), QUADRATURE_OK);
    }
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
}

/**
 * Build a test library for pruning tests:
 *   Daft Punk (MBID) → Discovery (3 tracks), RAM (2 tracks)
 *   Golden Features (no MBID) → SECT (2 tracks, track 7 features Daft Punk)
 *   Indexer error on Discovery path.
 *
 * Returns album IDs via out params.
 */
static void
build_prune_fixture(quadrature_db_t *db, int64_t *disc_out, int64_t *ram_out, int64_t *sect_out)
{
    int64_t dp
        = test_goc_artist(db, "Daft Punk", "Daft Punk", "056e4f3e-d505-4dad-8ec1-d04f521cbb56");
    int64_t gf = test_goc_artist(db, "Golden Features", NULL, NULL);

    int64_t disc_id = test_insert_album(db, "DaftPunk/Discovery", "Discovery", dp, 2001);
    int64_t ram_id = test_insert_album(db, "DaftPunk/RAM", "Random Access Memories", dp, 2013);
    int64_t sect_id = test_insert_album(db, "GoldenFeatures/SECT", "SECT", gf, 2021);

    const int64_t ta_dp[1] = { dp };
    const char *dp_names[1] = { "Daft Punk" };
    const int64_t ta_gf[1] = { gf };
    const char *gf_names[1] = { "Golden Features" };
    const char *j[1] = { "" };

    test_insert_track_full(
        db, disc_id, "One More Time", "One More Time", 1, 1, 200000, ta_dp, dp_names, j, 1);
    test_insert_track_full(
        db, disc_id, "Aerodynamic", "Aerodynamic", 2, 1, 200000, ta_dp, dp_names, j, 1);
    test_insert_track_full(
        db, disc_id, "Digital Love", "Digital Love", 3, 1, 200000, ta_dp, dp_names, j, 1);
    test_insert_track_full(
        db, ram_id, "Get Lucky", "Get Lucky", 1, 1, 200000, ta_dp, dp_names, j, 1);
    test_insert_track_full(
        db, ram_id, "Lose Yourself", "Lose Yourself", 2, 1, 200000, ta_dp, dp_names, j, 1);
    test_insert_track_full(db, sect_id, "Ariana", "Ariana", 1, 1, 200000, ta_gf, gf_names, j, 1);

    /* Track 7: featured credit — Golden Features feat. Daft Punk */
    int64_t feat_tid = test_insert_track_full(
        db, sect_id, "Touch", "Touch feat. Daft Punk", 2, 1, 200000, ta_gf, gf_names, j, 1);
    const int64_t feat_ids[2] = { gf, dp };
    const char *feat_joins[2] = { "", " feat. " };
    test_link_track_artists(db, feat_tid, feat_ids, feat_joins, 2);

    db_lock(db);
    sqlite3_exec(db->db,
                 "INSERT INTO indexer_errors(path, message) "
                 "VALUES('DaftPunk/Discovery/cover.jpg', 'artwork extraction failed')",
                 NULL,
                 NULL,
                 NULL);
    db_unlock(db);

    *disc_out = disc_id;
    *ram_out = ram_id;
    *sect_out = sect_id;
}

Test(database, prune_orphan_albums_cascades_tracks_and_fts)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    /* Prune Discovery — 3 tracks + FTS entries should cascade */
    int64_t orphan[] = { disc_id };
    test_prune_albums(db, orphan, 1);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM albums"), 2);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM tracks"), 4);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM track_artists"), 5);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM albums_fts"), 2);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM tracks_fts"), 4);

    db_close(db);
}

Test(database, prune_orphan_albums_full_wipe)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    int64_t all[] = { disc_id, ram_id, sect_id };
    test_prune_albums(db, all, 3);
    cr_assert_eq(db_prune_orphan_artists(db), QUADRATURE_OK);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM albums"), 0);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM tracks"), 0);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM track_artists"), 0);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists"), 0);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM albums_fts"), 0);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM tracks_fts"), 0);
    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists_fts"), 0);

    db_close(db);
}

Test(database, prune_orphan_artist_after_album_deletion)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    /* Delete SECT → Golden Features becomes orphaned */
    int64_t orphan[] = { sect_id };
    test_prune_albums(db, orphan, 1);
    cr_assert_eq(db_prune_orphan_artists(db), QUADRATURE_OK);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists"),
                 1,
                 "Golden Features should be pruned — only Daft Punk remains");

    char *survivor = test_read_text(db, "SELECT name FROM artists WHERE id = ?", 1);
    cr_assert_not_null(survivor);
    cr_assert_str_eq(survivor, "Daft Punk");
    free(survivor);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists_fts"), 1);

    db_close(db);
}

Test(database, featured_artist_survives_album_deletion)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    /* Delete SECT — Daft Punk's featured credit on track 7 goes away,
     * but Daft Punk survives via own albums. */
    int64_t orphan[] = { sect_id };
    test_prune_albums(db, orphan, 1);
    cr_assert_eq(db_prune_orphan_artists(db), QUADRATURE_OK);

    /* Track 7 completely gone */
    cr_assert_eq(
        test_count_rows_param(db, "SELECT COUNT(*) FROM track_artists WHERE track_id = ?", 7), 0);

    /* Daft Punk retains 5 credits from Discovery (3) + RAM (2) */
    cr_assert_eq(
        test_count_rows_param(db, "SELECT COUNT(*) FROM track_artists WHERE artist_id = ?", 1), 5);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists"), 1);

    db_close(db);
}

Test(database, prune_orphan_errors_removes_stale_paths)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM indexer_errors"), 1);

    /* After pruning Discovery album, the error for its path should be cleaned */
    int64_t orphan[] = { disc_id };
    test_prune_albums(db, orphan, 1);
    cr_assert_eq(db_prune_orphan_errors(db, "/music"), QUADRATURE_OK);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM indexer_errors"),
                 0,
                 "Error for deleted album path should be cleaned");

    db_close(db);
}

/* mb_status guard behaviors (title/release_id immutable after RESOLVED,
 * respect_user_edits policy) are now tested against the reconciler
 * directly in test_reconciler.c. */

/* ═══════════════════════════════════════════════════════════════════════════
 * Album mtime batch with sizes
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(database, mtime_batch_writes_size)
{
    quadrature_db_t *db = NULL;
    db_open(NULL, false, &db);

    /* Create an artist and album */
    int64_t artist_id = test_goc_artist(db, "Test Artist", NULL, NULL);
    cr_assert_gt(artist_id, 0);

    int64_t album_id = test_insert_album(db, "test/album", "Test Album", artist_id, 2024);
    cr_assert_gt(album_id, 0);

    /* Write mtime + size */
    int64_t ids[] = { album_id };
    int64_t mtimes[] = { 1700000000 };
    int64_t sizes[] = { (3LL << 32) | 12345 }; /* 3 files, 12345 bytes */
    db_set_album_mtimes_batch(db, ids, mtimes, sizes, 1);

    /* Read back via db_get_album_mtimes_page */
    db_album_mtime_t *out = NULL;
    size_t count = 0;
    db_get_album_mtimes_page(db, 0, 100, &out, &count);
    cr_assert_eq(count, 1);
    cr_assert_eq(out[0].last_updated_at, 1700000000);
    cr_assert_eq(out[0].last_updated_size, (3LL << 32) | 12345);
    db_free_album_mtimes(out, count);

    db_close(db);
}
