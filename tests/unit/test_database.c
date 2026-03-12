#include <criterion/criterion.h>
#include "quadrature/database.h"
#include "../../src/database/internal.h"
#include <unistd.h>
#include <pthread.h>
#include <string.h>

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
    // TODO: read albums.artist_id back and assert it equals mb_artist_id, not filetag_artist_id
    // Hint: query "SELECT artist_id FROM albums WHERE id = ?" using db->db directly (see helper pattern above)
    // ~5 lines: prepare stmt, bind album_id, step, read int64, finalize, then cr_assert_eq

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
    // TODO: same pattern — read artist_id back and assert it equals artist_v2

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
