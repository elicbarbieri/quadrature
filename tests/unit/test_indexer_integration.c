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
#include "mb_test_env.h"
#include "quadrature/indexer.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libavformat/avformat.h>

ReportHook(PRE_ALL)(struct criterion_test_set *tests)
{
    (void)tests;
    avformat_network_init();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FLAC file generation (same approach as e2e tests)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
mkdirs(const char *path)
{
    cr_assert_eq(g_mkdir_with_parents(path, 0755), 0, "g_mkdir_with_parents failed for %s", path);
}

static void
rm_rf(const char *path)
{
    g_autofree char *cmd = g_strdup_printf("rm -rf '%s'", path);
    cr_assert_eq(system(cmd), 0, "rm -rf failed for %s", path);
}

/**
 * Advance a FILE's mtime to the future so Phase 1 sees the album as dirty.
 *
 * Phase 1's delta detection uses max(file.mtime) + sum(file.size) across
 * the album directory (src/indexer/indexer.c:627,722) — the directory's
 * own mtime is ignored. So to simulate "user touched a file" or to guard
 * against sub-second mtime granularity after an ffmpeg rewrite, this
 * helper must target a FILE path, not the containing directory.
 */
static void
bump_mtime_future(const char *path)
{
    struct stat st;
    cr_assert_eq(stat(path, &st), 0, "stat(%s) failed", path);
    struct timeval times[2] = {
        { .tv_sec = st.st_atime, .tv_usec = 0 },
        { .tv_sec = st.st_mtime + 3600, .tv_usec = 0 },
    };
    cr_assert_eq(utimes(path, times), 0, "utimes(%s) failed", path);
}

static bool
ffmpeg_available(void)
{
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

static void
tracker_callback(indexer_event_t event,
                 const indexer_progress_t *progress,
                 const library_cache_changeset_t *changeset,
                 void *user_data)
{
    (void)changeset;
    test_tracker_t *t = user_data;
    switch (event) {
    case INDEXER_STARTED:
        t->started++;
        break;
    case INDEXER_PROGRESS:
        t->progress++;
        if (progress)
            t->last_progress = *progress;
        break;
    case INDEXER_COMPLETED:
        t->completed++;
        t->success = true;
        if (progress)
            t->last_progress = *progress;
        break;
    case INDEXER_LIBRARY_UPDATED:
        t->library_updated++;
        break;
    case INDEXER_CANCELLED:
        t->completed++;
        break;
    case INDEXER_ERROR:
        t->completed++;
        break;
    case INDEXER_ARTWORK_UPDATED:
        break;
    }
}

/**
 * Build libpq conninfo for the self-hosted MusicBrainz PG from env vars.
 * Returns NULL (caller should cr_skip) when MB_PG_PASSWORD is unset.
 * Mirrors the pattern in test_mb_resolve.c.
 */
static const char *
test_mb_pg_conninfo(void)
{
    /* HTTP test mode: NULL conninfo → resolver dispatches to HTTP backend.
     * Tests using test_config_mb() then run end-to-end against public APIs. */
    if (quad_test_use_http())
        return NULL;
    const char *pw = getenv("MB_PG_PASSWORD");
    if (!pw || !pw[0])
        return NULL;
    static char buf[512];
    const char *host = getenv("MB_HOST");
    const char *dbname = getenv("MB_DBNAME");
    const char *user = getenv("MB_USER");
    const char *timeout = getenv("MB_PG_CONNECT_TIMEOUT");
    snprintf(buf,
             sizeof(buf),
             "host=%s dbname=%s user=%s password=%s connect_timeout=%s",
             (host && host[0]) ? host : "localhost",
             (dbname && dbname[0]) ? dbname : "musicbrainz_db",
             (user && user[0]) ? user : "musicbrainz",
             pw,
             (timeout && timeout[0]) ? timeout : "5");
    return buf;
}

/**
 * Default config: Phase 1+2 only, no MB, no artwork, no fingerprinting.
 * Used by stories that test core scan/upsert behavior in isolation.
 */
static indexer_config_t
test_config_basic(test_tracker_t *tracker)
{
    return (indexer_config_t){
        .thread_count = 2,
        .process_artwork = false,
        .mb_resolve = false,
        .callback = tracker_callback,
        .user_data = tracker,
    };
}

/**
 * MB-enabled config for Phase 6 tests. Caller is responsible for passing
 * pg_conninfo from test_mb_pg_conninfo() and cr_skip'ing when it's NULL.
 */
static indexer_config_t
test_config_mb(test_tracker_t *tracker, const char *pg_conninfo)
{
    indexer_config_t c = test_config_basic(tracker);
    c.mb_resolve = true;
    c.pg_conninfo = pg_conninfo; /* NULL in HTTP mode → routes to HTTP backend */
    return c;
}

static void
run_indexer_cfg(const char *library_root,
                const char *data_root,
                test_tracker_t *tracker,
                const indexer_config_t *config)
{
    indexer_t *indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, library_root, data_root), QUADRATURE_OK);
    indexer_wait(indexer);

    cr_assert(tracker->completed > 0, "Indexer did not complete for %s", library_root);
    cr_assert(tracker->success, "Indexer failed for %s", library_root);

    indexer_destroy(indexer);
}

static void
run_indexer(const char *library_root, const char *data_root, test_tracker_t *tracker)
{
    indexer_config_t c = test_config_basic(tracker);
    run_indexer_cfg(library_root, data_root, tracker, &c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library builders — create realistic FLAC files with tags
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Build a Daft Punk library:
 *   DaftPunk/Discovery/  — 3 tracks (One More Time, Aerodynamic, Digital Love)
 *   DaftPunk/RAM/        — 2 tracks (Get Lucky, Lose Yourself to Dance)
 */
static void
build_daft_punk_library(const char *root)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/Daft Punk/Discovery", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } disc_tracks[] = {
        { 1, "One More Time", 321 },
        { 2, "Aerodynamic", 212 },
        { 3, "Digital Love", 301 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, disc_tracks[i].num, disc_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", disc_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", disc_tracks[i].title);
        const char *tags[] = { title_tag,
                               "artist=Daft Punk",
                               "album=Discovery",
                               "album_artist=Daft Punk",
                               tracknum,
                               "date=2001",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, disc_tracks[i].dur), 0);
    }

    snprintf(path, sizeof(path), "%s/Daft Punk/Random Access Memories", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } ram_tracks[] = {
        { 1, "Get Lucky", 367 },
        { 2, "Lose Yourself to Dance", 353 },
    };
    for (int i = 0; i < 2; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, ram_tracks[i].num, ram_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", ram_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", ram_tracks[i].title);
        const char *tags[] = { title_tag,
                               "artist=Daft Punk",
                               "album=Random Access Memories",
                               "album_artist=Daft Punk",
                               tracknum,
                               "date=2013",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, ram_tracks[i].dur), 0);
    }
}

/**
 * Build a Golden Features album:
 *   GoldenFeatures/SECT/ — 2 tracks
 */
static void
build_golden_features_library(const char *root)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/Golden Features/SECT", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } tracks[] = {
        { 1, "Ariana", 198 },
        { 2, "Falling", 224 },
    };
    for (int i = 0; i < 2; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = { title_tag,    "artist=Golden Features",
                               "album=SECT", "album_artist=Golden Features",
                               tracknum,     "date=2021",
                               NULL };
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

static void
story1_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s1_lib, sizeof(s1_lib), "/tmp/quad_integ_%d_s1_lib", pid);
    snprintf(s1_data, sizeof(s1_data), "/tmp/quad_integ_%d_s1_data", pid);
    rm_rf(s1_lib);
    rm_rf(s1_data);
    mkdirs(s1_data);

    build_daft_punk_library(s1_lib);
}

