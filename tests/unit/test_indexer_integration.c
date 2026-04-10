/**
 * Integration tests for the indexer pipeline.
 *
 * These tests create real audio files (tiny FLACs via ffmpeg), run the actual
 * indexer_scan() pipeline, and verify results through both direct DB queries
 * and the library_cache API.
 *
 * No MusicBrainz resolution — tests Phase 1 (scan) and Phase 2 (metadata)
 * only, which is the core indexer behavior.
 *
 * USER STORIES:
 *   1. Fresh library scan — create files, index, verify through cache
 *   2. Re-scan unchanged — verify mtime skip works
 *   3. Delete album folder — re-index, verify orphan pruning through cache
 *   4. Add new album — re-index, verify incremental addition
 *   5. Cancel mid-scan — verify clean state
 *
 * Requires: ffmpeg in PATH (for FLAC generation)
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "test_helpers.h"
#include "quadrature/indexer.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <libavformat/avformat.h>

ReportHook(PRE_ALL)(struct criterion_test_set *tests) {
    (void)tests;
    avformat_network_init();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FLAC file generation (same approach as e2e tests)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void mkdirs(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    (void)system(cmd);
}

static char *shell_escape(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 4 + 1);
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
        } else {
            *p++ = s[i];
        }
    }
    *p = '\0';
    return out;
}

static int create_flac(const char *path, const char *const *metadata_pairs,
                       int duration_secs) {
    char cmd[8192];
    double dur = duration_secs > 0 ? (double)duration_secs : 1;
    int off = snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -loglevel error -f lavfi -i sine=frequency=60:sample_rate=8000 "
        "-t %.1f -ac 1 ", dur);

    for (const char *const *p = metadata_pairs; *p; p++) {
        char *escaped = shell_escape(*p);
        off += snprintf(cmd + off, sizeof(cmd) - off, "-metadata '%s' ", escaped);
        free(escaped);
    }

    char *epath = shell_escape(path);
    off += snprintf(cmd + off, sizeof(cmd) - off, "'%s'", epath);
    free(epath);

    return system(cmd);
}

static void rm_rf(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static bool ffmpeg_available(void) {
    return system("ffmpeg -version > /dev/null 2>&1") == 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Indexer + callback helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int started, progress, completed, library_updated;
    bool success;
    indexer_progress_t last_progress;
} test_tracker_t;

static void tracker_callback(indexer_event_t event,
                             const indexer_progress_t *progress,
                             void *user_data) {
    test_tracker_t *t = user_data;
    switch (event) {
        case INDEXER_STARTED:         t->started++; break;
        case INDEXER_PROGRESS:
            t->progress++;
            if (progress) t->last_progress = *progress;
            break;
        case INDEXER_COMPLETED:
            t->completed++; t->success = true;
            if (progress) t->last_progress = *progress;
            break;
        case INDEXER_LIBRARY_UPDATED: t->library_updated++; break;
        case INDEXER_CANCELLED:       t->completed++; break;
        case INDEXER_ERROR:           t->completed++; break;
        case INDEXER_ARTWORK_UPDATED: break;
    }
}

static void run_indexer(const char *library_root, const char *data_root,
                        test_tracker_t *tracker) {
    indexer_config_t config = {
        .thread_count    = 2,
        .process_artwork = false,
        .mb_resolve      = false,
        .callback        = tracker_callback,
        .user_data       = tracker,
    };

    indexer_t *indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, library_root, data_root), QUADRATURE_OK);
    indexer_wait(indexer);

    cr_assert(tracker->completed > 0, "Indexer did not complete for %s", library_root);
    cr_assert(tracker->success, "Indexer failed for %s", library_root);

    indexer_destroy(indexer);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library builders — create realistic FLAC files with tags
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Build a Daft Punk library:
 *   DaftPunk/Discovery/  — 3 tracks (One More Time, Aerodynamic, Digital Love)
 *   DaftPunk/RAM/        — 2 tracks (Get Lucky, Lose Yourself to Dance)
 */
