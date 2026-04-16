#include <criterion/criterion.h>
#include "test_helpers.h"
#include <pthread.h>

#define TEST_DB_FILE "test_database.db"

// Local helper: count tracks via direct SQL (db_get_track_count was removed)
static quadrature_result_t test_get_track_count(quadrature_db_t* db, size_t* out) {
    if (!db || !out) return QUADRATURE_ERROR_INVALID_PARAM;
    db_lock(db);
    sqlite3_stmt* stmt = NULL;
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

Test(database, null_safety_and_validation) {
    db_track_t* track = NULL;

    // --- Null database handle ---
    cr_assert_eq(db_open_memory(NULL), QUADRATURE_ERROR_INVALID_PARAM);
    db_close(NULL);  // Should not crash
    cr_assert_null(db_path(NULL));

    // Free functions accept null safely
    db_track_free(NULL);

    // Operations with null database
    cr_assert_eq(db_get_track(NULL, 1, &track), QUADRATURE_ERROR_INVALID_PARAM);

    // --- Null parameters with valid database ---
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    cr_assert_eq(db_get_track(db, 1, NULL), QUADRATURE_ERROR_INVALID_PARAM);

    db_close(db);
}

// ============================================================================
// Lifecycle (Memory and File-based)
// ============================================================================

Test(database, lifecycle) {
    quadrature_db_t* db = NULL;

    // --- In-memory database ---
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);
    cr_assert_not_null(db);
    cr_assert_null(db_path(db));
    db_close(db);

    // --- File-based database ---
    db = NULL;
    cr_assert_eq(db_open(TEST_DB_FILE, &db), QUADRATURE_OK);
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
    cr_assert_eq(db_open(TEST_DB_FILE, &db), QUADRATURE_OK);
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

Test(database, empty_operations) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    size_t count = 999;
    db_track_t* track = NULL;

    // Track count is zero
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 0);

    // Get nonexistent track
    cr_assert_eq(db_get_track(db, 1, &track), QUADRATURE_ERROR_FILE_NOT_FOUND);
    cr_assert_eq(db_get_track(db, -1, &track), QUADRATURE_ERROR_FILE_NOT_FOUND);

    db_close(db);
}

// ============================================================================
// Track Upsert Operations
// ============================================================================

Test(database, track_upsert) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    // Create artist and album for tracks
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    cr_assert_neq(artist_id, 0);

    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music", "Test Album",
        artist_id, 2024, &album_id), QUADRATURE_OK);
    cr_assert_neq(album_id, 0);

    // --- INSERT ---
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    db_index_item_t item1 = {
        .path = "/music/track1.mp3",
        .title = "First Song",
        .album = "Test Album",
        .duration_ms = 180000,
        .track_num = 1,
        .year = 2024,
        .mtime = 1000000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item1, album_id, NULL), QUADRATURE_OK);

    db_index_item_t item2 = {
        .path = "/music/track2.mp3",
        .title = "Second Song",
        .album = "Test Album",
        .duration_ms = 200000,
        .track_num = 2,
        .year = 2024,
        .mtime = 1000001,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item2, album_id, NULL), QUADRATURE_OK);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Verify inserts
    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 2);

    // --- UPDATE ---
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    db_index_item_t update = {
        .path = "/music/track1.mp3",
        .title = "Updated Title",
        .album = "Test Album",
        .duration_ms = 185000,
        .track_num = 1,
        .year = 2024,
        .mtime = 2000000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &update, album_id, NULL), QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Count unchanged
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 2);

    db_close(db);
}

// ============================================================================
// Transaction Rollback
// ============================================================================

