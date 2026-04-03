/**
 * Advanced tests for library_cache.c
 *
 * Covers: COW refresh, search dedup with merged libraries, availability flag
 * filtering, NULL MBID edge cases, add_slot dynamics, concurrent COW reads,
 * cross-library appearance dedup, album track sort order, invalid global IDs,
 * and remove-middle-slot compaction.
 *
 * Each test suite has its own fixture to isolate library configurations.
 */

#include <criterion/criterion.h>
#include "quadrature/library.h"
#include "quadrature/database.h"
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared MBID constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MBID_DAFT_PUNK     "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define MBID_APHEX_TWIN    "f22942a1-6f70-4f48-866e-238cb2308fbd"
#define MBID_BOARDS_CANADA "aa1eea01-beef-face-0001-aabbccddeeff"

#define MBRID_RAM          "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define MBRID_DISCOVERY    "d073287b-d1bd-4f11-a933-a4386f8cf701"
#define MBRID_HOMEWORK     "647d7016-2683-42c6-b027-83114a7c3eec"
#define MBRID_HAA_A        "abbc40a0-2bcc-449e-bdd0-2dbae3213517"
#define MBRID_HAA_B        "77a2f001-ae10-45fe-8d56-6069edfe20fe"
#define MBRID_SAW          "11111111-1111-1111-1111-111111111111"
#define MBRID_DRUKQS       "22222222-2222-2222-2222-222222222222"
#define MBRID_GEOGADDI     "33333333-3333-3333-3333-333333333333"

/* ═══════════════════════════════════════════════════════════════════════════
 * DB helper utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cleanup_db(const char *path) {
    char buf[280];
    unlink(path);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path);
    unlink(buf);
}

static void create_track(quadrature_db_t *db, int64_t album_id,
                          const char *title, int track_num, int disc_num) {
    db_index_item_t item = {
        .path        = title,
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

static int64_t find_artist_id(library_cache_t *cache, const char *name) {
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

static bool has_album_title(const GPtrArray *albums, const char *title) {
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) return true;
    }
    return false;
}

static int count_album_title(const GPtrArray *albums, const char *title) {
    int count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) count++;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FIXTURE: cow_refresh — single-library COW refresh path
 * ═══════════════════════════════════════════════════════════════════════════ */

static char cow_db_path[256];
static library_cache_t *cow_cache = NULL;

static void build_cow_library(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(cow_db_path, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk", MBID_DAFT_PUNK);
    cr_assert(dp_id > 0);

    int64_t disc_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/Discovery",
        "Discovery", dp_id, 2001, &disc_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, disc_id, MBRID_DISCOVERY), QUADRATURE_OK);

    create_track(db, disc_id, "One More Time", 1, 1);
    create_track(db, disc_id, "Aerodynamic", 2, 1);

    db_track_artist_t ta = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 1, &ta, 1);
    db_set_track_artists(db, 2, &ta, 1);

    db_sync_album_fts(db, disc_id);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

static void cow_setup(void) {
    snprintf(cow_db_path, sizeof(cow_db_path), "/tmp/test_cow_%d.db", getpid());
    cleanup_db(cow_db_path);
    build_cow_library();

    library_cache_source_t src = {
        .db_path = cow_db_path, .music_base = "/music",
        .display_name = "COW Lib", .bitmap_index = 0,
    };
    cr_assert_eq(library_cache_create_multi(&src, 1, &cow_cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cow_cache, 0);
}

static void cow_teardown(void) {
    library_cache_destroy(cow_cache);
    cow_cache = NULL;
    cleanup_db(cow_db_path);
}

/* ── P1: COW Refresh Tests ────────────────────────────────────────────── */

Test(cow_refresh, new_album_appears_after_refresh, .init = cow_setup, .fini = cow_teardown) {
    /* Before: 1 artist, 1 album */
    GPtrArray *albums = library_cache_get_albums_filtered(cow_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(albums->len, 1, "expected 1 album before refresh, got %u", albums->len);
    g_ptr_array_unref(albums);

    /* Add a new album to the DB behind the cache's back */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(cow_db_path, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk", MBID_DAFT_PUNK);
    int64_t hw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/Homework",
        "Homework", dp_id, 1997, &hw_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, hw_id, MBRID_HOMEWORK), QUADRATURE_OK);
    create_track(db, hw_id, "Around the World", 1, 1);

    db_track_artist_t ta = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 3, &ta, 1);
    db_sync_album_fts(db, hw_id);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);

    /* COW refresh (full) + synchronous wait */
    library_cache_refresh_slot(cow_cache, 0, NULL, 0);
    library_cache_await_slot(cow_cache, 0);

    /* After: 2 albums */
    albums = library_cache_get_albums_filtered(cow_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(albums->len, 2, "expected 2 albums after refresh, got %u", albums->len);
    cr_assert(has_album_title(albums, "Discovery"), "missing Discovery after refresh");
    cr_assert(has_album_title(albums, "Homework"), "missing Homework after refresh");
    g_ptr_array_unref(albums);
}

Test(cow_refresh, existing_track_survives_refresh, .init = cow_setup, .fini = cow_teardown) {
    /* Get track data before refresh */
    const library_track_info_t *before = library_cache_get_track(cow_cache, 1);
    cr_assert_not_null(before);
    cr_assert_str_eq(before->title, "One More Time");

    /* Full COW refresh (no DB changes — entities should be seeded from old) */
    library_cache_refresh_slot(cow_cache, 0, NULL, 0);
    library_cache_await_slot(cow_cache, 0);

    /* Track should still be accessible with same data */
    const library_track_info_t *after = library_cache_get_track(cow_cache, 1);
    cr_assert_not_null(after, "track 1 should survive COW refresh");
    cr_assert_str_eq(after->title, "One More Time");
}

Test(cow_refresh, delta_refresh_updates_changed_album, .init = cow_setup, .fini = cow_teardown) {
    /* Add a third track to the existing album */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(cow_db_path, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t album_id = 1; /* Discovery is album 1 */
    create_track(db, album_id, "Digital Love", 3, 1);

    db_track_artist_t ta = { .artist_id = 1, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 3, &ta, 1);
    db_sync_album_fts(db, album_id);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);

    /* Delta refresh — only album 1 changed */
    int64_t changed[] = { 1 };
    library_cache_refresh_slot(cow_cache, 0, changed, 1);
    library_cache_await_slot(cow_cache, 0);

    /* New track should be visible */
    const library_track_info_t *t3 = library_cache_get_track(cow_cache, 3);
    cr_assert_not_null(t3, "track 3 should appear after delta refresh");
    cr_assert_str_eq(t3->title, "Digital Love");

    /* Album track count should be updated */
    const library_album_info_t *album = library_cache_get_album(cow_cache, 1);
    cr_assert_not_null(album);
    cr_assert_eq(album->track_count, 3, "album should have 3 tracks after delta refresh, got %u", album->track_count);
}

