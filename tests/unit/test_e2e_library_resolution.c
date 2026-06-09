/**
 * End-to-end integration test: library resolution via SOLR + MB PostgreSQL.
 *
 * Creates real FLAC files mimicking the actual elicb_music/ and Music/ libraries,
 * runs the full indexer pipeline (Phases 1-6, SOLR-only, no AcoustID), then
 * verifies everything through the library_cache API that the UI uses.
 *
 * Structured as user stories — each story sets up its own state, runs dozens
 * of assertions, then tears down. Stories are independent.
 *
 * USER STORIES:
 *   1. User imports two messy libraries
 *   2. User tags a library with Picard
 *   3. User moves an album between libraries
 *   4. User deletes an album folder and re-indexes
 *   5. User removes a library entirely
 *   6. MB resolution updates featured artist credits (ODESZA → BRONSON merge)
 *
 * Required env vars (test skips if absent):
 *   MB_PG_PASSWORD  — MusicBrainz PostgreSQL password
 *   MB_SOLR_URL     — SOLR endpoint (e.g. http://100.64.0.1:8983)
 *
 * Run:
 *   source .env && cd build && ninja test_e2e_library_resolution
 *   MB_SOLR_URL=http://100.64.0.1:8983 ./test_e2e_library_resolution --verbose
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "test_helpers.h"
#include "quadrature/indexer.h"
#include "quadrature/database.h"
#include "mb_test_env.h"
#include "quadrature/library.h"
#include "quadrature/library_search.h"
#include <stdbool.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>

// Initialize FFmpeg before any tests run (before Criterion forks)
ReportHook(PRE_ALL)(struct criterion_test_set *tests)
{
    (void)tests;
    avformat_network_init();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Known MBIDs from real Picard-tagged files
 * ═══════════════════════════════════════════════════════════════════════════ */

/* BRONSON (artist: ODESZA x Golden Features side project) */
#define MBID_BRONSON_ARTIST        "887b5b46-3f15-4475-b2bd-4d026c2b2031"
#define MBID_BRONSON_RELEASE       "5ed617d7-898f-4e05-82a1-bfc586a4b013"
#define MBID_BRONSON_RELEASE_GROUP "d95b8366-994d-448d-8689-422b20b6cabb"

/* Totally Enormous Extinct Dinosaurs — credited on BRONSON track 10 (DAWN).
 * MB's canonical artist name is "TEED"; Picard writes the credit-as-credited
 * ("Totally Enormous Extinct Dinosaurs") in ARTISTS but ships the same MBID. */
#define MBID_TEED_ARTIST "bd075a82-b196-4752-a1bb-3d87be3236a0"

/* Daft Punk — Random Access Memories */
#define MBID_DAFT_PUNK_ARTIST  "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define MBID_RAM_RELEASE       "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define MBID_RAM_RELEASE_GROUP "aa997ea0-2936-40bd-884d-3af8a0e064dc"

/* ODESZA — The Last Goodbye Tour Live (digital release, 27 tracks) */
#define MBID_ODESZA_ARTIST             "2e222fce-02ae-4221-b1c6-3c3242b423b6"
#define MBID_ODESZA_LIVE_RELEASE       "c3f6f487-f59a-467f-94a8-0f006a5deaf4"
#define MBID_ODESZA_LIVE_RELEASE_GROUP "2502f14d-0b97-4085-aba5-c62e1c166a65"

/* BRONSON track titles + durations (seconds, from MB release 5ed617d7) */
static const char *BRONSON_TRACKS[]
    = { "FOUNDATION", "HEART ATTACK", "BLINE",   "KNOW ME",     "VAULTS",
        "TENSE",      "CALL OUT",     "CONTACT", "KEEP MOVING", "DAWN" };
static const int BRONSON_DURATIONS[] = { 184, 209, 265, 180, 244, 200, 179, 207, 246, 443 };
#define BRONSON_TRACK_COUNT 10

/* RAM disc 1 track titles + durations (seconds, from MB release 5000a285) */
static const char *RAM_TRACKS[] = { "Give Life Back to Music",
                                    "The Game of Love",
                                    "Giorgio by Moroder",
                                    "Within",
                                    "Instant Crush",
                                    "Lose Yourself to Dance",
                                    "Touch",
                                    "Get Lucky",
                                    "Beyond",
                                    "Motherboard",
                                    "Fragments of Time",
                                    "Doin It Right",
                                    "Contact" };
static const int RAM_DURATIONS[]
    = { 274, 321, 544, 228, 337, 353, 498, 367, 290, 341, 279, 251, 381 };
#define RAM_TRACK_COUNT 13

/* ═══════════════════════════════════════════════════════════════════════════
 * Connection helpers (same pattern as test_mb_resolve.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *
env_or(const char *name, const char *fallback)
{
    const char *val = getenv(name);
    return (val && val[0]) ? val : fallback;
}

static const char *
mb_pg_conninfo(void)
{
    /* HTTP test mode: return NULL → resolver picks the HTTP backend. */
    if (quad_test_use_http())
        return NULL;
    const char *pw = getenv("MB_PG_PASSWORD");
    if (!pw || !pw[0])
        return NULL;
    static char buf[512];
    snprintf(buf,
             sizeof(buf),
             "host=%s dbname=%s user=%s password=%s connect_timeout=%s",
             env_or("MB_HOST", "localhost"),
             env_or("MB_DBNAME", "musicbrainz_db"),
             env_or("MB_USER", "musicbrainz"),
             pw,
             env_or("MB_PG_CONNECT_TIMEOUT", "5"));
    return buf;
}

