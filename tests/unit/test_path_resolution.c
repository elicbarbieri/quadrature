/**
 * Unit tests for track path resolution logic.
 *
 * Tests library_cache_resolve_track_path() which resolves:
 *   music_base + album_rel_path + track_rel_path → absolute path
 *
 * The key edge case is multi-disc albums where disc-2 tracks live in a
 * sibling directory, giving relative paths like "../Disc 2/track.flac".
 * g_canonicalize_filename() handles the ".." traversals.
 */

#include <criterion/criterion.h>
#include "quadrature/library.h"
#include "quadrature/database.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

/* Single-library cache helper for tests */
static quadrature_result_t test_cache_create(const char *db_path,
                                              const char *music_base,
                                              library_cache_t **out) {
    if (!db_path || !out) return QUADRATURE_ERROR_INVALID_PARAM;
    library_cache_source_t src = {
        .db_path = db_path, .music_base = music_base,
        .display_name = NULL, .bitmap_index = 0,
    };
    return library_cache_create_multi(&src, 1, out);
}

// =============================================================================
// Helpers
// =============================================================================

static char test_db_path[256];
static const char *MUSIC_BASE = "/home/user/Music";

static void init_test_db_path(void) {
    snprintf(test_db_path, sizeof(test_db_path),
             "/tmp/test_path_resolution_%d.db", getpid());
}

static void cleanup_test_db(void) {
    char wal_path[280], shm_path[280];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", test_db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", test_db_path);
    unlink(test_db_path);
    unlink(wal_path);
    unlink(shm_path);
}

/**
 * Insert one album + one track and return the track_id.
 * Caller must already be inside a transaction.
 */
static int64_t insert_album_track(quadrature_db_t *db,
                                  const char *album_path,
                                  const char *album_title,
                                  const char *track_path,
                                  const char *track_title) {
    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    cr_assert(artist_id > 0);

    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, album_path, album_title,
                                         artist_id, 2024, &album_id),
                 QUADRATURE_OK);

    db_index_item_t item = {
        .path       = track_path,
        .title      = track_title,
        .album      = album_title,
        .duration_ms = 240000,
        .track_num  = 1,
        .disc_num   = 1,
        .year       = 2024,
        .mtime      = 1000000,
    };
    int64_t track_id = 0;
    cr_assert_eq(db_upsert_track_with_album(db, &item, album_id, &track_id),
                 QUADRATURE_OK);
    cr_assert(track_id > 0);

    /* Wire up track_artists so the cache can resolve artist info */
    db_track_artist_t ta = {
        .artist_id    = artist_id,
        .position     = 0,
        .join_phrase  = "",
    };
    db_set_track_artists(db, track_id, &ta, 1);
    db_sync_album_fts(db, album_id);

    return track_id;
}

/**
 * Create a DB with a single album+track, build a cache, and resolve the path.
 * Returns the resolved absolute path (caller must g_free).
 */
static char *resolve_one(const char *album_path,
                         const char *track_path) {
    init_test_db_path();
    cleanup_test_db();

    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(test_db_path, &db), QUADRATURE_OK);

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    int64_t track_id = insert_album_track(db, album_path, "Album",
                                           track_path, "Track");
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);

    library_cache_t *cache = NULL;
    cr_assert_eq(test_cache_create(test_db_path, MUSIC_BASE, &cache),
                 QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    char *resolved = library_cache_resolve_track_path(cache, track_id);

    library_cache_destroy(cache);
    cleanup_test_db();
    return resolved;
}

// =============================================================================
// Test Cases
// =============================================================================

Test(path_resolution, simple_relative_path) {
    char *path = resolve_one("rock/Beatles/Abbey-Road",
                             "01-come-together.flac");
    cr_assert_not_null(path);
    cr_assert_str_eq(path,
        "/home/user/Music/rock/Beatles/Abbey-Road/01-come-together.flac");
    g_free(path);
}

Test(path_resolution, track_in_subdirectory) {
    char *path = resolve_one("rock/Pink-Floyd/The-Wall",
                             "CD1/01-in-the-flesh.flac");
    cr_assert_not_null(path);
    cr_assert_str_eq(path,
        "/home/user/Music/rock/Pink-Floyd/The-Wall/CD1/01-in-the-flesh.flac");
    g_free(path);
}

Test(path_resolution, multi_disc_cross_directory) {
    char *path = resolve_one("rock/Tool/Lateralus Disc 1",
                             "../Lateralus Disc 2/01-the-grudge.flac");
    cr_assert_not_null(path);
    cr_assert_str_eq(path,
        "/home/user/Music/rock/Tool/Lateralus Disc 2/01-the-grudge.flac");
    g_free(path);
}

Test(path_resolution, multi_disc_deep_traversal) {
    /* From "rock/Tool/Lateralus Disc 1", two ".." levels reach "rock/".
     * So "../../jazz/Compilation/01-bonus.flac" → "rock/jazz/Compilation/..." */
    char *path = resolve_one("rock/Tool/Lateralus Disc 1",
                             "../../jazz/Compilation/01-bonus.flac");
    cr_assert_not_null(path);
    cr_assert_str_eq(path,
        "/home/user/Music/rock/jazz/Compilation/01-bonus.flac");
    g_free(path);
}

Test(path_resolution, spaces_in_path) {
    char *path = resolve_one("rock/Led Zeppelin/Houses of the Holy",
                             "01 The Song Remains The Same.flac");
    cr_assert_not_null(path);
    cr_assert_str_eq(path,
        "/home/user/Music/rock/Led Zeppelin/Houses of the Holy/"
        "01 The Song Remains The Same.flac");
    g_free(path);
}

Test(path_resolution, nonexistent_track_returns_null) {
    init_test_db_path();
    cleanup_test_db();

    /* Create an empty DB so the cache can open it */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(test_db_path, &db), QUADRATURE_OK);
    db_close(db);

    library_cache_t *cache = NULL;
    cr_assert_eq(test_cache_create(test_db_path, MUSIC_BASE, &cache),
                 QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    char *path = library_cache_resolve_track_path(cache, 99999);
    cr_assert_null(path);

    library_cache_destroy(cache);
    cleanup_test_db();
}

Test(path_resolution, null_cache_returns_null) {
    char *path = library_cache_resolve_track_path(NULL, 1);
    cr_assert_null(path);
}

Test(path_resolution, dot_in_relative_path) {
    char *path = resolve_one("rock/Artist/Album",
                             "./01-track.flac");
    cr_assert_not_null(path);
    cr_assert_str_eq(path,
        "/home/user/Music/rock/Artist/Album/01-track.flac");
    g_free(path);
}