Test(cow_refresh, genres_recomputed_after_refresh, .init = cow_setup, .fini = cow_teardown) {
    /* Add a new album with genre-tagged tracks */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(cow_db_path, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk", MBID_DAFT_PUNK);
    int64_t hw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DaftPunk/Homework",
        "Homework", dp_id, 1997, &hw_id), QUADRATURE_OK);

    db_index_item_t item = {
        .path = "Around the World", .title = "Around the World",
        .album = "unused", .duration_ms = 200000,
        .track_num = 1, .disc_num = 1, .year = 1997,
        .mtime = 9000001, .genre = "Electronic",
    };
    cr_assert_eq(db_upsert_track_with_album(db, &item, hw_id, NULL), QUADRATURE_OK);

    db_track_artist_t ta = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 3, &ta, 1);
    db_sync_album_fts(db, hw_id);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);

    /* Full COW refresh */
    library_cache_refresh_slot(cow_cache, 0, NULL, 0);
    library_cache_await_slot(cow_cache, 0);

    /* Find the new album and check genres */
    GPtrArray *albums = library_cache_get_albums_filtered(cow_cache, LIBRARY_SORT_NAME_ASC, "Homework", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert(albums->len > 0, "Homework should exist after refresh");
    const library_album_info_t *hw = g_ptr_array_index(albums, 0);
    cr_assert_not_null(hw->genres, "genres should be computed for new album after COW refresh");
    cr_assert(strstr(hw->genres, "electronic") != NULL,
        "genres should contain 'electronic', got '%s'", hw->genres);
    g_ptr_array_unref(albums);
}

Test(cow_refresh, appearance_tracks_after_cow_refresh, .init = cow_setup, .fini = cow_teardown) {
    /* Initially: only Daft Punk with Discovery — no featured credits, no appearances */
    int64_t dp_id = find_artist_id(cow_cache, "Daft Punk");
    const GPtrArray *before = library_cache_get_artist_appearance_tracks(cow_cache, dp_id);
    guint before_count = before ? before->len : 0;
    cr_assert_eq(before_count, 0,
        "no appearance tracks before adding featured credit, got %u", before_count);

    /* Add a new artist + album + featured credit for DP via DB mutation */
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(cow_db_path, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t at_id = db_get_or_create_artist_mb(db, "Aphex Twin", "Aphex Twin", MBID_APHEX_TWIN);
    int64_t saw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AT/SAW",
        "Selected Ambient Works", at_id, 1992, &saw_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, saw_id, MBRID_SAW), QUADRATURE_OK);
    create_track(db, saw_id, "Xtal", 1, 1);
    create_track(db, saw_id, "DP Collab", 2, 1);

    /* AT owns tracks; track 2 also features DP */
    db_track_artist_t ta_at = { .artist_id = at_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 3, &ta_at, 1);
    db_track_artist_t ta_feat[2] = {
        { .artist_id = at_id, .position = 0, .join_phrase = "" },
        { .artist_id = dp_id, .position = 1, .join_phrase = " feat. " },
    };
    db_set_track_artists(db, 4, ta_feat, 2);

    db_sync_album_fts(db, saw_id);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);

    /* COW refresh */
    library_cache_refresh_slot(cow_cache, 0, NULL, 0);
    library_cache_await_slot(cow_cache, 0);

    /* Re-find DP after refresh */
    dp_id = find_artist_id(cow_cache, "Daft Punk");
    const GPtrArray *after = library_cache_get_artist_appearance_tracks(cow_cache, dp_id);
    cr_assert_not_null(after, "appearance tracks should exist after COW refresh");
    cr_assert(after->len > 0,
        "appearance tracks should be populated after COW refresh, got 0");

    /* Verify the featured track is "DP Collab" */
    bool found = false;
    for (guint i = 0; i < after->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(after, i);
        if (strstr(t->title, "DP Collab") != NULL)
            found = true;
    }
    cr_assert(found, "appearance tracks after COW refresh should include 'DP Collab'");

    /* Appearance albums should also have SAW */
    const GPtrArray *app_albums = library_cache_get_artist_appearances(cow_cache, dp_id);
    cr_assert_not_null(app_albums);
    bool saw_found = false;
    for (guint i = 0; i < app_albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(app_albums, i);
        if (g_ascii_strcasecmp(a->title, "Selected Ambient Works") == 0)
            saw_found = true;
    }
    cr_assert(saw_found, "appearance albums after COW refresh should include SAW");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FIXTURE: merge2 — two-library merge for search/availability/NULL-MBID tests
 * ═══════════════════════════════════════════════════════════════════════════ */

static char m2_db_a[256], m2_db_b[256];
static library_cache_t *m2_cache = NULL;