static const char *
mb_solr_url(void)
{
    /* HTTP backend has built-in Solr equivalent (ws/2 search) — no env URL. */
    if (quad_test_use_http())
        return NULL;
    return env_or("MB_SOLR_URL", NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FLAC file generation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
mkdirs(const char *path)
{
    cr_assert_eq(g_mkdir_with_parents(path, 0755), 0, "g_mkdir_with_parents failed for %s", path);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library builders — create FLAC files with specific tag patterns
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Build lib_a: elicb_music pattern (messy tags, no MusicBrainz data).
 *
 * BRONSON/BRONSON (2020)/ — ZERO useful tags (only Date + Genre)
 * ODESZA/The Last Goodbye Tour Live/ — basic tags, tracks 17-18 credit "Bronson/Odesza"
 * Daft Punk/Random Access Memories (2013)/CD 01/ — basic tags, multi-disc
 */
static void
build_lib_a(const char *root)
{
    char path[1024], tracknum[32], title_tag[256];

    /* BRONSON — tag-less files (the real elicb_music pattern).
     * Duration matches the real album so SOLR scoring picks the right release. */
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON (2020)", root);
    mkdirs(path);
    for (int i = 0; i < BRONSON_TRACK_COUNT; i++) {
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s (FLAC 828 kbps).flac", path, i + 1, BRONSON_TRACKS[i]);
        const char *tags[] = { "date=2020", "genre=Electronic", NULL };
        cr_assert_eq(
            create_flac(fpath, tags, BRONSON_DURATIONS[i]), 0, "Failed to create: %s", fpath);
    }

    /* ODESZA — basic tags, tracks 17-18 credit Bronson */
    snprintf(path, sizeof(path), "%s/ODESZA/The Last Goodbye Tour Live", root);
    mkdirs(path);

    struct {
        int num;
        const char *title;
        const char *artist;
        int dur;
    } odesza_tracks[] = {
        { 1, "This Version Of You (Live)", "Odesza", 186 },
        { 2, "Behind the Sun (Live)", "Odesza", 190 },
        { 3, "Wide Awake (Live)", "Odesza", 202 },
        { 17, "TENSE (Live)", "Bronson/Odesza", 194 },
        { 18, "Keep Moving (Live)", "Bronson/Odesza", 142 },
    };

    for (size_t i = 0; i < sizeof(odesza_tracks) / sizeof(odesza_tracks[0]); i++) {
        g_autofree char *fpath = g_strdup_printf(
            "%s/%02d - %s.flac", path, odesza_tracks[i].num, odesza_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", odesza_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", odesza_tracks[i].title);

        char artist_tag[256], aa_tag[256];
        snprintf(artist_tag, sizeof(artist_tag), "artist=%s", odesza_tracks[i].artist);
        snprintf(aa_tag, sizeof(aa_tag), "album_artist=%s", odesza_tracks[i].artist);

        const char *tags[] = {
            title_tag,   artist_tag,         aa_tag, "album=The Last Goodbye Tour Live", tracknum,
            "date=2024", "genre=Electronic", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, odesza_tracks[i].dur), 0);
    }

    /* ODESZA — A Moment Apart (2017) — tag-less, feat. credits only as
     * plain strings. Matches prod elicb_music pattern: each track with a
     * "/"-delimited artist needs Phase 6 to canonicalize the secondary
     * credit against MB recording data. Track titles + durations from
     * MB release b989c328 so SOLR scores a clean match. */
    snprintf(path, sizeof(path), "%s/ODESZA/A Moment Apart (2017)", root);
    mkdirs(path);
    {
        struct {
            int num;
            const char *title;
            const char *artist;
            int dur;
        } ama[] = {
            { 1, "Intro", "Odesza", 50 },
            { 2, "A Moment Apart", "Odesza", 258 },
            { 3, "Higher Ground", "Odesza/Naomi Wild", 239 },
            { 4, "Boy", "Odesza", 222 },
            { 5, "Line of Sight", "Odesza/WYNNE/Mansionair", 249 },
            { 6, "Late Night", "Odesza", 258 },
        };
        for (size_t i = 0; i < sizeof(ama) / sizeof(ama[0]); i++) {
            g_autofree char *fpath
                = g_strdup_printf("%s/%02d - %s.flac", path, ama[i].num, ama[i].title);
            snprintf(tracknum, sizeof(tracknum), "track=%d", ama[i].num);
            snprintf(title_tag, sizeof(title_tag), "title=%s", ama[i].title);
            char artist_tag[256], aa_tag[256];
            snprintf(artist_tag, sizeof(artist_tag), "artist=%s", ama[i].artist);
            snprintf(aa_tag, sizeof(aa_tag), "album_artist=%s", ama[i].artist);
            const char *tags[]
                = { title_tag,   artist_tag,         aa_tag, "album=A Moment Apart", tracknum,
                    "date=2017", "genre=Electronic", NULL };
            cr_assert_eq(create_flac(fpath, tags, ama[i].dur), 0);
        }
    }

    /* Daft Punk — basic tags, multi-disc, no MB */
    snprintf(path, sizeof(path), "%s/Daft Punk/Random Access Memories (2013)/CD 01", root);
    mkdirs(path);
    for (int i = 0; i < RAM_TRACK_COUNT; i++) {
        g_autofree char *fpath = g_strdup_printf("%s/%02d - %s.flac", path, i + 1, RAM_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", RAM_TRACKS[i]);
        const char *tags[] = { title_tag,
                               "artist=Daft Punk",
                               "album=Random Access Memories (10th Anniversary Edition)",
                               "album_artist=Daft Punk",
                               tracknum,
                               "disc=1",
                               "date=2023",
                               "genre=Dance",
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, RAM_DURATIONS[i]), 0);
    }
}

/**
 * Build lib_b: Music/ pattern (Picard-tagged, full MusicBrainz data).
 *
 * BRONSON/BRONSON/ — full Picard tags
 * Daft Punk/Random Access Memories/ — full Picard tags
 */
static void
build_lib_b(const char *root)
{
    char path[1024], tracknum[32], title_tag[256];

    /* BRONSON — full Picard tags.
     * Track 10 (DAWN) carries a feat. credit exactly as real Picard writes it:
     *   ARTIST               = "BRONSON feat. Totally Enormous Extinct Dinosaurs"
     *   MUSICBRAINZ_ARTISTID = "<bronson-mbid>;<teed-mbid>"
     * Exercises Phase 2's ability to keep the parallel MBID list in lock-step
     * with the feat.-split ARTIST credits. */
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON", root);
    mkdirs(path);
    for (int i = 0; i < BRONSON_TRACK_COUNT; i++) {
        const bool is_dawn = (i == BRONSON_TRACK_COUNT - 1);

        g_autofree char *fpath
            = g_strdup_printf("%s/%02d BRONSON - %s.flac", path, i + 1, BRONSON_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", BRONSON_TRACKS[i]);

        const char *artist_tag = is_dawn ? "artist=BRONSON feat. Totally Enormous Extinct Dinosaurs"
                                         : "artist=BRONSON";
        const char *mb_artistid_tag = is_dawn ? "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST
                                                ";" MBID_TEED_ARTIST
                                              : "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST;

        const char *tags[] = { title_tag,
                               artist_tag,
                               "album=BRONSON",
                               "album_artist=BRONSON",
                               tracknum,
                               "date=2020",
                               "genre=Electronic",
                               "MUSICBRAINZ_ALBUMID=" MBID_BRONSON_RELEASE,
                               mb_artistid_tag,
                               "MUSICBRAINZ_RELEASEGROUPID=" MBID_BRONSON_RELEASE_GROUP,
                               "MUSICBRAINZ_ALBUMARTISTID=" MBID_BRONSON_ARTIST,
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, BRONSON_DURATIONS[i]), 0);
    }

    /* Daft Punk RAM — full Picard tags */
    snprintf(path, sizeof(path), "%s/Daft Punk/Random Access Memories", root);
    mkdirs(path);
    for (int i = 0; i < RAM_TRACK_COUNT; i++) {
        g_autofree char *fpath = g_strdup_printf("%s/%02d - %s.flac", path, i + 1, RAM_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", RAM_TRACKS[i]);
        const char *tags[] = { title_tag,
                               "artist=Daft Punk",
                               "album=Random Access Memories",
                               "album_artist=Daft Punk",
                               tracknum,
                               "disc=1",
                               "date=2023",
                               "genre=Dance",
                               "MUSICBRAINZ_ALBUMID=" MBID_RAM_RELEASE,
                               "MUSICBRAINZ_ARTISTID=" MBID_DAFT_PUNK_ARTIST,
                               "MUSICBRAINZ_RELEASEGROUPID=" MBID_RAM_RELEASE_GROUP,
                               "MUSICBRAINZ_ALBUMARTISTID=" MBID_DAFT_PUNK_ARTIST,
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, RAM_DURATIONS[i]), 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Indexer helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* (Legacy `test_tracker_t` / `test_callback` / `index_library` helpers
 * removed — all stories now use the prod-parity harness
 * (run_prod_indexers) defined after this block.) */

typedef struct {
    library_cache_t *cache;
    int bitmap_index;       /* Which slot to refresh */
    atomic_int lib_updated; /* Count of INDEXER_LIBRARY_UPDATED */
    atomic_int completed;   /* 1 once COMPLETED/CANCELLED/ERROR */
    atomic_int errored;
} ProdParityTracker;

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared fixture state — referenced by prod-parity helpers below AND by
 * every story setup. Declared here so the helpers compile against them.
 * ═══════════════════════════════════════════════════════════════════════════ */

static char lib_a_root[256];
static char lib_b_root[256];
static char lib_a_data[256];
static char lib_b_data[256];
static library_cache_t *cache = NULL;

#define MASK_A (1u << 0)
#define MASK_B (1u << 1)

/* Forward declaration — rm_rf is defined further down alongside other
 * shell-wrapping fs helpers but is needed by story_common_paths_init. */
static void rm_rf(const char *path);

/* ═══════════════════════════════════════════════════════════════════════════
 * Prod-parity async indexer + cache refresh harness
 *
 * Mirrors src/ui/main.c:207 → src/ui/window.c:1341-1367 →
 * src/ui/libraries/indexer_bridge.c:705-718:
 *   1. library_cache_create_multi on empty DB schemas
 *   2. library_cache_warm_slot (non-blocking) kicks off async warm
 *   3. indexer_scan (non-blocking) runs concurrently
 *   4. INDEXER_LIBRARY_UPDATED fires twice per library (post-Phase-3,
 *      post-Phase-6). Each firing triggers library_cache_refresh_slot
 *      on the slot's bitmap_index — same call indexer_bridge.c:660 makes
 *      after its 500ms debounce window. No debounce here (tests drive
 *      one event at a time; the refresh semantics under test are
 *      identical).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* (ProdParityTracker typedef is up with the shared fixture state so
 *  story_teardown can reach it.) */

static void
prod_parity_cb(indexer_event_t event,
               const indexer_progress_t *progress,
               const library_cache_changeset_t *changeset,
               void *user_data)
{
    (void)progress;
    (void)changeset;
    ProdParityTracker *t = user_data;
    switch (event) {
    case INDEXER_LIBRARY_UPDATED:
        atomic_fetch_add(&t->lib_updated, 1);
        break;
    case INDEXER_COMPLETED:
        atomic_store(&t->completed, 1);
        break;
    case INDEXER_CANCELLED:
        atomic_store(&t->completed, 1);
        break;
    case INDEXER_ERROR:
        atomic_store(&t->errored, 1);
        atomic_store(&t->completed, 1);
        break;
    default:
        break;
    }
}

/* Create a fresh DB file with the current schema (empty rows). Mirrors
 * what prod does implicitly on first app launch after `make db-clean`:
 * library_cache_create_multi must see a valid schema even though no
 * indexing has written any data yet. */
static void
bootstrap_empty_db(const char *data_dir)
{
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", data_dir);
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path, false, &db),
                 QUADRATURE_OK,
                 "bootstrap_empty_db: failed to create schema at %s",
                 db_path);
    db_close(db);
}

/* Drive the same "INDEXER_LIBRARY_UPDATED → library_cache_refresh_slot"
 * chain prod's indexer_bridge.c wires up. Observe the tracker's event
 * counter; every increment above the last-seen value triggers a fresh
 * refresh + await on that library's slot. Blocks until both indexers
 * have emitted COMPLETED and all generated refreshes have drained. */
static void
pump_until_indexers_done(library_cache_t *lib_cache, ProdParityTracker *trackers, int tracker_count)
{
    int *seen = g_new0(int, tracker_count);

    for (;;) {
        int all_done = 1;
        for (int i = 0; i < tracker_count; i++) {
            int cur_evs = atomic_load(&trackers[i].lib_updated);
            while (seen[i] < cur_evs) {
                library_cache_refresh_slot(lib_cache, trackers[i].bitmap_index, NULL);
                library_cache_await_slot(lib_cache, trackers[i].bitmap_index);
                seen[i]++;
            }
            if (!atomic_load(&trackers[i].completed))
                all_done = 0;
        }
        if (all_done) {
            /* Drain any straggler events that landed after the completion
             * store (indexer emits LIBRARY_UPDATED then COMPLETED, usually
             * in that order, but the atomic stores aren't sequenced from
             * the test's viewpoint). */
            int pending = 0;
            for (int i = 0; i < tracker_count; i++) {
                int cur_evs = atomic_load(&trackers[i].lib_updated);
                while (seen[i] < cur_evs) {
                    library_cache_refresh_slot(lib_cache, trackers[i].bitmap_index, NULL);
                    library_cache_await_slot(lib_cache, trackers[i].bitmap_index);
                    seen[i]++;
                    pending++;
                }
            }
            if (!pending)
                break;
        }
        g_usleep(10000); /* 10 ms — keep CPU load low */
    }
    g_free(seen);
}

/* ── Reusable story scaffolding ────────────────────────────────────────────
 * Every story that tests indexer → cache interactions should share the
 * same production-parity sequence. Two helpers cover the full life cycle:
 *
 *   setup_prod_cache()        bootstraps empty DB schemas at lib_a_data /
 *                             lib_b_data, calls library_cache_create_multi,
 *                             and awaits the initial async warm on both
 *                             slots. Safe to call once per story, before
 *                             any indexing.
 *
 *   run_prod_indexers(a, b)   kicks off concurrent indexers for the
 *                             selected libraries and drives the refresh
 *                             chain. Safe to call multiple times in the
 *                             same story (e.g., retag + re-scan flows) —
 *                             each call reuses the existing cache and
 *                             cumulatively refreshes its slots.
 * ──────────────────────────────────────────────────────────────────────── */

static void
setup_prod_cache(void)
{
    bootstrap_empty_db(lib_a_data);
    bootstrap_empty_db(lib_b_data);

    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", lib_a_data);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", lib_b_data);
    library_cache_source_t sources[2] = {
        { .db_path = db_a, .music_base = lib_a_root, .display_name = "Elicb", .bitmap_index = 0 },
        { .db_path = db_b, .music_base = lib_b_root, .display_name = "Music", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot(cache, 0);
    library_cache_warm_slot(cache, 1);
    library_cache_await_slot(cache, 0);
    library_cache_await_slot(cache, 1);
}

static void
run_prod_indexers(bool scan_a, bool scan_b, const char *pg, const char *solr)
{
    cr_assert(cache != NULL,
              "run_prod_indexers: cache not created — call setup_prod_cache() first");
    cr_assert(scan_a || scan_b, "run_prod_indexers: nothing to scan");

    ProdParityTracker trackers[2] = { 0 };
    trackers[0].cache = cache;
    trackers[0].bitmap_index = 0;
    trackers[1].cache = cache;
    trackers[1].bitmap_index = 1;
    /* If a slot is not scanned this round, mark its tracker completed so
     * pump_until_indexers_done doesn't wait for it. lib_updated stays 0
     * → no spurious refresh fires for the idle slot. */
    if (!scan_a)
        atomic_store(&trackers[0].completed, 1);
    if (!scan_b)
        atomic_store(&trackers[1].completed, 1);

    indexer_config_t cfg = {
        .thread_count = 2,
        .process_artwork = false,
        .mb_resolve = true,
        .pg_conninfo = pg,
        .mb_solr_url = solr,
        .acoustid_pg_conninfo = NULL,
        .acoustid_index_url = NULL,
        .fetch_artist_art = false,
        .fanart_api_key = NULL,
        .fetch_artist_bios = false,
        .callback = prod_parity_cb,
    };

    indexer_t *ia = NULL, *ib = NULL;
    if (scan_a) {
        cfg.user_data = &trackers[0];
        cr_assert_eq(indexer_create(&ia, &cfg), QUADRATURE_OK);
        cr_assert_eq(indexer_scan(ia, lib_a_root, lib_a_data), QUADRATURE_OK);
    }
    if (scan_b) {
        cfg.user_data = &trackers[1];
        cr_assert_eq(indexer_create(&ib, &cfg), QUADRATURE_OK);
        cr_assert_eq(indexer_scan(ib, lib_b_root, lib_b_data), QUADRATURE_OK);
    }

    pump_until_indexers_done(cache, trackers, 2);

    if (scan_a) {
        cr_assert(!atomic_load(&trackers[0].errored), "lib_a indexer reported INDEXER_ERROR");
        cr_assert(atomic_load(&trackers[0].lib_updated) >= 1,
                  "lib_a indexer emitted no LIBRARY_UPDATED events");
    }
    if (scan_b) {
        cr_assert(!atomic_load(&trackers[1].errored), "lib_b indexer reported INDEXER_ERROR");
        cr_assert(atomic_load(&trackers[1].lib_updated) >= 1,
                  "lib_b indexer emitted no LIBRARY_UPDATED events");
    }

    if (ia)
        indexer_destroy(ia);
    if (ib)
        indexer_destroy(ib);
}

/* Convenience: shared common prologue for every story — resolve tmp paths,
 * scrub them, recreate data dirs. Does NOT build libs or cache (caller
 * chooses build_lib_a/build_lib_b and the indexer sequence). */
static void
story_common_paths_init(void)
{
    pid_t pid = getpid();
    snprintf(lib_a_root, sizeof(lib_a_root), "/tmp/quad_e2e_%d_lib_a", pid);
    snprintf(lib_b_root, sizeof(lib_b_root), "/tmp/quad_e2e_%d_lib_b", pid);
    snprintf(lib_a_data, sizeof(lib_a_data), "/tmp/quad_e2e_%d_data_a", pid);
    snprintf(lib_b_data, sizeof(lib_b_data), "/tmp/quad_e2e_%d_data_b", pid);
    rm_rf(lib_a_root);
    rm_rf(lib_b_root);
    rm_rf(lib_a_data);
    rm_rf(lib_b_data);
    mkdirs(lib_a_data);
    mkdirs(lib_b_data);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cache query helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int
count_artist_name(const GPtrArray *artists, const char *name)
{
    int count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0)
            count++;
    }
    return count;
}

static const library_artist_info_t *
find_artist(const GPtrArray *artists, const char *name)
{
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0)
            return a;
    }
    return NULL;
}

static int
count_album_title(const GPtrArray *albums, const char *title)
{
    int count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0)
            count++;
    }
    return count;
}