Test(database, transaction_rollback) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    // Create artist and album
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music", "Album",
        artist_id, 2024, &album_id), QUADRATURE_OK);

    // Insert one track
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    db_index_item_t item = {
        .path = "/music/committed.mp3",
        .title = "Committed",
        .album = "Album",
        .duration_ms = 100000,
        .track_num = 1,
        .year = 2024,
        .mtime = 1000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 1);

    // Start new transaction, add track, then rollback
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    item.path = "/music/rolled_back.mp3";
    item.title = "Should Not Exist";
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);
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
static size_t count_artists(quadrature_db_t *db) {
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
Test(database, artist_normalized_rename_preserves_id) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    /* Phase 2: create via simple name (no MBID) */
    int64_t id_phase2 = db_get_or_create_artist(db, "2 Mex");
    cr_assert_gt(id_phase2, 0, "Phase 2 artist should be created");
    cr_assert_eq(count_artists(db), 1, "Should have exactly 1 artist");

    /* Phase 4: resolve via MB — name differs by space but normalizes the same */
    int64_t id_phase4 = db_get_or_create_artist_mb(db, "2Mex", "2Mex", "696c9bcb-1234-5678-abcd-000000000001");
    cr_assert_gt(id_phase4, 0, "MB artist should be found/created");
    cr_assert_eq(id_phase4, id_phase2, "Normalized rename should return original id (no orphan created)");
    cr_assert_eq(count_artists(db), 1, "Still exactly 1 artist after normalized rename");

    db_close(db);
}

/* Phase 4 called twice with the same MBID should return the same id. */
Test(database, artist_mb_lookup_by_mbid_is_idempotent) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t id1 = db_get_or_create_artist_mb(db, "Madlib", "Madlib", "some-mbid-aaa");
    int64_t id2 = db_get_or_create_artist_mb(db, "Madlib", "Madlib", "some-mbid-aaa");
    cr_assert_eq(id1, id2, "Same MBID should always return the same artist_id");
    cr_assert_eq(count_artists(db), 1, "One artist row for repeated MBID lookup");

    db_close(db);
}

/* Step 2: exact name match (case-insensitive) with no MBID should rename in-place. */
Test(database, artist_mb_exact_nocase_rename) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t id_phase2 = db_get_or_create_artist(db, "ac/dc");
    cr_assert_gt(id_phase2, 0);

    int64_t id_mb = db_get_or_create_artist_mb(db, "AC/DC", "AC/DC", "mbid-acdc-001");
    cr_assert_eq(id_mb, id_phase2, "NOCASE rename should preserve artist_id");
    cr_assert_eq(count_artists(db), 1);

    db_close(db);
}

/* db_prune_orphan_artists must delete artists that have no track_artists rows. */
Test(database, prune_orphan_artists_removes_unreferenced) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    /* Create two artists */
    int64_t orphan_id  = db_get_or_create_artist(db, "OrphanArtist");
    int64_t linked_id  = db_get_or_create_artist(db, "LinkedArtist");
    cr_assert_gt(orphan_id, 0);
    cr_assert_gt(linked_id, 0);

    /* Create an album and a track linked to linked_id only */
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music", "Album", linked_id, 2020, &album_id), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    db_index_item_t item = {
        .path = "/music/track.mp3", .title = "Track", .album = "Album",
        .duration_ms = 60000, .track_num = 1, .year = 2020, .mtime = 1000,
    };
    db_index_item_t *artists_list[1] = {NULL};
    (void)artists_list;
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);

    /* Manually insert a track_artists row for linked_id */
    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db->db,
        "INSERT INTO track_artists(track_id, artist_id, position) "
        "SELECT id, ?, 0 FROM tracks WHERE path = '/music/track.mp3' LIMIT 1",
        -1, &ins, NULL);
    sqlite3_bind_int64(ins, 1, linked_id);
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

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
Test(database, prune_orphan_artists_preserves_album_artist) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t orphan_id = db_get_or_create_artist(db, "TrueOrphan");
    int64_t album_artist_id = db_get_or_create_artist(db, "AlbumOnlyArtist");
    cr_assert_gt(orphan_id, 0);
    cr_assert_gt(album_artist_id, 0);

    /* Create an album whose artist_id references album_artist_id — no track_artists rows */
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/ao", "Album",
        album_artist_id, 2024, &album_id), QUADRATURE_OK);

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

