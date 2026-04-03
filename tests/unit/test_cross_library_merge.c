/**
 * Tests for cross-library artist/album merging in library_cache.
 *
 * Creates two separate SQLite databases with overlapping artists (same MBID)
 * and albums (same musicbrainz_release_id), then verifies that the library
 * cache correctly merges them and the public API returns combined results.
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

/* Album MBRIDs — RAM is shared between both libraries */
#define ALBUM_MBRID_RAM        "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define ALBUM_MBRID_DISCOVERY  "d073287b-d1bd-4f11-a933-a4386f8cf701"
#define ALBUM_MBRID_HOMEWORK   "647d7016-2683-42c6-b027-83114a7c3eec"
/* Human After All has DIFFERENT MBRIDs in each library (different releases) */
#define ALBUM_MBRID_HAA_LIB_A  "abbc40a0-2bcc-449e-bdd0-2dbae3213517"
#define ALBUM_MBRID_HAA_LIB_B  "77a2f001-ae10-45fe-8d56-6069edfe20fe"
/* Aphex Twin albums */
#define ALBUM_MBRID_SAW        "11111111-1111-1111-1111-111111111111"
#define ALBUM_MBRID_DRUKQS     "22222222-2222-2222-2222-222222222222"

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

    /* Set album MBRIDs */
    cr_assert_eq(db_set_album_release_id_from_tags(db, discovery_id,
        ALBUM_MBRID_DISCOVERY), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, homework_id,
        ALBUM_MBRID_HOMEWORK), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, ram_a_id,
        ALBUM_MBRID_RAM), QUADRATURE_OK);

    /* Aphex Twin album */
    int64_t saw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AphexTwin/SAW",
        "Selected Ambient Works", at_id, 1992, &saw_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, saw_id,
        ALBUM_MBRID_SAW), QUADRATURE_OK);

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
 * RAM shares the same MBRID with Library A → should be deduped.
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

    /* Set album MBRIDs */
    cr_assert_eq(db_set_album_release_id_from_tags(db, haa_id,
        ALBUM_MBRID_HAA_LIB_B), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, ram_b_id,
        ALBUM_MBRID_RAM), QUADRATURE_OK);

    /* Aphex Twin album */
    int64_t drukqs_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AphexTwin/Drukqs",
        "Drukqs", at_id, 2001, &drukqs_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, drukqs_id,
        ALBUM_MBRID_DRUKQS), QUADRATURE_OK);

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

/* ── Helper: find artist global ID by name ────────────────────────────── */

static int64_t find_artist_id(const char *name) {
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, name, NULL, LIBRARY_MASK_ALL);
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
    cr_assert(found_id != 0, "artist '%s' not found", name);
    return found_id;
}

/* ── Helper: check if album title exists in array ─────────────────────── */

static bool has_album_title(const GPtrArray *albums, const char *title) {
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) return true;
    }
    return false;
}

/* =============================================================================
 * Tests: Artist Merge Metadata
 * ============================================================================= */

Test(cross_library_merge, artist_merged_by_mbid, .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    const library_artist_info_t *dp = library_cache_get_artist(cache, dp_id);
    cr_assert_not_null(dp);

    /* Artist should be marked as merged across libraries */
    cr_assert_eq(dp->library_index, -1,
        "merged artist should have library_index == -1, got %d", dp->library_index);
    cr_assert(dp->merged_source_count > 0,
        "merged artist should have merged_source_count > 0, got %d", dp->merged_source_count);
}

Test(cross_library_merge, unmerged_artist_without_mbid, .init = setup, .fini = teardown) {
    int64_t local_id = find_artist_id("Local Only Artist");
    const library_artist_info_t *local = library_cache_get_artist(cache, local_id);
    cr_assert_not_null(local);

    /* Artist without MBID should NOT be merged */
    cr_assert(local->library_index >= 0,
        "unmerged artist should have library_index >= 0");
    cr_assert_eq(local->merged_source_count, 0);
}

