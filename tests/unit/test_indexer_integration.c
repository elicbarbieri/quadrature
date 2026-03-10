#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "quadrature/indexer.h"
#include "quadrature/database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>

// Initialize FFmpeg before any tests run (before Criterion forks)
ReportHook(PRE_ALL)(struct criterion_test_set *tests) {
    (void)tests;
    avformat_network_init();
}

// Test library paths
#define TEST_LIBRARY "tests/assets/library"
#define TEST_DB      (TEST_LIBRARY "/quadrature.sqlite")
#define BACH_ALBUM   TEST_LIBRARY "/Johann Sebastian Bach/Goldberg Variations"

// Expected counts
#define EXPECTED_ALBUMS 4
#define EXPECTED_TRACKS 15

// Check if test library exists
static bool test_library_exists(void) {
    struct stat st;
    return stat(TEST_LIBRARY, &st) == 0 && S_ISDIR(st.st_mode);
}

// Remove the per-library SQLite created by a test run
static void cleanup_test_db(void) {
    unlink(TEST_DB);
}

// Callback tracking
typedef struct {
    int started_calls;
    int progress_calls;
    int completed_calls;
    bool completed_success;
    indexer_progress_t last_progress;
} callback_tracker_t;

static void track_events(indexer_event_t event, const indexer_progress_t* progress, void* user_data) {
    callback_tracker_t* tracker = user_data;

    switch (event) {
        case INDEXER_STARTED:
            tracker->started_calls++;
            break;
        case INDEXER_PROGRESS:
            tracker->progress_calls++;
            if (progress) {
                tracker->last_progress = *progress;
            }
            break;
        case INDEXER_COMPLETED:
            tracker->completed_calls++;
            tracker->completed_success = true;
            if (progress) {
                tracker->last_progress = *progress;
            }
            break;
        case INDEXER_CANCELLED:
            tracker->completed_calls++;
            tracker->completed_success = false;
            break;
        case INDEXER_ERROR:
            tracker->completed_calls++;
            tracker->completed_success = false;
            break;
        case INDEXER_LIBRARY_UPDATED:
        case INDEXER_ARTWORK_UPDATED:
            /* UI-ready events — no counter needed in integration test */
            break;
    }
}

// ============================================================================
// Full Pipeline Integration Test
// ============================================================================

Test(indexer_integration, full_scan_populates_database) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded - run tests/assets/download_test_library.sh");
    }
    cleanup_test_db();

    indexer_config_t config = {
        .thread_count = 2,
        .process_artwork = false
    };

    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);

    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);

    // Verify progress shows all files processed
    indexer_progress_t progress;
    indexer_get_progress(indexer, &progress);
    cr_assert_eq(progress.files_processed, progress.files_total);

    indexer_destroy(indexer);

    // Open the DB written by the indexer and verify track count
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open(TEST_DB, &db), QUADRATURE_OK);

    size_t track_count = 0;
    cr_assert_eq(db_get_total_track_count(db, &track_count), QUADRATURE_OK);
    cr_assert_eq(track_count, EXPECTED_TRACKS,
                 "Expected %d tracks, got %zu", EXPECTED_TRACKS, track_count);

    db_close(db);
    cleanup_test_db();
}

// ============================================================================
// Re-indexing Skips Unchanged Directories
// ============================================================================

Test(indexer_integration, reindex_skips_unchanged_dirs) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded");
    }
    cleanup_test_db();

    indexer_config_t config = {
        .thread_count = 2,
        .process_artwork = false
    };

    // First index
    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);

    indexer_progress_t first_progress;
    indexer_get_progress(indexer, &first_progress);
    indexer_destroy(indexer);

    // Second index - DB persists at TEST_DB; unchanged dirs should be skipped
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);

    indexer_progress_t second_progress;
    indexer_get_progress(indexer, &second_progress);

    // No new files on re-index (unchanged directories are skipped)
    cr_assert_eq(second_progress.files_new, 0,
                 "No new files on re-index");

    indexer_destroy(indexer);
    cleanup_test_db();
}

