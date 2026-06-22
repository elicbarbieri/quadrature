/**
 * Unit tests for library_cache.c
 *
 * Tests the foundation-layer LibraryCache component including entity caching,
 * track navigation, list queries, search, and cache management.
 */

#include <criterion/criterion.h>
#include "test_helpers.h"
#include "quadrature/library.h"
#include "quadrature/database.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

/* Single-library cache helper for tests */
static quadrature_result_t
test_cache_create(const char *db_path, const char *music_base, library_cache_t **out)
{
    if (!db_path || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;
    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = music_base,
        .display_name = NULL,
        .bitmap_index = 0,
    };
    return library_cache_create_multi(&src, 1, out);
}

// =============================================================================
// Test Fixtures
// =============================================================================

// Use PID in path to allow parallel test execution
static char test_db_path[256];

static quadrature_db_t *test_db = NULL;
static library_cache_t *test_cache = NULL;

// Generate unique database path for this process
static void
init_test_db_path(void)
{
    snprintf(test_db_path, sizeof(test_db_path), "/tmp/test_library_cache_%d.db", getpid());
}

// Clean up any existing test database files
static void
cleanup_test_db(void)
{
    char wal_path[280], shm_path[280];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", test_db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", test_db_path);
    unlink(test_db_path);
    unlink(wal_path);
    unlink(shm_path);
}

// Set up a test database with sample data
static void
setup_test_data(void)
{
    // Initialize unique path and clean up any previous test database
    init_test_db_path();
    cleanup_test_db();

    // Create file-based database (library_cache_create needs to open it readonly)
    quadrature_result_t res = db_open(test_db_path, false, &test_db);
    cr_assert_eq(res, QUADRATURE_OK);
    cr_assert_not_null(test_db);

    cr_assert_eq(db_begin_transaction(test_db), QUADRATURE_OK);
    int64_t test_artist_id = test_goc_artist(test_db, "Test Artist", NULL, NULL);
    int64_t another_artist_id = test_goc_artist(test_db, "Another Artist", NULL, NULL);
    cr_assert(test_artist_id > 0);
    cr_assert(another_artist_id > 0);

    int64_t album1_id
        = test_insert_album(test_db, "Artist1/Album1", "First Album", test_artist_id, 2020);
    int64_t album2_id
        = test_insert_album(test_db, "Artist1/Album2", "Double Album", test_artist_id, 2021);
    int64_t album3_id
        = test_insert_album(test_db, "Artist2/Album", "Another Album", another_artist_id, 2022);

    const int64_t ta[1] = { test_artist_id };
    const char *names[1] = { "Test Artist" };
    const char *joins[1] = { "" };
    const int64_t oa[1] = { another_artist_id };
    const char *onames[1] = { "Another Artist" };

    /* Album 1 — single disc, 3 tracks. */
    test_insert_track_full(
        test_db, album1_id, "01-track1.mp3", "Track One", 1, 1, 180000, ta, names, joins, 1);
    test_insert_track_full(
        test_db, album1_id, "02-track2.mp3", "Track Two", 2, 1, 200000, ta, names, joins, 1);
    test_insert_track_full(
        test_db, album1_id, "03-track3.mp3", "Track Three", 3, 1, 220000, ta, names, joins, 1);

    /* Album 2 — two discs, 2 tracks each. */
    test_insert_track_full(
        test_db, album2_id, "CD1/01-intro.mp3", "Disc 1 Intro", 1, 1, 150000, ta, names, joins, 1);
    test_insert_track_full(
        test_db, album2_id, "CD1/02-main.mp3", "Disc 1 Main", 2, 1, 300000, ta, names, joins, 1);
    test_insert_track_full(test_db,
                           album2_id,
                           "CD2/01-opening.mp3",
                           "Disc 2 Opening",
                           1,
                           2,
                           180000,
                           ta,
                           names,
                           joins,
                           1);
    test_insert_track_full(test_db,
                           album2_id,
                           "CD2/02-finale.mp3",
                           "Disc 2 Finale",
                           2,
                           2,
                           400000,
                           ta,
                           names,
                           joins,
                           1);

    /* Album 3 — different artist. */
    test_insert_track_full(
        test_db, album3_id, "track.mp3", "Other Track", 1, 1, 240000, oa, onames, joins, 1);

    cr_assert_eq(db_commit(test_db), QUADRATURE_OK);
}