static int
count_album_title_prefix(const GPtrArray *albums, const char *prefix)
{
    int count = 0;
    size_t len = strlen(prefix);
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strncasecmp(a->title, prefix, len) == 0)
            count++;
    }
    return count;
}

static int64_t
find_artist_id_in_library(library_cache_t *c, const char *name, uint32_t mask)
{
    GPtrArray *artists
        = library_cache_get_artists_filtered(c, LIBRARY_SORT_NAME_ASC, name, NULL, mask);
    if (!artists)
        return 0;
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

static void
rm_rf(const char *path)
{
    g_autofree char *cmd = g_strdup_printf("rm -rf '%s'", path);
    cr_assert_eq(system(cmd), 0, "rm -rf failed for %s", path);
}

/* (Shared fixture state + MASK_A/MASK_B moved up alongside the prod-parity
 *  harness so the helpers can reference them at compile time.) */

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 1: User imports two messy libraries
 *
 * "I have ~/elicb_music with ripped files (no MB tags, some have basic tags,
 *  some have nothing) and ~/Music with Picard-tagged files. I add both to
 *  quadrature. What does the library view show me?"
 *
 * Setup: index both libraries with full MB resolution (SOLR-only).
 * Verify: the ALL view shows correct artists, albums, dedup, search, nav.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