/* db_get_artists_page must not return artists that have no track_artists rows. */
Test(database, get_artists_page_excludes_orphans) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    /* Create an orphan artist with no tracks */
    db_get_or_create_artist(db, "OrphanWithNoTracks");

    /* Create a real artist with a track */
    int64_t real_id = db_get_or_create_artist(db, "RealArtist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/m2", "Album2", real_id, 2021, &album_id), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    db_index_item_t item = {
        .path = "/m2/song.mp3", .title = "Song", .album = "Album2",
        .duration_ms = 30000, .track_num = 1, .year = 2021, .mtime = 2000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db->db,
        "INSERT INTO track_artists(track_id, artist_id, position) "
        "SELECT id, ?, 0 FROM tracks WHERE path = '/m2/song.mp3' LIMIT 1",
        -1, &ins, NULL);
    sqlite3_bind_int64(ins, 1, real_id);
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    db_artist_t *results = NULL;
    size_t out_count = 0, total_count = 0;
    db_page_opts_t opts = {
        .offset = 0, .limit = 100, .sort = DB_SORT_NAME_ASC,
        .search_text = NULL, .filters = NULL,
    };
    quadrature_result_t res = db_get_artists_page(db, &opts, &results, &out_count, &total_count);
    cr_assert_eq(res, QUADRATURE_OK);
    cr_assert_eq(out_count, 1, "Page should contain only the artist with tracks");
    cr_assert_eq(total_count, 1, "Total should exclude orphan");
    if (results && out_count > 0)
        cr_assert_str_eq(results[0].name, "RealArtist");
    db_artists_free(results, out_count);

    db_close(db);
}

// ============================================================================
// MB-Resolved Album Re-Index Protection
// ============================================================================

/* Regression test for: mb_status guard used wrong constant (!=1 instead of <2),
 * allowing a re-index to clobber artist_id on albums already at MB_STATUS_RESOLVED.
 *
 * Scenario (mirrors real Apollo 440 case):
 *   Pass 1 — metadata phase: album created with file-tag artist ("Apollo Four Forty")
 *   MB phase: artist updated to canonical MB artist ("Apollo 440"), mb_status → RESOLVED
 *   Pass 2 — re-index: metadata phase runs again with file-tag artist
 *   Expected: album.artist_id must still be the MB-resolved artist after pass 2.
 */
Test(database, mb_resolved_album_artist_not_clobbered_on_reindex) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    /* ── Pass 1: metadata phase ── */
    int64_t filetag_artist_id = db_get_or_create_artist(db, "Apollo Four Forty");
    cr_assert_gt(filetag_artist_id, 0);

    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/apollo440/ghoyo", "Gettin' High on Your Own Supply",
        filetag_artist_id, 1999, &album_id), QUADRATURE_OK);
    cr_assert_gt(album_id, 0);

    /* ── MB phase: resolve to canonical artist + set status ── */
    int64_t mb_artist_id = db_get_or_create_artist_mb(db,
        "Apollo 440", "Apollo 440", "1ff10dff-7ac7-4e53-bc02-d5c3cbd8448b");
    cr_assert_gt(mb_artist_id, 0);
    cr_assert_neq(mb_artist_id, filetag_artist_id); /* sanity: two distinct artists */

    cr_assert_eq(db_update_album_artist(db, album_id, mb_artist_id, false), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, album_id,
        "Gettin' High on Your Own Supply",
        "a12184c1-cbdc-3ab4-b5c8-aa0685aa9c47",
        "d9b8543e-b058-305a-8bd3-e841382a6168",
        1999, MB_STATUS_RESOLVED), QUADRATURE_OK);

    /* ── Pass 2: re-index — metadata phase runs again with file-tag artist ── */
    int64_t album_id2 = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/apollo440/ghoyo", "Gettin' High on Your Own Supply",
        filetag_artist_id, 1999, &album_id2), QUADRATURE_OK);
    cr_assert_eq(album_id2, album_id); /* same album row */

    /* ── Assert: MB-resolved artist must survive the re-index ── */
    int64_t actual_artist = test_read_int64(db, "SELECT artist_id FROM albums WHERE id = ?", album_id);
    cr_assert_eq(actual_artist, mb_artist_id,
        "MB-resolved artist_id must survive re-index (got filetag artist instead)");

    db_close(db);
}

/* Complement: un-resolved album (mb_status=0) MUST have its artist updated on re-index. */
Test(database, unresolved_album_artist_updated_on_reindex) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t artist_v1 = db_get_or_create_artist(db, "Old Tag Name");
    int64_t artist_v2 = db_get_or_create_artist(db, "Corrected Tag Name");
    cr_assert_gt(artist_v1, 0);
    cr_assert_gt(artist_v2, 0);

    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/somealbum", "Some Album",
        artist_v1, 2020, &album_id), QUADRATURE_OK);

    /* Re-index with corrected tag — mb_status is still 0 (NOT_ATTEMPTED) */
    int64_t album_id2 = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/somealbum", "Some Album",
        artist_v2, 2020, &album_id2), QUADRATURE_OK);
    cr_assert_eq(album_id2, album_id);

    /* ── Assert: artist_id must be updated to artist_v2 ── */
    int64_t actual_artist = test_read_int64(db, "SELECT artist_id FROM albums WHERE id = ?", album_id);
    cr_assert_eq(actual_artist, artist_v2,
        "Unresolved album artist_id must be updated on re-index");

    db_close(db);
}