static void build_daft_punk_library(const char *root) {
    char path[1024], fpath[1024];

    snprintf(path, sizeof(path), "%s/Daft Punk/Discovery", root);
    mkdirs(path);

    struct { int num; const char *title; int dur; } disc_tracks[] = {
        { 1, "One More Time",  321 },
        { 2, "Aerodynamic",    212 },
        { 3, "Digital Love",   301 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac", path,
                 disc_tracks[i].num, disc_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", disc_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", disc_tracks[i].title);
        const char *tags[] = {
            title_tag, "artist=Daft Punk", "album=Discovery",
            "album_artist=Daft Punk", tracknum, "date=2001", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, disc_tracks[i].dur), 0);
    }

    snprintf(path, sizeof(path), "%s/Daft Punk/Random Access Memories", root);
    mkdirs(path);

    struct { int num; const char *title; int dur; } ram_tracks[] = {
        { 1, "Get Lucky",                367 },
        { 2, "Lose Yourself to Dance",   353 },
    };
    for (int i = 0; i < 2; i++) {
        char tracknum[16], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac", path,
                 ram_tracks[i].num, ram_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", ram_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", ram_tracks[i].title);
        const char *tags[] = {
            title_tag, "artist=Daft Punk", "album=Random Access Memories",
            "album_artist=Daft Punk", tracknum, "date=2013", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, ram_tracks[i].dur), 0);
    }
}

/**
 * Build a Golden Features album:
 *   GoldenFeatures/SECT/ — 2 tracks
 */