story_import_setup(void)
{
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    /* In HTTP mode pg/solr are NULL → resolver dispatches to HTTP backend.
     * In PG mode both must be set; the public Solr-equivalent isn't reachable. */
    if (!quad_test_use_http()) {
        if (!pg)
            cr_skip("MB_PG_PASSWORD not set");
        if (!solr)
            cr_skip("MB_SOLR_URL not set");
    }

    story_common_paths_init();
    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    /* Prod parity: empty-DB cache first, then concurrent indexers with
     * INDEXER_LIBRARY_UPDATED-driven refresh (matches main.c:207 +
     * indexer_bridge.c:660). */
    setup_prod_cache();
    run_prod_indexers(true, true, pg, solr);
}

static void
story_teardown(void)
{
    if (cache) {
        library_cache_destroy(cache);
        cache = NULL;
    }
    rm_rf(lib_a_root);
    rm_rf(lib_b_root);
    rm_rf(lib_a_data);
    rm_rf(lib_b_data);
}

Test(e2e,
     user_imports_two_messy_libraries,
     .init = story_import_setup,
     .fini = story_teardown,
     .timeout = 300)
{
    /* ── Artist dedup in ALL view ──────────────────────────────────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    /* BRONSON: lib_a has "Bronson" (ODESZA track credit, no MBID) + tag-less
     * BRONSON album ("Unknown Artist"). lib_b has "BRONSON" (full MBID).
     * After resolution, user expects 1 artist. */
    cr_assert_eq(count_artist_name(artists, "BRONSON"),
                 1,
                 "BRONSON should appear as 1 artist in ALL view — track credit "
                 "'Bronson' and tag-less album must merge with Picard-tagged 'BRONSON'");

    /* ODESZA: only in lib_a, should appear once */
    int odesza = count_artist_name(artists, "ODESZA");
    if (!odesza)
        odesza = count_artist_name(artists, "Odesza");
    cr_assert_eq(odesza, 1, "ODESZA should appear as 1 artist");

    /* Daft Punk: in both libraries, should be deduped to 1 */
    cr_assert_eq(count_artist_name(artists, "Daft Punk"),
                 1,
                 "Daft Punk should appear as 1 artist (deduped across libraries)");

    /* ── Artist MBIDs ──────────────────────────────────────────────── */

    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    if (!bronson)
        bronson = find_artist(artists, "Bronson");
    cr_assert_not_null(bronson, "BRONSON artist not found");
    cr_assert(bronson->musicbrainz_id != NULL && bronson->musicbrainz_id[0],
              "BRONSON artist should have MBID after resolution");
    cr_assert_str_eq(bronson->musicbrainz_id, MBID_BRONSON_ARTIST);

    /* ── TEED (feat. artist on DAWN) — no orphan alias row ─────────────
     * Picard tagged DAWN with ARTIST="BRONSON feat. Totally Enormous Extinct
     * Dinosaurs" + MUSICBRAINZ_ARTISTID="<bronson>;<teed>". Phase 2 must
     * consume the parallel MBID list; otherwise it creates an MBID-less
     * "Totally Enormous Extinct Dinosaurs" row that Phase 6 cannot reconcile
     * with the canonical "TEED" (bd075a82), leaving an orphan artist behind. */
    const library_artist_info_t *teed = NULL;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (a->musicbrainz_id && strcmp(a->musicbrainz_id, MBID_TEED_ARTIST) == 0) {
            cr_assert_null(teed,
                           "Two artist rows share the TEED MBID — Phase 2 split credits "
                           "off ARTIST without consuming MUSICBRAINZ_ARTISTID, then Phase "
                           "6 inserted a second TEED row with the MBID.");
            teed = a;
        }
    }
    cr_assert_not_null(teed,
                       "TEED artist (MBID %s) not found — feat. credit on DAWN not resolved",
                       MBID_TEED_ARTIST);

    /* The orphan from the original bug: a row named literally "Totally
     * Enormous Extinct Dinosaurs" (no MBID) left behind by Phase 2 when it
     * ignored MUSICBRAINZ_ARTISTID. Must not exist after a clean index. */
    cr_assert_null(find_artist(artists, "Totally Enormous Extinct Dinosaurs"),
                   "Orphan artist 'Totally Enormous Extinct Dinosaurs' (no MBID) still "
                   "present — Phase 2 created it from the ARTIST tag and Phase 6 could "
                   "not merge it into the canonical 'TEED' row.");

    const library_artist_info_t *dp = find_artist(artists, "Daft Punk");
    cr_assert_not_null(dp, "Daft Punk not found");
    cr_assert(dp->musicbrainz_id != NULL && dp->musicbrainz_id[0], "Daft Punk should have MBID");
    cr_assert_str_eq(dp->musicbrainz_id, MBID_DAFT_PUNK_ARTIST);

    int64_t bronson_id = bronson->artist_id;
    g_ptr_array_unref(artists);

    /* ── Album dedup in ALL view ───────────────────────────────────── */

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    /* BRONSON album: lib_a has "BRONSON (2020)" (from folder, no RGID),
     * lib_b has "BRONSON" (Picard, with RGID). Should dedup to 1. */
    cr_assert_eq(count_album_title(albums, "BRONSON"),
                 1,
                 "BRONSON album should appear once in ALL view (deduped by RGID)");

    /* RAM: lib_a has "Random Access Memories (10th Anniversary Edition)",
     * lib_b has "Random Access Memories". Same release_group_id → dedup to 1. */
    cr_assert_eq(count_album_title_prefix(albums, "Random Access Memories"),
                 1,
                 "Random Access Memories should appear once (deduped across editions)");

    /* ── Album MBID fields ─────────────────────────────────────────── */

    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, "BRONSON") == 0) {
            cr_assert(a->musicbrainz_release_group_id != NULL,
                      "BRONSON album should have release_group_id");
            cr_assert_str_eq(a->musicbrainz_release_group_id, MBID_BRONSON_RELEASE_GROUP);
            break;
        }
    }
    g_ptr_array_unref(albums);

    /* ── Tag-less BRONSON in lib_a resolved via SOLR ───────────────── */

    GPtrArray *albums_a
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    for (guint i = 0; i < albums_a->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a, i);
        if (strstr(a->title, "BRONSON") != NULL) {
            cr_assert(a->musicbrainz_release_group_id != NULL
                          && a->musicbrainz_release_group_id[0] != '\0',
                      "Tag-less BRONSON in lib_a should have been resolved by SOLR. "
                      "release_group_id is NULL — folder-path fallback not working");
            break;
        }
    }
    g_ptr_array_unref(albums_a);

    /* ── BRONSON featured appearances ──────────────────────────────── */

    GPtrArray *appearances
        = library_cache_get_artist_appearance_tracks(cache, bronson_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(appearances,
                       "BRONSON should have appearance tracks (credited on ODESZA album)");
    cr_assert(appearances->len >= 2,
              "BRONSON should appear on >= 2 ODESZA tracks (TENSE, KEEP MOVING), "
              "got %u",
              appearances->len);
    g_ptr_array_unref(appearances);

    /* ── Search ────────────────────────────────────────────────────── */

    library_search_results_t *results = library_cache_search(
        cache, "Bronson", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);

    int search_artists = 0;
    if (results->artists) {
        for (guint i = 0; i < results->artists->len; i++) {
            const library_artist_info_t *a = g_ptr_array_index(results->artists, i);
            if (g_ascii_strcasecmp(a->name, "BRONSON") == 0
                || g_ascii_strcasecmp(a->name, "Bronson") == 0)
                search_artists++;
        }
    }
    cr_assert_eq(
        search_artists, 1, "Search 'Bronson' should return 1 artist, got %d", search_artists);

    bool search_found_album = false;
    if (results->albums) {
        for (guint i = 0; i < results->albums->len; i++) {
            const library_album_info_t *a = g_ptr_array_index(results->albums, i);
            if (g_ascii_strcasecmp(a->title, "BRONSON") == 0)
                search_found_album = true;
        }
    }
    cr_assert(search_found_album, "Search should find BRONSON album");
    library_search_results_free(results);

    /* ── Track navigation ──────────────────────────────────────────── */

    GPtrArray *all_albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    int64_t odesza_album_id = 0;
    for (guint i = 0; i < all_albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(all_albums, i);
        if (strstr(a->title, "Last Goodbye") != NULL) {
            odesza_album_id = a->album_id;
            break;
        }
    }
    g_ptr_array_unref(all_albums);
    cr_assert(odesza_album_id > 0, "ODESZA live album not found");

    GPtrArray *tracks = library_cache_get_tracks_by_album(cache, odesza_album_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(tracks);
    cr_assert(tracks->len >= 3, "ODESZA album should have >= 3 tracks, got %u", tracks->len);

    const library_track_info_t *first = g_ptr_array_index(tracks, 0);
    int64_t next = library_cache_get_next_track_id(cache, first->track_id);
    cr_assert(next > 0, "Next track from first should exist");

    int64_t prev = library_cache_get_prev_track_id(cache, next);
    cr_assert_eq(prev, first->track_id, "Prev from second should be first");
    g_ptr_array_unref(tracks);

    /* ── Path resolution ───────────────────────────────────────────── */

    char *path = library_cache_resolve_track_path(cache, first->track_id);
    cr_assert_not_null(path, "Track path should resolve");
    cr_assert(strstr(path, ".flac") != NULL, "Path should contain .flac: %s", path);
    g_free(path);

    /* ══════════════════════════════════════════════════════════════════
     * Cross-library merge verification (absorbed from test_cross_library_merge)
     * ══════════════════════════════════════════════════════════════════ */

    /* ── Single-library filters show own content only ─────────────── */

    GPtrArray *artists_a
        = library_cache_get_artists_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(artists_a);
    cr_assert(artists_a->len >= 1, "lib_a should have artists");
    g_ptr_array_unref(artists_a);

    GPtrArray *artists_b
        = library_cache_get_artists_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_B);
    cr_assert_not_null(artists_b);
    cr_assert(artists_b->len >= 1, "lib_b should have artists");
    g_ptr_array_unref(artists_b);

    /* ── Daft Punk deduped: in both libraries, shows once in ALL ───── */

    int64_t dp_id_a = find_artist_id_in_library(cache, "Daft Punk", MASK_A);
    int64_t dp_id_b = find_artist_id_in_library(cache, "Daft Punk", MASK_B);
    cr_assert(dp_id_a > 0, "Daft Punk should be in lib_a");
    cr_assert(dp_id_b > 0, "Daft Punk should be in lib_b");

    /* Both IDs should resolve to the same albums in ALL view */
    GPtrArray *dp_albums_a = library_cache_get_albums_by_artist(cache, dp_id_a, LIBRARY_MASK_ALL);
    GPtrArray *dp_albums_b = library_cache_get_albums_by_artist(cache, dp_id_b, LIBRARY_MASK_ALL);
    cr_assert_not_null(dp_albums_a);
    cr_assert_not_null(dp_albums_b);
    cr_assert_eq(dp_albums_a->len,
                 dp_albums_b->len,
                 "Both Daft Punk IDs should return same album count in ALL view");
    g_ptr_array_unref(dp_albums_a);
    g_ptr_array_unref(dp_albums_b);

    /* ── RAM deduped across editions (same RGID) ──────────────────── */

    GPtrArray *albums_a2
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    bool found_ram_a = false;
    for (guint i = 0; i < albums_a2->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a2, i);
        if (g_ascii_strncasecmp(a->title, "Random Access Memories", 22) == 0)
            found_ram_a = true;
    }
    cr_assert(found_ram_a, "lib_a should have its own RAM edition");
    g_ptr_array_unref(albums_a2);

    GPtrArray *albums_b2
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_B);
    bool found_ram_b = false;
    for (guint i = 0; i < albums_b2->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_b2, i);
        if (g_ascii_strncasecmp(a->title, "Random Access Memories", 22) == 0)
            found_ram_b = true;
    }
    cr_assert(found_ram_b, "lib_b should have its own RAM edition");
    g_ptr_array_unref(albums_b2);

    /* But ALL view shows only 1 (deduped by RGID) */
    GPtrArray *all_albums2 = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_eq(count_album_title_prefix(all_albums2, "Random Access Memories"),
                 1,
                 "RAM should be deduped to 1 in ALL view (same RGID, different editions)");
    g_ptr_array_unref(all_albums2);

    /* ── Search dedup: searching returns merged results ────────────── */

    library_search_results_t *dp_search = library_cache_search(
        cache, "Daft Punk", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(dp_search);
    int dp_artist_count = 0;
    if (dp_search->artists) {
        for (guint i = 0; i < dp_search->artists->len; i++) {
            const library_artist_info_t *a = g_ptr_array_index(dp_search->artists, i);
            if (g_ascii_strcasecmp(a->name, "Daft Punk") == 0)
                dp_artist_count++;
        }
    }
    cr_assert_eq(dp_artist_count,
                 1,
                 "Search 'Daft Punk' should return 1 artist (deduped), got %d",
                 dp_artist_count);
    library_search_results_free(dp_search);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 2: User tags a library with Picard
 *
 * "I ran Picard on my elicb_music/BRONSON folder. Picard used Lookup
 *  (folder name → SOLR search) and wrote MusicBrainz tags. I re-index.
 *  Now the BRONSON album should be properly resolved and the 'Bronson'
 *  track credit on the ODESZA album should merge with the album artist."
 *
 * Setup: index lib_a (messy), then add MB tags to BRONSON files,
 *        re-index lib_a, also index lib_b. Verify merged state.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
story_picard_setup(void)
{
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    /* In HTTP mode pg/solr are NULL → resolver dispatches to HTTP backend.
     * In PG mode both must be set; the public Solr-equivalent isn't reachable. */
    if (!quad_test_use_http()) {
        if (!pg)
            cr_skip("MB_PG_PASSWORD not set");
        if (!solr)
            cr_skip("MB_SOLR_URL not set");
    }

    story_common_paths_init();
    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    /* Prod-parity cache + initial scan of lib_a only. lib_a is messy
     * (BRONSON tag-less). lib_b is left for later so the retag-then-scan
     * cycle happens against the initial state, mirroring: "user tags
     * their elicb library with Picard, Quadrature is already running
     * and watching lib_a's folder". */
    setup_prod_cache();
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/false, pg, solr);

    /* Simulate Picard: overwrite BRONSON FLACs with Picard-tagged versions. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON (2020)", lib_a_root);
    for (int i = 0; i < BRONSON_TRACK_COUNT; i++) {
        char tracknum[32], title_tag[256];
        g_autofree char *fpath
            = g_strdup_printf("%s/%02d - %s (FLAC 828 kbps).flac", path, i + 1, BRONSON_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", BRONSON_TRACKS[i]);
        const char *tags[] = { title_tag,
                               "artist=BRONSON",
                               "album=BRONSON",
                               "album_artist=BRONSON",
                               tracknum,
                               "date=2020",
                               "genre=Electronic",
                               "MUSICBRAINZ_ALBUMID=" MBID_BRONSON_RELEASE,
                               "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST,
                               "MUSICBRAINZ_RELEASEGROUPID=" MBID_BRONSON_RELEASE_GROUP,
                               "MUSICBRAINZ_ALBUMARTISTID=" MBID_BRONSON_ARTIST,
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, BRONSON_DURATIONS[i]), 0);
    }

    /* Re-scan lib_a (picks up new tags) and scan lib_b — both with the
     * refresh pump so the cache sees every library-updated event. */
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/true, pg, solr);
}