// ============================================================================
// Concurrent Read Safety
// ============================================================================

static void* reader_thread(void* arg) {
    quadrature_db_t* db = (quadrature_db_t*)arg;
    for (int i = 0; i < 100; i++) {
        size_t count;
        test_get_track_count(db, &count);
    }
    return NULL;
}

Test(database, concurrent_reads) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

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

Test(database, fresh_db_version) {
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);

    /* user_version must be set after migrations run (currently 1) */
    db_lock(db);
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db->db, "PRAGMA user_version", -1, &stmt, NULL);
    cr_assert_eq(sqlite3_step(stmt), SQLITE_ROW);
    int version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);

    cr_assert_geq(version, 1, "Fresh DB must have user_version >= 1");
    db_close(db);
}

Test(database, idempotent_reopen) {
    const char* path = "test_migration_idempotent.db";
    unlink(path);

    /* Open, write data, close */
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open(path, &db), QUADRATURE_OK);
    int64_t artist_id = db_get_or_create_artist(db, "Migration Test Artist");
    cr_assert_gt(artist_id, 0);
    db_close(db);

    /* Reopen — should succeed without re-running migrations */
    db = NULL;
    cr_assert_eq(db_open(path, &db), QUADRATURE_OK);

    /* Data intact */
    int64_t artist_id2 = db_get_or_create_artist(db, "Migration Test Artist");
    cr_assert_eq(artist_id, artist_id2);

    /* Version still correct */
    db_lock(db);
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db->db, "PRAGMA user_version", -1, &stmt, NULL);
    cr_assert_eq(sqlite3_step(stmt), SQLITE_ROW);
    cr_assert_geq(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    db_unlock(db);

    db_close(db);
    unlink(path);
}

Test(database, rejects_newer_database) {
    const char* path = "test_migration_downgrade.db";
    unlink(path);

    /* Create a DB with a future version */
    sqlite3* raw = NULL;
    cr_assert_eq(sqlite3_open(path, &raw), SQLITE_OK);
    sqlite3_exec(raw, "PRAGMA user_version = 999", NULL, NULL, NULL);
    sqlite3_close(raw);

    /* db_open should reject it */
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open(path, &db), QUADRATURE_ERROR_INTERNAL);

    unlink(path);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * db_log_error_ex: structured error round-trip
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(database, log_error_ex_writes_structured_fields) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    /* Write a structured error */
    quadrature_result_t res = db_log_error_ex(db, "/test/path",
        INDEXER_ERR_FFMPEG_DECODE, 2, INDEXER_SEV_ERROR,
        "Failed to decode audio", 1);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Read it back via direct SQL */
    db_lock(db);
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db->db,
        "SELECT path, error_code, phase, severity, message, scan_generation "
        "FROM indexer_errors ORDER BY id DESC LIMIT 1",
        -1, &stmt, NULL);
    int rc = sqlite3_step(stmt);
    cr_assert_eq(rc, SQLITE_ROW);

    cr_assert_str_eq((const char*)sqlite3_column_text(stmt, 0), "/test/path");
    cr_assert_eq(sqlite3_column_int(stmt, 1), INDEXER_ERR_FFMPEG_DECODE);
    cr_assert_eq(sqlite3_column_int(stmt, 2), 2);  /* phase */
    cr_assert_eq(sqlite3_column_int(stmt, 3), INDEXER_SEV_ERROR);
    cr_assert_str_eq((const char*)sqlite3_column_text(stmt, 4), "Failed to decode audio");
    cr_assert_eq(sqlite3_column_int64(stmt, 5), 1);  /* scan_generation */

    sqlite3_finalize(stmt);
    db_unlock(db);

    db_close(db);
}