static void build_m2_lib_a(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(m2_db_a, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk", MBID_DAFT_PUNK);
    int64_t at_id = db_get_or_create_artist_mb(db, "Aphex Twin", "Aphex Twin", MBID_APHEX_TWIN);

    /* Artist with same name in both libs but NO MBID — should NOT merge */
    int64_t ambig_id = db_get_or_create_artist(db, "Ambient Artist");

    /* Artist with NULL MBID, unique to lib A */
    int64_t local_id = db_get_or_create_artist(db, "Local Only A");

    /* Daft Punk albums */
    int64_t disc_id = 0, ram_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DP/Discovery", "Discovery", dp_id, 2001, &disc_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, disc_id, MBRID_DISCOVERY), QUADRATURE_OK);
    cr_assert_eq(db_upsert_folder_album(db, "DP/RAM", "Random Access Memories", dp_id, 2013, &ram_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, ram_id, MBRID_RAM), QUADRATURE_OK);

    /* Aphex Twin album */
    int64_t saw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AT/SAW", "Selected Ambient Works", at_id, 1992, &saw_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, saw_id, MBRID_SAW), QUADRATURE_OK);

    /* Ambiguous artist — album with NO MBRID */
    int64_t ambig_album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "Ambient/Chill", "Chill Vibes", ambig_id, 2020, &ambig_album_id), QUADRATURE_OK);
    /* Intentionally no db_set_album_release_id_from_tags — NULL MBRID */

    /* Local artist album */
    int64_t local_album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "Local/Stuff", "Local Album", local_id, 2023, &local_album_id), QUADRATURE_OK);

    /* Tracks */
    create_track(db, disc_id, "One More Time", 1, 1);
    create_track(db, ram_id, "Get Lucky", 1, 1);
    create_track(db, saw_id, "Xtal", 1, 1);
    /* SAW track 2 features Daft Punk */
    create_track(db, saw_id, "Pulsewidth feat DP", 2, 1);
    create_track(db, ambig_album_id, "Ambient Track A", 1, 1);
    create_track(db, local_album_id, "Local Track", 1, 1);

    /* Track artists */
    db_track_artist_t ta_dp = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 1, &ta_dp, 1);
    db_set_track_artists(db, 2, &ta_dp, 1);

    db_track_artist_t ta_at = { .artist_id = at_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 3, &ta_at, 1);

    /* Featured: Aphex Twin primary + Daft Punk featured on SAW track 2 */
    db_track_artist_t ta_feat[2] = {
        { .artist_id = at_id, .position = 0, .join_phrase = "" },
        { .artist_id = dp_id, .position = 1, .join_phrase = " feat. " },
    };
    db_set_track_artists(db, 4, ta_feat, 2);

    db_track_artist_t ta_ambig = { .artist_id = ambig_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 5, &ta_ambig, 1);

    db_track_artist_t ta_local = { .artist_id = local_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 6, &ta_local, 1);

    db_sync_album_fts(db, disc_id);
    db_sync_album_fts(db, ram_id);
    db_sync_album_fts(db, saw_id);
    db_sync_album_fts(db, ambig_album_id);
    db_sync_album_fts(db, local_album_id);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

static void build_m2_lib_b(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(m2_db_b, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk", MBID_DAFT_PUNK);
    int64_t at_id = db_get_or_create_artist_mb(db, "Aphex Twin", "Aphex Twin", MBID_APHEX_TWIN);

    /* Same name, no MBID — should NOT merge with lib A's "Ambient Artist" */
    int64_t ambig_id = db_get_or_create_artist(db, "Ambient Artist");

    /* DP albums: HAA (different MBRID) + RAM (same MBRID — deduped) */
    int64_t haa_id = 0, ram_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DP/HAA", "Human After All", dp_id, 2005, &haa_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, haa_id, MBRID_HAA_B), QUADRATURE_OK);
    cr_assert_eq(db_upsert_folder_album(db, "DP/RAM", "Random Access Memories", dp_id, 2013, &ram_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, ram_id, MBRID_RAM), QUADRATURE_OK);

    /* Aphex Twin album */
    int64_t drukqs_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "AT/Drukqs", "Drukqs", at_id, 2001, &drukqs_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, drukqs_id, MBRID_DRUKQS), QUADRATURE_OK);

    /* Ambiguous artist — album with NO MBRID (different from lib A's) */
    int64_t ambig_album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "Ambient/Zen", "Zen Vibes", ambig_id, 2021, &ambig_album_id), QUADRATURE_OK);

    /* Tracks */
    create_track(db, haa_id, "Robot Rock", 1, 1);
    create_track(db, ram_id, "Get Lucky B", 1, 1);
    create_track(db, drukqs_id, "Vordhosbn", 1, 1);
    create_track(db, ambig_album_id, "Ambient Track B", 1, 1);

    db_track_artist_t ta_dp = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 1, &ta_dp, 1);
    db_set_track_artists(db, 2, &ta_dp, 1);

    db_track_artist_t ta_at = { .artist_id = at_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 3, &ta_at, 1);

    db_track_artist_t ta_ambig = { .artist_id = ambig_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 4, &ta_ambig, 1);

    db_sync_album_fts(db, haa_id);
    db_sync_album_fts(db, ram_id);
    db_sync_album_fts(db, drukqs_id);
    db_sync_album_fts(db, ambig_album_id);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