Test(e2e,
     user_tags_library_with_picard,
     .init = story_picard_setup,
     .fini = story_teardown,
     .timeout = 300)
{
    /* After Picard tagging + re-index, BRONSON in lib_a should now be resolved */

    /* ── lib_a BRONSON should now have RGID ────────────────────────── */

    GPtrArray *albums_a
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    bool found_resolved = false;
    for (guint i = 0; i < albums_a->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a, i);
        if (strstr(a->title, "BRONSON") != NULL) {
            if (a->musicbrainz_release_group_id && a->musicbrainz_release_group_id[0]) {
                found_resolved = true;
                cr_assert_str_eq(a->musicbrainz_release_group_id,
                                 MBID_BRONSON_RELEASE_GROUP,
                                 "After Picard, BRONSON RGID should match");
            }
        }
    }
    cr_assert(found_resolved, "After Picard tagging, lib_a BRONSON should have release_group_id");
    g_ptr_array_unref(albums_a);

    /* ── Cross-library dedup should now work ────────────────────────── */

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    /* Both lib_a and lib_b have BRONSON with same RGID → dedup to 1 */
    cr_assert_eq(count_album_title(albums, "BRONSON"),
                 1,
                 "After Picard, BRONSON should be deduped to 1 in ALL view");
    g_ptr_array_unref(albums);

    /* ── BRONSON artist should be merged with MBID ─────────────────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    cr_assert_eq(count_artist_name(artists, "BRONSON"),
                 1,
                 "After Picard, BRONSON should be 1 artist in ALL view");

    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    cr_assert_not_null(bronson);
    cr_assert_str_eq(
        bronson->musicbrainz_id, MBID_BRONSON_ARTIST, "BRONSON MBID should match after Picard");

    /* NOTE: "Bronson" track credit on ODESZA tracks does NOT merge here —
     * the ODESZA album hasn't been MB-resolved yet, so track credits still
     * have no MBID. Story 6 tests the merge after ODESZA gets resolved. */

    g_ptr_array_unref(artists);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 3: User moves an album between libraries
 *
 * "I deleted ~/elicb_music/BRONSON and moved it to ~/Music/BRONSON,
 *  then tagged it with Picard. The old library's BRONSON relics should
 *  not appear, and the new copy should dedup with existing lib_b data."
 *
 * Setup: index both, then delete BRONSON from lib_a, re-index lib_a,
 *        refresh cache. Verify orphan handling.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(e2e,
     user_moves_album_between_libraries,
     .init = story_import_setup,
     .fini = story_teardown,
     .timeout = 300)
{
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();

    /* Delete BRONSON from lib_a (simulates user moving it to lib_b) */
    char bronson_dir[512];
    snprintf(bronson_dir, sizeof(bronson_dir), "%s/BRONSON", lib_a_root);
    rm_rf(bronson_dir);

    /* Re-scan lib_a with prod parity — pump_until_indexers_done wires
     * INDEXER_LIBRARY_UPDATED → library_cache_refresh_slot (same chain
     * indexer_bridge.c:660 runs in the UI). No separate refresh_slot
     * call needed; the pump handles it. */
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/false, pg, solr);

    /* ── Orphan BRONSON album should be gone from lib_a ────────────── */

    GPtrArray *albums_a
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    cr_assert_eq(count_album_title_prefix(albums_a, "BRONSON"),
                 0,
                 "After deleting BRONSON folder from lib_a, no BRONSON album should "
                 "remain in lib_a view (orphan rows should be pruned)");
    g_ptr_array_unref(albums_a);

    /* ── ALL view should show exactly 1 BRONSON (from lib_b) ───────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    int bronson_count = count_artist_name(artists, "BRONSON");
    if (!bronson_count)
        bronson_count = count_artist_name(artists, "Bronson");
    cr_assert_eq(bronson_count,
                 1,
                 "After move, BRONSON should appear once in ALL view (from lib_b only), "
                 "got %d (orphan artist from lib_a leaking)",
                 bronson_count);
    g_ptr_array_unref(artists);

    /* ── ODESZA tracks still in lib_a, "Bronson" credit still there ── */

    GPtrArray *odesza_artists
        = library_cache_get_artists_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    int odesza = count_artist_name(odesza_artists, "ODESZA");
    if (!odesza)
        odesza = count_artist_name(odesza_artists, "Odesza");
    cr_assert(odesza >= 1, "ODESZA should still be in lib_a");
    g_ptr_array_unref(odesza_artists);

    /* ── No crashes navigating ─────────────────────────────────────── */

    GPtrArray *all_albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    for (guint i = 0; i < all_albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(all_albums, i);
        GPtrArray *t = library_cache_get_tracks_by_album(cache, a->album_id, LIBRARY_MASK_ALL);
        if (t) {
            for (guint j = 0; j < t->len; j++) {
                const library_track_info_t *track = g_ptr_array_index(t, j);
                char *p = library_cache_resolve_track_path(cache, track->track_id);
                cr_assert_not_null(p, "Track path should resolve");
                g_free(p);
            }
            g_ptr_array_unref(t);
        }
    }
    g_ptr_array_unref(all_albums);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 4: User deletes album folder and re-indexes
 *
 * "I deleted a folder from my library and hit refresh. The album and its
 *  tracks should disappear from the library view."
 *
 * Setup: index both, delete the ODESZA folder from lib_a, re-index,
 *        refresh cache. Verify the album and its tracks are gone.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(e2e,
     user_deletes_album_folder,
     .init = story_import_setup,
     .fini = story_teardown,
     .timeout = 300)
{
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();

    /* Verify ODESZA album exists before deletion */
    GPtrArray *before
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    bool had_odesza = false;
    for (guint i = 0; i < before->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(before, i);
        if (strstr(a->title, "Last Goodbye"))
            had_odesza = true;
    }
    cr_assert(had_odesza, "ODESZA album should exist before deletion");
    g_ptr_array_unref(before);

    /* Delete ODESZA from lib_a */
    char odesza_dir[512];
    snprintf(odesza_dir, sizeof(odesza_dir), "%s/ODESZA", lib_a_root);
    rm_rf(odesza_dir);

    /* Re-scan lib_a with prod-parity refresh pump. */
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/false, pg, solr);

    /* ── ODESZA album should be gone from lib_a ────────────────────── */

    GPtrArray *albums_a
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    for (guint i = 0; i < albums_a->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a, i);
        cr_assert(strstr(a->title, "Last Goodbye") == NULL,
                  "ODESZA album should not appear after folder deletion, "
                  "but found '%s' (orphan rows persisting)",
                  a->title);
    }
    g_ptr_array_unref(albums_a);

    /* ── Verify Phase 6 artist rename: "Bronson" → "BRONSON" ─────── */

    GPtrArray *artists_a
        = library_cache_get_artists_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);

    /* Phase 2 created "Bronson" (no MBID) from the ODESZA track credit split.
     * Phase 6 resolved the ODESZA album via SOLR and called
     * db_get_or_create_artist("BRONSON", mbid) — Step 2 (name NOCASE)
     * found the Phase 2 "Bronson" row and rename_artist_inplace() updated it
     * to "BRONSON" with MBID. After deleting ODESZA, the BRONSON album still
     * references this artist → it persists. There should be exactly 1 entry
     * matching "BRONSON" (the merged result), not a separate "Bronson". */
    cr_assert_eq(count_artist_name(artists_a, "BRONSON"),
                 1,
                 "BRONSON should still be in lib_a — Phase 6 renamed 'Bronson' → 'BRONSON' "
                 "with MBID, and the BRONSON album still references it");

    /* ODESZA artist should also be gone if their only album was deleted */
    int odesza_a = count_artist_name(artists_a, "ODESZA");
    if (!odesza_a)
        odesza_a = count_artist_name(artists_a, "Odesza");
    cr_assert_eq(
        odesza_a, 0, "ODESZA should be pruned from lib_a after their only album is deleted");
    g_ptr_array_unref(artists_a);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 5: User removes a library entirely
 *
 * "I removed my elicb_music library from quadrature settings. Only the
 *  Music library should remain visible. No crashes, no stale data."
 *
 * Setup: index both, remove lib_a slot from cache.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(e2e, user_removes_library, .init = story_import_setup, .fini = story_teardown, .timeout = 300)
{
    /* Verify both libraries visible before removal */
    GPtrArray *before = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert(before->len >= 2, "Should have artists from both libraries");
    g_ptr_array_unref(before);

    /* Remove lib_a slot */
    cr_assert_eq(library_cache_remove_slot(cache, 0), QUADRATURE_OK);

    /* ── Only lib_b data visible ───────────────────────────────────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    /* BRONSON from lib_b should still be there */
    cr_assert(count_artist_name(artists, "BRONSON") >= 1,
              "BRONSON should still be visible from lib_b");

    /* No "Unknown Artist" or orphan junk from lib_a */
    cr_assert_eq(count_artist_name(artists, "Unknown Artist"),
                 0,
                 "No 'Unknown Artist' should remain after lib_a removal");

    /* ODESZA was only in lib_a — should be gone */
    int odesza = count_artist_name(artists, "ODESZA");
    if (!odesza)
        odesza = count_artist_name(artists, "Odesza");
    cr_assert_eq(odesza, 0, "ODESZA (only in lib_a) should not appear after lib_a removal");

    g_ptr_array_unref(artists);

    /* ── Daft Punk still visible (was in lib_b) ────────────────────── */

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    bool found_ram = false;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strncasecmp(a->title, "Random Access Memories", 22) == 0)
            found_ram = true;
    }
    cr_assert(found_ram, "RAM from lib_b should still be visible");
    g_ptr_array_unref(albums);

    /* ── Search still works ────────────────────────────────────────── */

    library_search_results_t *results = library_cache_search(
        cache, "Daft Punk", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);
    cr_assert(results->artists && results->artists->len >= 1,
              "Search 'Daft Punk' should still work after lib_a removal");
    library_search_results_free(results);

    /* ── Clean destroy — ASan catches leaks ────────────────────────── */
    library_cache_destroy(cache);
    cache = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 6: MB resolution updates featured artist credits
 *
 * "I tagged my ODESZA live album with Picard and re-indexed. Tracks 17-18
 *  were originally credited 'Bronson/Odesza' (no MBID). After MB resolution,
 *  Phase 6 replaces those credits with 'BRONSON' (MBID) from MB recording
 *  data. Now the BRONSON from ODESZA track credits merges with the BRONSON
 *  artist from lib_b."
 *
 * Setup: index both libraries (lib_a messy, lib_b Picard-tagged), then add
 *        MB tags to the ODESZA files in lib_a (simulating Picard Lookup),
 *        re-index lib_a, refresh cache. Verify artist credit merge.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