static void
story1_teardown(void)
{
    rm_rf(s1_lib);
    rm_rf(s1_data);
}

Test(indexer, fresh_library_scan, .init = story1_setup, .fini = story1_teardown, .timeout = 60)
{
    /* ── Index ── */
    test_tracker_t tracker = { 0 };
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
    cr_assert_eq(db_open(db_path, false, &db), QUADRATURE_OK);

    size_t track_count = 0;
    cr_assert_eq(db_get_entity_count(db, DB_ENTITY_TRACK, &track_count), QUADRATURE_OK);
    cr_assert_eq(track_count, 5, "Should have 5 tracks");
    db_close(db);

    /* ── Verify through library_cache ── */
    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = s1_lib,
        .display_name = "Test Library",
        .bitmap_index = 0,
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
    GPtrArray *disc_tracks
        = library_cache_get_tracks_by_album(cache, disc->album_id, LIBRARY_MASK_ALL);
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
        if (strstr(t->title, "Lucky"))
            found_lucky = true;
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
    GPtrArray *dp_albums = library_cache_get_albums_by_artist(cache, dp_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(dp_albums);
    cr_assert_eq(dp_albums->len, 2, "Daft Punk should have 2 albums");
    g_ptr_array_unref(dp_albums);

    library_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 2: Re-scan with mtime delta detection
 *
 * "I re-index without changing files — the indexer must skip every directory.
 *  Then I bump Discovery's mtime (e.g. `touch` from a sync tool) — the indexer
 *  must re-process Discovery but skip RAM. No tracks are newly inserted, and
 *  the library cache remains consistent."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s2_lib[256], s2_data[256];

static void
story2_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s2_lib, sizeof(s2_lib), "/tmp/quad_integ_%d_s2_lib", pid);
    snprintf(s2_data, sizeof(s2_data), "/tmp/quad_integ_%d_s2_data", pid);
    rm_rf(s2_lib);
    rm_rf(s2_data);
    mkdirs(s2_data);

    build_daft_punk_library(s2_lib);
}

static void
story2_teardown(void)
{
    rm_rf(s2_lib);
    rm_rf(s2_data);
}

Test(indexer, rescan_skips_unchanged, .init = story2_setup, .fini = story2_teardown, .timeout = 60)
{
    /* ── First index ── */
    test_tracker_t t1 = { 0 };
    run_indexer(s2_lib, s2_data, &t1);
    cr_assert_eq(t1.last_progress.files_new, 5, "First scan: all 5 files new");

    /* ── Second index (unchanged) — mtime skip ── */
    test_tracker_t t2 = { 0 };
    run_indexer(s2_lib, s2_data, &t2);
    cr_assert_eq(
        t2.last_progress.files_new, 0, "Re-scan of unchanged library should find 0 new files");
    cr_assert_eq(t2.last_progress.files_total,
                 0,
                 "Re-scan should not queue any files (all skipped by mtime)");

    /* ── Third index after touching a file's mtime inside Discovery ──
     *
     * Models a backup/sync tool bumping a single file's timestamp (or any
     * workflow that updates mtime without touching content). Phase 1
     * computes max(file.mtime) per album (not dir mtime), so bumping ONE
     * file's mtime is enough to invalidate Discovery's cached fingerprint
     * and requeue the whole album. Phase 2 upserts existing rows (files_new
     * stays at 0); RAM's fingerprint is unchanged, so it stays in the
     * files_unchanged bucket. */
    char disc_track1[1280];
    snprintf(
        disc_track1, sizeof(disc_track1), "%s/Daft Punk/Discovery/01 - One More Time.flac", s2_lib);
    bump_mtime_future(disc_track1);

    test_tracker_t t3 = { 0 };
    run_indexer(s2_lib, s2_data, &t3);

    /* Note: `files_new` is incremented per successful upsert (not per
     * INSERT), so we cannot use it to distinguish edit from initial-scan.
     * The meaningful signals are files_total (queued by Phase 1) and
     * files_unchanged (bulk-skipped albums). */
    cr_assert_eq(t3.last_progress.files_total,
                 3,
                 "Phase 1 should queue only Discovery's 3 files, not RAM's 2");
    cr_assert_eq(
        t3.last_progress.files_processed, 3, "Phase 2 should process the 3 re-queued files");
    cr_assert_eq(t3.last_progress.files_unchanged,
                 2,
                 "RAM's 2 files should be bulk-skipped via mtime+size match");

    /* ── Verify library cache remains consistent after the touch rescan ──
     *
     * The touch-and-rescan cycle must not duplicate tracks, lose titles,
     * or alter the album shape. This is the end-to-end guarantee that
     * Phase 2's upsert path is idempotent for unchanged content. */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s2_data);

    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = s2_lib,
        .display_name = "Test",
        .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(albums->len, 2, "Touch rescan must not duplicate albums");

    const library_album_info_t *disc = test_find_album(albums, "Discovery");
    cr_assert_not_null(disc, "Discovery must still exist after touch rescan");

    GPtrArray *disc_tracks
        = library_cache_get_tracks_by_album(cache, disc->album_id, LIBRARY_MASK_ALL);
    cr_assert_eq(disc_tracks->len, 3, "Discovery must still have exactly 3 tracks");
    cr_assert_str_eq(((const library_track_info_t *)g_ptr_array_index(disc_tracks, 0))->title,
                     "One More Time");
    g_ptr_array_unref(disc_tracks);
    g_ptr_array_unref(albums);

    library_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 3: Delete album folder → re-index → orphans pruned
 *
 * "I delete the Discovery folder from disk and re-index. The indexer's
 *  Phase 1 scan should detect the missing directory, prune the orphan
 *  album + tracks from the DB, and the library cache should only show RAM."
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s3_lib[256], s3_data[256];

static void
story3_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s3_lib, sizeof(s3_lib), "/tmp/quad_integ_%d_s3_lib", pid);
    snprintf(s3_data, sizeof(s3_data), "/tmp/quad_integ_%d_s3_data", pid);
    rm_rf(s3_lib);
    rm_rf(s3_data);
    mkdirs(s3_data);

    build_daft_punk_library(s3_lib);
    build_golden_features_library(s3_lib);
}

static void
story3_teardown(void)
{
    rm_rf(s3_lib);
    rm_rf(s3_data);
}