static void m2_setup(void) {
    pid_t pid = getpid();
    snprintf(m2_db_a, sizeof(m2_db_a), "/tmp/test_adv_a_%d.db", pid);
    snprintf(m2_db_b, sizeof(m2_db_b), "/tmp/test_adv_b_%d.db", pid);
    cleanup_db(m2_db_a);
    cleanup_db(m2_db_b);
    build_m2_lib_a();
    build_m2_lib_b();

    library_cache_source_t sources[2] = {
        { .db_path = m2_db_a, .music_base = "/music_a",
          .display_name = "Library A", .bitmap_index = 0 },
        { .db_path = m2_db_b, .music_base = "/music_b",
          .display_name = "Library B", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &m2_cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(m2_cache, 0);
    library_cache_warm_slot_blocking(m2_cache, 1);
}

static void m2_teardown(void) {
    library_cache_destroy(m2_cache);
    m2_cache = NULL;
    cleanup_db(m2_db_a);
    cleanup_db(m2_db_b);
}

/* ── P1: Search Dedup Tests ───────────────────────────────────────────── */

Test(search_dedup, search_artist_no_duplicates, .init = m2_setup, .fini = m2_teardown) {
    /* Search for "Daft Punk" — should return exactly 1 result (the merged rep),
     * not 2 (one per library). */
    library_search_results_t *r = library_cache_search(
        m2_cache, "Daft Punk", LIBRARY_SEARCH_FILTER_ARTISTS, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(r);

    int dp_count = 0;
    for (guint i = 0; i < r->artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(r->artists, i);
        if (g_ascii_strcasecmp(a->name, "Daft Punk") == 0) dp_count++;
    }
    cr_assert_eq(dp_count, 1,
        "search should return 1 Daft Punk (merged rep), got %d", dp_count);

    library_search_results_free(r);
}

Test(search_dedup, search_album_no_duplicates, .init = m2_setup, .fini = m2_teardown) {
    /* Search for "Random Access" — RAM exists in both libraries with same MBRID.
     * Should return exactly 1 result after dedup. */
    library_search_results_t *r = library_cache_search(
        m2_cache, "Random Access", LIBRARY_SEARCH_FILTER_ALBUMS, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(r);

    int ram_count = 0;
    for (guint i = 0; i < r->albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(r->albums, i);
        if (g_ascii_strcasecmp(a->title, "Random Access Memories") == 0) ram_count++;
    }
    cr_assert_eq(ram_count, 1,
        "search should return 1 RAM (deduped by MBRID), got %d", ram_count);

    library_search_results_free(r);
}

Test(search_dedup, search_respects_library_mask, .init = m2_setup, .fini = m2_teardown) {
    /* Search only Library A — should find Discovery but not HAA */
    uint32_t mask_a = (1u << 0);
    library_search_results_t *r = library_cache_search(
        m2_cache, "Discovery", LIBRARY_SEARCH_FILTER_ALBUMS, 10, NULL, mask_a);
    cr_assert_not_null(r);
    cr_assert(r->albums->len > 0, "Discovery should appear in Library A search");
    library_search_results_free(r);

    r = library_cache_search(
        m2_cache, "Human After All", LIBRARY_SEARCH_FILTER_ALBUMS, 10, NULL, mask_a);
    cr_assert_not_null(r);
    cr_assert_eq(r->albums->len, 0,
        "HAA should NOT appear when searching only Library A");
    library_search_results_free(r);
}

/* ── P1: Availability Flag Tests ──────────────────────────────────────── */

Test(availability, unavailable_library_hidden_from_artists, .init = m2_setup, .fini = m2_teardown) {
    /* Both available: should see Local Only A */
    GPtrArray *artists = library_cache_get_artists_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Local Only A", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert(artists->len > 0, "Local Only A should be visible when lib A is available");
    g_ptr_array_unref(artists);

    /* Mark Library A unavailable */
    library_cache_set_available(m2_cache, 0, FALSE);

    artists = library_cache_get_artists_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Local Only A", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert_eq(artists->len, 0,
        "Local Only A should vanish when Library A is unavailable");
    g_ptr_array_unref(artists);

    /* Restore */
    library_cache_set_available(m2_cache, 0, TRUE);
}

Test(availability, unavailable_library_hidden_from_albums, .init = m2_setup, .fini = m2_teardown) {
    /* Mark Library B unavailable */
    library_cache_set_available(m2_cache, 1, FALSE);

    /* HAA only in Library B — should vanish */
    GPtrArray *albums = library_cache_get_albums_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Human After All", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 0,
        "HAA should vanish when Library B is unavailable");
    g_ptr_array_unref(albums);

    /* Discovery in Library A — should still be visible */
    albums = library_cache_get_albums_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Discovery", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert(albums->len > 0, "Discovery should remain visible");
    g_ptr_array_unref(albums);

    library_cache_set_available(m2_cache, 1, TRUE);
}

Test(availability, unavailable_library_hidden_from_search, .init = m2_setup, .fini = m2_teardown) {
    library_cache_set_available(m2_cache, 1, FALSE);

    library_search_results_t *r = library_cache_search(
        m2_cache, "Drukqs", LIBRARY_SEARCH_FILTER_ALBUMS, 10, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(r);
    cr_assert_eq(r->albums->len, 0,
        "Drukqs should vanish from search when Library B is unavailable");
    library_search_results_free(r);

    library_cache_set_available(m2_cache, 1, TRUE);
}

Test(availability, single_entity_getters_still_work, .init = m2_setup, .fini = m2_teardown) {
    /* Get a track ID from Library B before marking unavailable */
    GPtrArray *albums = library_cache_get_albums_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Drukqs", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert(albums->len > 0);
    const library_album_info_t *drukqs = g_ptr_array_index(albums, 0);
    int64_t drukqs_id = drukqs->album_id;
    g_ptr_array_unref(albums);

    /* Mark Library B unavailable */
    library_cache_set_available(m2_cache, 1, FALSE);

    /* Single-entity getter should STILL work (for in-flight operations) */
    const library_album_info_t *album = library_cache_get_album(m2_cache, drukqs_id);
    cr_assert_not_null(album, "get_album should still work for unavailable library");
    cr_assert_str_eq(album->title, "Drukqs");

    library_cache_set_available(m2_cache, 1, TRUE);
}

/* ── P1: NULL MBID Merge Edge Cases ───────────────────────────────────── */

Test(null_mbid, same_name_no_mbid_not_merged, .init = m2_setup, .fini = m2_teardown) {
    /* "Ambient Artist" exists in both libraries with NO MBID.
     * They should NOT be merged — should appear as 2 separate entries. */
    GPtrArray *artists = library_cache_get_artists_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Ambient Artist", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    int count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, "Ambient Artist") == 0) {
            count++;
            /* Each should NOT be merged */
            cr_assert_eq(a->merged_source_count, 0,
                "Ambient Artist (no MBID) should not have merged sources");
            cr_assert(a->library_index >= 0,
                "Ambient Artist should have a valid library_index (not merged)");
        }
    }
    g_ptr_array_unref(artists);

    cr_assert_eq(count, 2,
        "2 separate 'Ambient Artist' entries expected (no MBID merge), got %d", count);
}

Test(null_mbid, null_mbrid_albums_never_dedup, .init = m2_setup, .fini = m2_teardown) {
    /* "Chill Vibes" (lib A) and "Zen Vibes" (lib B) both by "Ambient Artist"
     * with NO MBRID — both should appear, never deduped */
    GPtrArray *albums = library_cache_get_albums_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    bool found_chill = has_album_title(albums, "Chill Vibes");
    bool found_zen   = has_album_title(albums, "Zen Vibes");
    g_ptr_array_unref(albums);

    cr_assert(found_chill, "Chill Vibes (lib A, no MBRID) should be present");
    cr_assert(found_zen,   "Zen Vibes (lib B, no MBRID) should be present");
}

Test(null_mbid, null_mbrid_album_not_deduped_against_mbrid_album, .init = m2_setup, .fini = m2_teardown) {
    /* Verify that albums with NULL MBRID don't accidentally get deduped
     * against albums that DO have MBRIDs */
    GPtrArray *albums = library_cache_get_albums_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    /* Count all unique album titles */
    /* Expected albums:
     *   Lib A: Discovery, RAM, SAW, Chill Vibes, Local Album = 5
     *   Lib B: HAA, RAM(deduped), Drukqs, Zen Vibes = 3 (RAM deduped)
     *   Total unique: 5 + 3 = 8
     */
    cr_assert_eq(albums->len, 8,
        "should have 8 albums total (RAM deduped, NULL-MBRID ones kept), got %u", albums->len);

    /* RAM should appear only once */
    cr_assert_eq(count_album_title(albums, "Random Access Memories"), 1,
        "RAM should appear once (deduped by MBRID)");

    g_ptr_array_unref(albums);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FIXTURE: three_lib — for add_slot, remove-middle, appearance dedup
 * ═══════════════════════════════════════════════════════════════════════════ */

static char t3_db_a[256], t3_db_b[256], t3_db_c[256];
static library_cache_t *t3_cache = NULL;

static void build_t3_lib_c(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(t3_db_c, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t dp_id = db_get_or_create_artist_mb(db, "Daft Punk", "Daft Punk", MBID_DAFT_PUNK);
    int64_t boc_id = db_get_or_create_artist_mb(db, "Boards of Canada", "Boards of Canada", MBID_BOARDS_CANADA);

    /* DP album: Homework (unique MBRID, not in lib A or B) */
    int64_t hw_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "DP/Homework", "Homework", dp_id, 1997, &hw_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, hw_id, MBRID_HOMEWORK), QUADRATURE_OK);

    /* Boards of Canada album (unique to lib C) */
    int64_t geo_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "BOC/Geogaddi", "Geogaddi", boc_id, 2002, &geo_id), QUADRATURE_OK);
    cr_assert_eq(db_set_album_release_id_from_tags(db, geo_id, MBRID_GEOGADDI), QUADRATURE_OK);

    create_track(db, hw_id, "Around the World", 1, 1);
    create_track(db, geo_id, "Music is Math", 1, 1);

    db_track_artist_t ta_dp = { .artist_id = dp_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 1, &ta_dp, 1);
    db_track_artist_t ta_boc = { .artist_id = boc_id, .position = 0, .join_phrase = "" };
    db_set_track_artists(db, 2, &ta_boc, 1);

    db_sync_album_fts(db, hw_id);
    db_sync_album_fts(db, geo_id);

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

