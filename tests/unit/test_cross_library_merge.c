/**
 * Behavioral tests for cross-library artist/album merging in library_cache.
 *
 * Creates two separate SQLite databases with overlapping artists (same MBID)
 * and albums (same musicbrainz_release_group_id), then verifies the public API
 * returns correct results for all library mask combinations.
 *
 * These tests assert OBSERVABLE BEHAVIOR only — no assertions about internal
 * merge state (library_index, merged_source_ids, merged_source_count).
 *
 * Core behavioral rules:
 *   Rule 1: MBID artists appear once when viewing all libraries
 *   Rule 2: Single-library filter shows that library's actual content
 *   Rule 3: Multi-library view deduplicates albums by MBRID
 *   Rule 4: Any library's artist ID resolves to the merged view
 *   Rule 5: Non-MBID artists never merge
 *   Rule 6: Featured appearances follow the same mask rules
 *   Rule 7: Library removal cleanly updates all views
 */

#include <criterion/criterion.h>
#include "quadrature/library.h"
#include "quadrature/database.h"
#include <unistd.h>
#include <stdio.h>

/* ── Test data constants ──────────────────────────────────────────────── */

/* Shared MBIDs for cross-library merge */
#define ARTIST_MBID_DAFT_PUNK  "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define ARTIST_MBID_APHEX_TWIN "f22942a1-6f70-4f48-866e-238cb2308fbd"

/* Album release IDs — RAM is shared between both libraries */
#define ALBUM_MBRID_RAM        "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define ALBUM_MBRID_DISCOVERY  "d073287b-d1bd-4f11-a933-a4386f8cf701"
#define ALBUM_MBRID_HOMEWORK   "647d7016-2683-42c6-b027-83114a7c3eec"
/* Human After All has DIFFERENT release IDs in each library (different editions) */
#define ALBUM_MBRID_HAA_LIB_A  "abbc40a0-2bcc-449e-bdd0-2dbae3213517"
#define ALBUM_MBRID_HAA_LIB_B  "77a2f001-ae10-45fe-8d56-6069edfe20fe"
/* Aphex Twin albums */
#define ALBUM_MBRID_SAW        "11111111-1111-1111-1111-111111111111"
#define ALBUM_MBRID_DRUKQS     "22222222-2222-2222-2222-222222222222"

/* Release-group IDs — dedup keys on these (album identity, not edition) */
#define ALBUM_RGID_RAM         "aa997ea0-2936-40bd-884d-3af8a0e064dc"
#define ALBUM_RGID_DISCOVERY   "bb001111-1111-1111-1111-111111111111"
#define ALBUM_RGID_HOMEWORK    "bb002222-2222-2222-2222-222222222222"
#define ALBUM_RGID_HAA         "bb003333-3333-3333-3333-333333333333"
#define ALBUM_RGID_SAW         "bb004444-4444-4444-4444-444444444444"
#define ALBUM_RGID_DRUKQS      "bb005555-5555-5555-5555-555555555555"

/* Library masks */
#define MASK_A  (1u << 0)
#define MASK_B  (1u << 1)

/* ── Fixture state ────────────────────────────────────────────────────── */

static char db_path_a[256];
static char db_path_b[256];
static library_cache_t *cache = NULL;

static void cleanup_db(const char *path) {
    char buf[280];
    unlink(path);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path);
    unlink(buf);
}

/**
 * Helper: create a track in the database.
 */
static void create_track(quadrature_db_t *db, int64_t album_id,
                          const char *title, int track_num, int disc_num) {
    db_index_item_t item = {
        .path        = title,  /* unique within album */
        .title       = title,
        .album       = "unused",
        .duration_ms = 200000,
        .track_num   = (uint16_t)track_num,
        .disc_num    = (uint16_t)disc_num,
        .year        = 2020,
        .mtime       = 1000000 + track_num,
    };
    quadrature_result_t res = db_upsert_track_with_album(db, &item, album_id, NULL);
    cr_assert_eq(res, QUADRATURE_OK, "failed to create track '%s'", title);
}

/**
 * Build Library A: "main library"
 *
 * Artists:
 *   - Daft Punk (MBID set) → 3 albums: Discovery, Homework, RAM
 *   - Aphex Twin (MBID set) → 1 album: SAW
 *   - Local Only Artist (no MBID) → 1 album
 *
 * Daft Punk appears as a track artist on Aphex Twin's SAW album (track 3)
 * to test artist_appearances merge.
 */