Test(indexer,
     delete_folder_prunes_orphans,
     .init = story3_setup,
     .fini = story3_teardown,
     .timeout = 60)
{
    /* Index the full library (DP + GF) */
    test_tracker_t t1 = { 0 };
    run_indexer(s3_lib, s3_data, &t1);
    cr_assert_eq(t1.last_progress.files_new, 7, "First scan: 5 DP + 2 GF = 7 files");

    /* Verify initial state through cache */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s3_data);
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s3_lib,
            .display_name = "Test",
            .bitmap_index = 0,
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
    test_tracker_t t2 = { 0 };
    run_indexer(s3_lib, s3_data, &t2);

    /* ── Verify through cache: Discovery gone, RAM + SECT remain ── */
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s3_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 2, "After delete: 2 albums remain");
        cr_assert(test_has_album_title(albums, "Random Access Memories"), "RAM should survive");
        cr_assert(test_has_album_title(albums, "SECT"), "SECT should survive");
        cr_assert(!test_has_album_title(albums, "Discovery"), "Discovery should be pruned");
        g_ptr_array_unref(albums);

        /* Both artists survive (DP still has RAM, GF still has SECT) */
        GPtrArray *artists = library_cache_get_artists_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(artists->len, 2, "Both artists should survive");
        g_ptr_array_unref(artists);

        /* Search should NOT find deleted tracks */
        library_search_results_t *results = library_cache_search(
            cache, "One More Time", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
        bool found = false;
        if (results && results->tracks) {
            for (guint i = 0; i < results->tracks->len; i++) {
                const library_track_info_t *t = g_ptr_array_index(results->tracks, i);
                if (strstr(t->title, "One More Time"))
                    found = true;
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

    test_tracker_t t3 = { 0 };
    run_indexer(s3_lib, s3_data, &t3);

    /* Golden Features should now be orphaned (no albums, no track credits) */
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s3_lib,
            .display_name = "Test",
            .bitmap_index = 0,
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

static void
story4_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s4_lib, sizeof(s4_lib), "/tmp/quad_integ_%d_s4_lib", pid);
    snprintf(s4_data, sizeof(s4_data), "/tmp/quad_integ_%d_s4_data", pid);
    rm_rf(s4_lib);
    rm_rf(s4_data);
    mkdirs(s4_data);

    build_daft_punk_library(s4_lib);
}

static void
story4_teardown(void)
{
    rm_rf(s4_lib);
    rm_rf(s4_data);
}

Test(indexer,
     add_new_album_incremental,
     .init = story4_setup,
     .fini = story4_teardown,
     .timeout = 60)
{
    /* First index — Daft Punk only */
    test_tracker_t t1 = { 0 };
    run_indexer(s4_lib, s4_data, &t1);
    cr_assert_eq(t1.last_progress.files_new, 5);

    /* Add Golden Features */
    build_golden_features_library(s4_lib);

    /* Re-index — only GF should be new */
    test_tracker_t t2 = { 0 };
    run_indexer(s4_lib, s4_data, &t2);
    cr_assert_eq(
        t2.last_progress.files_new, 2, "Only 2 new Golden Features tracks should be detected");

    /* Verify total through cache */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s4_data);

    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = s4_lib,
        .display_name = "Test",
        .bitmap_index = 0,
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

static void
story5_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s5_lib, sizeof(s5_lib), "/tmp/quad_integ_%d_s5_lib", pid);
    snprintf(s5_data, sizeof(s5_data), "/tmp/quad_integ_%d_s5_data", pid);
    rm_rf(s5_lib);
    rm_rf(s5_data);
    mkdirs(s5_data);

    build_daft_punk_library(s5_lib);
}

static void
story5_teardown(void)
{
    rm_rf(s5_lib);
    rm_rf(s5_data);
}

Test(indexer, cancel_stops_cleanly, .init = story5_setup, .fini = story5_teardown, .timeout = 60)
{
    test_tracker_t tracker = { 0 };
    indexer_config_t config = {
        .thread_count = 1,
        .process_artwork = false,
        .mb_resolve = false,
        .callback = tracker_callback,
        .user_data = &tracker,
    };

    indexer_t *indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, s5_lib, s5_data), QUADRATURE_OK);

    /* Cancel immediately */
    indexer_cancel(indexer);
    indexer_wait(indexer);

    cr_assert_eq(
        tracker.completed, 1, "Should have exactly 1 completion callback (cancelled or finished)");

    indexer_destroy(indexer);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Re-scan idempotency for every album shape
 *
 * Re-scanning an unchanged library must be a no-op. That means: every album
 * the first scan created must still exist (same album_id) after the second
 * scan, and Phase 1 must queue zero work.
 *
 * Phase 1 enforces this via mark-and-sweep on the album_mtimes hashtable:
 * each branch that recognises an album as "still on disk" removes its entry
 * from album_mtimes. Whatever remains at end of scan is treated as orphaned
 * and db_delete_album'd. A branch that LOOKS at the entry without removing
 * it will cause its album to be deleted-and-recreated every scan — losing
 * album_id and everything keyed on it (MB resolution state, ratings, etc.).
 *
 * This is the exact bug that existed for sibling-style multi-disc albums
 * before this test was added. The fixture covers all three album shapes
 * (single-disc, child-style multi-disc, sibling-style multi-disc) so any
 * future regression in any branch is caught.
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s_idem_lib[256], s_idem_data[256];

/**
 * Build a library with one of every album shape Phase 1 handles:
 *
 *   Single/Single Album/           — single-disc
 *      01 - Track.flac
 *      02 - Track.flac
 *   Child/Child Album/             — child-style multi-disc (subdirs)
 *      CD 1/01 - Track.flac
 *      CD 2/01 - Track.flac
 *   Sibling/Sibling Album (CD 1)/  — sibling-style multi-disc (peers)
 *      01 - Track.flac
 *   Sibling/Sibling Album (CD 2)/
 *      01 - Track.flac
 *
 * Synthetic path for the sibling-style album is `<root>/Sibling/Sibling Album`.
 */
static void
build_all_shapes_library(const char *root)
{
    char dir[1024];
    g_autofree char *fpath = NULL;

    /* Single-disc */
    snprintf(dir, sizeof(dir), "%s/Single/Single Album", root);
    mkdirs(dir);
    for (int i = 1; i <= 2; i++) {
        char tnum[16], ttag[64];
        snprintf(tnum, sizeof(tnum), "track=%d", i);
        snprintf(ttag, sizeof(ttag), "title=Single Track %d", i);
        fpath = g_strdup_printf("%s/%02d - Single Track %d.flac", dir, i, i);
        const char *tags[] = { ttag,
                               "artist=Single Artist",
                               "album=Single Album",
                               "album_artist=Single Artist",
                               tnum,
                               "date=2020",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, 30), 0);
        g_clear_pointer(&fpath, g_free);
    }

    /* Child-style multi-disc: subdirs CD 1 / CD 2 */
    for (int disc = 1; disc <= 2; disc++) {
        snprintf(dir, sizeof(dir), "%s/Child/Child Album/CD %d", root, disc);
        mkdirs(dir);
        char tnum[16], ttag[64], dtag[16];
        snprintf(tnum, sizeof(tnum), "track=1");
        snprintf(dtag, sizeof(dtag), "discnumber=%d", disc);
        snprintf(ttag, sizeof(ttag), "title=Child Disc %d Track", disc);
        fpath = g_strdup_printf("%s/01 - Child Disc %d Track.flac", dir, disc);
        const char *tags[] = { ttag,
                               "artist=Child Artist",
                               "album=Child Album",
                               "album_artist=Child Artist",
                               tnum,
                               dtag,
                               "date=2020",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, 30), 0);
        g_clear_pointer(&fpath, g_free);
    }

    /* Sibling-style multi-disc, unparenthesised form: "Sibling Album Disc N" */
    for (int disc = 1; disc <= 2; disc++) {
        snprintf(dir, sizeof(dir), "%s/Sibling/Sibling Album Disc %d", root, disc);
        mkdirs(dir);
        char tnum[16], ttag[64], dtag[16];
        snprintf(tnum, sizeof(tnum), "track=1");
        snprintf(dtag, sizeof(dtag), "discnumber=%d", disc);
        snprintf(ttag, sizeof(ttag), "title=Sibling Disc %d Track", disc);
        fpath = g_strdup_printf("%s/01 - Sibling Disc %d Track.flac", dir, disc);
        const char *tags[] = { ttag,
                               "artist=Sibling Artist",
                               "album=Sibling Album",
                               "album_artist=Sibling Artist",
                               tnum,
                               dtag,
                               "date=2020",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, 30), 0);
        g_clear_pointer(&fpath, g_free);
    }

    /* Sibling-style multi-disc, parenthesised form: "Paren Album (Disc N)".
     * Covers the common Picard/MusicBrainz naming convention for multi-disc
     * releases that look like sibling peer dirs at the filesystem level. */
    for (int disc = 1; disc <= 2; disc++) {
        snprintf(dir, sizeof(dir), "%s/Paren/Paren Album (Disc %d)", root, disc);
        mkdirs(dir);
        char tnum[16], ttag[64], dtag[16];
        snprintf(tnum, sizeof(tnum), "track=1");
        snprintf(dtag, sizeof(dtag), "discnumber=%d", disc);
        snprintf(ttag, sizeof(ttag), "title=Paren Disc %d Track", disc);
        fpath = g_strdup_printf("%s/01 - Paren Disc %d Track.flac", dir, disc);
        const char *tags[] = { ttag,
                               "artist=Paren Artist",
                               "album=Paren Album",
                               "album_artist=Paren Artist",
                               tnum,
                               dtag,
                               "date=2020",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, 30), 0);
        g_clear_pointer(&fpath, g_free);
    }
}