Test(database, log_error_legacy_wrapper_defaults) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    /* Legacy wrapper should write defaults: error_code=0, phase=0, severity=2 */
    db_log_error(db, "/legacy/path", "Legacy error", 5);

    db_lock(db);
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db->db,
        "SELECT error_code, phase, severity FROM indexer_errors LIMIT 1",
        -1, &stmt, NULL);
    cr_assert_eq(sqlite3_step(stmt), SQLITE_ROW);
    cr_assert_eq(sqlite3_column_int(stmt, 0), 0);  /* INDEXER_ERR_UNKNOWN */
    cr_assert_eq(sqlite3_column_int(stmt, 1), 0);  /* phase 0 */
    cr_assert_eq(sqlite3_column_int(stmt, 2), 2);  /* INDEXER_SEV_ERROR */
    sqlite3_finalize(stmt);
    db_unlock(db);

    db_close(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Album mtime batch with sizes
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * db_reconcile_album_tracks: whole-album wipe cascade (tracks, track_artists, FTS)
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Prune each given album by reconciling against an empty current-path set.
 *  Matches how Phase 1's orphan sweep invokes the API. */
static void test_prune_albums(quadrature_db_t *db, const int64_t *ids, size_t count) {
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    for (size_t i = 0; i < count; i++) {
        cr_assert_eq(db_reconcile_album_tracks(db, ids[i], NULL, 0), QUADRATURE_OK);
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
static void build_prune_fixture(quadrature_db_t *db,
                                int64_t *disc_out, int64_t *ram_out,
                                int64_t *sect_out) {
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk",
        "056e4f3e-d505-4dad-8ec1-d04f521cbb56");
    int64_t gf = db_get_or_create_artist(db, "Golden Features");

    int64_t disc_id = 0, ram_id = 0, sect_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/Discovery",
        "Discovery", dp, 2001, &disc_id), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, disc_id, "Discovery",
        "d073287b-d1bd-4f11-a933-a4386f8cf701",
        "cc001111-1111-1111-1111-111111111111",
        2001, MB_STATUS_RESOLVED), QUADRATURE_OK);

    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/RAM",
        "Random Access Memories", dp, 2013, &ram_id), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, ram_id, "Random Access Memories",
        "8ecfafd1-89a8-423a-968f-3fff47f0b0f9",
        "aa997ea0-2936-40bd-884d-3af8a0e064dc",
        2013, MB_STATUS_RESOLVED), QUADRATURE_OK);

    cr_assert_eq(db_upsert_folder_album(db, "GoldenFeatures/SECT",
        "SECT", gf, 2021, &sect_id), QUADRATURE_OK);

    test_create_track(db, disc_id, "One More Time", 1, 1);
    test_create_track(db, disc_id, "Aerodynamic", 2, 1);
    test_create_track(db, disc_id, "Digital Love", 3, 1);
    test_create_track(db, ram_id, "Get Lucky", 1, 1);
    test_create_track(db, ram_id, "Lose Yourself to Dance", 2, 1);
    test_create_track(db, sect_id, "Ariana", 1, 1);
    test_create_track(db, sect_id, "Touch feat. Daft Punk", 2, 1);

    db_track_artist_t ta_dp = { .artist_id = dp, .position = 0, .join_phrase = "" };
    for (int64_t tid = 1; tid <= 5; tid++)
        db_set_track_artists(db, tid, &ta_dp, 1);

    db_track_artist_t ta_gf = { .artist_id = gf, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 6, &ta_gf, 1);

    db_track_artist_t ta_feat[2] = {
        { .artist_id = gf, .position = 0, .join_phrase = "" },
        { .artist_id = dp, .position = 1, .join_phrase = " feat. " },
    };
    db_set_track_artists(db, 7, ta_feat, 2);

    db_sync_album_fts(db, disc_id);
    db_sync_album_fts(db, ram_id);
    db_sync_album_fts(db, sect_id);

    db_lock(db);
    sqlite3_exec(db->db,
        "INSERT INTO indexer_errors(path, error_code, phase, message) "
        "VALUES('DaftPunk/Discovery/cover.jpg', 3, 3, 'artwork extraction failed')",
        NULL, NULL, NULL);
    db_unlock(db);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    *disc_out = disc_id;
    *ram_out = ram_id;
    *sect_out = sect_id;
}

Test(database, prune_orphan_albums_cascades_tracks_and_fts) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

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

Test(database, prune_orphan_albums_full_wipe) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

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