static void build_library_a(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path_a, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    /* Artists */
    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk",
                                                ARTIST_MBID_DAFT_PUNK);
    cr_assert(dp_id > 0);
    int64_t at_id = db_get_or_create_artist_mb(db, "Aphex Twin", "Aphex Twin",
                                                ARTIST_MBID_APHEX_TWIN);
    cr_assert(at_id > 0);
    int64_t local_id = db_get_or_create_artist(db, "Local Only Artist");
    cr_assert(local_id > 0);

    /* Daft Punk albums */
    int64_t discovery_id = 0, homework_id = 0, ram_a_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/Discovery",
        "Discovery", dp_id, 2001, &discovery_id), QUADRATURE_OK);
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/Homework",
        "Homework", dp_id, 1997, &homework_id), QUADRATURE_OK);
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/RAM",
        "Random Access Memories", dp_id, 2013, &ram_a_id), QUADRATURE_OK);

    /* Set album release IDs + release-group IDs */
    cr_assert_eq(db_update_album_mb(db, discovery_id, "Discovery",
        ALBUM_MBRID_DISCOVERY, ALBUM_RGID_DISCOVERY, 2001,
        MB_STATUS_RESOLVED), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, homework_id, "Homework",
        ALBUM_MBRID_HOMEWORK, ALBUM_RGID_HOMEWORK, 1997,
        MB_STATUS_RESOLVED), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, ram_a_id, "Random Access Memories",
        ALBUM_MBRID_RAM, ALBUM_RGID_RAM, 2013,
        MB_STATUS_RESOLVED), QUADRATURE_OK);

    /* Aphex Twin album */
    int64_t saw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AphexTwin/SAW",
        "Selected Ambient Works", at_id, 1992, &saw_id), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, saw_id, "Selected Ambient Works",
        ALBUM_MBRID_SAW, ALBUM_RGID_SAW, 1992,
        MB_STATUS_RESOLVED), QUADRATURE_OK);

    /* Local-only artist album (no MBID → won't merge) */
    int64_t local_album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "LocalOnly/Album",
        "Local Album", local_id, 2023, &local_album_id), QUADRATURE_OK);

    /* Tracks for Daft Punk albums */
    create_track(db, discovery_id, "One More Time", 1, 1);
    create_track(db, discovery_id, "Aerodynamic", 2, 1);
    create_track(db, homework_id, "Around the World", 1, 1);
    create_track(db, ram_a_id, "Get Lucky", 1, 1);
    create_track(db, ram_a_id, "Lose Yourself to Dance", 2, 1);

    /* Tracks for Aphex Twin — track 3 features Daft Punk (appearance) */
    create_track(db, saw_id, "Xtal", 1, 1);
    create_track(db, saw_id, "Tha", 2, 1);
    create_track(db, saw_id, "Pulsewidth feat. Daft Punk", 3, 1);

    /* Track for local artist */
    create_track(db, local_album_id, "Local Track", 1, 1);

    /* Link track artists: Daft Punk → all DP tracks */
    db_track_artist_t ta_dp = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    /* track IDs 1-5 are Daft Punk album tracks */
    for (int64_t tid = 1; tid <= 5; tid++)
        db_set_track_artists(db, tid, &ta_dp, 1);

    /* Aphex Twin → SAW tracks 1-3 */
    db_track_artist_t ta_at = { .artist_id = at_id, .position = 0, .join_phrase = "" };
    for (int64_t tid = 6; tid <= 8; tid++)
        db_set_track_artists(db, tid, &ta_at, 1);

    /* Daft Punk as featured artist on SAW track 3 */
    db_track_artist_t ta_feat[2] = {
        { .artist_id = at_id, .position = 0, .join_phrase = "" },
        { .artist_id = dp_id, .position = 1, .join_phrase = " feat. " },
    };
    db_set_track_artists(db, 8, ta_feat, 2);

    /* Local artist */
    db_track_artist_t ta_local = { .artist_id = local_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 9, &ta_local, 1);

    /* Sync FTS */
    db_sync_album_fts(db, discovery_id);
    db_sync_album_fts(db, homework_id);
    db_sync_album_fts(db, ram_a_id);
    db_sync_album_fts(db, saw_id);
    db_sync_album_fts(db, local_album_id);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

/**
 * Build Library B: "secondary library"
 *
 * Artists:
 *   - Daft Punk (same MBID) → 2 albums: Human After All (different MBRID), RAM (same MBRID)
 *   - Aphex Twin (same MBID) → 1 album: Drukqs
 *
 * RAM shares the same MBRID with Library A → should be deduped in ALL view.
 * Human After All has a different MBRID → unique album.
 */