static void t3_setup(void) {
    pid_t pid = getpid();
    snprintf(t3_db_a, sizeof(t3_db_a), "/tmp/test_t3a_%d.db", pid);
    snprintf(t3_db_b, sizeof(t3_db_b), "/tmp/test_t3b_%d.db", pid);
    snprintf(t3_db_c, sizeof(t3_db_c), "/tmp/test_t3c_%d.db", pid);
    cleanup_db(t3_db_a);
    cleanup_db(t3_db_b);
    cleanup_db(t3_db_c);

    /* Reuse m2 lib A and lib B builders */
    snprintf(m2_db_a, sizeof(m2_db_a), "%s", t3_db_a);
    snprintf(m2_db_b, sizeof(m2_db_b), "%s", t3_db_b);
    build_m2_lib_a();
    build_m2_lib_b();
    build_t3_lib_c();

    /* Start with only 2 libraries — lib C added dynamically in tests */
    library_cache_source_t sources[2] = {
        { .db_path = t3_db_a, .music_base = "/music_a",
          .display_name = "Library A", .bitmap_index = 0 },
        { .db_path = t3_db_b, .music_base = "/music_b",
          .display_name = "Library B", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &t3_cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(t3_cache, 0);
    library_cache_warm_slot_blocking(t3_cache, 1);
}

static void t3_teardown(void) {
    library_cache_destroy(t3_cache);
    t3_cache = NULL;
    cleanup_db(t3_db_a);
    cleanup_db(t3_db_b);
    cleanup_db(t3_db_c);
}

/* ── P2: Add Slot to Running Cache ────────────────────────────────────── */

Test(add_slot, third_library_merges_correctly, .init = t3_setup, .fini = t3_teardown) {
    /* Before: Daft Punk merged from A+B */
    int64_t dp_id = find_artist_id(t3_cache, "Daft Punk");
    const library_artist_info_t *dp = library_cache_get_artist(t3_cache, dp_id);
    cr_assert_not_null(dp);
    cr_assert(dp->merged_source_count > 0, "Daft Punk should be merged before add_slot");

    /* Add Library C */
    library_cache_source_t src_c = {
        .db_path = t3_db_c, .music_base = "/music_c",
        .display_name = "Library C", .bitmap_index = 2,
    };
    int bi = library_cache_add_slot(t3_cache, &src_c);
    cr_assert_eq(bi, 2, "add_slot should return bitmap_index 2");
    cr_assert_eq(library_cache_get_library_count(t3_cache), 3);

    /* Warm the new slot */
    library_cache_warm_slot_blocking(t3_cache, 2);

    /* Daft Punk should now be merged from A+B+C */
    dp_id = find_artist_id(t3_cache, "Daft Punk");
    dp = library_cache_get_artist(t3_cache, dp_id);
    cr_assert_not_null(dp);
    cr_assert_eq(dp->merged_source_count, 2,
        "Daft Punk should have 2 merged sources (from 3 libraries), got %d",
        dp->merged_source_count);

    /* Daft Punk albums: Discovery(A) + RAM(A,deduped) + HAA(B) + Homework(C) = 4 */
    const GPtrArray *albums = library_cache_get_albums_by_artist(t3_cache, dp_id);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 4,
        "Daft Punk should have 4 albums after adding lib C, got %u", albums->len);
    cr_assert(has_album_title(albums, "Homework"), "missing Homework from lib C");
}

Test(add_slot, new_artist_from_added_library, .init = t3_setup, .fini = t3_teardown) {
    /* Boards of Canada only in lib C — shouldn't exist before add */
    GPtrArray *artists = library_cache_get_artists_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Boards of Canada", NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(artists->len, 0, "BOC should not exist before adding lib C");
    g_ptr_array_unref(artists);

    /* Add and warm lib C */
    library_cache_source_t src_c = {
        .db_path = t3_db_c, .music_base = "/music_c",
        .display_name = "Library C", .bitmap_index = 2,
    };
    library_cache_add_slot(t3_cache, &src_c);
    library_cache_warm_slot_blocking(t3_cache, 2);

    /* Now BOC should exist */
    artists = library_cache_get_artists_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Boards of Canada", NULL, LIBRARY_MASK_ALL);
    cr_assert(artists->len > 0, "BOC should appear after adding lib C");
    g_ptr_array_unref(artists);
}

/* ── P2: Concurrent Reads During COW Refresh ──────────────────────────── */

typedef struct {
    library_cache_t *cache;
    int iterations;
} ConcurrentReaderCtx;

static void *concurrent_reader(void *arg) {
    ConcurrentReaderCtx *ctx = arg;
    for (int i = 0; i < ctx->iterations; i++) {
        /* Mixed read operations — must not crash or return corrupt data */
        GPtrArray *artists = library_cache_get_artists_filtered(
            ctx->cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        if (artists) g_ptr_array_unref(artists);

        GPtrArray *albums = library_cache_get_albums_filtered(
            ctx->cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        if (albums) g_ptr_array_unref(albums);

        library_cache_get_track(ctx->cache, 1);
        library_cache_get_album(ctx->cache, 1);
        library_cache_get_artist(ctx->cache, 1);

        library_search_results_t *r = library_cache_search(
            ctx->cache, "Daft", LIBRARY_SEARCH_FILTER_ALL, 5, NULL, LIBRARY_MASK_ALL);
        library_search_results_free(r);

        library_cache_get_next_track_id(ctx->cache, 1);
    }
    return NULL;
}

Test(concurrent_cow, readers_during_refresh_no_crash, .init = m2_setup, .fini = m2_teardown) {
    /* Launch reader threads */
    ConcurrentReaderCtx ctx = { .cache = m2_cache, .iterations = 200 };
    pthread_t readers[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&readers[i], NULL, concurrent_reader, &ctx);

    /* While readers are hammering, do a COW refresh */
    library_cache_refresh_slot(m2_cache, 0, NULL, 0);
    library_cache_await_slot(m2_cache, 0);

    /* Join readers */
    for (int i = 0; i < 4; i++)
        pthread_join(readers[i], NULL);

    /* If we get here without crash, ASAN violation, or deadlock — test passes */
    /* Verify data is still consistent */
    GPtrArray *artists = library_cache_get_artists_filtered(
        m2_cache, LIBRARY_SORT_NAME_ASC, "Daft Punk", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert(artists->len > 0, "Daft Punk should still be findable after concurrent COW");
    g_ptr_array_unref(artists);
}

/* ── P2: Appearance Track Dedup Across Libraries ──────────────────────── */

Test(appearance_dedup, featured_credit_not_duplicated, .init = m2_setup, .fini = m2_teardown) {
    /* Daft Punk appears as featured on Aphex Twin's SAW in lib A.
     * Lib B has no such feature. So appearances should be exactly 1 album. */
    int64_t dp_id = find_artist_id(m2_cache, "Daft Punk");
    const GPtrArray *appearances = library_cache_get_artist_appearances(m2_cache, dp_id);

    if (appearances) {
        /* SAW should appear exactly once (only in lib A) */
        int saw_count = 0;
        for (guint i = 0; i < appearances->len; i++) {
            const library_album_info_t *a = g_ptr_array_index(appearances, i);
            if (g_ascii_strcasecmp(a->title, "Selected Ambient Works") == 0)
                saw_count++;
        }
        cr_assert_eq(saw_count, 1,
            "SAW should appear once in DP's appearances, got %d", saw_count);
    }
}

Test(appearance_dedup, appearance_tracks_populated_from_featured_credits, .init = m2_setup, .fini = m2_teardown) {
    /* Daft Punk is a featured track artist on SAW track 2 ("Pulsewidth feat DP").
     * artist_appearance_tracks must contain this track after cache warming. */
    int64_t dp_id = find_artist_id(m2_cache, "Daft Punk");
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(m2_cache, dp_id);

    cr_assert_not_null(app_tracks,
        "appearance tracks should not be NULL for artist with featured credits");
    cr_assert(app_tracks->len > 0,
        "appearance tracks should contain featured credit tracks, got 0");

    /* Verify "Pulsewidth feat DP" is present */
    bool found_pulsewidth = false;
    for (guint i = 0; i < app_tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(app_tracks, i);
        if (strstr(t->title, "Pulsewidth") != NULL)
            found_pulsewidth = true;

        /* Each appearance track should be from an album where DP is NOT the primary artist */
        const library_album_info_t *album = library_cache_get_album(m2_cache, t->album_id);
        cr_assert_not_null(album, "appearance track's album should exist");
        const library_artist_info_t *album_artist = library_cache_get_artist(m2_cache, album->artist_id);
        if (album_artist) {
            cr_assert_neq(g_ascii_strcasecmp(album_artist->name, "Daft Punk"), 0,
                "appearance track should not be on a Daft Punk album");
        }
    }
    cr_assert(found_pulsewidth,
        "appearance tracks should include 'Pulsewidth feat DP'");
}

Test(appearance_dedup, appearance_tracks_count_matches_credits, .init = m2_setup, .fini = m2_teardown) {
    /* Lib A has exactly 1 track featuring Daft Punk on an Aphex Twin album:
     * SAW track 2 ("Pulsewidth feat DP"). Lib B has none.
     * So appearance_tracks should have exactly 1 track. */
    int64_t dp_id = find_artist_id(m2_cache, "Daft Punk");
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(m2_cache, dp_id);
    cr_assert_not_null(app_tracks);
    cr_assert_eq(app_tracks->len, 1,
        "DP should have exactly 1 appearance track (Pulsewidth), got %u",
        app_tracks->len);
}

Test(appearance_dedup, appearance_albums_count_matches_credits, .init = m2_setup, .fini = m2_teardown) {
    /* Daft Punk appears on exactly 1 album: SAW (in Lib A). */
    int64_t dp_id = find_artist_id(m2_cache, "Daft Punk");
    const GPtrArray *appearances = library_cache_get_artist_appearances(m2_cache, dp_id);
    cr_assert_not_null(appearances);
    cr_assert_eq(appearances->len, 1,
        "DP should have exactly 1 appearance album (SAW), got %u",
        appearances->len);
}

Test(appearance_dedup, no_features_no_appearances, .init = m2_setup, .fini = m2_teardown) {
    /* "Ambient Artist" and "Local Only A" are never featured on other albums.
     * They should have no appearance tracks or albums. */
    int64_t local_id = find_artist_id(m2_cache, "Local Only A");
    const GPtrArray *albums = library_cache_get_artist_appearances(m2_cache, local_id);
    const GPtrArray *tracks = library_cache_get_artist_appearance_tracks(m2_cache, local_id);

    cr_assert_eq(albums ? albums->len : 0, 0,
        "Local Only A should have 0 appearance albums");
    cr_assert_eq(tracks ? tracks->len : 0, 0,
        "Local Only A should have 0 appearance tracks");
}

Test(appearance_dedup, album_artist_excluded_from_own_album_appearances, .init = m2_setup, .fini = m2_teardown) {
    /* Aphex Twin is the album artist of SAW and all SAW tracks list Aphex Twin
     * as a track artist. Aphex Twin should NOT appear in its own appearances. */
    int64_t at_id = find_artist_id(m2_cache, "Aphex Twin");
    const GPtrArray *app_tracks = library_cache_get_artist_appearance_tracks(m2_cache, at_id);
    const GPtrArray *app_albums = library_cache_get_artist_appearances(m2_cache, at_id);

    /* Aphex Twin has no featured credits on non-AT albums */
    guint track_count = app_tracks ? app_tracks->len : 0;
    guint album_count = app_albums ? app_albums->len : 0;
    cr_assert_eq(track_count, 0,
        "Aphex Twin should not appear in its own album's appearance tracks, got %u",
        track_count);
    cr_assert_eq(album_count, 0,
        "Aphex Twin should not appear in its own album's appearances, got %u",
        album_count);
}

/* ── P2: Album Tracks Sort Order ──────────────────────────────────────── */

/* Use the cow fixture which has a single-library with Discovery (2 tracks) */

Test(sort_order, tracks_by_album_sorted_by_disc_and_track, .init = cow_setup, .fini = cow_teardown) {
    const GPtrArray *tracks = library_cache_get_tracks_by_album(cow_cache, 1);
    cr_assert_not_null(tracks);
    cr_assert_eq(tracks->len, 2, "Discovery should have 2 tracks");

    const library_track_info_t *t1 = g_ptr_array_index(tracks, 0);
    const library_track_info_t *t2 = g_ptr_array_index(tracks, 1);

    /* Verify disc_num, track_num ordering */
    cr_assert(t1->disc_num < t2->disc_num ||
              (t1->disc_num == t2->disc_num && t1->track_num < t2->track_num),
        "tracks should be sorted by (disc_num, track_num)");

    cr_assert_eq(t1->track_num, 1);
    cr_assert_eq(t2->track_num, 2);
}

/* Multi-disc sort order test — need a fixture with multi-disc album */

static char sort_db_path[256];
static library_cache_t *sort_cache = NULL;

static void build_sort_library(void) {
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(sort_db_path, &db), QUADRATURE_OK);
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    int64_t artist_id = db_get_or_create_artist(db, "Test Artist");
    int64_t album_id = 0;
    cr_assert_eq(db_upsert_folder_album(db, "Test/Album", "Multi-Disc Album",
        artist_id, 2020, &album_id), QUADRATURE_OK);

    /* Insert tracks OUT OF ORDER to verify sort corrects them */
    create_track(db, album_id, "Disc 2 Track 2", 2, 2);
    create_track(db, album_id, "Disc 1 Track 1", 1, 1);
    create_track(db, album_id, "Disc 2 Track 1", 1, 2);
    create_track(db, album_id, "Disc 1 Track 3", 3, 1);
    create_track(db, album_id, "Disc 1 Track 2", 2, 1);

    db_track_artist_t ta = { .artist_id = artist_id, .position = 0, .join_phrase = "" };
    for (int64_t tid = 1; tid <= 5; tid++)
        db_set_track_artists(db, tid, &ta, 1);

    db_sync_album_fts(db, album_id);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    db_close(db);
}

static void sort_setup(void) {
    snprintf(sort_db_path, sizeof(sort_db_path), "/tmp/test_sort_%d.db", getpid());
    cleanup_db(sort_db_path);
    build_sort_library();

    library_cache_source_t src = {
        .db_path = sort_db_path, .music_base = "/music",
        .display_name = "Sort Lib", .bitmap_index = 0,
    };
    cr_assert_eq(library_cache_create_multi(&src, 1, &sort_cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(sort_cache, 0);
}

static void sort_teardown(void) {
    library_cache_destroy(sort_cache);
    sort_cache = NULL;
    cleanup_db(sort_db_path);
}

Test(sort_order, multi_disc_tracks_sorted_correctly, .init = sort_setup, .fini = sort_teardown) {
    const GPtrArray *tracks = library_cache_get_tracks_by_album(sort_cache, 1);
    cr_assert_not_null(tracks);
    cr_assert_eq(tracks->len, 5, "should have 5 tracks");

    /* Expected order: D1T1, D1T2, D1T3, D2T1, D2T2 */
    const uint16_t expected_disc[]  = { 1, 1, 1, 2, 2 };
    const uint16_t expected_track[] = { 1, 2, 3, 1, 2 };

    for (guint i = 0; i < tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(tracks, i);
        cr_assert_eq(t->disc_num, expected_disc[i],
            "track %u: expected disc %u, got %u", i, expected_disc[i], t->disc_num);
        cr_assert_eq(t->track_num, expected_track[i],
            "track %u: expected track %u, got %u", i, expected_track[i], t->track_num);
    }
}

Test(sort_order, first_track_id_set_correctly, .init = sort_setup, .fini = sort_teardown) {
    const library_album_info_t *album = library_cache_get_album(sort_cache, 1);
    cr_assert_not_null(album);
    cr_assert(album->first_track_id != 0, "first_track_id should be set");

    /* first_track_id should point to disc 1, track 1 */
    const library_track_info_t *first = library_cache_get_track(sort_cache, album->first_track_id);
    cr_assert_not_null(first);
    cr_assert_eq(first->disc_num, 1, "first track should be disc 1");
    cr_assert_eq(first->track_num, 1, "first track should be track 1");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P3: Invalid Global IDs
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(invalid_ids, fabricated_bitmap_index, .init = cow_setup, .fini = cow_teardown) {
    /* Global ID with bitmap_index 15 — doesn't exist */
    int64_t bad_id = LIBRARY_MAKE_GLOBAL_ID(15, 1);

    cr_assert_null(library_cache_get_track(cow_cache, bad_id));
    cr_assert_null(library_cache_get_album(cow_cache, bad_id));
    cr_assert_null(library_cache_get_artist(cow_cache, bad_id));
    cr_assert_null(library_cache_get_track_artists(cow_cache, bad_id));
    cr_assert_null(library_cache_get_tracks_by_album(cow_cache, bad_id));
    cr_assert_null(library_cache_get_albums_by_artist(cow_cache, bad_id));
    cr_assert_eq(library_cache_get_next_track_id(cow_cache, bad_id), 0);
    cr_assert_eq(library_cache_get_prev_track_id(cow_cache, bad_id), 0);
}

Test(invalid_ids, out_of_range_local_id, .init = cow_setup, .fini = cow_teardown) {
    /* Valid bitmap_index but absurdly large local ID */
    int64_t bad_id = LIBRARY_MAKE_GLOBAL_ID(0, 999999);

    cr_assert_null(library_cache_get_track(cow_cache, bad_id));
    cr_assert_null(library_cache_get_album(cow_cache, bad_id));
    cr_assert_null(library_cache_get_artist(cow_cache, bad_id));
    cr_assert_eq(library_cache_get_next_track_id(cow_cache, bad_id), 0);
}

Test(invalid_ids, zero_and_negative_ids, .init = cow_setup, .fini = cow_teardown) {
    cr_assert_null(library_cache_get_track(cow_cache, 0));
    cr_assert_null(library_cache_get_album(cow_cache, 0));
    cr_assert_null(library_cache_get_artist(cow_cache, 0));
    cr_assert_null(library_cache_get_track(cow_cache, -1));
    cr_assert_null(library_cache_get_album(cow_cache, -1));
    cr_assert_null(library_cache_get_artist(cow_cache, -1));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P3: Remove Middle Slot
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(remove_middle, slot_0_removed_others_still_work, .init = t3_setup, .fini = t3_teardown) {
    /* First add lib C so we have 3 slots */
    library_cache_source_t src_c = {
        .db_path = t3_db_c, .music_base = "/music_c",
        .display_name = "Library C", .bitmap_index = 2,
    };
    library_cache_add_slot(t3_cache, &src_c);
    library_cache_warm_slot_blocking(t3_cache, 2);
    cr_assert_eq(library_cache_get_library_count(t3_cache), 3);

    /* Verify BOC exists in lib C */
    GPtrArray *artists = library_cache_get_artists_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Boards of Canada", NULL, LIBRARY_MASK_ALL);
    cr_assert(artists->len > 0, "BOC should exist before remove");
    g_ptr_array_unref(artists);

    /* Remove slot 0 (Library A) — the FIRST slot */
    cr_assert_eq(library_cache_remove_slot(t3_cache, 0), QUADRATURE_OK);
    cr_assert_eq(library_cache_get_library_count(t3_cache), 2);

    /* Library B (bitmap 1) should still work */
    GPtrArray *albums = library_cache_get_albums_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Human After All", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert(albums->len > 0, "HAA from Library B should survive after removing A");
    g_ptr_array_unref(albums);

    /* Library C (bitmap 2) should still work */
    artists = library_cache_get_artists_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Boards of Canada", NULL, LIBRARY_MASK_ALL);
    cr_assert(artists->len > 0, "BOC from Library C should survive after removing A");
    g_ptr_array_unref(artists);

    /* Library A entities should be gone */
    albums = library_cache_get_albums_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Discovery", NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 0, "Discovery from Library A should be gone");
    g_ptr_array_unref(albums);

    /* Local Only A should be gone */
    artists = library_cache_get_artists_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Local Only A", NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(artists->len, 0, "Local Only A should be gone after removing lib A");
    g_ptr_array_unref(artists);
}

Test(remove_middle, merge_updates_after_middle_removal, .init = t3_setup, .fini = t3_teardown) {
    /* Add lib C */
    library_cache_source_t src_c = {
        .db_path = t3_db_c, .music_base = "/music_c",
        .display_name = "Library C", .bitmap_index = 2,
    };
    library_cache_add_slot(t3_cache, &src_c);
    library_cache_warm_slot_blocking(t3_cache, 2);

    /* Daft Punk merged from A+B+C */
    int64_t dp_id = find_artist_id(t3_cache, "Daft Punk");
    const library_artist_info_t *dp = library_cache_get_artist(t3_cache, dp_id);
    cr_assert_eq(dp->merged_source_count, 2, "DP should have 2 sources from 3 libs");

    /* Remove Library B (middle slot) */
    cr_assert_eq(library_cache_remove_slot(t3_cache, 1), QUADRATURE_OK);

    /* Re-find Daft Punk — still merged from A+C */
    dp_id = find_artist_id(t3_cache, "Daft Punk");
    dp = library_cache_get_artist(t3_cache, dp_id);
    cr_assert_not_null(dp);
    cr_assert_eq(dp->merged_source_count, 1,
        "DP should have 1 merged source after removing B (A+C remain), got %d",
        dp->merged_source_count);

    /* Albums: Discovery(A) + RAM(A) + Homework(C) = 3. HAA was only in B (removed). */
    const GPtrArray *albums = library_cache_get_albums_by_artist(t3_cache, dp_id);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 3,
        "DP should have 3 albums after removing B (A+C), got %u", albums->len);
    cr_assert(has_album_title(albums, "Discovery"), "missing Discovery");
    cr_assert(has_album_title(albums, "Homework"), "missing Homework");
    cr_assert(has_album_title(albums, "Random Access Memories"), "missing RAM");
    cr_assert(!has_album_title(albums, "Human After All"), "HAA should be gone with lib B");
}

Test(remove_middle, bitmap_index_stable_after_removal, .init = t3_setup, .fini = t3_teardown) {
    /* Add lib C at bitmap_index 2 */
    library_cache_source_t src_c = {
        .db_path = t3_db_c, .music_base = "/music_c",
        .display_name = "Library C", .bitmap_index = 2,
    };
    library_cache_add_slot(t3_cache, &src_c);
    library_cache_warm_slot_blocking(t3_cache, 2);

    /* Get a Geogaddi album ID (lib C, bitmap 2) */
    GPtrArray *albums = library_cache_get_albums_filtered(
        t3_cache, LIBRARY_SORT_NAME_ASC, "Geogaddi", NULL, LIBRARY_MASK_ALL);
    cr_assert(albums->len > 0);
    int64_t geo_id = ((library_album_info_t *)g_ptr_array_index(albums, 0))->album_id;
    g_ptr_array_unref(albums);

    /* Verify bitmap_index encoded in the ID */
    cr_assert_eq(LIBRARY_GLOBAL_ID_LIB(geo_id), 2,
        "Geogaddi should encode bitmap_index 2");

    /* Remove Library A (bitmap 0) */
    library_cache_remove_slot(t3_cache, 0);

    /* geo_id should still resolve correctly — bitmap_index is stable */
    const library_album_info_t *geo = library_cache_get_album(t3_cache, geo_id);
    cr_assert_not_null(geo, "Geogaddi should still be accessible by same global ID after removing slot 0");
    cr_assert_str_eq(geo->title, "Geogaddi");
}