static void
story_idem_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s_idem_lib, sizeof(s_idem_lib), "/tmp/quad_integ_%d_idem_lib", pid);
    snprintf(s_idem_data, sizeof(s_idem_data), "/tmp/quad_integ_%d_idem_data", pid);
    rm_rf(s_idem_lib);
    rm_rf(s_idem_data);
    mkdirs(s_idem_data);
    build_all_shapes_library(s_idem_lib);
}

static void
story_idem_teardown(void)
{
    rm_rf(s_idem_lib);
    rm_rf(s_idem_data);
}

Test(indexer,
     rescan_idempotent_all_album_shapes,
     .init = story_idem_setup,
     .fini = story_idem_teardown,
     .timeout = 60)
{
    /* ── First scan: populate from scratch ── */
    test_tracker_t t1 = { 0 };
    run_indexer(s_idem_lib, s_idem_data, &t1);
    cr_assert_eq(t1.last_progress.files_new,
                 8,
                 "First scan should index all 8 files (2 single + 2 child + 2 sibling + 2 paren)");

    /* Snapshot the four album_ids through the library cache. */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s_idem_data);

    int64_t single_id_1 = 0, child_id_1 = 0, sibling_id_1 = 0, paren_id_1 = 0;
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s_idem_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 4, "First scan: 4 albums (one of each shape)");

        const library_album_info_t *s = test_find_album(albums, "Single Album");
        const library_album_info_t *c = test_find_album(albums, "Child Album");
        const library_album_info_t *b = test_find_album(albums, "Sibling Album");
        const library_album_info_t *p = test_find_album(albums, "Paren Album");
        cr_assert_not_null(s, "Single-disc album missing after first scan");
        cr_assert_not_null(c, "Child-style multi-disc album missing after first scan");
        cr_assert_not_null(b, "Sibling-style (unparenthesised) album missing after first scan");
        cr_assert_not_null(p,
                           "Sibling-style (parenthesised) album missing after first scan "
                           "— try_strip_disc_suffix must match \"Album (Disc N)\"");
        single_id_1 = s->album_id;
        child_id_1 = c->album_id;
        sibling_id_1 = b->album_id;
        paren_id_1 = p->album_id;

        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }

    /* ── Second scan with no changes: must be a true no-op ── */
    test_tracker_t t2 = { 0 };
    run_indexer(s_idem_lib, s_idem_data, &t2);
    cr_assert_eq(t2.last_progress.files_total,
                 0,
                 "Re-scan must queue zero files — every album shape must mark itself "
                 "as seen in album_mtimes so the orphan sweep doesn't prune it");
    cr_assert_eq(t2.last_progress.files_new, 0, "Re-scan must not re-index any files");

    /* ── Album IDs must be stable across scans ──
     *
     * An album that gets erroneously orphan-deleted will be re-created on
     * the next scan with a fresh album_id. Anything keyed on the old id
     * (MB resolution state, play counts, ratings, playlist refs) is lost.
     * Stable IDs are the load-bearing invariant. */
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s_idem_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 4, "Re-scan must not change album count");

        const library_album_info_t *s = test_find_album(albums, "Single Album");
        const library_album_info_t *c = test_find_album(albums, "Child Album");
        const library_album_info_t *b = test_find_album(albums, "Sibling Album");
        const library_album_info_t *p = test_find_album(albums, "Paren Album");
        cr_assert_not_null(s, "Single-disc album missing after re-scan");
        cr_assert_not_null(c, "Child-style multi-disc album missing after re-scan");
        cr_assert_not_null(b,
                           "Sibling-style album missing after re-scan "
                           "— almost certainly orphan-deleted by the end-of-scan sweep");
        cr_assert_not_null(p, "Paren-form sibling album missing after re-scan");

        cr_assert_eq(s->album_id,
                     single_id_1,
                     "Single-disc album_id changed (%" G_GINT64_FORMAT " → %" G_GINT64_FORMAT
                     ") — Phase 1 deleted and recreated it",
                     single_id_1,
                     s->album_id);
        cr_assert_eq(c->album_id,
                     child_id_1,
                     "Child-style multi-disc album_id changed (%" G_GINT64_FORMAT
                     " → %" G_GINT64_FORMAT ") — Phase 1 deleted and recreated it",
                     child_id_1,
                     c->album_id);
        cr_assert_eq(b->album_id,
                     sibling_id_1,
                     "Sibling-style multi-disc album_id changed (%" G_GINT64_FORMAT
                     " → %" G_GINT64_FORMAT
                     ") — Phase 1 deleted and recreated it. The sibling branch of "
                     "scan_directory_recursive must g_hash_table_remove the synthetic "
                     "path from album_mtimes after lookup.",
                     sibling_id_1,
                     b->album_id);
        cr_assert_eq(p->album_id,
                     paren_id_1,
                     "Paren-form sibling album_id changed (%" G_GINT64_FORMAT " → %" G_GINT64_FORMAT
                     ") — Phase 1 deleted and recreated it",
                     paren_id_1,
                     p->album_id);

        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 6: Picard-tagged files preserve MB data through Phase 2
 *
 * "My ~/Music library has files tagged by Picard with full MusicBrainz data.
 *  After indexing (no MB resolution needed), the library should have
 *  release_group_id and artist MBID from the file tags."
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Real MBIDs from the user's library */
#define MBID_DAFT_PUNK_ARTIST      "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define MBID_RAM_RELEASE           "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define MBID_RAM_RELEASE_GROUP     "aa997ea0-2936-40bd-884d-3af8a0e064dc"
#define MBID_BRONSON_ARTIST        "887b5b46-3f15-4475-b2bd-4d026c2b2031"
#define MBID_BRONSON_RELEASE       "5ed617d7-898f-4e05-82a1-bfc586a4b013"
#define MBID_BRONSON_RELEASE_GROUP "d95b8366-994d-448d-8689-422b20b6cabb"
#define MBID_ODESZA_ARTIST         "2e222fce-02ae-4221-b1c6-3c3242b423b6"

static void
index_no_mb(const char *library_root, const char *data_root)
{
    test_tracker_t tracker = { 0 };
    run_indexer(library_root, data_root, &tracker);
}

#define MASK_A (1u << 0)
#define MASK_B (1u << 1)