static void build_library_b(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path_b, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    /* Artists (same MBIDs as Library A) */
    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk",
                                                ARTIST_MBID_DAFT_PUNK);
    cr_assert(dp_id > 0);
    int64_t at_id = db_get_or_create_artist_mb(db, "Aphex Twin", "Aphex Twin",
                                                ARTIST_MBID_APHEX_TWIN);
    cr_assert(at_id > 0);

    /* Daft Punk albums */
    int64_t haa_id = 0, ram_b_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/HAA",
        "Human After All", dp_id, 2005, &haa_id), QUADRATURE_OK);
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/RAM",
        "Random Access Memories", dp_id, 2013, &ram_b_id), QUADRATURE_OK);

    /* Set album release IDs + release-group IDs */
    cr_assert_eq(db_update_album_mb(db, haa_id, "Human After All",
        ALBUM_MBRID_HAA_LIB_B, ALBUM_RGID_HAA, 2005,
        MB_STATUS_RESOLVED), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, ram_b_id, "Random Access Memories",
        ALBUM_MBRID_RAM, ALBUM_RGID_RAM, 2013,
        MB_STATUS_RESOLVED), QUADRATURE_OK);

    /* Aphex Twin album */
    int64_t drukqs_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AphexTwin/Drukqs",
        "Drukqs", at_id, 2001, &drukqs_id), QUADRATURE_OK);
    cr_assert_eq(db_update_album_mb(db, drukqs_id, "Drukqs",
        ALBUM_MBRID_DRUKQS, ALBUM_RGID_DRUKQS, 2001,
        MB_STATUS_RESOLVED), QUADRATURE_OK);

    /* Tracks */
    create_track(db, haa_id, "Robot Rock", 1, 1);
    create_track(db, haa_id, "Steam Machine", 2, 1);
    create_track(db, ram_b_id, "Get Lucky", 1, 1);
    create_track(db, drukqs_id, "Vordhosbn", 1, 1);

    /* Link track artists */
    db_track_artist_t ta_dp = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    for (int64_t tid = 1; tid <= 3; tid++)
        db_set_track_artists(db, tid, &ta_dp, 1);
    db_track_artist_t ta_at = { .artist_id = at_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 4, &ta_at, 1);

    /* Sync FTS */
    db_sync_album_fts(db, haa_id);
    db_sync_album_fts(db, ram_b_id);
    db_sync_album_fts(db, drukqs_id);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

/* ── Fixture ──────────────────────────────────────────────────────────── */

static void setup(void) {
    pid_t pid = getpid();
    snprintf(db_path_a, sizeof(db_path_a), "/tmp/test_merge_lib_a_%d.db", pid);
    snprintf(db_path_b, sizeof(db_path_b), "/tmp/test_merge_lib_b_%d.db", pid);
    cleanup_db(db_path_a);
    cleanup_db(db_path_b);

    build_library_a();
    build_library_b();

    /* Create multi-library cache with both databases */
    library_cache_source_t sources[2] = {
        { .db_path = db_path_a, .music_base = "/music_a",
          .display_name = "Library A", .bitmap_index = 0 },
        { .db_path = db_path_b, .music_base = "/music_b",
          .display_name = "Library B", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    cr_assert_not_null(cache);

    /* Warm both slots synchronously */
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);
}

static void teardown(void) {
    library_cache_destroy(cache);
    cache = NULL;
    cleanup_db(db_path_a);
    cleanup_db(db_path_b);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

/** Find artist global ID by name using the given library mask. */
static int64_t find_artist_id_in_library(const char *name, uint32_t mask) {
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, name, NULL, mask);
    cr_assert_not_null(artists, "artist query for '%s' returned NULL", name);

    int64_t found_id = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            found_id = a->artist_id;
            break;
        }
    }
    g_ptr_array_unref(artists);
    return found_id;
}

/** Find artist global ID by name across all libraries. Asserts found. */
static int64_t find_artist_id(const char *name) {
    int64_t id = find_artist_id_in_library(name, LIBRARY_MASK_ALL);
    cr_assert(id != 0, "artist '%s' not found", name);
    return id;
}

/** Check if album title exists in array. */
static bool has_album_title(const GPtrArray *albums, const char *title) {
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) return true;
    }
    return false;
}

/** Count how many times an album title appears in array. */
static int count_album_title(const GPtrArray *albums, const char *title) {
    int count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) count++;
    }
    return count;
}

/** Check that all albums in the array come from the expected library bitmap. */
static bool all_albums_from_library(const GPtrArray *albums, int expected_bitmap) {
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (LIBRARY_GLOBAL_ID_LIB(a->album_id) != expected_bitmap)
            return false;
    }
    return true;
}

/* =============================================================================
 * Section 1: Artist Visibility in Filtered Lists
 * ============================================================================= */

/* Rule 1: MBID artists appear once when viewing all libraries */
Test(cross_library_merge, mbid_artist_appears_once_in_all_libraries,
     .init = setup, .fini = teardown) {
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Daft Punk", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    int dp_count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, "Daft Punk") == 0)
            dp_count++;
    }
    g_ptr_array_unref(artists);

    cr_assert_eq(dp_count, 1,
        "Daft Punk should appear exactly once in all-library view, got %d", dp_count);
}

/* Rule 2: artist exists in both libraries → appears in each library filter */
Test(cross_library_merge, mbid_artist_appears_in_each_library_filter,
     .init = setup, .fini = teardown) {
    int64_t dp_a = find_artist_id_in_library("Daft Punk", MASK_A);
    cr_assert(dp_a != 0, "Daft Punk should appear when filtering by Library A");

    int64_t dp_b = find_artist_id_in_library("Daft Punk", MASK_B);
    cr_assert(dp_b != 0, "Daft Punk should appear when filtering by Library B");
}