static void
setup(void)
{
    setup_test_data();

    // Close write connection before creating cache (cache opens its own readonly connection)
    db_close(test_db);
    test_db = NULL;

    // Create cache with db path
    quadrature_result_t res = test_cache_create(test_db_path, "/music", &test_cache);
    cr_assert_eq(res, QUADRATURE_OK);
    cr_assert_not_null(test_cache);

    // Warm the cache — all data must be pre-populated before reads
    library_cache_warm_slot_blocking(test_cache, 0);
}

static void
teardown(void)
{
    library_cache_destroy(test_cache);
    test_cache = NULL;
    cleanup_test_db();
}

// =============================================================================
// Lifecycle Tests
// =============================================================================

Test(library_cache, create_destroy)
{
    // Use unique path for this test
    char db_path[256], wal_path[280], shm_path[280];
    snprintf(db_path, sizeof(db_path), "/tmp/test_library_cache_lifecycle_%d.db", getpid());
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);

    // Clean up any previous test db
    unlink(db_path);
    unlink(wal_path);
    unlink(shm_path);

    // Create a file-based database
    quadrature_db_t *db = NULL;
    db_open(db_path, false, &db);
    db_close(db); // Close so cache can open it readonly

    library_cache_t *cache = NULL;
    quadrature_result_t res = test_cache_create(db_path, "/music", &cache);
    cr_assert_eq(res, QUADRATURE_OK);
    cr_assert_not_null(cache);

    library_cache_destroy(cache);

    // Cleanup
    unlink(db_path);
    unlink(wal_path);
    unlink(shm_path);
}

Test(library_cache, null_safety)
{
    // Null parameters should not crash
    cr_assert_eq(test_cache_create(NULL, "/music", NULL), QUADRATURE_ERROR_INVALID_PARAM);

    library_cache_t *cache = NULL;
    cr_assert_eq(test_cache_create("/nonexistent/path.db", "/music", NULL),
                 QUADRATURE_ERROR_INVALID_PARAM);
    cr_assert_eq(test_cache_create(NULL, "/music", &cache), QUADRATURE_ERROR_INVALID_PARAM);

    // Destroy accepts NULL
    library_cache_destroy(NULL);

    /* Note: Getter/setter functions on NULL cache are g_assert() crashes
     * (invariant violation). Callers must ensure cache is non-NULL. */
}

// =============================================================================
// Entity Caching Tests
// =============================================================================

Test(library_cache, get_track_caches_result, .init = setup, .fini = teardown)
{
    // First call should fetch from DB
    const library_track_info_t *track1 = library_cache_get_track(test_cache, 1);
    cr_assert_not_null(track1);
    cr_assert_eq(track1->track_id, 1);
    cr_assert_str_eq(track1->title, "Track One");

    // Second call should return same pointer (cached)
    const library_track_info_t *track1_again = library_cache_get_track(test_cache, 1);
    cr_assert_eq(track1, track1_again, "Expected cached pointer to be returned");
}

Test(library_cache, get_track_triggers_album_prefetch, .init = setup, .fini = teardown)
{
    // Get a track - this should also prefetch album track IDs
    const library_track_info_t *track = library_cache_get_track(test_cache, 1);
    cr_assert_not_null(track);

    // Now next/prev should work without additional DB queries
    int64_t next = library_cache_get_next_track_id(test_cache, 1);
    cr_assert_eq(next, 2);
}

Test(library_cache, get_track_nonexistent, .init = setup, .fini = teardown)
{
    const library_track_info_t *track = library_cache_get_track(test_cache, 9999);
    cr_assert_null(track);
}