static void
build_picard_tagged_library(const char *root)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/Daft Punk/Random Access Memories", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } tracks[] = {
        { 1, "Give Life Back to Music", 274 },
        { 2, "The Game of Love", 321 },
        { 3, "Get Lucky", 367 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = { title_tag,
                               "artist=Daft Punk",
                               "album=Random Access Memories",
                               "album_artist=Daft Punk",
                               tracknum,
                               "date=2013",
                               "MUSICBRAINZ_ALBUMID=" MBID_RAM_RELEASE,
                               "MUSICBRAINZ_RELEASEGROUPID=" MBID_RAM_RELEASE_GROUP,
                               "MUSICBRAINZ_ARTISTID=" MBID_DAFT_PUNK_ARTIST,
                               "MUSICBRAINZ_ALBUMARTISTID=" MBID_DAFT_PUNK_ARTIST,
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

static char s6_lib[256], s6_data[256];

static void
story6_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s6_lib, sizeof(s6_lib), "/tmp/quad_integ_%d_s6_lib", pid);
    snprintf(s6_data, sizeof(s6_data), "/tmp/quad_integ_%d_s6_data", pid);
    rm_rf(s6_lib);
    rm_rf(s6_data);
    mkdirs(s6_data);
    build_picard_tagged_library(s6_lib);
}

static void
story6_teardown(void)
{
    rm_rf(s6_lib);
    rm_rf(s6_data);
}

Test(indexer,
     picard_tags_preserve_release_group_id,
     .init = story6_setup,
     .fini = story6_teardown,
     .timeout = 60)
{
    test_tracker_t tracker = { 0 };
    run_indexer(s6_lib, s6_data, &tracker);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s6_data);

    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = s6_lib,
        .display_name = "Music",
        .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(albums->len, 1);
    const library_album_info_t *ram = g_ptr_array_index(albums, 0);
    cr_assert(ram->musicbrainz_release_group_id != NULL
                  && ram->musicbrainz_release_group_id[0] != '\0',
              "Phase 2 must read MUSICBRAINZ_RELEASEGROUPID from Picard tags");
    cr_assert_str_eq(ram->musicbrainz_release_group_id, MBID_RAM_RELEASE_GROUP);
    g_ptr_array_unref(albums);

    library_cache_destroy(cache);
}

Test(indexer,
     picard_tags_preserve_artist_mbid,
     .init = story6_setup,
     .fini = story6_teardown,
     .timeout = 60)
{
    test_tracker_t tracker = { 0 };
    run_indexer(s6_lib, s6_data, &tracker);

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s6_data);

    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = s6_lib,
        .display_name = "Music",
        .bitmap_index = 0,
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