/* Rule 5: non-MBID artists only appear in their own library */
Test(cross_library_merge, non_mbid_artist_only_in_own_library,
     .init = setup, .fini = teardown) {
    int64_t local_a = find_artist_id_in_library("Local Only Artist", MASK_A);
    cr_assert(local_a != 0,
        "Local Only Artist should appear when Library A is enabled");

    int64_t local_b = find_artist_id_in_library("Local Only Artist", MASK_B);
    cr_assert_eq(local_b, 0,
        "Local Only Artist should NOT appear when only Library B is enabled");
}

/* Each library has its own global ID for the same MBID artist */
Test(cross_library_merge, different_library_ids_for_same_mbid_artist,
     .init = setup, .fini = teardown) {
    int64_t dp_a = find_artist_id_in_library("Daft Punk", MASK_A);
    int64_t dp_b = find_artist_id_in_library("Daft Punk", MASK_B);
    cr_assert(dp_a != 0 && dp_b != 0);
    cr_assert_neq(dp_a, dp_b,
        "Library A and B should have different global IDs for Daft Punk");
    cr_assert_eq(LIBRARY_GLOBAL_ID_LIB(dp_a), 0, "Library A ID should have bitmap 0");
    cr_assert_eq(LIBRARY_GLOBAL_ID_LIB(dp_b), 1, "Library B ID should have bitmap 1");
}

/* =============================================================================
 * Section 2: Album Queries — All Libraries (MBRID Dedup)
 * ============================================================================= */

/* Rule 3: combined cross-library view with MBRID dedup */
Test(cross_library_merge, all_libraries_shows_combined_albums,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    /* Discovery (A) + Homework (A) + RAM (deduped) + HAA (B) = 4 */
    cr_assert_eq(albums->len, 4,
        "Daft Punk should have 4 unique albums across libraries, got %u", albums->len);
    cr_assert(has_album_title(albums, "Discovery"), "missing Discovery");
    cr_assert(has_album_title(albums, "Homework"), "missing Homework");
    cr_assert(has_album_title(albums, "Random Access Memories"), "missing RAM");
    cr_assert(has_album_title(albums, "Human After All"), "missing Human After All");
    g_ptr_array_unref(albums);
}

/* RAM appears once despite existing in both libraries (same MBRID) */
Test(cross_library_merge, shared_mbrid_album_deduped_in_all_view,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    cr_assert_eq(count_album_title(albums, "Random Access Memories"), 1,
        "RAM should appear once in all-library view (deduped by MBRID)");
    g_ptr_array_unref(albums);
}

/* HAA has unique MBRID → always appears */
Test(cross_library_merge, different_mbrid_album_not_deduped,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    cr_assert_eq(count_album_title(albums, "Human After All"), 1,
        "Human After All should appear once");
    g_ptr_array_unref(albums);
}

/* Aphex Twin: SAW (A) + Drukqs (B) = 2 */
Test(cross_library_merge, aphex_twin_combined_albums,
     .init = setup, .fini = teardown) {
    int64_t at_id = find_artist_id("Aphex Twin");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, at_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    cr_assert_eq(albums->len, 2,
        "Aphex Twin should have 2 albums, got %u", albums->len);
    cr_assert(has_album_title(albums, "Selected Ambient Works"), "missing SAW");
    cr_assert(has_album_title(albums, "Drukqs"), "missing Drukqs");
    g_ptr_array_unref(albums);
}

/* =============================================================================
 * Section 3: Album Queries — Single Library (No Dedup)
 * ============================================================================= */

/* Rule 2: single library shows its own content, all albums from that library */
Test(cross_library_merge, single_library_shows_own_albums_only,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, MASK_A);
    cr_assert_not_null(albums);

    cr_assert_eq(albums->len, 3,
        "Library A should show 3 Daft Punk albums, got %u", albums->len);
    cr_assert(has_album_title(albums, "Discovery"), "missing Discovery");
    cr_assert(has_album_title(albums, "Homework"), "missing Homework");
    cr_assert(has_album_title(albums, "Random Access Memories"), "missing RAM");
    cr_assert(all_albums_from_library(albums, 0),
        "All albums should be from Library A (bitmap 0)");
    g_ptr_array_unref(albums);
}

/**
 * KEY BEHAVIOR CHANGE: Library B has RAM (same MBRID as Library A's RAM).
 * When filtering to Library B only, the user should see Library B's actual
 * content: Human After All AND RAM. No cross-library dedup in single-lib view.
 */
Test(cross_library_merge, single_library_includes_shared_mbrid_album,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, MASK_B);
    cr_assert_not_null(albums);

    cr_assert_eq(albums->len, 2,
        "Library B should show 2 Daft Punk albums (HAA + RAM), got %u", albums->len);
    cr_assert(has_album_title(albums, "Human After All"), "missing HAA");
    cr_assert(has_album_title(albums, "Random Access Memories"), "missing RAM");
    cr_assert(all_albums_from_library(albums, 1),
        "All albums should be from Library B (bitmap 1)");
    g_ptr_array_unref(albums);
}