static void build_golden_features_library(const char *root) {
    char path[1024], fpath[1024];

    snprintf(path, sizeof(path), "%s/Golden Features/SECT", root);
    mkdirs(path);

    struct { int num; const char *title; int dur; } tracks[] = {
        { 1, "Ariana",   198 },
        { 2, "Falling",  224 },
    };
    for (int i = 0; i < 2; i++) {
        char tracknum[16], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac", path,
                 tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = {
            title_tag, "artist=Golden Features", "album=SECT",
            "album_artist=Golden Features", tracknum, "date=2021", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 1: Fresh library scan
 *
 * "I point quadrature at a folder with Daft Punk albums. After indexing,
 *  the library cache should show all artists, albums, tracks, and search
 *  should work."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s1_lib[256], s1_data[256];

static void story1_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s1_lib, sizeof(s1_lib), "/tmp/quad_integ_%d_s1_lib", pid);
    snprintf(s1_data, sizeof(s1_data), "/tmp/quad_integ_%d_s1_data", pid);
    rm_rf(s1_lib); rm_rf(s1_data);
    mkdirs(s1_data);

    build_daft_punk_library(s1_lib);
}

static void story1_teardown(void) {
    rm_rf(s1_lib); rm_rf(s1_data);
}

Test(indexer, fresh_library_scan,
     .init = story1_setup, .fini = story1_teardown, .timeout = 60) {

    /* ── Index ── */
    test_tracker_t tracker = {0};
    run_indexer(s1_lib, s1_data, &tracker);

    /* ── Verify indexer progress ── */
    cr_assert_eq(tracker.started, 1);
    cr_assert_gt(tracker.progress, 0);
    cr_assert_eq(tracker.last_progress.files_total, 5);
    cr_assert_eq(tracker.last_progress.files_processed, 5);
    cr_assert_eq(tracker.last_progress.files_new, 5);
    cr_assert_gt(tracker.last_progress.dirs_scanned, 0);

    /* ── Verify through DB ── */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s1_data);
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path, &db), QUADRATURE_OK);

    size_t track_count = 0;
    cr_assert_eq(db_get_total_track_count(db, &track_count), QUADRATURE_OK);
    cr_assert_eq(track_count, 5, "Should have 5 tracks");
    db_close(db);

    /* ── Verify through library_cache ── */
    library_cache_source_t src = {
        .db_path = db_path, .music_base = s1_lib,
        .display_name = "Test Library", .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    /* Artists: should find Daft Punk */
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);
    cr_assert_eq(artists->len, 1, "Should have 1 artist (Daft Punk)");
    const library_artist_info_t *dp = g_ptr_array_index(artists, 0);
    cr_assert_str_eq(dp->name, "Daft Punk");
    int64_t dp_id = dp->artist_id;
    g_ptr_array_unref(artists);

    /* Albums: Discovery + RAM */
    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 2, "Should have 2 albums");
    cr_assert(test_has_album_title(albums, "Discovery"));
    cr_assert(test_has_album_title(albums, "Random Access Memories"));

    /* Grab album IDs for further queries */
    const library_album_info_t *disc = test_find_album(albums, "Discovery");
    cr_assert_not_null(disc);

    const library_album_info_t *ram = test_find_album(albums, "Random Access Memories");
    cr_assert_not_null(ram);
    g_ptr_array_unref(albums);

    /* Tracks by album: Discovery should have 3 tracks in order */
    GPtrArray *disc_tracks = library_cache_get_tracks_by_album(
        cache, disc->album_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(disc_tracks);
    cr_assert_eq(disc_tracks->len, 3);
    const library_track_info_t *t1 = g_ptr_array_index(disc_tracks, 0);
    cr_assert_str_eq(t1->title, "One More Time");
    cr_assert_eq(t1->track_num, 1);
    const library_track_info_t *t2 = g_ptr_array_index(disc_tracks, 1);
    cr_assert_str_eq(t2->title, "Aerodynamic");
    const library_track_info_t *t3 = g_ptr_array_index(disc_tracks, 2);
    cr_assert_str_eq(t3->title, "Digital Love");
    g_ptr_array_unref(disc_tracks);

    /* Track navigation */
    int64_t next = library_cache_get_next_track_id(cache, t1->track_id);
    cr_assert_eq(next, t2->track_id, "Next track from 'One More Time' should be 'Aerodynamic'");
    int64_t prev = library_cache_get_prev_track_id(cache, t2->track_id);
    cr_assert_eq(prev, t1->track_id, "Prev track from 'Aerodynamic' should be 'One More Time'");

    /* Last track returns 0 */
    int64_t past_end = library_cache_get_next_track_id(cache, t3->track_id);
    cr_assert_eq(past_end, 0, "Next from last track should return 0");

    /* Search */
    library_search_results_t *results = library_cache_search(
        cache, "Lucky", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);
    cr_assert_not_null(results->tracks);
    bool found_lucky = false;
    for (guint i = 0; i < results->tracks->len; i++) {
        const library_track_info_t *t = g_ptr_array_index(results->tracks, i);
        if (strstr(t->title, "Lucky")) found_lucky = true;
    }
    cr_assert(found_lucky, "Search for 'Lucky' should find 'Get Lucky'");
    library_search_results_free(results);

    /* Path resolution */
    char *path = library_cache_resolve_track_path(cache, t1->track_id);
    cr_assert_not_null(path);
    cr_assert(strstr(path, "One More Time") != NULL, "Path should contain track filename");
    cr_assert(strstr(path, ".flac") != NULL, "Path should end with .flac");
    g_free(path);

    /* Albums by artist */
    GPtrArray *dp_albums = library_cache_get_albums_by_artist(
        cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(dp_albums);
    cr_assert_eq(dp_albums->len, 2, "Daft Punk should have 2 albums");
    g_ptr_array_unref(dp_albums);

    library_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 2: Re-scan unchanged library
 *
 * "I re-index without changing any files. The indexer should detect that
 *  mtimes haven't changed and skip all directories. Zero new files."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s2_lib[256], s2_data[256];

static void story2_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s2_lib, sizeof(s2_lib), "/tmp/quad_integ_%d_s2_lib", pid);
    snprintf(s2_data, sizeof(s2_data), "/tmp/quad_integ_%d_s2_data", pid);
    rm_rf(s2_lib); rm_rf(s2_data);
    mkdirs(s2_data);

    build_daft_punk_library(s2_lib);
}

static void story2_teardown(void) {
    rm_rf(s2_lib); rm_rf(s2_data);
}

Test(indexer, rescan_skips_unchanged,
     .init = story2_setup, .fini = story2_teardown, .timeout = 60) {

    /* First index */
    test_tracker_t t1 = {0};
    run_indexer(s2_lib, s2_data, &t1);
    cr_assert_eq(t1.last_progress.files_new, 5, "First scan: all 5 files new");

    /* Second index — unchanged */
    test_tracker_t t2 = {0};
    run_indexer(s2_lib, s2_data, &t2);
    cr_assert_eq(t2.last_progress.files_new, 0,
        "Re-scan of unchanged library should find 0 new files");
    cr_assert_eq(t2.last_progress.files_total, 0,
        "Re-scan should not process any files (all skipped by mtime)");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 3: Delete album folder → re-index → orphans pruned
 *
 * "I delete the Discovery folder from disk and re-index. The indexer's
 *  Phase 1 scan should detect the missing directory, prune the orphan
 *  album + tracks from the DB, and the library cache should only show RAM."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s3_lib[256], s3_data[256];

static void story3_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s3_lib, sizeof(s3_lib), "/tmp/quad_integ_%d_s3_lib", pid);
    snprintf(s3_data, sizeof(s3_data), "/tmp/quad_integ_%d_s3_data", pid);
    rm_rf(s3_lib); rm_rf(s3_data);
    mkdirs(s3_data);

    build_daft_punk_library(s3_lib);
    build_golden_features_library(s3_lib);
}