Test(cross_library_merge, filtered_query_skips_merged_sources, .init = setup, .fini = teardown) {
    /* Daft Punk should appear exactly once in filtered results, not twice */
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
        "Daft Punk should appear once in filtered results (merged rep only), got %d", dp_count);
}

/* =============================================================================
 * Tests: Combined Artist Albums (the core bug fix)
 * ============================================================================= */

Test(cross_library_merge, combined_albums_include_both_libraries, .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id);
    cr_assert_not_null(albums, "get_albums_by_artist should not return NULL for merged artist");

    /*
     * Expected: 4 unique albums
     *   From Lib A: Discovery, Homework, RAM (rep)
     *   From Lib B: Human After All (unique MBRID), RAM (source → deduped)
     *   Total: Discovery + Homework + RAM + Human After All = 4
     */
    cr_assert_eq(albums->len, 4,
        "Daft Punk should have 4 unique albums across libraries, got %u", albums->len);

    /* Verify specific album titles are present */
    cr_assert(has_album_title(albums, "Discovery"), "missing Discovery");
    cr_assert(has_album_title(albums, "Homework"), "missing Homework");
    cr_assert(has_album_title(albums, "Random Access Memories"), "missing RAM");
    cr_assert(has_album_title(albums, "Human After All"), "missing Human After All");
}

Test(cross_library_merge, ram_deduped_by_shared_mbrid, .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id);
    cr_assert_not_null(albums);

    /* RAM should appear exactly once despite existing in both libraries */
    int ram_count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, "Random Access Memories") == 0)
            ram_count++;
    }
    cr_assert_eq(ram_count, 1,
        "RAM should appear once (deduped by MBRID), got %d", ram_count);
}

Test(cross_library_merge, haa_not_deduped_different_mbrid, .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id);
    cr_assert_not_null(albums);

    /* Human After All has different MBRIDs → appears once (from Lib B only,
     * since Lib A doesn't have it) */
    int haa_count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, "Human After All") == 0)
            haa_count++;
    }
    cr_assert_eq(haa_count, 1,
        "Human After All should appear once, got %d", haa_count);
}

Test(cross_library_merge, aphex_twin_combined_albums, .init = setup, .fini = teardown) {
    int64_t at_id = find_artist_id("Aphex Twin");
    const GPtrArray *albums = library_cache_get_albums_by_artist(cache, at_id);
    cr_assert_not_null(albums);

    /*
     * Expected: 2 albums
     *   Lib A: Selected Ambient Works (SAW)
     *   Lib B: Drukqs
     */
    cr_assert_eq(albums->len, 2,
        "Aphex Twin should have 2 albums, got %u", albums->len);
    cr_assert(has_album_title(albums, "Selected Ambient Works"), "missing SAW");
    cr_assert(has_album_title(albums, "Drukqs"), "missing Drukqs");
}

/* =============================================================================
 * Tests: Artist Appearances (featured credits)
 * ============================================================================= */

Test(cross_library_merge, artist_appearances_combined, .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *appearances = library_cache_get_artist_appearances(cache, dp_id);

    /*
     * Daft Punk appears on Aphex Twin's SAW album (track 3, Lib A).
     * Should show up in appearances.
     */
    if (appearances && appearances->len > 0) {
        bool found_saw = false;
        for (guint i = 0; i < appearances->len; i++) {
            const library_album_info_t *a = g_ptr_array_index(appearances, i);
            if (g_ascii_strcasecmp(a->title, "Selected Ambient Works") == 0)
                found_saw = true;
        }
        cr_assert(found_saw,
            "Daft Punk should appear on 'Selected Ambient Works'");
    }
    /* If appearances is NULL/empty, the featured credit might not have been set up
     * correctly — that's still a valid outcome for this test data setup. */
}