/* Aphex Twin per-library: SAW in A, Drukqs in B */
Test(cross_library_merge, single_library_aphex_twin,
     .init = setup, .fini = teardown) {
    int64_t at_id = find_artist_id("Aphex Twin");

    GPtrArray *albums_a = library_cache_get_albums_by_artist(cache, at_id, MASK_A);
    cr_assert_not_null(albums_a);
    cr_assert_eq(albums_a->len, 1, "Aphex Twin in Library A: 1 album");
    cr_assert(has_album_title(albums_a, "Selected Ambient Works"));
    g_ptr_array_unref(albums_a);

    GPtrArray *albums_b = library_cache_get_albums_by_artist(cache, at_id, MASK_B);
    cr_assert_not_null(albums_b);
    cr_assert_eq(albums_b->len, 1, "Aphex Twin in Library B: 1 album");
    cr_assert(has_album_title(albums_b, "Drukqs"));
    g_ptr_array_unref(albums_b);
}

/* =============================================================================
 * Section 4: Library Mask Transitions (The Original Bug)
 * ============================================================================= */

/* Switching from ALL to single-library and back */
Test(cross_library_merge, mask_transition_all_to_single,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    GPtrArray *all = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(all);
    cr_assert_eq(all->len, 4, "ALL libraries: 4 albums");
    g_ptr_array_unref(all);

    GPtrArray *b_only = library_cache_get_albums_by_artist(cache, dp_id, MASK_B);
    cr_assert_not_null(b_only);
    cr_assert_eq(b_only->len, 2, "Library B only: 2 albums");
    g_ptr_array_unref(b_only);

    GPtrArray *a_only = library_cache_get_albums_by_artist(cache, dp_id, MASK_A);
    cr_assert_not_null(a_only);
    cr_assert_eq(a_only->len, 3, "Library A only: 3 albums");
    g_ptr_array_unref(a_only);
}

/* Expanding from single library to all */
Test(cross_library_merge, mask_transition_single_to_all,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    GPtrArray *a_only = library_cache_get_albums_by_artist(cache, dp_id, MASK_A);
    cr_assert_not_null(a_only);
    cr_assert_eq(a_only->len, 3, "Library A: 3 albums");
    g_ptr_array_unref(a_only);

    GPtrArray *all = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(all);
    cr_assert_eq(all->len, 4, "ALL libraries: 4 albums");
    g_ptr_array_unref(all);
}

/* Toggling between single libraries on detail page */
Test(cross_library_merge, mask_transition_between_single_libraries,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    GPtrArray *a = library_cache_get_albums_by_artist(cache, dp_id, MASK_A);
    cr_assert_not_null(a);
    cr_assert_eq(a->len, 3, "Library A: 3 albums");
    g_ptr_array_unref(a);

    GPtrArray *b = library_cache_get_albums_by_artist(cache, dp_id, MASK_B);
    cr_assert_not_null(b);
    cr_assert_eq(b->len, 2, "Library B: 2 albums");
    g_ptr_array_unref(b);

    GPtrArray *a2 = library_cache_get_albums_by_artist(cache, dp_id, MASK_A);
    cr_assert_not_null(a2);
    cr_assert_eq(a2->len, 3, "Library A again: still 3 albums");
    g_ptr_array_unref(a2);
}

/* =============================================================================
 * Section 5: Cross-Library ID Resolution (Rule 4)
 * ============================================================================= */

/* Library B's artist ID + mask=ALL → all 4 albums */
Test(cross_library_merge, library_b_artist_id_returns_all_albums,
     .init = setup, .fini = teardown) {
    int64_t dp_b = find_artist_id_in_library("Daft Punk", MASK_B);
    cr_assert(dp_b != 0);

    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_b, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums,
        "Library B's artist ID should return albums when queried with mask=ALL");
    cr_assert_eq(albums->len, 4,
        "Library B's Daft Punk ID + mask=ALL should return 4 albums, got %u", albums->len);
    g_ptr_array_unref(albums);
}

/* Library A's artist ID + mask=ALL → same 4 albums */
Test(cross_library_merge, library_a_artist_id_returns_all_albums,
     .init = setup, .fini = teardown) {
    int64_t dp_a = find_artist_id_in_library("Daft Punk", MASK_A);
    cr_assert(dp_a != 0);

    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_a, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 4,
        "Library A's Daft Punk ID + mask=ALL should return 4 albums, got %u", albums->len);
    g_ptr_array_unref(albums);
}