static void story3_teardown(void) {
    rm_rf(s3_lib); rm_rf(s3_data);
}

Test(indexer, delete_folder_prunes_orphans,
     .init = story3_setup, .fini = story3_teardown, .timeout = 60) {

    /* Index the full library (DP + GF) */
    test_tracker_t t1 = {0};
    run_indexer(s3_lib, s3_data, &t1);
    cr_assert_eq(t1.last_progress.files_new, 7, "First scan: 5 DP + 2 GF = 7 files");

    /* Verify initial state through cache */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s3_data);
    {
        library_cache_source_t src = {
            .db_path = db_path, .music_base = s3_lib,
            .display_name = "Test", .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 3, "Initial: 3 albums (Discovery, RAM, SECT)");
        g_ptr_array_unref(albums);

        GPtrArray *artists = library_cache_get_artists_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(artists->len, 2, "Initial: 2 artists (Daft Punk, Golden Features)");
        g_ptr_array_unref(artists);

        library_cache_destroy(cache);
    }

    /* ── Delete Discovery folder from disk ── */
    {
        char disc_path[1024];
        snprintf(disc_path, sizeof(disc_path), "%s/Daft Punk/Discovery", s3_lib);
        rm_rf(disc_path);
    }

    /* ── Re-index ── */
    test_tracker_t t2 = {0};
    run_indexer(s3_lib, s3_data, &t2);

    /* ── Verify through cache: Discovery gone, RAM + SECT remain ── */
    {
        library_cache_source_t src = {
            .db_path = db_path, .music_base = s3_lib,
            .display_name = "Test", .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 2, "After delete: 2 albums remain");
        cr_assert(test_has_album_title(albums, "Random Access Memories"),
            "RAM should survive");
        cr_assert(test_has_album_title(albums, "SECT"),
            "SECT should survive");
        cr_assert(!test_has_album_title(albums, "Discovery"),
            "Discovery should be pruned");
        g_ptr_array_unref(albums);

        /* Both artists survive (DP still has RAM, GF still has SECT) */
        GPtrArray *artists = library_cache_get_artists_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(artists->len, 2, "Both artists should survive");
        g_ptr_array_unref(artists);

        /* Search should NOT find deleted tracks */
        library_search_results_t *results = library_cache_search(
            cache, "One More Time", LIBRARY_SEARCH_FILTER_ALL, 0, NULL,
            LIBRARY_MASK_ALL);
        bool found = false;
        if (results && results->tracks) {
            for (guint i = 0; i < results->tracks->len; i++) {
                const library_track_info_t *t = g_ptr_array_index(results->tracks, i);
                if (strstr(t->title, "One More Time")) found = true;
            }
        }
        cr_assert(!found, "Deleted track 'One More Time' should not appear in search");
        library_search_results_free(results);

        /* Search should still find surviving tracks */
        results = library_cache_search(
            cache, "Lucky", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
        cr_assert_not_null(results);
        cr_assert(results->tracks && results->tracks->len > 0,
            "Surviving track 'Get Lucky' should appear in search");
        library_search_results_free(results);

        library_cache_destroy(cache);
    }

    /* ── Now delete the entire Golden Features folder ── */
    {
        char gf_path[1024];
        snprintf(gf_path, sizeof(gf_path), "%s/Golden Features", s3_lib);
        rm_rf(gf_path);
    }

    test_tracker_t t3 = {0};
    run_indexer(s3_lib, s3_data, &t3);

    /* Golden Features should now be orphaned (no albums, no track credits) */
    {
        library_cache_source_t src = {
            .db_path = db_path, .music_base = s3_lib,
            .display_name = "Test", .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *artists = library_cache_get_artists_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(artists->len, 1, "Golden Features should be pruned — only Daft Punk remains");
        cr_assert_str_eq(((library_artist_info_t *)g_ptr_array_index(artists, 0))->name,
            "Daft Punk");
        g_ptr_array_unref(artists);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 1, "Only RAM should remain");
        cr_assert(test_has_album_title(albums, "Random Access Memories"));
        g_ptr_array_unref(albums);

        library_cache_destroy(cache);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 4: Add new album to existing library
 *
 * "I copy a new album folder into my library and re-index. Only the new
 *  folder should be processed. The existing data is untouched."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s4_lib[256], s4_data[256];

static void story4_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s4_lib, sizeof(s4_lib), "/tmp/quad_integ_%d_s4_lib", pid);
    snprintf(s4_data, sizeof(s4_data), "/tmp/quad_integ_%d_s4_data", pid);
    rm_rf(s4_lib); rm_rf(s4_data);
    mkdirs(s4_data);

    build_daft_punk_library(s4_lib);
}