story_mb_credits_setup(void)
{
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    /* In HTTP mode pg/solr are NULL → resolver dispatches to HTTP backend.
     * In PG mode both must be set; the public Solr-equivalent isn't reachable. */
    if (!quad_test_use_http()) {
        if (!pg)
            cr_skip("MB_PG_PASSWORD not set");
        if (!solr)
            cr_skip("MB_SOLR_URL not set");
    }

    story_common_paths_init();
    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    /* Round 1: prod-parity cache + initial concurrent scan. lib_a's ODESZA
     * album is tag-less; Phase 5/6 must resolve it via SOLR. */
    setup_prod_cache();
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/true, pg, solr);

    /* Simulate Picard retagging the ODESZA album in lib_a. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/ODESZA/The Last Goodbye Tour Live", lib_a_root);

    struct {
        int num;
        const char *title;
        const char *artist;
        int dur;
    } odesza_tracks[] = {
        { 1, "This Version Of You (Live)", "Odesza", 186 },
        { 2, "Behind the Sun (Live)", "Odesza", 190 },
        { 3, "Wide Awake (Live)", "Odesza", 202 },
        { 17, "TENSE (Live)", "Bronson/Odesza", 194 },
        { 18, "Keep Moving (Live)", "Bronson/Odesza", 142 },
    };

    for (size_t i = 0; i < sizeof(odesza_tracks) / sizeof(odesza_tracks[0]); i++) {
        char tracknum[32], title_tag[256];
        g_autofree char *fpath = g_strdup_printf(
            "%s/%02d - %s.flac", path, odesza_tracks[i].num, odesza_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", odesza_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", odesza_tracks[i].title);

        char artist_tag[256], aa_tag[256];
        snprintf(artist_tag, sizeof(artist_tag), "artist=%s", odesza_tracks[i].artist);
        snprintf(aa_tag, sizeof(aa_tag), "album_artist=%s", odesza_tracks[i].artist);

        const char *tags[] = { title_tag,
                               artist_tag,
                               aa_tag,
                               "album=The Last Goodbye Tour Live",
                               tracknum,
                               "date=2024",
                               "genre=Electronic",
                               "MUSICBRAINZ_ALBUMID=" MBID_ODESZA_LIVE_RELEASE,
                               "MUSICBRAINZ_ARTISTID=" MBID_ODESZA_ARTIST,
                               "MUSICBRAINZ_RELEASEGROUPID=" MBID_ODESZA_LIVE_RELEASE_GROUP,
                               "MUSICBRAINZ_ALBUMARTISTID=" MBID_ODESZA_ARTIST,
                               NULL };
        cr_assert_eq(create_flac(fpath, tags, odesza_tracks[i].dur), 0);
    }

    /* Round 2: re-scan lib_a only. Phase 2 picks up the new MUSICBRAINZ_*
     * tags, Phase 6 fetches canonical credits from PG. Each
     * LIBRARY_UPDATED event fires another library_cache_refresh_slot via
     * pump_until_indexers_done. */
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/false, pg, solr);
}