/* Both library IDs produce the same album set */
Test(cross_library_merge, both_ids_return_identical_album_sets,
     .init = setup, .fini = teardown) {
    int64_t dp_a = find_artist_id_in_library("Daft Punk", MASK_A);
    int64_t dp_b = find_artist_id_in_library("Daft Punk", MASK_B);
    cr_assert(dp_a != 0 && dp_b != 0);
    cr_assert_neq(dp_a, dp_b);

    GPtrArray *albums_a = library_cache_get_albums_by_artist(cache, dp_a, LIBRARY_MASK_ALL);
    GPtrArray *albums_b = library_cache_get_albums_by_artist(cache, dp_b, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums_a);
    cr_assert_not_null(albums_b);

    cr_assert_eq(albums_a->len, albums_b->len,
        "Both IDs should return same number of albums (%u vs %u)",
        albums_a->len, albums_b->len);

    /* Verify same titles */
    cr_assert(has_album_title(albums_a, "Discovery"));
    cr_assert(has_album_title(albums_b, "Discovery"));
    cr_assert(has_album_title(albums_a, "Human After All"));
    cr_assert(has_album_title(albums_b, "Human After All"));

    g_ptr_array_unref(albums_a);
    g_ptr_array_unref(albums_b);
}

/* Use Library A's ID but filter to Library B's content */
Test(cross_library_merge, cross_library_id_with_single_mask,
     .init = setup, .fini = teardown) {
    int64_t dp_a = find_artist_id_in_library("Daft Punk", MASK_A);
    cr_assert(dp_a != 0);

    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_a, MASK_B);
    cr_assert_not_null(albums,
        "Library A's ID + mask=B should still return Library B's albums");
    cr_assert_eq(albums->len, 2,
        "Should return Library B's 2 albums (HAA + RAM), got %u", albums->len);
    cr_assert(has_album_title(albums, "Human After All"));
    cr_assert(has_album_title(albums, "Random Access Memories"));
    g_ptr_array_unref(albums);
}

/* =============================================================================
 * Section 6: Featured Appearances ("Appears On")
 * ============================================================================= */

/* Daft Punk appears on SAW in Library A → visible in ALL view */
Test(cross_library_merge, appearances_show_in_all_libraries,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *appearances = library_cache_get_artist_appearances(
        cache, dp_id, LIBRARY_MASK_ALL);

    cr_assert_not_null(appearances);
    cr_assert(appearances->len > 0, "Daft Punk should have appearances");
    cr_assert(has_album_title(appearances, "Selected Ambient Works"),
        "Daft Punk should appear on SAW");
    g_ptr_array_unref(appearances);
}

/* SAW is in Library A → visible when filtering to Library A */
Test(cross_library_merge, appearances_show_in_source_library,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *appearances = library_cache_get_artist_appearances(
        cache, dp_id, MASK_A);

    cr_assert_not_null(appearances);
    cr_assert(appearances->len > 0,
        "Daft Punk appearances should show when Library A is active");
    cr_assert(has_album_title(appearances, "Selected Ambient Works"));
    g_ptr_array_unref(appearances);
}

/* SAW is NOT in Library B → appearances hidden when filtering to Library B */
Test(cross_library_merge, appearances_hidden_in_other_library,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *appearances = library_cache_get_artist_appearances(
        cache, dp_id, MASK_B);

    guint count = appearances ? appearances->len : 0;
    cr_assert_eq(count, 0,
        "Daft Punk should have 0 appearances in Library B, got %u", count);
    if (appearances) g_ptr_array_unref(appearances);
}

/* Featured track credits populate correctly */
Test(cross_library_merge, appearance_tracks_populated,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *tracks = library_cache_get_artist_appearance_tracks(
        cache, dp_id, LIBRARY_MASK_ALL);

    cr_assert_not_null(tracks, "Daft Punk should have appearance tracks");
    cr_assert(tracks->len > 0);

    bool found = false;
    for (guint i = 0; i < tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(tracks, i);
        if (strstr(t->title, "Pulsewidth") != NULL) found = true;
    }
    cr_assert(found, "Should include 'Pulsewidth feat. Daft Punk'");
    g_ptr_array_unref(tracks);
}