Test(database, prune_orphan_artist_after_album_deletion) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    /* Delete SECT → Golden Features becomes orphaned */
    int64_t orphan[] = { sect_id };
    test_prune_albums(db, orphan, 1);
    cr_assert_eq(db_prune_orphan_artists(db), QUADRATURE_OK);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists"), 1,
        "Golden Features should be pruned — only Daft Punk remains");

    char *survivor = test_read_text(db, "SELECT name FROM artists WHERE id = ?", 1);
    cr_assert_not_null(survivor);
    cr_assert_str_eq(survivor, "Daft Punk");
    free(survivor);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists_fts"), 1);

    db_close(db);
}

Test(database, featured_artist_survives_album_deletion) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    /* Delete SECT — Daft Punk's featured credit on track 7 goes away,
     * but Daft Punk survives via own albums. */
    int64_t orphan[] = { sect_id };
    test_prune_albums(db, orphan, 1);
    cr_assert_eq(db_prune_orphan_artists(db), QUADRATURE_OK);

    /* Track 7 completely gone */
    cr_assert_eq(test_count_rows_param(db,
        "SELECT COUNT(*) FROM track_artists WHERE track_id = ?", 7), 0);

    /* Daft Punk retains 5 credits from Discovery (3) + RAM (2) */
    cr_assert_eq(test_count_rows_param(db,
        "SELECT COUNT(*) FROM track_artists WHERE artist_id = ?", 1), 5);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM artists"), 1);

    db_close(db);
}

Test(database, prune_orphan_errors_removes_stale_paths) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);

    int64_t disc_id, ram_id, sect_id;
    build_prune_fixture(db, &disc_id, &ram_id, &sect_id);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM indexer_errors"), 1);

    /* After pruning Discovery album, the error for its path should be cleaned */
    int64_t orphan[] = { disc_id };
    test_prune_albums(db, orphan, 1);
    cr_assert_eq(db_prune_orphan_errors(db, "/music"), QUADRATURE_OK);

    cr_assert_eq(test_count_rows(db, "SELECT COUNT(*) FROM indexer_errors"), 0,
        "Error for deleted album path should be cleaned");

    db_close(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * mb_status guards: resolved data protected from re-index clobber
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(database, mb_resolved_title_not_clobbered_on_reindex) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t aid = db_get_or_create_artist(db, "Daft Punk");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/RAM",
        "Random Access Memories", aid, 2013, &album_id), QUADRATURE_OK);

    /* MB resolution corrects title */
    cr_assert_eq(db_update_album_mb(db, album_id,
        "Random Access Memories (10th Anniversary Edition)",
        "8ecfafd1-89a8-423a-968f-3fff47f0b0f9",
        "aa997ea0-2936-40bd-884d-3af8a0e064dc",
        2013, MB_STATUS_RESOLVED), QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Re-index: file tags still say original title */
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    int64_t id2 = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/RAM",
        "Random Access Memories", aid, 2013, &id2), QUADRATURE_OK);
    cr_assert_eq(id2, album_id);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Title must be the MB-corrected version */
    char *title = test_read_text(db,
        "SELECT title FROM albums WHERE id = ?", album_id);
    cr_assert_str_eq(title, "Random Access Memories (10th Anniversary Edition)",
        "MB-resolved title must survive re-index, got '%s'", title);
    free(title);

    db_close(db);
}

/**
 * db_set_album_release_id_from_tags gates on mb_status != RESOLVED.
 * Per METADATA.md: "updates regardless of current status — as long as not
 * already RESOLVED." Status 0, 1, 3, 4 all allow update; only 2 blocks it.
 */