Test(library_cache, get_album, .init = setup, .fini = teardown)
{
    const library_album_info_t *album = library_cache_get_album(test_cache, 1, LIBRARY_MASK_ALL);
    cr_assert_not_null(album);
    cr_assert_eq(album->album_id, 1);
    cr_assert_str_eq(album->title, "First Album");
    cr_assert_eq(album->track_count, 3);
}

Test(library_cache, get_artist, .init = setup, .fini = teardown)
{
    const library_artist_info_t *artist = library_cache_get_artist(test_cache, 1, LIBRARY_MASK_ALL);
    cr_assert_not_null(artist);
    cr_assert_eq(artist->artist_id, 1);
    cr_assert_str_eq(artist->name, "Test Artist");
}

// =============================================================================
// Track Navigation Tests
// =============================================================================

Test(library_cache, next_track_simple_album, .init = setup, .fini = teardown)
{
    // Track 1 -> Track 2
    int64_t next = library_cache_get_next_track_id(test_cache, 1);
    cr_assert_eq(next, 2);

    // Track 2 -> Track 3
    next = library_cache_get_next_track_id(test_cache, 2);
    cr_assert_eq(next, 3);
}

Test(library_cache, next_track_last_returns_zero, .init = setup, .fini = teardown)
{
    // Track 3 is last in album 1
    int64_t next = library_cache_get_next_track_id(test_cache, 3);
    cr_assert_eq(next, 0);
}

Test(library_cache, prev_track_first_returns_zero, .init = setup, .fini = teardown)
{
    // Track 1 is first in album 1
    int64_t prev = library_cache_get_prev_track_id(test_cache, 1);
    cr_assert_eq(prev, 0);
}

Test(library_cache, prev_track_simple, .init = setup, .fini = teardown)
{
    // Track 3 -> Track 2
    int64_t prev = library_cache_get_prev_track_id(test_cache, 3);
    cr_assert_eq(prev, 2);

    // Track 2 -> Track 1
    prev = library_cache_get_prev_track_id(test_cache, 2);
    cr_assert_eq(prev, 1);
}

Test(library_cache, next_track_multi_disc, .init = setup, .fini = teardown)
{
    // Multi-disc album: tracks 4-7
    // Disc 1: track 4, track 5
    // Disc 2: track 6, track 7

    // Track 4 (disc 1, track 1) -> Track 5 (disc 1, track 2)
    int64_t next = library_cache_get_next_track_id(test_cache, 4);
    cr_assert_eq(next, 5);

    // Track 5 (disc 1, track 2) -> Track 6 (disc 2, track 1)
    next = library_cache_get_next_track_id(test_cache, 5);
    cr_assert_eq(next, 6);

    // Track 6 (disc 2, track 1) -> Track 7 (disc 2, track 2)
    next = library_cache_get_next_track_id(test_cache, 6);
    cr_assert_eq(next, 7);

    // Track 7 is last
    next = library_cache_get_next_track_id(test_cache, 7);
    cr_assert_eq(next, 0);
}

Test(library_cache, navigation_invalid_track, .init = setup, .fini = teardown)
{
    cr_assert_eq(library_cache_get_next_track_id(test_cache, 0), 0);
    cr_assert_eq(library_cache_get_next_track_id(test_cache, LIBRARY_MASK_ALL), 0);
    cr_assert_eq(library_cache_get_next_track_id(test_cache, 9999), 0);

    cr_assert_eq(library_cache_get_prev_track_id(test_cache, 0), 0);
    cr_assert_eq(library_cache_get_prev_track_id(test_cache, LIBRARY_MASK_ALL), 0);
    cr_assert_eq(library_cache_get_prev_track_id(test_cache, 9999), 0);
}

// =============================================================================
// List Query Tests
// =============================================================================

