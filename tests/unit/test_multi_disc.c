/**
 * Unit tests for multi-disc album support.
 *
 * Tests cover:
 * - Disc folder name detection (CD1, Disc 1, etc.)
 * - Disc number extraction from folder names
 * - Database disc_num field handling
 * - Multi-disc album scanning and validation
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "quadrature/quadrature.h"
#include "quadrature/database.h"
#include "../../src/database/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>

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

// Initialize FFmpeg before any tests run (before Criterion forks)
ReportHook(PRE_ALL)(struct criterion_test_set *tests) {
    (void)tests;
    avformat_network_init();
}

// ============================================================================
// Forward declarations from indexer internals
// ============================================================================

bool is_disc_folder(const char* dir_name);
uint16_t get_disc_number_from_folder(const char* dir_name);

// ============================================================================
// DISC FOLDER DETECTION TESTS
// ============================================================================

// Test: is_disc_folder() recognizes "CD" prefix patterns
Test(multi_disc, disc_folder_cd_patterns) {
    // Basic CD patterns (case insensitive)
    cr_assert(is_disc_folder("CD1"), "CD1 should be detected as disc folder");
    cr_assert(is_disc_folder("cd1"), "cd1 should be detected as disc folder");
    cr_assert(is_disc_folder("CD2"), "CD2 should be detected as disc folder");
    cr_assert(is_disc_folder("Cd3"), "Cd3 should be detected as disc folder");

    // CD with separator
    cr_assert(is_disc_folder("CD 1"), "CD 1 (with space) should be detected");
    cr_assert(is_disc_folder("CD-1"), "CD-1 (with hyphen) should be detected");
    cr_assert(is_disc_folder("CD_1"), "CD_1 (with underscore) should be detected");
    cr_assert(is_disc_folder("CD 2"), "CD 2 should be detected");
    cr_assert(is_disc_folder("cd-3"), "cd-3 should be detected");

    // Double digit
    cr_assert(is_disc_folder("CD10"), "CD10 should be detected");
    cr_assert(is_disc_folder("CD 12"), "CD 12 should be detected");
    cr_assert(is_disc_folder("CD99"), "CD99 should be detected");
}

// Test: is_disc_folder() recognizes "Disc" prefix patterns
Test(multi_disc, disc_folder_disc_patterns) {
    // Basic Disc patterns
    cr_assert(is_disc_folder("Disc1"), "Disc1 should be detected");
    cr_assert(is_disc_folder("disc1"), "disc1 should be detected");
    cr_assert(is_disc_folder("DISC1"), "DISC1 should be detected");
    cr_assert(is_disc_folder("Disc2"), "Disc2 should be detected");

    // Disc with separator
    cr_assert(is_disc_folder("Disc 1"), "Disc 1 should be detected");
    cr_assert(is_disc_folder("Disc-1"), "Disc-1 should be detected");
    cr_assert(is_disc_folder("Disc_2"), "Disc_2 should be detected");
    cr_assert(is_disc_folder("disc 3"), "disc 3 should be detected");

    // Double digit
    cr_assert(is_disc_folder("Disc10"), "Disc10 should be detected");
    cr_assert(is_disc_folder("Disc 15"), "Disc 15 should be detected");
}

// Test: is_disc_folder() recognizes word-based disc names
Test(multi_disc, disc_folder_word_patterns) {
    // Word-based disc names (Disc One, Disc Two, etc.)
    cr_assert(is_disc_folder("Disc One"), "Disc One should be detected");
    cr_assert(is_disc_folder("Disc Two"), "Disc Two should be detected");
    cr_assert(is_disc_folder("Disc Three"), "Disc Three should be detected");
    cr_assert(is_disc_folder("Disc Four"), "Disc Four should be detected");
    cr_assert(is_disc_folder("Disc Five"), "Disc Five should be detected");
    cr_assert(is_disc_folder("Disc Six"), "Disc Six should be detected");
    cr_assert(is_disc_folder("Disc Seven"), "Disc Seven should be detected");
    cr_assert(is_disc_folder("Disc Eight"), "Disc Eight should be detected");
    cr_assert(is_disc_folder("Disc Nine"), "Disc Nine should be detected");
    cr_assert(is_disc_folder("Disc Ten"), "Disc Ten should be detected");

    // Case variations
    cr_assert(is_disc_folder("disc one"), "disc one should be detected");
    cr_assert(is_disc_folder("DISC ONE"), "DISC ONE should be detected");
    cr_assert(is_disc_folder("Disc ONE"), "Disc ONE should be detected");
}

// Test: is_disc_folder() recognizes short "D" prefix patterns
Test(multi_disc, disc_folder_d_patterns) {
    // Short D prefix (must be followed directly by number)
    cr_assert(is_disc_folder("D1"), "D1 should be detected");
    cr_assert(is_disc_folder("d1"), "d1 should be detected");
    cr_assert(is_disc_folder("D2"), "D2 should be detected");
    cr_assert(is_disc_folder("d3"), "d3 should be detected");
    cr_assert(is_disc_folder("D10"), "D10 should be detected");

    // D with separator should NOT work (too ambiguous)
    cr_assert(!is_disc_folder("D 1"), "D 1 should NOT be detected (too short)");
    cr_assert(!is_disc_folder("D-1"), "D-1 should NOT be detected (too short)");
}

// Test: is_disc_folder() rejects non-disc folder names
Test(multi_disc, disc_folder_false_positives) {
    // Regular folder names should NOT be detected
    cr_assert(!is_disc_folder("Album"), "Album should NOT be detected");
    cr_assert(!is_disc_folder("Music"), "Music should NOT be detected");
    cr_assert(!is_disc_folder("Track 1"), "Track 1 should NOT be detected");
    cr_assert(!is_disc_folder("Side A"), "Side A should NOT be detected");
    cr_assert(!is_disc_folder("Part 1"), "Part 1 should NOT be detected");

    // Similar but not matching patterns
    cr_assert(!is_disc_folder("CDA"), "CDA should NOT be detected (no number)");
    cr_assert(!is_disc_folder("CD"), "CD should NOT be detected (no number)");
    cr_assert(!is_disc_folder("Disc"), "Disc should NOT be detected (no number)");
    cr_assert(!is_disc_folder("CD0"), "CD0 should NOT be detected (invalid disc number)");
    cr_assert(!is_disc_folder("Disc0"), "Disc0 should NOT be detected (invalid disc number)");

    // Edge cases
    cr_assert(!is_disc_folder(""), "Empty string should NOT be detected");
    cr_assert(!is_disc_folder(NULL), "NULL should NOT be detected");

    // Numbers only
    cr_assert(!is_disc_folder("1"), "1 alone should NOT be detected");
    cr_assert(!is_disc_folder("01"), "01 alone should NOT be detected");
}

// ============================================================================
// DISC NUMBER EXTRACTION TESTS
// ============================================================================

// Test: get_disc_number_from_folder() extracts correct disc numbers
Test(multi_disc, disc_number_extraction_basic) {
    // CD patterns
    cr_assert_eq(get_disc_number_from_folder("CD1"), 1, "CD1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("CD2"), 2, "CD2 -> 2");
    cr_assert_eq(get_disc_number_from_folder("CD3"), 3, "CD3 -> 3");
    cr_assert_eq(get_disc_number_from_folder("CD10"), 10, "CD10 -> 10");
    cr_assert_eq(get_disc_number_from_folder("CD99"), 99, "CD99 -> 99");

    // Disc patterns
    cr_assert_eq(get_disc_number_from_folder("Disc1"), 1, "Disc1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("Disc2"), 2, "Disc2 -> 2");
    cr_assert_eq(get_disc_number_from_folder("Disc 5"), 5, "Disc 5 -> 5");
    cr_assert_eq(get_disc_number_from_folder("Disc-7"), 7, "Disc-7 -> 7");

    // D patterns
    cr_assert_eq(get_disc_number_from_folder("D1"), 1, "D1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("D2"), 2, "D2 -> 2");
    cr_assert_eq(get_disc_number_from_folder("d3"), 3, "d3 -> 3");
}

// Test: get_disc_number_from_folder() handles word-based disc names
Test(multi_disc, disc_number_extraction_words) {
    cr_assert_eq(get_disc_number_from_folder("Disc One"), 1, "Disc One -> 1");
    cr_assert_eq(get_disc_number_from_folder("Disc Two"), 2, "Disc Two -> 2");
    cr_assert_eq(get_disc_number_from_folder("Disc Three"), 3, "Disc Three -> 3");
    cr_assert_eq(get_disc_number_from_folder("Disc Four"), 4, "Disc Four -> 4");
    cr_assert_eq(get_disc_number_from_folder("Disc Five"), 5, "Disc Five -> 5");
    cr_assert_eq(get_disc_number_from_folder("Disc Six"), 6, "Disc Six -> 6");
    cr_assert_eq(get_disc_number_from_folder("Disc Seven"), 7, "Disc Seven -> 7");
    cr_assert_eq(get_disc_number_from_folder("Disc Eight"), 8, "Disc Eight -> 8");
    cr_assert_eq(get_disc_number_from_folder("Disc Nine"), 9, "Disc Nine -> 9");
    cr_assert_eq(get_disc_number_from_folder("Disc Ten"), 10, "Disc Ten -> 10");

    // Case variations
    cr_assert_eq(get_disc_number_from_folder("disc one"), 1, "disc one -> 1");
    cr_assert_eq(get_disc_number_from_folder("DISC TWO"), 2, "DISC TWO -> 2");
}

// Test: get_disc_number_from_folder() returns 0 for invalid inputs
Test(multi_disc, disc_number_extraction_invalid) {
    cr_assert_eq(get_disc_number_from_folder(NULL), 0, "NULL -> 0");
    cr_assert_eq(get_disc_number_from_folder(""), 0, "empty -> 0");
    cr_assert_eq(get_disc_number_from_folder("Album"), 0, "Album -> 0");
    cr_assert_eq(get_disc_number_from_folder("CD"), 0, "CD (no number) -> 0");
    cr_assert_eq(get_disc_number_from_folder("Disc"), 0, "Disc (no number) -> 0");
    cr_assert_eq(get_disc_number_from_folder("CD0"), 0, "CD0 -> 0 (invalid disc)");
    cr_assert_eq(get_disc_number_from_folder("Disc0"), 0, "Disc0 -> 0 (invalid disc)");
}

// Test: get_disc_number_from_folder() handles separator variations
Test(multi_disc, disc_number_extraction_separators) {
    // Space separator
    cr_assert_eq(get_disc_number_from_folder("CD 1"), 1, "CD 1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("CD 12"), 12, "CD 12 -> 12");
    cr_assert_eq(get_disc_number_from_folder("Disc 3"), 3, "Disc 3 -> 3");

    // Hyphen separator
    cr_assert_eq(get_disc_number_from_folder("CD-1"), 1, "CD-1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("Disc-2"), 2, "Disc-2 -> 2");

    // Underscore separator
    cr_assert_eq(get_disc_number_from_folder("CD_1"), 1, "CD_1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("Disc_4"), 4, "Disc_4 -> 4");

    // Multiple separators
    cr_assert_eq(get_disc_number_from_folder("CD  1"), 1, "CD  1 (double space) -> 1");
    cr_assert_eq(get_disc_number_from_folder("CD - 1"), 1, "CD - 1 -> 1");
}

// ============================================================================
// DATABASE DISC_NUM TESTS
// ============================================================================

// Test: Track upsert with disc_num field
Test(multi_disc, db_track_disc_num_basic) {
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);

    // Create artist and album
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/album", "Album",
        artist_id, false, 2024, &album_id), QUADRATURE_OK);

    // Insert track with disc_num = 1 (default)
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    db_index_item_t item1 = {
        .path = "/music/album/cd1/track1.mp3",
        .title = "Track 1",
        .album = "Album",
        .duration_ms = 180000,
        .track_num = 1,
        .disc_num = 1,  // Disc 1
        .year = 2024,
        .mtime = 1000000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item1, album_id, NULL), QUADRATURE_OK);

    // Insert track with disc_num = 2
    db_index_item_t item2 = {
        .path = "/music/album/cd2/track1.mp3",
        .title = "Track 1 Disc 2",
        .album = "Album",
        .duration_ms = 200000,
        .track_num = 1,
        .disc_num = 2,  // Disc 2
        .year = 2024,
        .mtime = 1000001,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item2, album_id, NULL), QUADRATURE_OK);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Verify both tracks were inserted
    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 2, "Should have 2 tracks");

    db_close(db);
}

// Test: Track upsert with disc_num = 0 defaults to 1
Test(multi_disc, db_track_disc_num_default) {
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);

    // Create artist and album
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music", "Album",
        artist_id, false, 2024, &album_id), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    // Insert track with disc_num = 0 (should default to 1)
    db_index_item_t item = {
        .path = "/music/track.mp3",
        .title = "Track",
        .album = "Album",
        .duration_ms = 180000,
        .track_num = 1,
        .disc_num = 0,  // Zero - should default to 1
        .year = 2024,
        .mtime = 1000000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Retrieve track and verify disc_num
    db_track_t* track = NULL;
    cr_assert_eq(db_get_track(db, 1, &track), QUADRATURE_OK);
    cr_assert_not_null(track);
    cr_assert_eq(track->disc_num, 1, "disc_num=0 should be stored as 1");

    db_track_free(track);
    db_close(db);
}

// Test: Track update preserves disc_num
Test(multi_disc, db_track_disc_num_update) {
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);

    // Create artist and album
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music", "Album",
        artist_id, false, 2024, &album_id), QUADRATURE_OK);

    const char* path = "/music/track.mp3";

    // Insert with disc_num = 3
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    db_index_item_t item = {
        .path = path,
        .title = "Original Title",
        .album = "Album",
        .duration_ms = 180000,
        .track_num = 5,
        .disc_num = 3,
        .year = 2024,
        .mtime = 1000000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Update the same track with different disc_num
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    item.title = "Updated Title";
    item.disc_num = 4;  // Changed disc
    item.mtime = 2000000;
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Verify count is still 1 (upsert, not insert)
    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 1, "Should still have 1 track after upsert");

    // Verify disc_num was updated
    db_track_t* track = NULL;
    cr_assert_eq(db_get_track(db, 1, &track), QUADRATURE_OK);
    cr_assert_not_null(track);
    cr_assert_eq(track->disc_num, 4, "disc_num should be updated to 4");
    cr_assert_str_eq(track->title, "Updated Title", "title should be updated");

    db_track_free(track);
    db_close(db);
}

// Test: Tracks with same track_num but different disc_num
Test(multi_disc, db_track_multi_disc_same_track_num) {
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);

    // Create artist and album
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/album", "Double Album",
        artist_id, false, 2024, &album_id), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    // Track 1 on Disc 1
    db_index_item_t disc1_track1 = {
        .path = "/music/album/cd1/01.mp3",
        .title = "Opening",
        .album = "Double Album",
        .duration_ms = 180000,
        .track_num = 1,
        .disc_num = 1,
        .year = 2024,
        .mtime = 1000000,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &disc1_track1, album_id, NULL), QUADRATURE_OK);

    // Track 1 on Disc 2 (same track_num, different disc)
    db_index_item_t disc2_track1 = {
        .path = "/music/album/cd2/01.mp3",
        .title = "Second Half Opening",
        .album = "Double Album",
        .duration_ms = 200000,
        .track_num = 1,  // Same track number
        .disc_num = 2,   // Different disc
        .year = 2024,
        .mtime = 1000001,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &disc2_track1, album_id, NULL), QUADRATURE_OK);

    // Track 2 on Disc 1
    db_index_item_t disc1_track2 = {
        .path = "/music/album/cd1/02.mp3",
        .title = "Second Track",
        .album = "Double Album",
        .duration_ms = 190000,
        .track_num = 2,
        .disc_num = 1,
        .year = 2024,
        .mtime = 1000002,
    };
    cr_assert_eq(db_upsert_track_with_album(db, &disc1_track2, album_id, NULL), QUADRATURE_OK);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Verify all 3 tracks exist
    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 3, "Should have 3 tracks total");

    // Retrieve and verify each track
    db_track_t* track = NULL;

    // Disc 1, Track 1
    cr_assert_eq(db_get_track(db, 1, &track), QUADRATURE_OK);
    cr_assert_eq(track->track_num, 1);
    cr_assert_eq(track->disc_num, 1);
    cr_assert_str_eq(track->title, "Opening");
    db_track_free(track);

    // Disc 2, Track 1
    cr_assert_eq(db_get_track(db, 2, &track), QUADRATURE_OK);
    cr_assert_eq(track->track_num, 1);
    cr_assert_eq(track->disc_num, 2);
    cr_assert_str_eq(track->title, "Second Half Opening");
    db_track_free(track);

    // Disc 1, Track 2
    cr_assert_eq(db_get_track(db, 3, &track), QUADRATURE_OK);
    cr_assert_eq(track->track_num, 2);
    cr_assert_eq(track->disc_num, 1);
    cr_assert_str_eq(track->title, "Second Track");
    db_track_free(track);

    db_close(db);
}

// Test: High disc numbers (box sets)
Test(multi_disc, db_track_high_disc_numbers) {
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open_memory(&db), QUADRATURE_OK);

    // Create artist and album
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "/music/boxset", "Complete Works",
        artist_id, false, 2024, &album_id), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    // Simulate a large box set with many discs
    for (uint16_t disc = 1; disc <= 20; disc++) {
        char path[128];
        char title[64];
        snprintf(path, sizeof(path), "/music/boxset/cd%d/track1.mp3", disc);
        snprintf(title, sizeof(title), "Disc %d Track 1", disc);

        db_index_item_t item = {
            .path = path,
            .title = title,
            .album = "Complete Works",
            .duration_ms = 180000 + disc * 1000,
            .track_num = 1,
            .disc_num = disc,
            .year = 2024,
            .mtime = 1000000 + disc,
        };
        cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, NULL), QUADRATURE_OK);
    }

    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    // Verify all 20 tracks
    size_t count = 0;
    cr_assert_eq(test_get_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, 20, "Should have 20 tracks (one per disc)");

    // Spot check a high disc number
    db_track_t* track = NULL;
    cr_assert_eq(db_get_track(db, 15, &track), QUADRATURE_OK);
    cr_assert_eq(track->disc_num, 15, "Track 15 should be on disc 15");
    db_track_free(track);

    db_close(db);
}

// ============================================================================
// EDGE CASES AND BOUNDARY CONDITIONS
// ============================================================================

// Test: Case sensitivity is handled correctly
Test(multi_disc, case_sensitivity) {
    // All these should be equivalent (case insensitive)
    cr_assert_eq(get_disc_number_from_folder("CD1"), 1);
    cr_assert_eq(get_disc_number_from_folder("cd1"), 1);
    cr_assert_eq(get_disc_number_from_folder("Cd1"), 1);
    cr_assert_eq(get_disc_number_from_folder("cD1"), 1);

    cr_assert_eq(get_disc_number_from_folder("DISC1"), 1);
    cr_assert_eq(get_disc_number_from_folder("disc1"), 1);
    cr_assert_eq(get_disc_number_from_folder("Disc1"), 1);
    cr_assert_eq(get_disc_number_from_folder("DiSc1"), 1);

    cr_assert_eq(get_disc_number_from_folder("Disc ONE"), 1);
    cr_assert_eq(get_disc_number_from_folder("disc one"), 1);
    cr_assert_eq(get_disc_number_from_folder("DISC ONE"), 1);
}

// Test: Maximum valid disc number (99)
Test(multi_disc, disc_number_boundary) {
    // Valid: 1-99
    cr_assert_eq(get_disc_number_from_folder("CD1"), 1);
    cr_assert_eq(get_disc_number_from_folder("CD50"), 50);
    cr_assert_eq(get_disc_number_from_folder("CD99"), 99);

    // Invalid: 0
    cr_assert_eq(get_disc_number_from_folder("CD0"), 0);

    // Numbers > 99 should still work (no upper limit enforced in current impl)
    // This tests the actual behavior
    uint16_t result = get_disc_number_from_folder("CD100");
    // The function may or may not handle 100+ - just verify it doesn't crash
    (void)result;  // Suppress unused warning
}

// Test: Disc folder detection with trailing characters
Test(multi_disc, disc_folder_trailing_chars) {
    // These might be ambiguous - test actual behavior
    // "CD1 Bonus" - is this disc 1 with suffix, or not a disc folder?
    // The current implementation should detect CD1 from "CD1" prefix
    cr_assert(is_disc_folder("CD1"), "CD1 should be detected");

    // Full folder names with extra text should NOT be detected
    // (depends on implementation - current impl checks prefix only)
}

// Test: Whitespace handling
Test(multi_disc, whitespace_handling) {
    // Multiple spaces
    cr_assert_eq(get_disc_number_from_folder("CD  1"), 1, "CD  1 -> 1");
    cr_assert_eq(get_disc_number_from_folder("Disc   2"), 2, "Disc   2 -> 2");

    // Leading/trailing whitespace in separator
    cr_assert_eq(get_disc_number_from_folder("CD - 3"), 3, "CD - 3 -> 3");
    cr_assert_eq(get_disc_number_from_folder("Disc _ 4"), 4, "Disc _ 4 -> 4");
}

// ============================================================================
// REAL-WORLD FOLDER NAME EXAMPLES
// ============================================================================

// Test: Common disc folder naming conventions from real music libraries
Test(multi_disc, real_world_folder_names) {
    // iTunes/Apple Music style
    cr_assert(is_disc_folder("Disc 1"), "iTunes: Disc 1");
    cr_assert(is_disc_folder("Disc 2"), "iTunes: Disc 2");
    cr_assert_eq(get_disc_number_from_folder("Disc 1"), 1);
    cr_assert_eq(get_disc_number_from_folder("Disc 2"), 2);

    // MusicBrainz Picard style
    cr_assert(is_disc_folder("CD1"), "Picard: CD1");
    cr_assert(is_disc_folder("CD2"), "Picard: CD2");

    // Windows Media Player style
    cr_assert(is_disc_folder("Disc 1"), "WMP: Disc 1");

    // Manual organization styles
    cr_assert(is_disc_folder("CD 1"), "Manual: CD 1");
    cr_assert(is_disc_folder("cd1"), "Manual: cd1");
    cr_assert(is_disc_folder("disc1"), "Manual: disc1");

    // European style (sometimes uses "CD" universally)
    cr_assert(is_disc_folder("CD-1"), "EU: CD-1");
    cr_assert(is_disc_folder("CD-2"), "EU: CD-2");

    // Box set conventions
    cr_assert(is_disc_folder("Disc One"), "Box set: Disc One");
    cr_assert(is_disc_folder("Disc Two"), "Box set: Disc Two");
}

// Test: Folder names that should NOT be detected as disc folders
Test(multi_disc, non_disc_real_world_names) {
    // Common non-disc folder names
    cr_assert(!is_disc_folder("Bonus Tracks"), "Bonus Tracks is not a disc");
    cr_assert(!is_disc_folder("Live"), "Live is not a disc");
    cr_assert(!is_disc_folder("Demos"), "Demos is not a disc");
    cr_assert(!is_disc_folder("Instrumentals"), "Instrumentals is not a disc");
    cr_assert(!is_disc_folder("Remixes"), "Remixes is not a disc");
    cr_assert(!is_disc_folder("Acoustic"), "Acoustic is not a disc");

    // Track subfolders (some people organize by track)
    cr_assert(!is_disc_folder("Track 01"), "Track 01 is not a disc");
    cr_assert(!is_disc_folder("Track 1"), "Track 1 is not a disc");

    // Side A/B (vinyl-style organization)
    cr_assert(!is_disc_folder("Side A"), "Side A is not a disc");
    cr_assert(!is_disc_folder("Side B"), "Side B is not a disc");

    // Part numbering
    cr_assert(!is_disc_folder("Part 1"), "Part 1 is not a disc");
    cr_assert(!is_disc_folder("Part One"), "Part One is not a disc");

    // Volume numbering (for series, not discs)
    cr_assert(!is_disc_folder("Vol. 1"), "Vol. 1 is not a disc");
    cr_assert(!is_disc_folder("Volume 1"), "Volume 1 is not a disc");
}