static void story4_teardown(void) {
    rm_rf(s4_lib); rm_rf(s4_data);
}

Test(indexer, add_new_album_incremental,
     .init = story4_setup, .fini = story4_teardown, .timeout = 60) {

    /* First index — Daft Punk only */
    test_tracker_t t1 = {0};
    run_indexer(s4_lib, s4_data, &t1);
    cr_assert_eq(t1.last_progress.files_new, 5);

    /* Add Golden Features */
    build_golden_features_library(s4_lib);

    /* Re-index — only GF should be new */
    test_tracker_t t2 = {0};
    run_indexer(s4_lib, s4_data, &t2);
    cr_assert_eq(t2.last_progress.files_new, 2,
        "Only 2 new Golden Features tracks should be detected");

    /* Verify total through cache */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s4_data);

    library_cache_source_t src = {
        .db_path = db_path, .music_base = s4_lib,
        .display_name = "Test", .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(albums->len, 3, "Should now have 3 albums");
    cr_assert(test_has_album_title(albums, "SECT"), "New album should appear");
    g_ptr_array_unref(albums);

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(artists->len, 2, "Should have 2 artists");
    g_ptr_array_unref(artists);

    library_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 5: Cancel mid-scan
 *
 * "I start indexing and immediately cancel. The indexer should stop cleanly
 *  with a completion callback, no crashes."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s5_lib[256], s5_data[256];

static void story5_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s5_lib, sizeof(s5_lib), "/tmp/quad_integ_%d_s5_lib", pid);
    snprintf(s5_data, sizeof(s5_data), "/tmp/quad_integ_%d_s5_data", pid);
    rm_rf(s5_lib); rm_rf(s5_data);
    mkdirs(s5_data);

    build_daft_punk_library(s5_lib);
}

static void story5_teardown(void) {
    rm_rf(s5_lib); rm_rf(s5_data);
}

Test(indexer, cancel_stops_cleanly,
     .init = story5_setup, .fini = story5_teardown, .timeout = 60) {

    test_tracker_t tracker = {0};
    indexer_config_t config = {
        .thread_count    = 1,
        .process_artwork = false,
        .mb_resolve      = false,
        .callback        = tracker_callback,
        .user_data       = &tracker,
    };

    indexer_t *indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, s5_lib, s5_data), QUADRATURE_OK);

    /* Cancel immediately */
    indexer_cancel(indexer);
    indexer_wait(indexer);

    cr_assert_eq(tracker.completed, 1,
        "Should have exactly 1 completion callback (cancelled or finished)");

    indexer_destroy(indexer);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 6: Picard-tagged files preserve MB data through Phase 2
 *
 * "My ~/Music library has files tagged by Picard with full MusicBrainz data.
 *  After indexing (no MB resolution needed), the library should have
 *  release_group_id and artist MBID from the file tags."
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Real MBIDs from the user's library */
#define MBID_DAFT_PUNK_ARTIST  "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define MBID_RAM_RELEASE       "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define MBID_RAM_RELEASE_GROUP "aa997ea0-2936-40bd-884d-3af8a0e064dc"
#define MBID_BRONSON_ARTIST        "887b5b46-3f15-4475-b2bd-4d026c2b2031"
#define MBID_BRONSON_RELEASE       "5ed617d7-898f-4e05-82a1-bfc586a4b013"
#define MBID_BRONSON_RELEASE_GROUP "d95b8366-994d-448d-8689-422b20b6cabb"
#define MBID_ODESZA_ARTIST         "2e222fce-02ae-4221-b1c6-3c3242b423b6"