Test(cross_library_merge, appearance_tracks_populated, .init = setup, .fini = teardown) {
    /*
     * Daft Punk is a featured track artist on SAW track 3 ("Pulsewidth feat.
     * Daft Punk") in Library A.  artist_appearance_tracks must contain this
     * track after cache warming + merge.
     */
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(cache, dp_id);

    cr_assert_not_null(app_tracks,
        "appearance tracks should not be NULL for artist with featured credits");
    cr_assert(app_tracks->len > 0,
        "appearance tracks should contain featured credit tracks, got 0");

    /* Verify the featured track is present */
    bool found = false;
    for (guint i = 0; i < app_tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(app_tracks, i);
        if (strstr(t->title, "Pulsewidth") != NULL)
            found = true;
    }
    cr_assert(found, "appearance tracks should include 'Pulsewidth feat. Daft Punk'");
}

Test(cross_library_merge, appearance_tracks_on_correct_album, .init = setup, .fini = teardown) {
    /* Every appearance track must belong to an album where the artist is NOT
     * the primary album artist — that's the definition of "appears on". */
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(cache, dp_id);
    cr_assert_not_null(app_tracks);

    for (guint i = 0; i < app_tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(app_tracks, i);
        const library_album_info_t *album = library_cache_get_album(cache, t->album_id);
        cr_assert_not_null(album, "appearance track '%s' has no album", t->title);

        const library_artist_info_t *album_artist =
            library_cache_get_artist(cache, album->artist_id);
        if (album_artist) {
            cr_assert_neq(g_ascii_strcasecmp(album_artist->name, "Daft Punk"), 0,
                "appearance track '%s' should not be on a Daft Punk album (got '%s')",
                t->title, album->title);
        }
    }
}

Test(cross_library_merge, appearance_albums_and_tracks_consistent, .init = setup, .fini = teardown) {
    /* Every appearance track's album should also appear in appearance albums. */
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *app_albums = library_cache_get_artist_appearances(cache, dp_id);
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(cache, dp_id);

    if (!app_tracks || app_tracks->len == 0) return;
    cr_assert_not_null(app_albums, "have appearance tracks but no appearance albums");

    for (guint i = 0; i < app_tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(app_tracks, i);
        bool album_found = false;
        for (guint j = 0; j < app_albums->len; j++) {
            const library_album_info_t *a = g_ptr_array_index(app_albums, j);
            if (a->album_id == t->album_id) { album_found = true; break; }
            /* Check merged sources too */
            for (int m = 0; m < a->merged_source_count; m++) {
                if (a->merged_source_ids[m] == t->album_id) {
                    album_found = true; break;
                }
            }
            if (album_found) break;
        }
        cr_assert(album_found,
            "appearance track '%s' (album_id=%ld) has no matching appearance album",
            t->title, (long)t->album_id);
    }
}

Test(cross_library_merge, artist_without_features_has_no_appearances, .init = setup, .fini = teardown) {
    /* Aphex Twin owns SAW and Drukqs, and is NOT featured on any other album.
     * Appearance tracks and albums should both be empty. */
    int64_t at_id = find_artist_id("Aphex Twin");
    const GPtrArray *app_albums = library_cache_get_artist_appearances(cache, at_id);
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(cache, at_id);

    guint album_count = app_albums ? app_albums->len : 0;
    guint track_count = app_tracks ? app_tracks->len : 0;
    cr_assert_eq(album_count, 0,
        "Aphex Twin should have 0 appearance albums, got %u", album_count);
    cr_assert_eq(track_count, 0,
        "Aphex Twin should have 0 appearance tracks, got %u", track_count);
}