static void
build_basic_tagged_ram(const char *root)
{
    char path[1024];
    snprintf(
        path, sizeof(path), "%s/Daft Punk/Random Access Memories (10th Anniversary Edition)", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } tracks[] = {
        { 1, "Give Life Back to Music", 274 },
        { 2, "The Game of Love", 321 },
        { 3, "Get Lucky", 367 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = { title_tag,
                               "artist=Daft Punk",
                               "album=Random Access Memories (10th Anniversary Edition)",
                               "album_artist=Daft Punk",
                               tracknum,
                               "date=2023",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

static void
build_odesza_with_bronson_credits(const char *root)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/ODESZA/The Last Goodbye Tour Live", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        const char *artist;
        int dur;
    } tracks[] = {
        { 1, "This Version Of You (Live)", "ODESZA", 186 },
        { 2, "Behind the Sun (Live)", "ODESZA", 190 },
        { 3, "HEART ATTACK (Live)", "Bronson", 194 },
        { 4, "VAULTS (Live)", "Bronson", 200 },
        { 5, "Wide Awake (Live)", "ODESZA", 202 },
    };
    for (int i = 0; i < 5; i++) {
        char tracknum[16], title_tag[256], artist_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        snprintf(artist_tag, sizeof(artist_tag), "artist=%s", tracks[i].artist);
        const char *tags[] = { title_tag,
                               artist_tag,
                               "album_artist=ODESZA",
                               "album=The Last Goodbye Tour Live",
                               tracknum,
                               "date=2024",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

static void
build_bronson_picard_tagged(const char *root)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } tracks[] = {
        { 1, "FOUNDATION", 184 },
        { 2, "HEART ATTACK", 209 },
        { 3, "VAULTS", 244 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = { title_tag,
                               "artist=BRONSON",
                               "album=BRONSON",
                               "album_artist=BRONSON",
                               tracknum,
                               "date=2020",
                               "MUSICBRAINZ_ALBUMID=" MBID_BRONSON_RELEASE,
                               "MUSICBRAINZ_RELEASEGROUPID=" MBID_BRONSON_RELEASE_GROUP,
                               "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST,
                               "MUSICBRAINZ_ALBUMARTISTID=" MBID_BRONSON_ARTIST,
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

/* --- Album dedup: untagged vs Picard-tagged --- */

static char s7a_lib_a[256], s7a_lib_b[256], s7a_data_a[256], s7a_data_b[256];

static void
story7a_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s7a_lib_a, sizeof(s7a_lib_a), "/tmp/quad_integ_%d_s7a_la", pid);
    snprintf(s7a_lib_b, sizeof(s7a_lib_b), "/tmp/quad_integ_%d_s7a_lb", pid);
    snprintf(s7a_data_a, sizeof(s7a_data_a), "/tmp/quad_integ_%d_s7a_da", pid);
    snprintf(s7a_data_b, sizeof(s7a_data_b), "/tmp/quad_integ_%d_s7a_db", pid);
    rm_rf(s7a_lib_a);
    rm_rf(s7a_lib_b);
    rm_rf(s7a_data_a);
    rm_rf(s7a_data_b);
    mkdirs(s7a_data_a);
    mkdirs(s7a_data_b);

    build_basic_tagged_ram(s7a_lib_a);
    build_picard_tagged_library(s7a_lib_b);
    index_no_mb(s7a_lib_a, s7a_data_a);
    index_no_mb(s7a_lib_b, s7a_data_b);
}

static void
story7a_teardown(void)
{
    rm_rf(s7a_lib_a);
    rm_rf(s7a_lib_b);
    rm_rf(s7a_data_a);
    rm_rf(s7a_data_b);
}

/**
 * Untagged album (no RGID) cannot dedup against Picard-tagged album (has RGID).
 * Requires Phase 6 MB resolution on the untagged library to fetch the RGID.
 */
Test(indexer,
     untagged_album_not_deduped_without_mb_resolution,
     .init = story7a_setup,
     .fini = story7a_teardown,
     .timeout = 60)
{
    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", s7a_data_a);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", s7a_data_b);

    library_cache_source_t sources[2] = {
        { .db_path = db_a,
          .music_base = s7a_lib_a,
          .display_name = "Elicb Music",
          .bitmap_index = 0 },
        { .db_path = db_b, .music_base = s7a_lib_b, .display_name = "Music", .bitmap_index = 1 },
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);

    GPtrArray *all = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    int ram_count = test_count_album_title_prefix(all, "Random Access Memories");
    cr_assert_eq(ram_count,
                 2,
                 "Expected 2 separate RAM albums in ALL view (untagged + Picard-tagged "
                 "cannot dedup without RGID), got %d. Dedup requires MB tags on both.",
                 ram_count);
    g_ptr_array_unref(all);

    library_cache_destroy(cache);
}

/* --- Artist merge: track credit without MBID --- */

static char s7b_lib_a[256], s7b_lib_b[256], s7b_data_a[256], s7b_data_b[256];

static void
story7b_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s7b_lib_a, sizeof(s7b_lib_a), "/tmp/quad_integ_%d_s7b_la", pid);
    snprintf(s7b_lib_b, sizeof(s7b_lib_b), "/tmp/quad_integ_%d_s7b_lb", pid);
    snprintf(s7b_data_a, sizeof(s7b_data_a), "/tmp/quad_integ_%d_s7b_da", pid);
    snprintf(s7b_data_b, sizeof(s7b_data_b), "/tmp/quad_integ_%d_s7b_db", pid);
    rm_rf(s7b_lib_a);
    rm_rf(s7b_lib_b);
    rm_rf(s7b_data_a);
    rm_rf(s7b_data_b);
    mkdirs(s7b_data_a);
    mkdirs(s7b_data_b);

    build_odesza_with_bronson_credits(s7b_lib_a);
    build_bronson_picard_tagged(s7b_lib_b);
    index_no_mb(s7b_lib_a, s7b_data_a);
    index_no_mb(s7b_lib_b, s7b_data_b);
}

static void
story7b_teardown(void)
{
    rm_rf(s7b_lib_a);
    rm_rf(s7b_lib_b);
    rm_rf(s7b_data_a);
    rm_rf(s7b_data_b);
}

/**
 * "Bronson" (untagged track credit in lib A) and "BRONSON" (Picard-tagged
 * album artist in lib B, has MBID) cannot merge because lib A's artist has
 * no MBID. Requires Phase 6 MB resolution on lib A.
 */
Test(indexer,
     untagged_track_credit_not_merged_without_mb_resolution,
     .init = story7b_setup,
     .fini = story7b_teardown,
     .timeout = 60)
{
    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", s7b_data_a);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", s7b_data_b);

    library_cache_source_t sources[2] = {
        { .db_path = db_a,
          .music_base = s7b_lib_a,
          .display_name = "Elicb Music",
          .bitmap_index = 0 },
        { .db_path = db_b, .music_base = s7b_lib_b, .display_name = "Music", .bitmap_index = 1 },
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
    cr_assert_eq(bronson_count,
                 2,
                 "Expected 2 unmerged BRONSON artists in ALL view (lib A untagged + "
                 "lib B Picard-tagged), got %d. Artist dedup requires MBID on both.",
                 bronson_count);
    g_ptr_array_unref(artists);

    /* Appearances: lib B's tagged BRONSON has no appearances on its own album,
     * and cannot merge with lib A's untagged 'Bronson' track credits. */
    int64_t bronson_b_id = test_find_artist_id_in_library(cache, "BRONSON", MASK_B);
    cr_assert(bronson_b_id > 0);

    GPtrArray *appearances
        = library_cache_get_artist_appearance_tracks(cache, bronson_b_id, LIBRARY_MASK_ALL);
    cr_assert_null(appearances,
                   "BRONSON (lib B, tagged) should have NO appearances — its 'Bronson' "
                   "track credits in lib A are a separate unmerged artist without MBID.");

    /* Search: both unmerged artists should match. */
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
    cr_assert_eq(search_count, 2, "Expected 2 unmerged BRONSON search hits, got %d.", search_count);
    library_search_results_free(results);

    library_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fictitious library builder (for stories that must not collide with real
 * MusicBrainz data). "Quadrature Test Artists — Integration Test Compilation"
 * is carefully chosen so Phase 6 cannot find a match under any resolution
 * strategy (no MB tag, unique artist name, unique release title).
 *
 *   Quadrature Test Artists/
 *     Integration Test Compilation 2025/
 *       01 - Quadrature Mashup.flac
 *       02 - Fictional Interlude.flac
 *       03 - Synthetic Finale.flac
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
build_fictitious_compilation(const char *root)
{
    char path[1024];
    snprintf(
        path, sizeof(path), "%s/Quadrature Test Artists/Integration Test Compilation 2025", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        int dur;
    } tracks[] = {
        { 1, "Quadrature Mashup", 120 },
        { 2, "Fictional Interlude", 180 },
        { 3, "Synthetic Finale", 150 },
    };
    for (int i = 0; i < 3; i++) {
        char tracknum[16], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s.flac", path, tracks[i].num, tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", tracks[i].title);
        const char *tags[] = { title_tag,
                               "artist=Quadrature Test Artists",
                               "album=Integration Test Compilation 2025",
                               "album_artist=Quadrature Test Artists",
                               tracknum,
                               "date=2025",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, tracks[i].dur), 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 8a: Non-MB-matched album — file tags remain authoritative
 *
 * "I have a fictitious compilation with no MusicBrainz IDs. Phase 6 runs
 *  against the live MB Postgres, finds no matching release (NO_MATCH), and
 *  the album stays writable. When I edit a track title in the file, the
 *  next scan propagates the edit to the library cache."
 *
 * Proves: Phase 2's upsert CASE statement writes file title when the album
 * is NOT MB-locked (mb_status != RESOLVED).
 *
 * Requires: MB_PG_PASSWORD env var (tests Phase 6 actually running).
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s8a_lib[256], s8a_data[256];

static void
story8a_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s8a_lib, sizeof(s8a_lib), "/tmp/quad_integ_%d_s8a_lib", pid);
    snprintf(s8a_data, sizeof(s8a_data), "/tmp/quad_integ_%d_s8a_data", pid);
    rm_rf(s8a_lib);
    rm_rf(s8a_data);
    mkdirs(s8a_data);

    build_fictitious_compilation(s8a_lib);
}

static void
story8a_teardown(void)
{
    rm_rf(s8a_lib);
    rm_rf(s8a_data);
}

Test(indexer,
     non_mb_album_tag_edit_propagates,
     .init = story8a_setup,
     .fini = story8a_teardown,
     .timeout = 120)
{
    const char *pg_conninfo = test_mb_pg_conninfo();
    /* HTTP mode returns NULL pg_conninfo intentionally — that's the routing
     * signal, not a missing-config error. Only skip in PG mode. */
    if (!pg_conninfo && !quad_test_use_http())
        cr_skip("MB_PG_PASSWORD not set — Phase 6 cannot run");

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s8a_data);

    /* ── First scan: Phase 6 runs, finds no match ── */
    test_tracker_t t1 = { 0 };
    indexer_config_t cfg = test_config_mb(&t1, pg_conninfo);
    run_indexer_cfg(s8a_lib, s8a_data, &t1, &cfg);

    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s8a_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        const library_album_info_t *comp
            = test_find_album(albums, "Integration Test Compilation 2025");
        cr_assert_not_null(comp, "Fictitious compilation must be indexed");

        GPtrArray *tracks
            = library_cache_get_tracks_by_album(cache, comp->album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 3);
        cr_assert_str_eq(((const library_track_info_t *)g_ptr_array_index(tracks, 0))->title,
                         "Quadrature Mashup",
                         "Phase 2 must write the file tag when no MB match");
        g_ptr_array_unref(tracks);
        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }

    /* ── Edit track 1's title and re-scan ── */
    char album_dir[1024], track1_path[1280];
    snprintf(album_dir,
             sizeof(album_dir),
             "%s/Quadrature Test Artists/Integration Test Compilation 2025",
             s8a_lib);
    snprintf(track1_path, sizeof(track1_path), "%s/01 - Quadrature Mashup.flac", album_dir);

    const char *edited[] = { "title=Quadrature Mashup (Remastered)",
                             "artist=Quadrature Test Artists",
                             "album=Integration Test Compilation 2025",
                             "album_artist=Quadrature Test Artists",
                             "track=1",
                             "date=2025",
                             NULL };
    cr_assert_eq(create_flac(track1_path, edited, 120), 0);
    bump_mtime_future(track1_path);

    test_tracker_t t2 = { 0 };
    indexer_config_t cfg2 = test_config_mb(&t2, pg_conninfo);
    run_indexer_cfg(s8a_lib, s8a_data, &t2, &cfg2);

    /* ── Verify cache shows edited title ── */
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s8a_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        const library_album_info_t *comp
            = test_find_album(albums, "Integration Test Compilation 2025");
        cr_assert_not_null(comp);

        GPtrArray *tracks
            = library_cache_get_tracks_by_album(cache, comp->album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 3, "Track count preserved across edit");
        cr_assert_str_eq(((const library_track_info_t *)g_ptr_array_index(tracks, 0))->title,
                         "Quadrature Mashup (Remastered)",
                         "Edit must propagate for non-MB-locked album");
        g_ptr_array_unref(tracks);
        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 8b: MB-tagged album — MusicBrainz owns the metadata
 *
 * "My library has files tagged by Picard with full MusicBrainz IDs. Phase 6
 *  resolves them to canonical MB titles. If I later:
 *    (1) edit a track title in the file, the edit must be IGNORED — MB owns
 *        the field and the DB upsert must preserve the MB title.
 *    (2) strip the MUSICBRAINZ_ALBUMID tag, the album's MBID must STAY in
 *        the DB — once resolved, the association is permanent until a user
 *        action explicitly unresolves it."
 *
 * Proves the two halves of the "MB is authoritative for MB-tagged files"
 * invariant, enforced at the SQL layer via mb_status-gated upsert.
 *
 * Requires: MB_PG_PASSWORD set AND the Daft Punk RAM release resolvable
 * (it is, in any standard MB replica).
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s8b_lib[256], s8b_data[256];

static void
story8b_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s8b_lib, sizeof(s8b_lib), "/tmp/quad_integ_%d_s8b_lib", pid);
    snprintf(s8b_data, sizeof(s8b_data), "/tmp/quad_integ_%d_s8b_data", pid);
    rm_rf(s8b_lib);
    rm_rf(s8b_data);
    mkdirs(s8b_data);

    build_picard_tagged_library(s8b_lib);
}