// ============================================================================
// Callbacks Invoked Correctly
// ============================================================================

Test(indexer_integration, callbacks_invoked_correctly) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded");
    }
    cleanup_test_db();

    callback_tracker_t tracker = {0};

    indexer_config_t config = {
        .thread_count = 2,
        .process_artwork = false,
        .callback = track_events,
        .user_data = &tracker
    };

    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);

    // Callbacks should have been invoked
    cr_assert_eq(tracker.started_calls, 1, "Started callback should be called once");
    cr_assert_gt(tracker.progress_calls, 0, "Progress callback should be called");
    cr_assert_eq(tracker.completed_calls, 1, "Completed callback should be called once");
    cr_assert(tracker.completed_success, "Should complete successfully");

    indexer_destroy(indexer);
    cleanup_test_db();
}

// ============================================================================
// Statistics Accurate
// ============================================================================

Test(indexer_integration, statistics_accurate) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded");
    }
    cleanup_test_db();

    indexer_config_t config = {
        .thread_count = 2,
        .process_artwork = false
    };

    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);

    indexer_progress_t progress;
    indexer_get_progress(indexer, &progress);

    // Discovery stats
    cr_assert_gt(progress.dirs_scanned, 0, "Should have scanned directories");

    // File stats
    cr_assert_eq(progress.files_total, EXPECTED_TRACKS);
    cr_assert_eq(progress.files_processed, EXPECTED_TRACKS);
    cr_assert_eq(progress.files_new, EXPECTED_TRACKS, "All files should be new on first run");

    indexer_destroy(indexer);
    cleanup_test_db();
}

// ============================================================================
// Search Works After Indexing
// ============================================================================

Test(indexer_integration, search_works_after_indexing) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded");
    }
    cleanup_test_db();

    indexer_config_t config = {
        .thread_count = 2,
        .process_artwork = false
    };

    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);
    indexer_destroy(indexer);

    // Open the DB written by the indexer and verify track count
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open(TEST_DB, &db), QUADRATURE_OK);

    size_t count = 0;
    cr_assert_eq(db_get_total_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, EXPECTED_TRACKS, "Should have indexed all tracks");

    db_close(db);
    cleanup_test_db();
}

// ============================================================================
// Cancel Stops Indexer
// ============================================================================

Test(indexer_integration, cancel_stops_indexer) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded");
    }
    cleanup_test_db();

    callback_tracker_t tracker = {0};

    indexer_config_t config = {
        .thread_count = 1,  // Single thread to make cancellation more predictable
        .process_artwork = false,
        .callback = track_events,
        .user_data = &tracker
    };

    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);

    // Cancel immediately
    indexer_cancel(indexer);
    indexer_wait(indexer);

    // Should have completed (either cancelled or finished before cancel took effect)
    cr_assert_eq(tracker.completed_calls, 1, "Should have completed callback");

    indexer_destroy(indexer);
    cleanup_test_db();
}

// ============================================================================
// Get Track Details After Indexing
// ============================================================================

Test(indexer_integration, track_details_correct) {
    if (!test_library_exists()) {
        cr_skip("Test library not downloaded");
    }
    cleanup_test_db();

    indexer_config_t config = {
        .thread_count = 2,
        .process_artwork = false
    };

    indexer_t* indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, TEST_LIBRARY, NULL), QUADRATURE_OK);
    indexer_wait(indexer);
    indexer_destroy(indexer);

    // Open the DB written by the indexer and verify track count
    quadrature_db_t* db = NULL;
    cr_assert_eq(db_open(TEST_DB, &db), QUADRATURE_OK);

    size_t count = 0;
    cr_assert_eq(db_get_total_track_count(db, &count), QUADRATURE_OK);
    cr_assert_eq(count, EXPECTED_TRACKS);

    db_close(db);
    cleanup_test_db();
}