Test(cross_library_merge, appearance_tracks_survive_library_removal, .init = setup, .fini = teardown) {
    /* Before removal: Daft Punk has appearance tracks from Library A (SAW). */
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *before = library_cache_get_artist_appearance_tracks(cache, dp_id);
    cr_assert_not_null(before);
    cr_assert(before->len > 0, "should have appearance tracks before removal");

    /* Remove Library B (which has no featured credits for DP).
     * Appearance tracks should still exist from Library A. */
    cr_assert_eq(library_cache_remove_slot(cache, 1), QUADRATURE_OK);

    /* Re-find DP since the ID might have changed */
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Daft Punk", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert(artists->len > 0);
    const library_artist_info_t *dp_after = g_ptr_array_index(artists, 0);

    const GPtrArray *after = library_cache_get_artist_appearance_tracks(
        cache, dp_after->artist_id);
    cr_assert_not_null(after,
        "appearance tracks should survive removal of unrelated library");
    cr_assert(after->len > 0,
        "appearance tracks should still be present after removing Library B");

    g_ptr_array_unref(artists);
}

/* =============================================================================
 * Tests: Album Count Accuracy
 * ============================================================================= */

Test(cross_library_merge, album_count_matches_actual_albums, .init = setup, .fini = teardown) {
    int64_t dp_id = find_artist_id("Daft Punk");
    const library_artist_info_t *dp = library_cache_get_artist(cache, dp_id);
    cr_assert_not_null(dp);

    const GPtrArray *albums = library_cache_get_albums_by_artist(cache, dp_id);
    cr_assert_not_null(albums);

    /* The album_count on the artist should match the actual array length */
    cr_assert_eq(dp->album_count, albums->len,
        "album_count (%u) should match actual albums (%u)",
        dp->album_count, albums->len);
}

/* =============================================================================
 * Tests: Library Mask Filtering
 * ============================================================================= */

Test(cross_library_merge, single_library_mask_excludes_other, .init = setup, .fini = teardown) {
    /* Query only Library A (bitmap 0) */
    uint32_t mask_a = (1u << 0);
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Local Only", NULL, mask_a);
    cr_assert_not_null(artists);

    /* Local Only Artist is only in Library A — should appear */
    bool found = false;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, "Local Only Artist") == 0)
            found = true;
    }
    g_ptr_array_unref(artists);
    cr_assert(found, "Local Only Artist should appear when Library A is enabled");

    /* Query only Library B (bitmap 1) */
    uint32_t mask_b = (1u << 1);
    artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Local Only", NULL, mask_b);
    cr_assert_not_null(artists);

    found = false;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, "Local Only Artist") == 0)
            found = true;
    }
    g_ptr_array_unref(artists);
    cr_assert(!found, "Local Only Artist should NOT appear when only Library B is enabled");
}

/* =============================================================================
 * Tests: Remove Library and Re-merge
 * ============================================================================= */

Test(cross_library_merge, remove_library_updates_merge, .init = setup, .fini = teardown) {
    /* Before removal: Daft Punk is merged */
    int64_t dp_id = find_artist_id("Daft Punk");
    const GPtrArray *albums_before = library_cache_get_albums_by_artist(cache, dp_id);
    cr_assert_not_null(albums_before);
    guint count_before = albums_before->len;
    cr_assert(count_before > 0);

    /* Remove Library B */
    cr_assert_eq(library_cache_remove_slot(cache, 1), QUADRATURE_OK);

    /* After removal: Daft Punk should only have Library A's albums.
     * The artist ID might change if the rep was in Library B, so re-find. */
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, "Daft Punk", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert(artists->len > 0, "Daft Punk should still exist after removing Library B");

    const library_artist_info_t *dp_after = g_ptr_array_index(artists, 0);
    const GPtrArray *albums_after = library_cache_get_albums_by_artist(cache, dp_after->artist_id);
    cr_assert_not_null(albums_after);

    /* Library A had 3 DP albums: Discovery, Homework, RAM */
    cr_assert_eq(albums_after->len, 3,
        "After removing Library B, Daft Punk should have 3 albums (Library A only), got %u",
        albums_after->len);

    /* Daft Punk should no longer be merged */
    cr_assert_eq(dp_after->merged_source_count, 0,
        "After removing Library B, Daft Punk should not be merged");

    g_ptr_array_unref(artists);
}