Test(library_cache, get_tracks_by_album_ordered, .init = setup, .fini = teardown)
{
    GPtrArray *tracks = library_cache_get_tracks_by_album(test_cache, 1, LIBRARY_MASK_ALL);
    cr_assert_not_null(tracks);
    cr_assert_eq(tracks->len, 3);

    // Verify order
    const library_track_info_t *t1 = g_ptr_array_index(tracks, 0);
    const library_track_info_t *t2 = g_ptr_array_index(tracks, 1);
    const library_track_info_t *t3 = g_ptr_array_index(tracks, 2);

    cr_assert_eq(t1->track_num, 1);
    cr_assert_eq(t2->track_num, 2);
    cr_assert_eq(t3->track_num, 3);

    // Clean up (the GPtrArray is not cached in current implementation)
    g_ptr_array_unref(tracks);
}

Test(library_cache, get_albums_by_artist, .init = setup, .fini = teardown)
{
    GPtrArray *albums = library_cache_get_albums_by_artist(test_cache, 1, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 2); // "First Album" and "Double Album"
    g_ptr_array_unref(albums);
}

Test(library_cache, get_artists_loads_all, .init = setup, .fini = teardown)
{
    GPtrArray *artists = library_cache_get_artists_filtered(
        test_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert_eq(artists->len, 2); // "Test Artist" and "Another Artist"
    g_ptr_array_unref(artists);

    // Second call should also work
    GPtrArray *artists2 = library_cache_get_artists_filtered(
        test_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists2);
    cr_assert_eq(artists2->len, 2);
    g_ptr_array_unref(artists2);
}

Test(library_cache, get_artists_sorted, .init = setup, .fini = teardown)
{
    // Name ascending
    GPtrArray *artists = library_cache_get_artists_filtered(
        test_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert_eq(artists->len, 2);

    const library_artist_info_t *first = g_ptr_array_index(artists, 0);
    const library_artist_info_t *second = g_ptr_array_index(artists, 1);
    cr_assert(strcmp(first->name, second->name) < 0, "Expected ascending sort");
    g_ptr_array_unref(artists);

    // Name descending
    GPtrArray *artists_desc = library_cache_get_artists_filtered(
        test_cache, LIBRARY_SORT_NAME_DESC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists_desc);

    first = g_ptr_array_index(artists_desc, 0);
    second = g_ptr_array_index(artists_desc, 1);
    cr_assert(strcmp(first->name, second->name) > 0, "Expected descending sort");
    g_ptr_array_unref(artists_desc);
}

Test(library_cache, get_albums_sorted, .init = setup, .fini = teardown)
{
    GPtrArray *albums = library_cache_get_albums_filtered(
        test_cache, LIBRARY_SORT_YEAR_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 3); // Three albums total

    // Verify year ascending order
    const library_album_info_t *a1 = g_ptr_array_index(albums, 0);
    const library_album_info_t *a2 = g_ptr_array_index(albums, 1);
    cr_assert(a1->year <= a2->year, "Expected year ascending sort");
    g_ptr_array_unref(albums);
}

// =============================================================================
// Search Tests
// =============================================================================

Test(library_cache, search_returns_results, .init = setup, .fini = teardown)
{
    // Search should return results
    library_search_results_t *results = library_cache_search(
        test_cache, "Track", LIBRARY_SEARCH_FILTER_ALL, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);
    cr_assert(results->tracks->len > 0, "Expected track results");

    library_search_results_free(results);
}

Test(library_cache, search_different_query_gives_results, .init = setup, .fini = teardown)
{
    // First search
    library_search_results_t *results1 = library_cache_search(
        test_cache, "Track", LIBRARY_SEARCH_FILTER_ALL, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results1);

    // Different query should also give results
    library_search_results_t *results2 = library_cache_search(
        test_cache, "Album", LIBRARY_SEARCH_FILTER_ALL, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results2);

    library_search_results_free(results1);
    library_search_results_free(results2);
}

Test(library_cache, search_filter_artists, .init = setup, .fini = teardown)
{
    library_search_results_t *results = library_cache_search(
        test_cache, "Test", LIBRARY_SEARCH_FILTER_ARTISTS, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);
    cr_assert(results->artists->len > 0, "Expected artist results");

    library_search_results_free(results);
}

// =============================================================================
// Prefetch Tests (verify no crash, can't easily verify kernel behavior)
// =============================================================================

Test(library_cache, prefetch_audio_files_resolves_paths, .init = setup, .fini = teardown)
{
    int64_t track_ids[] = { 1, 2, 3 };

    // Should not crash
    library_cache_prefetch_audio_files(test_cache, track_ids, 3);
}

Test(library_cache, prefetch_audio_files_empty, .init = setup, .fini = teardown)
{
    // Should handle empty/NULL gracefully
    library_cache_prefetch_audio_files(test_cache, NULL, 0);
    library_cache_prefetch_audio_files(test_cache, NULL, 5);

    int64_t track_ids[] = { 1 };
    library_cache_prefetch_audio_files(test_cache, track_ids, 0);
}

// =============================================================================
// Cache Management Tests
// =============================================================================

Test(library_cache, clear_removes_all, .init = setup, .fini = teardown)
{
    // Cache some data
    const library_track_info_t *track = library_cache_get_track(test_cache, 1);
    GPtrArray *artists = library_cache_get_artists_filtered(
        test_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(track);
    cr_assert_not_null(artists);
    g_ptr_array_unref(artists);

    // Clear everything
    library_cache_clear(test_cache);

    // Re-warm — data must be pre-populated before reads
    library_cache_warm_slot_blocking(test_cache, 0);

    // Data should be fresh after clear + re-warm
    const library_track_info_t *track2 = library_cache_get_track(test_cache, 1);
    GPtrArray *artists2 = library_cache_get_artists_filtered(
        test_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(track2);
    cr_assert_not_null(artists2);
    cr_assert_neq(track, track2);
    g_ptr_array_unref(artists2);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

static void *
reader_thread(void *arg)
{
    library_cache_t *cache = (library_cache_t *)arg;

    for (int i = 0; i < 100; i++) {
        // Mix of operations
        library_cache_get_track(cache, 1);
        library_cache_get_next_track_id(cache, 1);
        GPtrArray *artists = library_cache_get_artists_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        if (artists)
            g_ptr_array_unref(artists);
        library_search_results_t *results = library_cache_search(
            cache, "Track", LIBRARY_SEARCH_FILTER_ALL, 5, NULL, LIBRARY_MASK_ALL);
        library_search_results_free(results);
    }

    return NULL;
}

Test(library_cache, concurrent_reads, .init = setup, .fini = teardown)
{
    pthread_t threads[4];

    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, reader_thread, test_cache);
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    // If we get here without crash or deadlock, test passes
}

static void *
writer_thread(void *arg)
{
    library_cache_t *cache = (library_cache_t *)arg;

    // Use the COW refresh path (clear_slot + warm_slot) which is the actual
    // production concurrent read/write pattern. library_cache_clear() is a
    // teardown operation that is NOT safe to call with concurrent readers.
    for (int i = 0; i < 10; i++) {
        library_cache_clear_slot(cache, 0);
        library_cache_warm_slot(cache, 0);
        library_cache_await_slot(cache, 0);
        usleep(1000);
    }

    return NULL;
}

Test(library_cache, concurrent_read_write, .init = setup, .fini = teardown)
{
    // Warm the cache first so readers have data
    library_cache_warm_slot(test_cache, 0);
    library_cache_await_slot(test_cache, 0);

    pthread_t readers[3];
    pthread_t writer;

    pthread_create(&writer, NULL, writer_thread, test_cache);

    for (int i = 0; i < 3; i++) {
        pthread_create(&readers[i], NULL, reader_thread, test_cache);
    }

    pthread_join(writer, NULL);
    for (int i = 0; i < 3; i++) {
        pthread_join(readers[i], NULL);
    }
}