/* Appearance tracks must be on albums where artist is NOT the primary artist */
Test(cross_library_merge, appearance_tracks_on_correct_album,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *tracks = library_cache_get_artist_appearance_tracks(
        cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(tracks);

    for (guint i = 0; i < tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(tracks, i);
        const library_album_info_t *album = library_cache_get_album(cache, t->album_id, LIBRARY_MASK_ALL);
        cr_assert_not_null(album, "appearance track '%s' has no album", t->title);

        const library_artist_info_t *album_artist =
            library_cache_get_artist(cache, album->artist_id, LIBRARY_MASK_ALL);
        if (album_artist) {
            cr_assert_neq(g_ascii_strcasecmp(album_artist->name, "Daft Punk"), 0,
                "appearance track '%s' should not be on a Daft Punk album", t->title);
        }
    }
    g_ptr_array_unref(tracks);
}

/* Every appearance track's album should be in the appearance albums list */
Test(cross_library_merge, appearance_albums_and_tracks_consistent,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *app_albums = library_cache_get_artist_appearances(
        cache, dp_id, LIBRARY_MASK_ALL);
    GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(
        cache, dp_id, LIBRARY_MASK_ALL);

    if (!app_tracks || app_tracks->len == 0) {
        if (app_tracks) g_ptr_array_unref(app_tracks);
        if (app_albums) g_ptr_array_unref(app_albums);
        return;
    }
    cr_assert_not_null(app_albums, "have appearance tracks but no appearance albums");

    for (guint i = 0; i < app_tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(app_tracks, i);
        bool album_found = false;
        for (guint j = 0; j < app_albums->len; j++) {
            const library_album_info_t *a = g_ptr_array_index(app_albums, j);
            if (a->album_id == t->album_id) { album_found = true; break; }
        }
        cr_assert(album_found,
            "appearance track '%s' (album_id=%ld) has no matching appearance album",
            t->title, (long)t->album_id);
    }
    g_ptr_array_unref(app_tracks);
    g_ptr_array_unref(app_albums);
}

/* Appearances resolve correctly through cross-library artist IDs */
Test(cross_library_merge, appearance_via_cross_library_id,
     .init = setup, .fini = teardown) {
    int64_t dp_b = find_artist_id_in_library("Daft Punk", MASK_B);
    cr_assert(dp_b != 0);

    GPtrArray *appearances = library_cache_get_artist_appearances(
        cache, dp_b, LIBRARY_MASK_ALL);

    cr_assert_not_null(appearances,
        "Appearances should resolve through Library B's artist ID");
    cr_assert(appearances->len > 0);
    cr_assert(has_album_title(appearances, "Selected Ambient Works"),
        "Should find SAW via Library B's Daft Punk ID");
    g_ptr_array_unref(appearances);
}

/* Appearance tracks also resolve through cross-library IDs */
Test(cross_library_merge, appearance_tracks_via_cross_library_id,
     .init = setup, .fini = teardown) {
    int64_t dp_b = find_artist_id_in_library("Daft Punk", MASK_B);
    cr_assert(dp_b != 0);

    GPtrArray *tracks = library_cache_get_artist_appearance_tracks(
        cache, dp_b, LIBRARY_MASK_ALL);

    cr_assert_not_null(tracks,
        "Appearance tracks should resolve through Library B's artist ID");
    cr_assert(tracks->len > 0);
    g_ptr_array_unref(tracks);
}

/* Aphex Twin has no featured appearances */
Test(cross_library_merge, artist_without_features_has_no_appearances,
     .init = setup, .fini = teardown) {
    int64_t at_id = find_artist_id("Aphex Twin");
    GPtrArray *albums = library_cache_get_artist_appearances(
        cache, at_id, LIBRARY_MASK_ALL);
    GPtrArray *tracks = library_cache_get_artist_appearance_tracks(
        cache, at_id, LIBRARY_MASK_ALL);

    cr_assert_eq(albums ? albums->len : 0, 0,
        "Aphex Twin should have 0 appearance albums");
    cr_assert_eq(tracks ? tracks->len : 0, 0,
        "Aphex Twin should have 0 appearance tracks");
    if (albums) g_ptr_array_unref(albums);
    if (tracks) g_ptr_array_unref(tracks);
}

/* =============================================================================
 * Section 7: Library Removal
 * ============================================================================= */

/* After removing Library B, Daft Punk has only Library A's 3 albums */
Test(cross_library_merge, remove_library_reduces_albums,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    /* Before: 4 albums */
    GPtrArray *before = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(before);
    cr_assert_eq(before->len, 4);
    g_ptr_array_unref(before);

    /* Remove Library B */
    cr_assert_eq(library_cache_remove_slot(cache, 1), QUADRATURE_OK);

    /* Re-find (ID might change) */
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Daft Punk", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert(artists->len > 0, "Daft Punk should still exist after removing Library B");
    const library_artist_info_t *dp_after = g_ptr_array_index(artists, 0);

    GPtrArray *after = library_cache_get_albums_by_artist(
        cache, dp_after->artist_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(after);
    cr_assert_eq(after->len, 3,
        "After removing Library B, Daft Punk should have 3 albums, got %u", after->len);
    cr_assert(has_album_title(after, "Discovery"));
    cr_assert(has_album_title(after, "Homework"));
    cr_assert(has_album_title(after, "Random Access Memories"));

    g_ptr_array_unref(after);
    g_ptr_array_unref(artists);
}

/* Appearances from Library A survive removal of Library B */
Test(cross_library_merge, remove_library_appearances_survive,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    /* Before: has appearances */
    GPtrArray *before = library_cache_get_artist_appearance_tracks(
        cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(before);
    cr_assert(before->len > 0);
    g_ptr_array_unref(before);

    /* Remove Library B */
    cr_assert_eq(library_cache_remove_slot(cache, 1), QUADRATURE_OK);

    /* Re-find */
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Daft Punk", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert(artists->len > 0);
    const library_artist_info_t *dp_after = g_ptr_array_index(artists, 0);

    GPtrArray *after = library_cache_get_artist_appearance_tracks(
        cache, dp_after->artist_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(after, "Appearances should survive Library B removal");
    cr_assert(after->len > 0);
    g_ptr_array_unref(after);
    g_ptr_array_unref(artists);
}

/* =============================================================================
 * Section 9: Edge Cases
 * ============================================================================= */

/* Empty mask returns NULL */
Test(cross_library_merge, query_with_empty_mask_returns_null,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, 0);
    cr_assert_null(albums, "Empty mask should return NULL");
}

/* Non-MBID artist albums work normally */
Test(cross_library_merge, non_mbid_artist_albums_unaffected,
     .init = setup, .fini = teardown) {
    int64_t local_id = find_artist_id_in_library("Local Only Artist", MASK_A);
    cr_assert(local_id != 0);

    GPtrArray *albums = library_cache_get_albums_by_artist(cache, local_id, MASK_A);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 1, "Local Only Artist should have 1 album");
    cr_assert(has_album_title(albums, "Local Album"));
    g_ptr_array_unref(albums);
}

/* =============================================================================
 * Section 8: Merged Artist Counts (Computed On Demand)
 * ============================================================================= */

/* Album count across all libraries matches actual query result */
Test(cross_library_merge, merged_album_count_all_libraries,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    uint32_t album_count = 0, appearance_count = 0;
    library_cache_get_merged_artist_counts(cache, dp_id, LIBRARY_MASK_ALL,
                                            &album_count, &appearance_count);

    /* Daft Punk: Discovery + Homework + RAM (deduped) + HAA = 4 */
    cr_assert_eq(album_count, 4,
        "Daft Punk should have 4 albums across all libraries, got %u", album_count);
}

/* Album count per single library */
Test(cross_library_merge, merged_album_count_single_library,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    uint32_t count_a = 0;
    library_cache_get_merged_artist_counts(cache, dp_id, MASK_A, &count_a, NULL);
    cr_assert_eq(count_a, 3, "Library A: 3 albums, got %u", count_a);

    uint32_t count_b = 0;
    library_cache_get_merged_artist_counts(cache, dp_id, MASK_B, &count_b, NULL);
    cr_assert_eq(count_b, 2, "Library B: 2 albums, got %u", count_b);
}

/* Appearance count excludes artist's own albums */
Test(cross_library_merge, appearance_count_excludes_own_albums,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    uint32_t album_count = 0, appearance_count = 0;
    library_cache_get_merged_artist_counts(cache, dp_id, LIBRARY_MASK_ALL,
                                            &album_count, &appearance_count);

    /* Daft Punk appears on SAW track 3 in Library A only = 1 appearance track.
     * Must NOT count Daft Punk's own album tracks (Discovery, Homework, RAM, HAA). */
    cr_assert_eq(appearance_count, 1,
        "Daft Punk should have 1 appearance track (SAW track 3 only), got %u",
        appearance_count);
}

/* Artist with no appearances has appearance_count = 0 */
Test(cross_library_merge, appearance_count_zero_for_non_featured,
     .init = setup, .fini = teardown) {
    int64_t at_id = find_artist_id("Aphex Twin");
    uint32_t appearance_count = 0;
    library_cache_get_merged_artist_counts(cache, at_id, LIBRARY_MASK_ALL,
                                            NULL, &appearance_count);
    cr_assert_eq(appearance_count, 0,
        "Aphex Twin should have 0 appearances, got %u", appearance_count);
}

/* Count matches actual query result for each mask */
Test(cross_library_merge, count_matches_query_result,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    uint32_t masks[] = { LIBRARY_MASK_ALL, MASK_A, MASK_B };

    for (int m = 0; m < 3; m++) {
        uint32_t album_count = 0;
        library_cache_get_merged_artist_counts(cache, dp_id, masks[m],
                                                &album_count, NULL);
        GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id, masks[m]);
        uint32_t actual = albums ? albums->len : 0;
        if (albums) g_ptr_array_unref(albums);

        cr_assert_eq(album_count, actual,
            "Album count (%u) should match query result (%u) for mask 0x%x",
            album_count, actual, masks[m]);
    }
}

/* Appearance count filtered by library */
Test(cross_library_merge, appearance_count_filtered_by_library,
     .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");

    /* Library A has the SAW appearance */
    uint32_t app_a = 0;
    library_cache_get_merged_artist_counts(cache, dp_id, MASK_A, NULL, &app_a);
    cr_assert_eq(app_a, 1, "Library A: 1 appearance track, got %u", app_a);

    /* Library B has no appearances for Daft Punk */
    uint32_t app_b = 0;
    library_cache_get_merged_artist_counts(cache, dp_id, MASK_B, NULL, &app_b);
    cr_assert_eq(app_b, 0, "Library B: 0 appearance tracks, got %u", app_b);
}