static void index_no_mb(const char *library_root, const char *data_root) {
    test_tracker_t tracker = {0};
    run_indexer(library_root, data_root, &tracker);
}

#define MASK_A (1u << 0)
#define MASK_B (1u << 1)

static void build_picard_tagged_library(const char *root) {
    char path[1024], fpath[1024];
    snprintf(path, sizeof(path),
        "%s/Daft Punk/Random Access Memories", root);
    mkdirs(path);

    struct { int num; const char *title; int dur; } tracks[] = {
        { 1, "Give Life Back to Music", 274 },
        { 2, "The Game of Love",        321 },
        { 3, "Get Lucky",               367 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac",
                 path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = {
            title_tag, "artist=Daft Punk",
            "album=Random Access Memories",
            "album_artist=Daft Punk", tracknum, "date=2013",
            "MUSICBRAINZ_ALBUMID=" MBID_RAM_RELEASE,
            "MUSICBRAINZ_RELEASEGROUPID=" MBID_RAM_RELEASE_GROUP,
            "MUSICBRAINZ_ARTISTID=" MBID_DAFT_PUNK_ARTIST,
            "MUSICBRAINZ_ALBUMARTISTID=" MBID_DAFT_PUNK_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

static char s6_lib[256], s6_data[256];

static void story6_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s6_lib,  sizeof(s6_lib),  "/tmp/quad_integ_%d_s6_lib", pid);
    snprintf(s6_data, sizeof(s6_data), "/tmp/quad_integ_%d_s6_data", pid);
    rm_rf(s6_lib); rm_rf(s6_data);
    mkdirs(s6_data);
    build_picard_tagged_library(s6_lib);
}

static void story6_teardown(void) {
    rm_rf(s6_lib); rm_rf(s6_data);
}

Test(indexer, picard_tags_preserve_release_group_id,
     .init = story6_setup, .fini = story6_teardown, .timeout = 60) {

    test_tracker_t tracker = {0};
    run_indexer(s6_lib, s6_data, &tracker);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s6_data);

    library_cache_source_t src = {
        .db_path = db_path, .music_base = s6_lib,
        .display_name = "Music", .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(albums->len, 1);
    const library_album_info_t *ram = g_ptr_array_index(albums, 0);
    cr_assert(ram->musicbrainz_release_group_id != NULL &&
              ram->musicbrainz_release_group_id[0] != '\0',
        "Phase 2 must read MUSICBRAINZ_RELEASEGROUPID from Picard tags");
    cr_assert_str_eq(ram->musicbrainz_release_group_id, MBID_RAM_RELEASE_GROUP);
    g_ptr_array_unref(albums);

    library_cache_destroy(cache);
}

Test(indexer, picard_tags_preserve_artist_mbid,
     .init = story6_setup, .fini = story6_teardown, .timeout = 60) {

    test_tracker_t tracker = {0};
    run_indexer(s6_lib, s6_data, &tracker);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s6_data);

    library_cache_source_t src = {
        .db_path = db_path, .music_base = s6_lib,
        .display_name = "Music", .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(artists->len, 1);
    const library_artist_info_t *dp = g_ptr_array_index(artists, 0);
    cr_assert(dp->musicbrainz_id != NULL && dp->musicbrainz_id[0] != '\0',
        "Phase 2 must read MUSICBRAINZ_ALBUMARTISTID from Picard tags");
    cr_assert_str_eq(dp->musicbrainz_id, MBID_DAFT_PUNK_ARTIST);
    g_ptr_array_unref(artists);

    library_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 7: Cross-library dedup limitations without MB resolution
 *
 * "I have the same album in two libraries. One is Picard-tagged, the other
 *  has basic tags. MB resolution is disabled. The dedup can't work for the
 *  untagged copy because it has no RGID or artist MBID."
 *
 * These tests document real limitations that can only be resolved by running
 * Phase 6 (MB resolution) on the untagged library.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_basic_tagged_ram(const char *root) {
    char path[1024], fpath[1024];
    snprintf(path, sizeof(path),
        "%s/Daft Punk/Random Access Memories (10th Anniversary Edition)", root);
    mkdirs(path);

    struct { int num; const char *title; int dur; } tracks[] = {
        { 1, "Give Life Back to Music", 274 },
        { 2, "The Game of Love",        321 },
        { 3, "Get Lucky",               367 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac",
                 path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = {
            title_tag, "artist=Daft Punk",
            "album=Random Access Memories (10th Anniversary Edition)",
            "album_artist=Daft Punk", tracknum, "date=2023", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

static void build_odesza_with_bronson_credits(const char *root) {
    char path[1024], fpath[1024];
    snprintf(path, sizeof(path),
        "%s/ODESZA/The Last Goodbye Tour Live", root);
    mkdirs(path);

    struct { int num; const char *title; const char *artist; int dur; } tracks[] = {
        { 1, "This Version Of You (Live)", "ODESZA",  186 },
        { 2, "Behind the Sun (Live)",      "ODESZA",  190 },
        { 3, "HEART ATTACK (Live)",        "Bronson", 194 },
        { 4, "VAULTS (Live)",              "Bronson", 200 },
        { 5, "Wide Awake (Live)",          "ODESZA",  202 },
    };
    for (int i = 0; i < 5; i++) {
        char tracknum[16], title_tag[256], artist_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac",
                 path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        snprintf(artist_tag, sizeof(artist_tag), "artist=%s", tracks[i].artist);
        const char *tags[] = {
            title_tag, artist_tag, "album_artist=ODESZA",
            "album=The Last Goodbye Tour Live",
            tracknum, "date=2024", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

static void build_bronson_picard_tagged(const char *root) {
    char path[1024], fpath[1024];
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON", root);
    mkdirs(path);

    struct { int num; const char *title; int dur; } tracks[] = {
        { 1, "FOUNDATION",  184 },
        { 2, "HEART ATTACK", 209 },
        { 3, "VAULTS",       244 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac",
                 path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = {
            title_tag, "artist=BRONSON", "album=BRONSON",
            "album_artist=BRONSON", tracknum, "date=2020",
            "MUSICBRAINZ_ALBUMID=" MBID_BRONSON_RELEASE,
            "MUSICBRAINZ_RELEASEGROUPID=" MBID_BRONSON_RELEASE_GROUP,
            "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST,
            "MUSICBRAINZ_ALBUMARTISTID=" MBID_BRONSON_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

/* --- Album dedup: untagged vs Picard-tagged --- */

static char s7a_lib_a[256], s7a_lib_b[256], s7a_data_a[256], s7a_data_b[256];

static void story7a_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s7a_lib_a,  sizeof(s7a_lib_a),  "/tmp/quad_integ_%d_s7a_la", pid);
    snprintf(s7a_lib_b,  sizeof(s7a_lib_b),  "/tmp/quad_integ_%d_s7a_lb", pid);
    snprintf(s7a_data_a, sizeof(s7a_data_a), "/tmp/quad_integ_%d_s7a_da", pid);
    snprintf(s7a_data_b, sizeof(s7a_data_b), "/tmp/quad_integ_%d_s7a_db", pid);
    rm_rf(s7a_lib_a); rm_rf(s7a_lib_b);
    rm_rf(s7a_data_a); rm_rf(s7a_data_b);
    mkdirs(s7a_data_a); mkdirs(s7a_data_b);

    build_basic_tagged_ram(s7a_lib_a);
    build_picard_tagged_library(s7a_lib_b);
    index_no_mb(s7a_lib_a, s7a_data_a);
    index_no_mb(s7a_lib_b, s7a_data_b);
}

static void story7a_teardown(void) {
    rm_rf(s7a_lib_a); rm_rf(s7a_lib_b);
    rm_rf(s7a_data_a); rm_rf(s7a_data_b);
}

/**
 * Untagged album (no RGID) cannot dedup against Picard-tagged album (has RGID).
 * Requires Phase 6 MB resolution on the untagged library to fetch the RGID.
 */
Test(indexer, untagged_album_not_deduped_without_mb_resolution,
     .init = story7a_setup, .fini = story7a_teardown, .timeout = 60) {

    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", s7a_data_a);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", s7a_data_b);

    library_cache_source_t sources[2] = {
        { .db_path = db_a, .music_base = s7a_lib_a,
          .display_name = "Elicb Music", .bitmap_index = 0 },
        { .db_path = db_b, .music_base = s7a_lib_b,
          .display_name = "Music", .bitmap_index = 1 },
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);

    GPtrArray *all = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    int ram_count = test_count_album_title_prefix(all, "Random Access Memories");
    cr_assert_eq(ram_count, 1,
        "Same album shows as %d copies in ALL view. Expected 1. "
        "Untagged copy has no RGID — requires Phase 6 MB resolution.", ram_count);
    g_ptr_array_unref(all);

    library_cache_destroy(cache);
}

/* --- Artist merge: track credit without MBID --- */

static char s7b_lib_a[256], s7b_lib_b[256], s7b_data_a[256], s7b_data_b[256];

static void story7b_setup(void) {
    if (!ffmpeg_available()) cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s7b_lib_a,  sizeof(s7b_lib_a),  "/tmp/quad_integ_%d_s7b_la", pid);
    snprintf(s7b_lib_b,  sizeof(s7b_lib_b),  "/tmp/quad_integ_%d_s7b_lb", pid);
    snprintf(s7b_data_a, sizeof(s7b_data_a), "/tmp/quad_integ_%d_s7b_da", pid);
    snprintf(s7b_data_b, sizeof(s7b_data_b), "/tmp/quad_integ_%d_s7b_db", pid);
    rm_rf(s7b_lib_a); rm_rf(s7b_lib_b);
    rm_rf(s7b_data_a); rm_rf(s7b_data_b);
    mkdirs(s7b_data_a); mkdirs(s7b_data_b);

    build_odesza_with_bronson_credits(s7b_lib_a);
    build_bronson_picard_tagged(s7b_lib_b);
    index_no_mb(s7b_lib_a, s7b_data_a);
    index_no_mb(s7b_lib_b, s7b_data_b);
}

static void story7b_teardown(void) {
    rm_rf(s7b_lib_a); rm_rf(s7b_lib_b);
    rm_rf(s7b_data_a); rm_rf(s7b_data_b);
}

/**
 * "Bronson" (untagged track credit in lib A) and "BRONSON" (Picard-tagged
 * album artist in lib B, has MBID) cannot merge because lib A's artist has
 * no MBID. Requires Phase 6 MB resolution on lib A.
 */
Test(indexer, untagged_track_credit_not_merged_without_mb_resolution,
     .init = story7b_setup, .fini = story7b_teardown, .timeout = 60) {

    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", s7b_data_a);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", s7b_data_b);

    library_cache_source_t sources[2] = {
        { .db_path = db_a, .music_base = s7b_lib_a,
          .display_name = "Elicb Music", .bitmap_index = 0 },
        { .db_path = db_b, .music_base = s7b_lib_b,
          .display_name = "Music", .bitmap_index = 1 },
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);

    /* Artist merge */
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    int bronson_count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, "BRONSON") == 0)
            bronson_count++;
    }
    cr_assert_eq(bronson_count, 1,
        "BRONSON appears %d times in ALL view. Expected 1. "
        "'Bronson' (untagged track credit, no MBID) requires Phase 6 "
        "MB resolution to get its MBID for merge.", bronson_count);
    g_ptr_array_unref(artists);

    /* Appearances */
    int64_t bronson_b_id = test_find_artist_id_in_library(cache, "BRONSON", MASK_B);
    cr_assert(bronson_b_id > 0);

    GPtrArray *appearances = library_cache_get_artist_appearance_tracks(
        cache, bronson_b_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(appearances,
        "BRONSON appearance tracks is NULL. Expected >= 2 from ODESZA album. "
        "Untagged 'Bronson' track credit requires Phase 6 for MBID.");
    cr_assert(appearances->len >= 2,
        "BRONSON has %u appearance tracks, expected >= 2. "
        "Requires Phase 6 to resolve 'Bronson' to its MBID.", appearances->len);
    g_ptr_array_unref(appearances);

    /* Search */
    library_search_results_t *results = library_cache_search(
        cache, "Bronson", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    int search_count = 0;
    if (results && results->artists) {
        for (guint i = 0; i < results->artists->len; i++) {
            const library_artist_info_t *a = g_ptr_array_index(results->artists, i);
            if (g_ascii_strcasecmp(a->name, "BRONSON") == 0)
                search_count++;
        }
    }
    cr_assert_eq(search_count, 1,
        "Search 'Bronson' returns %d artists. Expected 1. "
        "Unmerged 'Bronson'/'BRONSON' requires Phase 6.", search_count);
    library_search_results_free(results);

    library_cache_destroy(cache);
}