static void
story8b_teardown(void)
{
    rm_rf(s8b_lib);
    rm_rf(s8b_data);
}

Test(indexer,
     mb_tagged_album_edits_are_ignored,
     .init = story8b_setup,
     .fini = story8b_teardown,
     .timeout = 180)
{
    const char *pg_conninfo = test_mb_pg_conninfo();
    /* HTTP mode returns NULL pg_conninfo intentionally — that's the routing
     * signal, not a missing-config error. Only skip in PG mode. */
    if (!pg_conninfo && !quad_test_use_http())
        cr_skip("MB_PG_PASSWORD not set — Phase 6 cannot run");

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s8b_data);

    /* ── First scan with Phase 6: RAM gets resolved, mb_status=2 ── */
    test_tracker_t t1 = { 0 };
    indexer_config_t cfg = test_config_mb(&t1, pg_conninfo);
    run_indexer_cfg(s8b_lib, s8b_data, &t1, &cfg);

    /* Snapshot the MB-canonical title + MBID for later comparison. */
    char *mb_canonical_title = NULL;
    char *mb_release_id = NULL;
    int64_t album_id = 0;
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s8b_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 1);
        const library_album_info_t *ram = g_ptr_array_index(albums, 0);
        cr_assert_not_null(ram->musicbrainz_release_id,
                           "Phase 6 must populate musicbrainz_release_id after resolve");
        cr_assert_str_eq(ram->musicbrainz_release_id,
                         MBID_RAM_RELEASE,
                         "Resolved MBID must match the one from the file's Picard tags");

        album_id = ram->album_id;
        mb_release_id = g_strdup(ram->musicbrainz_release_id);

        GPtrArray *tracks
            = library_cache_get_tracks_by_album(cache, ram->album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 3);
        const library_track_info_t *t0 = g_ptr_array_index(tracks, 0);
        mb_canonical_title = g_strdup(t0->title);
        g_ptr_array_unref(tracks);
        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }

    /* ── Part 1: Edit track 1's title, keep MB tags intact.
     *
     * Phase 2's upsert must see mb_status=RESOLVED and use the CASE to
     * preserve the MB title (db_write.c:121). Any new tag written to the
     * file is ignored for MB-owned fields. */
    char album_dir[1024], track1_path[1280];
    snprintf(album_dir, sizeof(album_dir), "%s/Daft Punk/Random Access Memories", s8b_lib);
    snprintf(track1_path, sizeof(track1_path), "%s/01 - Give Life Back to Music.flac", album_dir);

    const char *edited_with_mb[] = { "title=TOTALLY WRONG TITLE FROM USER EDIT",
                                     "artist=Daft Punk",
                                     "album=Random Access Memories",
                                     "album_artist=Daft Punk",
                                     "track=1",
                                     "date=2013",
                                     "MUSICBRAINZ_ALBUMID=" MBID_RAM_RELEASE,
                                     "MUSICBRAINZ_RELEASEGROUPID=" MBID_RAM_RELEASE_GROUP,
                                     "MUSICBRAINZ_ARTISTID=" MBID_DAFT_PUNK_ARTIST,
                                     "MUSICBRAINZ_ALBUMARTISTID=" MBID_DAFT_PUNK_ARTIST,
                                     NULL };
    cr_assert_eq(create_flac(track1_path, edited_with_mb, 274), 0);
    bump_mtime_future(track1_path);

    test_tracker_t t2 = { 0 };
    indexer_config_t cfg2 = test_config_mb(&t2, pg_conninfo);
    run_indexer_cfg(s8b_lib, s8b_data, &t2, &cfg2);

    /* Edit must be rejected by the DB: cache still shows MB title. */
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s8b_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 3);
        const library_track_info_t *t0 = g_ptr_array_index(tracks, 0);
        cr_assert_str_eq(t0->title,
                         mb_canonical_title,
                         "MB-locked track title must survive a file-tag edit "
                         "(db_write.c:121 CASE preserves when mb_status=RESOLVED)");
        cr_assert(strstr(t0->title, "TOTALLY WRONG") == NULL,
                  "User's invalid edit leaked into the cache");
        g_ptr_array_unref(tracks);
        library_cache_destroy(cache);
    }

    /* ── Part 2: Strip ALL MusicBrainz tags from a file, keep basic tags.
     *
     * The album's mb_status is still RESOLVED in the DB, so the upsert
     * must continue to preserve MB fields. The album's MBID in the albums
     * table MUST stay — Phase 6 resolution is sticky. */
    const char *stripped[] = { "title=Give Life Back to Music",
                               "artist=Daft Punk",
                               "album=Random Access Memories",
                               "album_artist=Daft Punk",
                               "track=1",
                               "date=2013",
                               /* NO MUSICBRAINZ_* tags */
                               NULL };
    cr_assert_eq(create_flac(track1_path, stripped, 274), 0);
    bump_mtime_future(track1_path);

    test_tracker_t t3 = { 0 };
    indexer_config_t cfg3 = test_config_mb(&t3, pg_conninfo);
    run_indexer_cfg(s8b_lib, s8b_data, &t3, &cfg3);

    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s8b_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        cr_assert_eq(albums->len, 1);
        const library_album_info_t *ram = g_ptr_array_index(albums, 0);
        cr_assert_not_null(ram->musicbrainz_release_id,
                           "Stripping the Picard tag from a file must NOT null the album's MBID");
        cr_assert_str_eq(ram->musicbrainz_release_id,
                         mb_release_id,
                         "Album MBID must persist across re-scan with stripped file tags");
        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }

    g_free(mb_canonical_title);
    g_free(mb_release_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 9: No-MusicBrainz mode — full user lifecycle
 *
 * "I use quadrature without MusicBrainz. No PG, no fingerprinting. My library
 *  is my source of truth. I expect every basic lifecycle operation to work:
 *    - initial scan builds the cache from file tags
 *    - editing a tag in place updates the cache on re-scan
 *    - deleting a file removes the track from the cache
 *    - renaming a file keeps the track without duplication or loss"
 *
 * Proves the non-MB happy path is a first-class, tested mode — not just
 * an accidental side-effect of Phase 6 being off.
 *
 * Does NOT require PG. Uses the fictitious compilation so results are
 * deterministic regardless of any environment.
 * ═══════════════════════════════════════════════════════════════════════════ */

static char s9_lib[256], s9_data[256];

static void
story9_setup(void)
{
    if (!ffmpeg_available())
        cr_skip("ffmpeg not in PATH");
    pid_t pid = getpid();
    snprintf(s9_lib, sizeof(s9_lib), "/tmp/quad_integ_%d_s9_lib", pid);
    snprintf(s9_data, sizeof(s9_data), "/tmp/quad_integ_%d_s9_data", pid);
    rm_rf(s9_lib);
    rm_rf(s9_data);
    mkdirs(s9_data);

    build_fictitious_compilation(s9_lib);
}

static void
story9_teardown(void)
{
    rm_rf(s9_lib);
    rm_rf(s9_data);
}

Test(indexer,
     no_mb_mode_full_lifecycle,
     .init = story9_setup,
     .fini = story9_teardown,
     .timeout = 90)
{
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", s9_data);

    char album_dir[1024];
    snprintf(album_dir,
             sizeof(album_dir),
             "%s/Quadrature Test Artists/Integration Test Compilation 2025",
             s9_lib);

    /* ══════════════════════════════════════════════════════════════════
     * PHASE A — Initial scan: cache has all three tracks
     * ══════════════════════════════════════════════════════════════════ */
    {
        test_tracker_t t = { 0 };
        run_indexer(s9_lib, s9_data, &t);
        cr_assert_eq(t.last_progress.files_new, 3, "No-MB initial scan must insert 3 tracks");
    }

    int64_t album_id = 0;
    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s9_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *albums = library_cache_get_albums_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
        const library_album_info_t *comp
            = test_find_album(albums, "Integration Test Compilation 2025");
        cr_assert_not_null(comp);
        album_id = comp->album_id;

        GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 3);
        cr_assert_str_eq(((const library_track_info_t *)g_ptr_array_index(tracks, 0))->title,
                         "Quadrature Mashup");
        g_ptr_array_unref(tracks);
        g_ptr_array_unref(albums);
        library_cache_destroy(cache);
    }

    /* ══════════════════════════════════════════════════════════════════
     * PHASE B — Tag edit: rewrite track 2's title, expect cache update
     * ══════════════════════════════════════════════════════════════════ */
    char track2_path[1280];
    snprintf(track2_path, sizeof(track2_path), "%s/02 - Fictional Interlude.flac", album_dir);
    const char *edited[] = { "title=Fictional Interlude (Director's Cut)",
                             "artist=Quadrature Test Artists",
                             "album=Integration Test Compilation 2025",
                             "album_artist=Quadrature Test Artists",
                             "track=2",
                             "date=2025",
                             NULL };
    cr_assert_eq(create_flac(track2_path, edited, 180), 0);
    bump_mtime_future(track2_path);

    {
        test_tracker_t t = { 0 };
        run_indexer(s9_lib, s9_data, &t);
        /* Note: files_new is misnamed — it increments on every successful
         * upsert. Use files_unchanged + library_cache state to verify. */
        cr_assert_eq(
            t.last_progress.files_unchanged, 0, "The album was touched; no files should bulk-skip");
    }

    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s9_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 3, "Track count must stay at 3 after edit");
        cr_assert_str_eq(((const library_track_info_t *)g_ptr_array_index(tracks, 1))->title,
                         "Fictional Interlude (Director's Cut)",
                         "No-MB mode: file tag edit must propagate to cache");
        g_ptr_array_unref(tracks);
        library_cache_destroy(cache);
    }

    /* ══════════════════════════════════════════════════════════════════
     * PHASE C — Delete a file: orphan track must be pruned
     * ══════════════════════════════════════════════════════════════════ */
    char track3_path[1280];
    snprintf(track3_path, sizeof(track3_path), "%s/03 - Synthetic Finale.flac", album_dir);
    cr_assert_eq(unlink(track3_path), 0, "unlink track 3 failed");
    /* Delete shrinks total_dir_size — Phase 1's size-delta check triggers
     * re-processing without needing an mtime bump. */

    {
        test_tracker_t t = { 0 };
        run_indexer(s9_lib, s9_data, &t);
    }

    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s9_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 2, "Delete must prune the orphan track (was 3, now 2)");
        for (guint i = 0; i < tracks->len; i++) {
            const library_track_info_t *t = g_ptr_array_index(tracks, i);
            cr_assert_str_neq(t->title, "Synthetic Finale", "Deleted track must not be in cache");
        }
        g_ptr_array_unref(tracks);
        library_cache_destroy(cache);
    }

    /* ══════════════════════════════════════════════════════════════════
     * PHASE D — Rename a file: track count stays, no duplicates
     *
     * Important nuance: `mv` within the same directory preserves both the
     * inode's mtime AND the file's size, so Phase 1's max(file.mtime) +
     * sum(file.size) fingerprint is unchanged and the album would NOT be
     * re-processed by a plain rename. A realistic file-manager workflow
     * almost always accompanies a rename with some metadata edit (e.g. a
     * tag fix) that bumps the file's mtime. We simulate that second step
     * explicitly with bump_mtime_future on the renamed file.
     *
     * The invariant under test: once Phase 1 is triggered, rename is
     * handled without duplicates or loss.
     * ══════════════════════════════════════════════════════════════════ */
    char track2_new_path[1280];
    snprintf(track2_new_path,
             sizeof(track2_new_path),
             "%s/02 - Fictional Interlude Renamed.flac",
             album_dir);
    {
        char cmd[2600];
        snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", track2_path, track2_new_path);
        cr_assert_eq(system(cmd), 0, "mv failed");
    }
    bump_mtime_future(track2_new_path);

    {
        test_tracker_t t = { 0 };
        run_indexer(s9_lib, s9_data, &t);
    }

    {
        library_cache_source_t src = {
            .db_path = db_path,
            .music_base = s9_lib,
            .display_name = "Test",
            .bitmap_index = 0,
        };
        library_cache_t *cache = NULL;
        cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
        library_cache_warm_slot_blocking(cache, 0);

        GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album_id, LIBRARY_MASK_ALL);
        cr_assert_eq(tracks->len, 2, "Rename must not change track count (no duplicate, no loss)");

        /* Resolve the renamed track's path — it must match the new filename. */
        bool found_new_path = false;
        for (guint i = 0; i < tracks->len; i++) {
            const library_track_info_t *tk = g_ptr_array_index(tracks, i);
            char *path = library_cache_resolve_track_path(cache, tk->track_id);
            if (path && strstr(path, "Fictional Interlude Renamed")) {
                found_new_path = true;
            }
            g_free(path);
        }
        cr_assert(found_new_path,
                  "Cache must resolve the renamed track to its new filesystem path");

        g_ptr_array_unref(tracks);
        library_cache_destroy(cache);
    }
}