Test(database, release_id_from_tags_gated_on_not_resolved) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t aid = db_get_or_create_artist(db, "ODESZA");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "ODESZA/LG",
        "The Last Goodbye", aid, 2022, &album_id), QUADRATURE_OK);

    /* Phase 2: first tag sets release_id (status 0 → 1) */
    cr_assert_eq(db_set_album_release_id_from_tags(db, album_id,
        "aaaa1111-1111-1111-1111-111111111111"), QUADRATURE_OK);

    /* Phase 2 again with different tag: status=1 → update allowed per spec */
    cr_assert_eq(db_set_album_release_id_from_tags(db, album_id,
        "bbbb2222-2222-2222-2222-222222222222"), QUADRATURE_OK);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Should have the SECOND release_id (update allowed on status=1) */
    char *rid = test_read_text(db,
        "SELECT musicbrainz_release_id FROM albums WHERE id = ?", album_id);
    cr_assert_str_eq(rid, "bbbb2222-2222-2222-2222-222222222222",
        "Re-tag should update release_id when status=HAS_RELEASE_ID");
    free(rid);

    /* Now resolve fully — after RESOLVED, tags should NOT overwrite */
    cr_assert_eq(db_update_album_mb(db, album_id, "The Last Goodbye",
        "cccc3333-3333-3333-3333-333333333333",
        "dddd4444-4444-4444-4444-444444444444",
        2022, MB_STATUS_RESOLVED), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    db_set_album_release_id_from_tags(db, album_id,
        "eeee5555-5555-5555-5555-555555555555");
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Should still have the RESOLVED release_id, not the tag override */
    char *rid2 = test_read_text(db,
        "SELECT musicbrainz_release_id FROM albums WHERE id = ?", album_id);
    cr_assert_str_eq(rid2, "cccc3333-3333-3333-3333-333333333333",
        "RESOLVED album must reject tag-sourced release_id override");
    free(rid2);

    db_close(db);
}

Test(database, full_mb_lifecycle_immutable_after_resolved) {
    quadrature_db_t *db = NULL;
    db_open_memory(&db);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t aid = db_get_or_create_artist(db, "BRONSON");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "BRONSON/BRONSON",
        "BRONSON", aid, 2020, &album_id), QUADRATURE_OK);

    /* Phase 2 → HAS_RELEASE_ID */
    db_set_album_release_id_from_tags(db, album_id,
        "5ed617d7-898f-4e05-82a1-bfc586a4b013");

    /* Phase 6 → RESOLVED with different data */
    db_update_album_mb(db, album_id, "BRONSON (Deluxe)",
        "aaaa1111-1111-1111-1111-111111111111",
        "d95b8366-994d-448d-8689-422b20b6cabb",
        2020, MB_STATUS_RESOLVED);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Re-index: Phase 2 tries to overwrite */
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    int64_t id2 = 0;
    db_upsert_folder_album(db, "BRONSON/BRONSON", "BRONSON", aid, 2020, &id2);
    db_set_album_release_id_from_tags(db, album_id,
        "5ed617d7-898f-4e05-82a1-bfc586a4b013");
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Everything must be preserved */
    char *title = test_read_text(db, "SELECT title FROM albums WHERE id = ?", album_id);
    cr_assert_str_eq(title, "BRONSON (Deluxe)");
    free(title);

    char *rid = test_read_text(db,
        "SELECT musicbrainz_release_id FROM albums WHERE id = ?", album_id);
    cr_assert_str_eq(rid, "aaaa1111-1111-1111-1111-111111111111");
    free(rid);

    char *rgid = test_read_text(db,
        "SELECT musicbrainz_release_group_id FROM albums WHERE id = ?", album_id);
    cr_assert_str_eq(rgid, "d95b8366-994d-448d-8689-422b20b6cabb");
    free(rgid);

    int64_t status = test_read_int64(db,
        "SELECT mb_status FROM albums WHERE id = ?", album_id);
    cr_assert_eq(status, MB_STATUS_RESOLVED);

    db_close(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Album mtime batch with sizes
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(database, mtime_batch_writes_size) {
    quadrature_db_t* db = NULL;
    db_open_memory(&db);

    /* Create an artist and album */
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    cr_assert_gt(artist_id, 0);

    int64_t album_id = 0;
    db_upsert_folder_album(db, "test/album", "Test Album", artist_id, 2024, &album_id);
    cr_assert_gt(album_id, 0);

    /* Write mtime + size */
    int64_t ids[] = { album_id };
    int64_t mtimes[] = { 1700000000 };
    int64_t sizes[] = { (3LL << 32) | 12345 };  /* 3 files, 12345 bytes */
    db_set_album_mtimes_batch(db, ids, mtimes, sizes, 1);

    /* Read back via db_get_album_mtimes_page */
    db_album_mtime_t* out = NULL;
    size_t count = 0;
    db_get_album_mtimes_page(db, 0, 100, &out, &count);
    cr_assert_eq(count, 1);
    cr_assert_eq(out[0].last_updated_at, 1700000000);
    cr_assert_eq(out[0].last_updated_size, (3LL << 32) | 12345);
    db_free_album_mtimes(out, count);

    db_close(db);
}