Test(e2e,
     mb_resolution_updates_featured_credits,
     .init = story_mb_credits_setup,
     .fini = story_teardown,
     .timeout = 300)
{
    /* ── Phase 6 should have replaced "Bronson" with "BRONSON" (MBID) ─ */

    /* After MB resolution, the ODESZA album's track credits are sourced from
     * MB recording data. Tracks 17-18 should now credit "BRONSON" with the
     * correct MBID instead of the Phase 2 "Bronson" (no MBID). */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    /* BRONSON should appear exactly once — the Phase 2 "Bronson" (no MBID)
     * was replaced by Phase 6's "BRONSON" (MBID), which merges with lib_b's
     * BRONSON. No duplicate "Bronson" vs "BRONSON" in ALL view. */
    int bronson_count = count_artist_name(artists, "BRONSON");
    if (!bronson_count)
        bronson_count = count_artist_name(artists, "Bronson");
    cr_assert_eq(bronson_count,
                 1,
                 "After MB resolution of ODESZA album, BRONSON should appear as 1 artist "
                 "(Phase 6 replaced 'Bronson' credit with 'BRONSON' + MBID)");

    /* Find BRONSON and verify it has the correct MBID */
    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    if (!bronson)
        bronson = find_artist(artists, "Bronson");
    cr_assert_not_null(bronson, "BRONSON artist not found after MB resolution");
    cr_assert(bronson->musicbrainz_id != NULL && bronson->musicbrainz_id[0],
              "BRONSON should have MBID after Phase 6 rewrote track credits");
    cr_assert_str_eq(
        bronson->musicbrainz_id, MBID_BRONSON_ARTIST, "BRONSON MBID should be correct");

    int64_t bronson_id = bronson->artist_id;

    /* ── BRONSON should have appearance tracks from ODESZA album ───── */

    GPtrArray *appearances
        = library_cache_get_artist_appearance_tracks(cache, bronson_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(appearances,
                       "BRONSON should have appearance tracks after ODESZA MB resolution — "
                       "Phase 6 should have written BRONSON (MBID) as track artist for "
                       "tracks 17-18, enabling the appearance lookup");
    cr_assert(appearances->len >= 2,
              "BRONSON should appear on >= 2 ODESZA tracks (TENSE, KEEP MOVING), "
              "got %u",
              appearances->len);
    g_ptr_array_unref(appearances);

    /* ── ODESZA should also have MBID ──────────────────────────────── */

    const library_artist_info_t *odesza = find_artist(artists, "ODESZA");
    if (!odesza)
        odesza = find_artist(artists, "Odesza");
    cr_assert_not_null(odesza, "ODESZA should exist");
    cr_assert(odesza->musicbrainz_id != NULL && odesza->musicbrainz_id[0],
              "ODESZA should have MBID after album resolution");

    g_ptr_array_unref(artists);

    /* ── Verify ODESZA album has release_group_id ──────────────────── */

    GPtrArray *albums
        = library_cache_get_albums_filtered(cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums);
    bool found_odesza_album = false;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (strstr(a->title, "Last Goodbye") != NULL) {
            found_odesza_album = true;
            cr_assert(a->musicbrainz_release_group_id != NULL && a->musicbrainz_release_group_id[0],
                      "ODESZA live album should have release_group_id after resolution");
            break;
        }
    }
    cr_assert(found_odesza_album, "ODESZA live album should be in lib_a");
    g_ptr_array_unref(albums);

    /* ── Search for "Bronson" should return 1 artist ──────────────── */

    library_search_results_t *results = library_cache_search(
        cache, "Bronson", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);
    int search_artists = 0;
    if (results->artists) {
        for (guint i = 0; i < results->artists->len; i++) {
            const library_artist_info_t *a = g_ptr_array_index(results->artists, i);
            if (g_ascii_strcasecmp(a->name, "BRONSON") == 0
                || g_ascii_strcasecmp(a->name, "Bronson") == 0)
                search_artists++;
        }
    }
    cr_assert_eq(search_artists,
                 1,
                 "Search 'Bronson' should return 1 artist after MB credit resolution, "
                 "got %d (Phase 2 'Bronson' orphan leaking)",
                 search_artists);
    library_search_results_free(results);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 7: Fresh `make db-clean && make debug` — cross-library MBID merge
 *
 * PROD REPRO captured 2026-04-16 against the live repo state:
 *
 *   $ sqlite3 ~/Music/quadrature.sqlite \
 *       "SELECT id, name, musicbrainz_id FROM artists WHERE name LIKE '%bronson%' COLLATE NOCASE"
 *     → 2028 | BRONSON | 887b5b46-3f15-4475-b2bd-4d026c2b2031
 *
 *   $ sqlite3 ~/elicb_music/quadrature.sqlite \
 *       "SELECT id, name, musicbrainz_id FROM artists WHERE name LIKE '%bronson%' COLLATE NOCASE"
 *     → 98   | BRONSON | 887b5b46-3f15-4475-b2bd-4d026c2b2031
 *
 *   Elicb DB: ODESZA/The Last Goodbye Tour Live (album_id=46, mb_status=2),
 *   tracks 616 (TENSE) and 617 (KEEP MOVING) correctly linked via
 *   track_artists → artists.id=98 → mbid 887b5b46… The indexer resolved
 *   everything correctly; both DBs have one BRONSON row with the same MBID.
 *
 *   UI SHOWS (screenshot): two artist cards for "Bronson" search:
 *     1. "BRONSON" (uppercase, Music badge only, 1 album, has artwork)
 *     2. "Bronson" (case-variant, Elicb badge only, 1 album · appears on
 *        2 tracks, no artwork)
 *
 *   Expected: ONE merged entity, MBID-attached, both library badges,
 *   1+1 albums, 2 appearances.
 *
 * This story reproduces the above. It SHOULD FAIL today on the search-result
 * and filtered-list assertions (library_cache cross-library MBID dedup bug).
 * When fixed, the merged entity must also expose the 2 appearance tracks
 * AND be reachable from both library masks.
 *
 * Difference from story 6: no retag-then-reindex shortcut. Single fresh
 * index of each library — the only path that matches `make db-clean &&
 * make debug`.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* (prod_trackers / prod_indexer_a / prod_indexer_b declared near top-level
 *  fixture statics so story_teardown can reach them.) */

static void
story_fresh_db_clean_setup(void)
{
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    /* In HTTP mode pg/solr are NULL → resolver dispatches to HTTP backend.
     * In PG mode both must be set; the public Solr-equivalent isn't reachable. */
    if (!quad_test_use_http()) {
        if (!pg)
            cr_skip("MB_PG_PASSWORD not set");
        if (!solr)
            cr_skip("MB_SOLR_URL not set");
    }

    story_common_paths_init();
    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    /* Prod-parity sequence — bootstrap empty DBs, create cache, warm async,
     * then run concurrent indexers with INDEXER_LIBRARY_UPDATED →
     * library_cache_refresh_slot pumping. Exactly the chain prod runs on
     * `make db-clean && make debug`. */
    setup_prod_cache();
    run_prod_indexers(/*scan_a=*/true, /*scan_b=*/true, pg, solr);

    /* ── COW-refresh bug repro ─────────────────────────────────────────────
     * After the two-refresh sequence, every MB-resolved artist in the cache
     * must carry its canonical MBID — matching what Phase 6 wrote to the DB.
     *
     * Any artist found here with NULL musicbrainz_id is a stale entity:
     * Phase 2 created the row with no MBID, the first COW refresh captured
     * it, Phase 6 renamed-in-place in the DB (same row id, new name/MBID),
     * and the second COW refresh then:
     *   - SEED (library_cache.c:1742-1746) blindly RC_ACQUIRE'd the stale
     *     entity from the old slot into the shadow, and
     *   - DELTA's on_warm_artist (library_cache.c:468) short-circuited the
     *     DB re-read because the shadow already had an entity at that id.
     * The fresh DB row never reached the cache — the zombie survives. */
    for (int slot_idx = 0; slot_idx < 2; slot_idx++) {
        GPtrArray *a = library_cache_get_artists_filtered(
            cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, (1u << slot_idx));
        cr_assert_not_null(a);

        /* Dump every artist visible in this slot so the failing assertion
         * below has the full picture in the test log. */
        g_printerr("=== slot %d (%s): %u artists ===\n",
                   slot_idx,
                   slot_idx == 0 ? "Elicb" : "Music",
                   a->len);
        int stale_count = 0;
        const library_artist_info_t *first_stale = NULL;
        for (guint i = 0; i < a->len; i++) {
            const library_artist_info_t *x = g_ptr_array_index(a, i);
            bool has_mbid = x->musicbrainz_id && x->musicbrainz_id[0];
            g_printerr("  [gid=%" G_GINT64_FORMAT "] name='%s' mbid='%s'\n",
                       x->artist_id,
                       x->name,
                       has_mbid ? x->musicbrainz_id : "(NULL — STALE)");
            /* The MB-resolved tag-less libraries here (ODESZA's A Moment
             * Apart, The Last Goodbye Tour Live, BRONSON) pull every artist
             * from MB recording data — which always carries an MBID. So
             * any NULL mbid on an album_count>0 or track_count>0 entity
             * is the zombie we're hunting for. album_count/track_count
             * both zero would be a harmless ghost (no DB references left). */
            if (!has_mbid && (x->album_count > 0 || x->track_count > 0)) {
                stale_count++;
                if (!first_stale)
                    first_stale = x;
            }
        }
        g_ptr_array_unref(a);

        cr_assert_eq(stale_count,
                     0,
                     "BUG REPRO — slot %d has %d stale artist entities with NULL MBID "
                     "but live album/track references. First stale: "
                     "name='%s' gid=%" G_GINT64_FORMAT ". "
                     "Root cause: library_cache.c:1742-1746 seeds artists from the old "
                     "slot unconditionally via RC_ACQUIRE (no change-set gate for "
                     "artists); library_cache.c:468 then skips the DB re-read because "
                     "the shadow is already populated. When Phase 6 renames an artist "
                     "in place (db_write.c:278 rename_artist_inplace), the DB row gets "
                     "the canonical name + MBID but the cache entity retains the "
                     "Phase-2 name/NULL-MBID forever. This is the cross-library merge "
                     "failure from the prod screenshot, reproduced via the full "
                     "indexer_scan → INDEXER_LIBRARY_UPDATED → library_cache_refresh_slot "
                     "chain the UI runs.",
                     slot_idx,
                     stale_count,
                     first_stale ? first_stale->name : "(none)",
                     first_stale ? first_stale->artist_id : 0);
    }
}

Test(e2e,
     fresh_db_clean_reindex_merges_cross_library_artists,
     .init = story_fresh_db_clean_setup,
     .fini = story_teardown,
     .timeout = 300)
{
    /* ── Precondition sanity: both DBs resolved BRONSON with the same MBID ── */

    quadrature_db_t *db_a = NULL, *db_b = NULL;
    char db_a_path[512], db_b_path[512];
    snprintf(db_a_path, sizeof(db_a_path), "%s/quadrature.sqlite", lib_a_data);
    snprintf(db_b_path, sizeof(db_b_path), "%s/quadrature.sqlite", lib_b_data);
    cr_assert_eq(db_open(db_a_path, true, &db_a), QUADRATURE_OK);
    cr_assert_eq(db_open(db_b_path, true, &db_b), QUADRATURE_OK);

    /* If this precondition fails, the indexer (not the cache) is the culprit
     * and the cache assertions below are moot. */
    /* [Precondition assertions via direct sqlite would require a helper; rely
     * on the cache-backed library_artist_info_t check instead — both slots
     * should expose exactly one BRONSON row with MBID populated.] */

    db_close(db_a);
    db_close(db_b);

    /* ── BUG REPRO #1: cross-library MBID dedup in filtered artist list ── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    int bronson_entities = 0;
    const library_artist_info_t *first_bronson = NULL;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, "BRONSON") == 0
            || g_ascii_strcasecmp(a->name, "Bronson") == 0) {
            bronson_entities++;
            if (!first_bronson)
                first_bronson = a;
        }
    }

    cr_assert_eq(bronson_entities,
                 1,
                 "BUG REPRO: library_cache_get_artists_filtered(MASK_ALL) returned %d "
                 "BRONSON entities. Prod shows two cards (\"BRONSON\" uppercase + "
                 "\"Bronson\" case-variant) even though both library DBs hold a single "
                 "artists row with the same MBID 887b5b46…. MBID dedup in the cache "
                 "merge path is not firing for this pair.",
                 bronson_entities);

    cr_assert_not_null(first_bronson, "BRONSON entity missing from cache");
    cr_assert(first_bronson->musicbrainz_id && first_bronson->musicbrainz_id[0],
              "Merged BRONSON entity must carry the MBID — without it, artist "
              "artwork lookup (keyed by UUID) returns the no-art sentinel and "
              "the UI renders a blank placeholder (as seen in the screenshot).");
    cr_assert_str_eq(first_bronson->musicbrainz_id, MBID_BRONSON_ARTIST);

    g_ptr_array_unref(artists);

    /* ── BUG REPRO #2: same bug surfaced through library_cache_search ── */

    /* The search view is what the screenshot shows — typing "BRONSON" into
     * the top search bar. library_cache_search must apply the same
     * cross-library MBID dedup. */
    library_search_results_t *results = library_cache_search(
        cache, "BRONSON", LIBRARY_SEARCH_FILTER_ARTISTS, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);
    cr_assert_not_null(results->artists);

    int search_bronson = 0;
    for (guint i = 0; i < results->artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(results->artists, i);
        if (g_ascii_strcasecmp(a->name, "BRONSON") == 0
            || g_ascii_strcasecmp(a->name, "Bronson") == 0)
            search_bronson++;
    }

    cr_assert_eq(search_bronson,
                 1,
                 "BUG REPRO: library_cache_search(\"BRONSON\", MASK_ALL) returned "
                 "%d BRONSON results. Screenshot case: search should collapse the "
                 "two per-library BRONSON rows into one entity via MBID dedup.",
                 search_bronson);

    library_search_results_free(results);

    /* ── Downstream expectations once the merge works ──────────────── */
    /* When the bug is fixed, the merged BRONSON entity must expose the
     * cross-library appearance credit (ODESZA's TENSE + KEEP MOVING tracks
     * from lib_a, while lib_b's Picard-tagged BRONSON contributes the album
     * ownership). Using the first_bronson artist_id captured above: */

    artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    if (!bronson)
        bronson = find_artist(artists, "Bronson");
    cr_assert_not_null(bronson);
    int64_t bronson_gid = bronson->artist_id;
    g_ptr_array_unref(artists);

    GPtrArray *appearances
        = library_cache_get_artist_appearance_tracks(cache, bronson_gid, LIBRARY_MASK_ALL);
    cr_assert_not_null(appearances,
                       "Merged BRONSON entity should expose 2 appearance tracks "
                       "(ODESZA TENSE + KEEP MOVING from lib_a, via track_artists).");
    cr_assert(appearances->len >= 2,
              "Expected >= 2 appearances on merged BRONSON, got %u",
              appearances->len);
    g_ptr_array_unref(appearances);
}
