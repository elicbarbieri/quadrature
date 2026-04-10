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
#include "quadrature/indexer.h"
#include "quadrature/database.h"
#include "quadrature/library.h"

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Known MBIDs from real Picard-tagged files
 * ═══════════════════════════════════════════════════════════════════════════ */

/* BRONSON (artist: ODESZA x Golden Features side project) */
#define MBID_BRONSON_ARTIST        "887b5b46-3f15-4475-b2bd-4d026c2b2031"
#define MBID_BRONSON_RELEASE       "5ed617d7-898f-4e05-82a1-bfc586a4b013"
#define MBID_BRONSON_RELEASE_GROUP "d95b8366-994d-448d-8689-422b20b6cabb"

/* Daft Punk — Random Access Memories */
#define MBID_DAFT_PUNK_ARTIST      "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define MBID_RAM_RELEASE           "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define MBID_RAM_RELEASE_GROUP     "aa997ea0-2936-40bd-884d-3af8a0e064dc"

/* ODESZA — The Last Goodbye Tour Live (digital release, 27 tracks) */
#define MBID_ODESZA_ARTIST              "2e222fce-02ae-4221-b1c6-3c3242b423b6"
#define MBID_ODESZA_LIVE_RELEASE        "c3f6f487-f59a-467f-94a8-0f006a5deaf4"
#define MBID_ODESZA_LIVE_RELEASE_GROUP  "2502f14d-0b97-4085-aba5-c62e1c166a65"

/* BRONSON track titles + durations (seconds, from MB release 5ed617d7) */
static const char *BRONSON_TRACKS[] = {
    "FOUNDATION", "HEART ATTACK", "BLINE", "KNOW ME", "VAULTS",
    "TENSE", "CALL OUT", "CONTACT", "KEEP MOVING", "DAWN"
};
static const int BRONSON_DURATIONS[] = {
    184, 209, 265, 180, 244, 200, 179, 207, 246, 443
};
#define BRONSON_TRACK_COUNT 10

/* RAM disc 1 track titles + durations (seconds, from MB release 5000a285) */
static const char *RAM_TRACKS[] = {
    "Give Life Back to Music", "The Game of Love", "Giorgio by Moroder",
    "Within", "Instant Crush", "Lose Yourself to Dance", "Touch",
    "Get Lucky", "Beyond", "Motherboard", "Fragments of Time",
    "Doin It Right", "Contact"
};
static const int RAM_DURATIONS[] = {
    274, 321, 544, 228, 337, 353, 498, 367, 290, 341, 279, 251, 381
};
#define RAM_TRACK_COUNT 13

/* ═══════════════════════════════════════════════════════════════════════════
 * Connection helpers (same pattern as test_mb_resolve.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *env_or(const char *name, const char *fallback) {
    const char *val = getenv(name);
    return (val && val[0]) ? val : fallback;
}

static const char *mb_pg_conninfo(void) {
    const char *pw = getenv("MB_PG_PASSWORD");
    if (!pw || !pw[0]) return NULL;
    static char buf[512];
    snprintf(buf, sizeof(buf),
             "host=%s dbname=%s user=%s password=%s connect_timeout=%s",
             env_or("MB_HOST", "localhost"),
             env_or("MB_DBNAME", "musicbrainz_db"),
             env_or("MB_USER", "musicbrainz"),
             pw,
             env_or("MB_PG_CONNECT_TIMEOUT", "5"));
    return buf;
}

static const char *mb_solr_url(void) {
    return env_or("MB_SOLR_URL", NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FLAC file generation
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

/**
 * Create a FLAC file with given metadata and duration.
 * Uses a 60Hz sine wave at 8kHz mono to keep files tiny (~1KB/sec).
 * duration_secs=0 defaults to 0.5s (for tag-less filler files).
 */
static int create_flac(const char *path, const char *const *metadata_pairs,
                        int duration_secs) {
    char cmd[8192];
    double dur = duration_secs > 0 ? (double)duration_secs : 0.5;
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
static void build_lib_a(const char *root) {
    char path[1024], tracknum[32], title_tag[256];

    /* BRONSON — tag-less files (the real elicb_music pattern).
     * Duration matches the real album so SOLR scoring picks the right release. */
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON (2020)", root);
    mkdirs(path);
    for (int i = 0; i < BRONSON_TRACK_COUNT; i++) {
        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s (FLAC 828 kbps).flac",
                 path, i + 1, BRONSON_TRACKS[i]);
        const char *tags[] = { "date=2020", "genre=Electronic", NULL };
        cr_assert_eq(create_flac(fpath, tags, BRONSON_DURATIONS[i]), 0,
                     "Failed to create: %s", fpath);
    }

    /* ODESZA — basic tags, tracks 17-18 credit Bronson */
    snprintf(path, sizeof(path), "%s/ODESZA/The Last Goodbye Tour Live", root);
    mkdirs(path);

    struct { int num; const char *title; const char *artist; int dur; } odesza_tracks[] = {
        { 1,  "This Version Of You (Live)", "Odesza",        186 },
        { 2,  "Behind the Sun (Live)",      "Odesza",        190 },
        { 3,  "Wide Awake (Live)",          "Odesza",        202 },
        { 17, "TENSE (Live)",               "Bronson/Odesza", 194 },
        { 18, "Keep Moving (Live)",         "Bronson/Odesza", 142 },
    };

    for (size_t i = 0; i < sizeof(odesza_tracks)/sizeof(odesza_tracks[0]); i++) {
        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac",
                 path, odesza_tracks[i].num, odesza_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", odesza_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", odesza_tracks[i].title);

        char artist_tag[256], aa_tag[256];
        snprintf(artist_tag, sizeof(artist_tag), "artist=%s", odesza_tracks[i].artist);
        snprintf(aa_tag, sizeof(aa_tag), "album_artist=%s", odesza_tracks[i].artist);

        const char *tags[] = {
            title_tag, artist_tag, aa_tag,
            "album=The Last Goodbye Tour Live",
            tracknum, "date=2024", "genre=Electronic", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, odesza_tracks[i].dur), 0);
    }

    /* Daft Punk — basic tags, multi-disc, no MB */
    snprintf(path, sizeof(path),
             "%s/Daft Punk/Random Access Memories (2013)/CD 01", root);
    mkdirs(path);
    for (int i = 0; i < RAM_TRACK_COUNT; i++) {
        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac", path, i + 1, RAM_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", RAM_TRACKS[i]);
        const char *tags[] = {
            title_tag, "artist=Daft Punk",
            "album=Random Access Memories (10th Anniversary Edition)",
            "album_artist=Daft Punk", tracknum,
            "disc=1", "date=2023", "genre=Dance", NULL
        };
        cr_assert_eq(create_flac(fpath, tags, RAM_DURATIONS[i]), 0);
    }
}

/**
 * Build lib_b: Music/ pattern (Picard-tagged, full MusicBrainz data).
 *
 * BRONSON/BRONSON/ — full Picard tags
 * Daft Punk/Random Access Memories/ — full Picard tags
 */
static void build_lib_b(const char *root) {
    char path[1024], tracknum[32], title_tag[256];

    /* BRONSON — full Picard tags */
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON", root);
    mkdirs(path);
    for (int i = 0; i < BRONSON_TRACK_COUNT; i++) {
        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%02d BRONSON - %s.flac",
                 path, i + 1, BRONSON_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", BRONSON_TRACKS[i]);
        const char *tags[] = {
            title_tag, "artist=BRONSON", "album=BRONSON", "album_artist=BRONSON",
            tracknum, "date=2020", "genre=Electronic",
            "MUSICBRAINZ_ALBUMID=" MBID_BRONSON_RELEASE,
            "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST,
            "MUSICBRAINZ_RELEASEGROUPID=" MBID_BRONSON_RELEASE_GROUP,
            "MUSICBRAINZ_ALBUMARTISTID=" MBID_BRONSON_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, BRONSON_DURATIONS[i]), 0);
    }

    /* Daft Punk RAM — full Picard tags */
    snprintf(path, sizeof(path), "%s/Daft Punk/Random Access Memories", root);
    mkdirs(path);
    for (int i = 0; i < RAM_TRACK_COUNT; i++) {
        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac", path, i + 1, RAM_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", RAM_TRACKS[i]);
        const char *tags[] = {
            title_tag, "artist=Daft Punk", "album=Random Access Memories",
            "album_artist=Daft Punk", tracknum, "disc=1", "date=2023", "genre=Dance",
            "MUSICBRAINZ_ALBUMID=" MBID_RAM_RELEASE,
            "MUSICBRAINZ_ARTISTID=" MBID_DAFT_PUNK_ARTIST,
            "MUSICBRAINZ_RELEASEGROUPID=" MBID_RAM_RELEASE_GROUP,
            "MUSICBRAINZ_ALBUMARTISTID=" MBID_DAFT_PUNK_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, RAM_DURATIONS[i]), 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Indexer helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int started, progress, completed, library_updated;
    bool success;
} test_tracker_t;

static void test_callback(indexer_event_t event, const indexer_progress_t *progress,
                           void *user_data) {
    (void)progress;
    test_tracker_t *t = user_data;
    switch (event) {
        case INDEXER_STARTED:         t->started++; break;
        case INDEXER_PROGRESS:        t->progress++; break;
        case INDEXER_COMPLETED:       t->completed++; t->success = true; break;
        case INDEXER_LIBRARY_UPDATED: t->library_updated++; break;
        case INDEXER_CANCELLED:       t->completed++; break;
        case INDEXER_ERROR:           t->completed++; break;
        case INDEXER_ARTWORK_UPDATED: break;
    }
}

static void index_library(const char *library_root, const char *data_root,
                           const char *pg, const char *solr) {
    test_tracker_t tracker = {0};
    indexer_config_t config = {
        .thread_count        = 2,
        .process_artwork     = false,
        .mb_resolve          = true,
        .pg_conninfo         = pg,
        .mb_solr_url         = solr,
        .acoustid_pg_conninfo = NULL,
        .acoustid_index_url   = NULL,
        .fetch_artist_art    = false,
        .fanart_api_key      = NULL,
        .fetch_artist_bios   = false,
        .callback            = test_callback,
        .user_data           = &tracker,
    };

    indexer_t *indexer = NULL;
    cr_assert_eq(indexer_create(&indexer, &config), QUADRATURE_OK);
    cr_assert_eq(indexer_scan(indexer, library_root, data_root), QUADRATURE_OK);
    indexer_wait(indexer);

    cr_assert(tracker.completed > 0, "Indexer did not complete for %s", library_root);
    cr_assert(tracker.success, "Indexer failed for %s", library_root);

    indexer_destroy(indexer);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cache query helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int count_artist_name(const GPtrArray *artists, const char *name) {
    int count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) count++;
    }
    return count;
}

static const library_artist_info_t *find_artist(const GPtrArray *artists,
                                                  const char *name) {
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) return a;
    }
    return NULL;
}

static int count_album_title(const GPtrArray *albums, const char *title) {
    int count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) count++;
    }
    return count;
}

static int count_album_title_prefix(const GPtrArray *albums, const char *prefix) {
    int count = 0;
    size_t len = strlen(prefix);
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strncasecmp(a->title, prefix, len) == 0) count++;
    }
    return count;
}

static int64_t find_artist_id_in_library(library_cache_t *c,
                                         const char *name, uint32_t mask) {
    GPtrArray *artists = library_cache_get_artists_filtered(
        c, LIBRARY_SORT_NAME_ASC, name, NULL, mask);
    if (!artists) return 0;
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

static void rm_rf(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared fixture state
 * ═══════════════════════════════════════════════════════════════════════════ */

static char lib_a_root[256];
static char lib_b_root[256];
static char lib_a_data[256];
static char lib_b_data[256];
static library_cache_t *cache = NULL;

#define MASK_A (1u << 0)
#define MASK_B (1u << 1)

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

static void story_import_setup(void) {
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    if (!pg)   cr_skip("MB_PG_PASSWORD not set");
    if (!solr) cr_skip("MB_SOLR_URL not set");

    pid_t pid = getpid();
    snprintf(lib_a_root, sizeof(lib_a_root), "/tmp/quad_e2e_%d_lib_a", pid);
    snprintf(lib_b_root, sizeof(lib_b_root), "/tmp/quad_e2e_%d_lib_b", pid);
    snprintf(lib_a_data, sizeof(lib_a_data), "/tmp/quad_e2e_%d_data_a", pid);
    snprintf(lib_b_data, sizeof(lib_b_data), "/tmp/quad_e2e_%d_data_b", pid);
    rm_rf(lib_a_root); rm_rf(lib_b_root);
    rm_rf(lib_a_data); rm_rf(lib_b_data);
    mkdirs(lib_a_data); mkdirs(lib_b_data);

    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    index_library(lib_a_root, lib_a_data, pg, solr);
    index_library(lib_b_root, lib_b_data, pg, solr);

    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", lib_a_data);

    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", lib_b_data);

    library_cache_source_t sources[2] = {
        { .db_path = db_a, .music_base = lib_a_root,
          .display_name = "Elicb Music", .bitmap_index = 0 },
        { .db_path = db_b, .music_base = lib_b_root,
          .display_name = "Music", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);
}

static void story_teardown(void) {
    if (cache) { library_cache_destroy(cache); cache = NULL; }
    rm_rf(lib_a_root); rm_rf(lib_b_root);
    rm_rf(lib_a_data); rm_rf(lib_b_data);
}

Test(e2e, user_imports_two_messy_libraries,
     .init = story_import_setup, .fini = story_teardown, .timeout = 300) {

    /* ── Artist dedup in ALL view ──────────────────────────────────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    /* BRONSON: lib_a has "Bronson" (ODESZA track credit, no MBID) + tag-less
     * BRONSON album ("Unknown Artist"). lib_b has "BRONSON" (full MBID).
     * After resolution, user expects 1 artist. */
    cr_assert_eq(count_artist_name(artists, "BRONSON"), 1,
        "BRONSON should appear as 1 artist in ALL view — track credit "
        "'Bronson' and tag-less album must merge with Picard-tagged 'BRONSON'");

    /* ODESZA: only in lib_a, should appear once */
    int odesza = count_artist_name(artists, "ODESZA");
    if (!odesza) odesza = count_artist_name(artists, "Odesza");
    cr_assert_eq(odesza, 1, "ODESZA should appear as 1 artist");

    /* Daft Punk: in both libraries, should be deduped to 1 */
    cr_assert_eq(count_artist_name(artists, "Daft Punk"), 1,
        "Daft Punk should appear as 1 artist (deduped across libraries)");

    /* ── Artist MBIDs ──────────────────────────────────────────────── */

    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    if (!bronson) bronson = find_artist(artists, "Bronson");
    cr_assert_not_null(bronson, "BRONSON artist not found");
    cr_assert(bronson->musicbrainz_id != NULL && bronson->musicbrainz_id[0],
        "BRONSON artist should have MBID after resolution");
    cr_assert_str_eq(bronson->musicbrainz_id, MBID_BRONSON_ARTIST);

    const library_artist_info_t *dp = find_artist(artists, "Daft Punk");
    cr_assert_not_null(dp, "Daft Punk not found");
    cr_assert(dp->musicbrainz_id != NULL && dp->musicbrainz_id[0],
        "Daft Punk should have MBID");
    cr_assert_str_eq(dp->musicbrainz_id, MBID_DAFT_PUNK_ARTIST);

    int64_t bronson_id = bronson->artist_id;
    g_ptr_array_unref(artists);

    /* ── Album dedup in ALL view ───────────────────────────────────── */

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    /* BRONSON album: lib_a has "BRONSON (2020)" (from folder, no RGID),
     * lib_b has "BRONSON" (Picard, with RGID). Should dedup to 1. */
    cr_assert_eq(count_album_title(albums, "BRONSON"), 1,
        "BRONSON album should appear once in ALL view (deduped by RGID)");

    /* RAM: lib_a has "Random Access Memories (10th Anniversary Edition)",
     * lib_b has "Random Access Memories". Same release_group_id → dedup to 1. */
    cr_assert_eq(count_album_title_prefix(albums, "Random Access Memories"), 1,
        "Random Access Memories should appear once (deduped across editions)");

    /* ── Album MBID fields ─────────────────────────────────────────── */

    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, "BRONSON") == 0) {
            cr_assert(a->musicbrainz_release_group_id != NULL,
                "BRONSON album should have release_group_id");
            cr_assert_str_eq(a->musicbrainz_release_group_id,
                MBID_BRONSON_RELEASE_GROUP);
            break;
        }
    }
    g_ptr_array_unref(albums);

    /* ── Tag-less BRONSON in lib_a resolved via SOLR ───────────────── */

    GPtrArray *albums_a = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    for (guint i = 0; i < albums_a->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a, i);
        if (strstr(a->title, "BRONSON") != NULL) {
            cr_assert(a->musicbrainz_release_group_id != NULL &&
                      a->musicbrainz_release_group_id[0] != '\0',
                "Tag-less BRONSON in lib_a should have been resolved by SOLR. "
                "release_group_id is NULL — folder-path fallback not working");
            break;
        }
    }
    g_ptr_array_unref(albums_a);

    /* ── BRONSON featured appearances ──────────────────────────────── */

    GPtrArray *appearances = library_cache_get_artist_appearance_tracks(
        cache, bronson_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(appearances,
        "BRONSON should have appearance tracks (credited on ODESZA album)");
    cr_assert(appearances->len >= 2,
        "BRONSON should appear on >= 2 ODESZA tracks (TENSE, KEEP MOVING), "
        "got %u", appearances->len);
    g_ptr_array_unref(appearances);

    /* ── Search ────────────────────────────────────────────────────── */

    library_search_results_t *results = library_cache_search(
        cache, "Bronson", LIBRARY_SEARCH_FILTER_ALL, 0, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(results);

    int search_artists = 0;
    if (results->artists) {
        for (guint i = 0; i < results->artists->len; i++) {
            const library_artist_info_t *a = g_ptr_array_index(results->artists, i);
            if (g_ascii_strcasecmp(a->name, "BRONSON") == 0 ||
                g_ascii_strcasecmp(a->name, "Bronson") == 0)
                search_artists++;
        }
    }
    cr_assert_eq(search_artists, 1,
        "Search 'Bronson' should return 1 artist, got %d", search_artists);

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

    GPtrArray *tracks = library_cache_get_tracks_by_album(
        cache, odesza_album_id, LIBRARY_MASK_ALL);
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

    GPtrArray *artists_a = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(artists_a);
    cr_assert(artists_a->len >= 1, "lib_a should have artists");
    g_ptr_array_unref(artists_a);

    GPtrArray *artists_b = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_B);
    cr_assert_not_null(artists_b);
    cr_assert(artists_b->len >= 1, "lib_b should have artists");
    g_ptr_array_unref(artists_b);

    /* ── Daft Punk deduped: in both libraries, shows once in ALL ───── */

    int64_t dp_id_a = find_artist_id_in_library(cache, "Daft Punk", MASK_A);
    int64_t dp_id_b = find_artist_id_in_library(cache, "Daft Punk", MASK_B);
    cr_assert(dp_id_a > 0, "Daft Punk should be in lib_a");
    cr_assert(dp_id_b > 0, "Daft Punk should be in lib_b");

    /* Both IDs should resolve to the same albums in ALL view */
    GPtrArray *dp_albums_a = library_cache_get_albums_by_artist(
        cache, dp_id_a, LIBRARY_MASK_ALL);
    GPtrArray *dp_albums_b = library_cache_get_albums_by_artist(
        cache, dp_id_b, LIBRARY_MASK_ALL);
    cr_assert_not_null(dp_albums_a);
    cr_assert_not_null(dp_albums_b);
    cr_assert_eq(dp_albums_a->len, dp_albums_b->len,
        "Both Daft Punk IDs should return same album count in ALL view");
    g_ptr_array_unref(dp_albums_a);
    g_ptr_array_unref(dp_albums_b);

    /* ── RAM deduped across editions (same RGID) ──────────────────── */

    GPtrArray *albums_a2 = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    bool found_ram_a = false;
    for (guint i = 0; i < albums_a2->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a2, i);
        if (g_ascii_strncasecmp(a->title, "Random Access Memories", 22) == 0)
            found_ram_a = true;
    }
    cr_assert(found_ram_a, "lib_a should have its own RAM edition");
    g_ptr_array_unref(albums_a2);

    GPtrArray *albums_b2 = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_B);
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
    cr_assert_eq(count_album_title_prefix(all_albums2, "Random Access Memories"), 1,
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
    cr_assert_eq(dp_artist_count, 1,
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

static void story_picard_setup(void) {
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    if (!pg)   cr_skip("MB_PG_PASSWORD not set");
    if (!solr) cr_skip("MB_SOLR_URL not set");

    pid_t pid = getpid();
    snprintf(lib_a_root, sizeof(lib_a_root), "/tmp/quad_e2e_%d_lib_a", pid);
    snprintf(lib_b_root, sizeof(lib_b_root), "/tmp/quad_e2e_%d_lib_b", pid);
    snprintf(lib_a_data, sizeof(lib_a_data), "/tmp/quad_e2e_%d_data_a", pid);
    snprintf(lib_b_data, sizeof(lib_b_data), "/tmp/quad_e2e_%d_data_b", pid);
    rm_rf(lib_a_root); rm_rf(lib_b_root);
    rm_rf(lib_a_data); rm_rf(lib_b_data);
    mkdirs(lib_a_data); mkdirs(lib_b_data);

    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    /* First index: lib_a is messy (BRONSON has no tags) */
    index_library(lib_a_root, lib_a_data, pg, solr);

    /* Simulate Picard: overwrite BRONSON FLACs with Picard-tagged versions */
    char path[1024];
    snprintf(path, sizeof(path), "%s/BRONSON/BRONSON (2020)", lib_a_root);
    for (int i = 0; i < BRONSON_TRACK_COUNT; i++) {
        char fpath[1024], tracknum[32], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s (FLAC 828 kbps).flac",
                 path, i + 1, BRONSON_TRACKS[i]);
        snprintf(tracknum, sizeof(tracknum), "track=%d", i + 1);
        snprintf(title_tag, sizeof(title_tag), "title=%s", BRONSON_TRACKS[i]);
        const char *tags[] = {
            title_tag, "artist=BRONSON", "album=BRONSON", "album_artist=BRONSON",
            tracknum, "date=2020", "genre=Electronic",
            "MUSICBRAINZ_ALBUMID=" MBID_BRONSON_RELEASE,
            "MUSICBRAINZ_ARTISTID=" MBID_BRONSON_ARTIST,
            "MUSICBRAINZ_RELEASEGROUPID=" MBID_BRONSON_RELEASE_GROUP,
            "MUSICBRAINZ_ALBUMARTISTID=" MBID_BRONSON_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, BRONSON_DURATIONS[i]), 0);
    }

    /* Re-index lib_a (should pick up new tags) */
    index_library(lib_a_root, lib_a_data, pg, solr);

    /* Index lib_b */
    index_library(lib_b_root, lib_b_data, pg, solr);

    /* Build cache */
    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", lib_a_data);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", lib_b_data);
    library_cache_source_t sources[2] = {
        { .db_path = db_a, .music_base = lib_a_root,
          .display_name = "Elicb Music", .bitmap_index = 0 },
        { .db_path = db_b, .music_base = lib_b_root,
          .display_name = "Music", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);
}

Test(e2e, user_tags_library_with_picard,
     .init = story_picard_setup, .fini = story_teardown, .timeout = 300) {

    /* After Picard tagging + re-index, BRONSON in lib_a should now be resolved */

    /* ── lib_a BRONSON should now have RGID ────────────────────────── */

    GPtrArray *albums_a = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    bool found_resolved = false;
    for (guint i = 0; i < albums_a->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a, i);
        if (strstr(a->title, "BRONSON") != NULL) {
            if (a->musicbrainz_release_group_id &&
                a->musicbrainz_release_group_id[0]) {
                found_resolved = true;
                cr_assert_str_eq(a->musicbrainz_release_group_id,
                    MBID_BRONSON_RELEASE_GROUP,
                    "After Picard, BRONSON RGID should match");
            }
        }
    }
    cr_assert(found_resolved,
        "After Picard tagging, lib_a BRONSON should have release_group_id");
    g_ptr_array_unref(albums_a);

    /* ── Cross-library dedup should now work ────────────────────────── */

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);

    /* Both lib_a and lib_b have BRONSON with same RGID → dedup to 1 */
    cr_assert_eq(count_album_title(albums, "BRONSON"), 1,
        "After Picard, BRONSON should be deduped to 1 in ALL view");
    g_ptr_array_unref(albums);

    /* ── BRONSON artist should be merged with MBID ─────────────────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists);

    cr_assert_eq(count_artist_name(artists, "BRONSON"), 1,
        "After Picard, BRONSON should be 1 artist in ALL view");

    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    cr_assert_not_null(bronson);
    cr_assert_str_eq(bronson->musicbrainz_id, MBID_BRONSON_ARTIST,
        "BRONSON MBID should match after Picard");

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

Test(e2e, user_moves_album_between_libraries,
     .init = story_import_setup, .fini = story_teardown, .timeout = 300) {
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();

    /* Delete BRONSON from lib_a (simulates user moving it to lib_b) */
    char bronson_dir[512];
    snprintf(bronson_dir, sizeof(bronson_dir), "%s/BRONSON", lib_a_root);
    rm_rf(bronson_dir);

    /* Re-index lib_a */
    index_library(lib_a_root, lib_a_data, pg, solr);

    /* Refresh cache slot 0 */
    library_cache_refresh_slot(cache, 0, NULL, 0);

    /* ── Orphan BRONSON album should be gone from lib_a ────────────── */

    GPtrArray *albums_a = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    cr_assert_eq(count_album_title_prefix(albums_a, "BRONSON"), 0,
        "After deleting BRONSON folder from lib_a, no BRONSON album should "
        "remain in lib_a view (orphan rows should be pruned)");
    g_ptr_array_unref(albums_a);

    /* ── ALL view should show exactly 1 BRONSON (from lib_b) ───────── */

    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    int bronson_count = count_artist_name(artists, "BRONSON");
    if (!bronson_count) bronson_count = count_artist_name(artists, "Bronson");
    cr_assert_eq(bronson_count, 1,
        "After move, BRONSON should appear once in ALL view (from lib_b only), "
        "got %d (orphan artist from lib_a leaking)", bronson_count);
    g_ptr_array_unref(artists);

    /* ── ODESZA tracks still in lib_a, "Bronson" credit still there ── */

    GPtrArray *odesza_artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    int odesza = count_artist_name(odesza_artists, "ODESZA");
    if (!odesza) odesza = count_artist_name(odesza_artists, "Odesza");
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

Test(e2e, user_deletes_album_folder,
     .init = story_import_setup, .fini = story_teardown, .timeout = 300) {
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();

    /* Verify ODESZA album exists before deletion */
    GPtrArray *before = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    bool had_odesza = false;
    for (guint i = 0; i < before->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(before, i);
        if (strstr(a->title, "Last Goodbye")) had_odesza = true;
    }
    cr_assert(had_odesza, "ODESZA album should exist before deletion");
    g_ptr_array_unref(before);

    /* Delete ODESZA from lib_a */
    char odesza_dir[512];
    snprintf(odesza_dir, sizeof(odesza_dir), "%s/ODESZA", lib_a_root);
    rm_rf(odesza_dir);

    /* Re-index + refresh */
    index_library(lib_a_root, lib_a_data, pg, solr);
    library_cache_refresh_slot(cache, 0, NULL, 0);

    /* ── ODESZA album should be gone from lib_a ────────────────────── */

    GPtrArray *albums_a = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums_a);
    for (guint i = 0; i < albums_a->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums_a, i);
        cr_assert(strstr(a->title, "Last Goodbye") == NULL,
            "ODESZA album should not appear after folder deletion, "
            "but found '%s' (orphan rows persisting)", a->title);
    }
    g_ptr_array_unref(albums_a);

    /* ── Verify Phase 6 artist rename: "Bronson" → "BRONSON" ─────── */

    GPtrArray *artists_a = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);

    /* Phase 2 created "Bronson" (no MBID) from the ODESZA track credit split.
     * Phase 6 resolved the ODESZA album via SOLR and called
     * db_get_or_create_artist_mb("BRONSON", mbid) — Step 2 (name NOCASE)
     * found the Phase 2 "Bronson" row and rename_artist_inplace() updated it
     * to "BRONSON" with MBID. After deleting ODESZA, the BRONSON album still
     * references this artist → it persists. There should be exactly 1 entry
     * matching "BRONSON" (the merged result), not a separate "Bronson". */
    cr_assert_eq(count_artist_name(artists_a, "BRONSON"), 1,
        "BRONSON should still be in lib_a — Phase 6 renamed 'Bronson' → 'BRONSON' "
        "with MBID, and the BRONSON album still references it");

    /* ODESZA artist should also be gone if their only album was deleted */
    int odesza_a = count_artist_name(artists_a, "ODESZA");
    if (!odesza_a) odesza_a = count_artist_name(artists_a, "Odesza");
    cr_assert_eq(odesza_a, 0,
        "ODESZA should be pruned from lib_a after their only album is deleted");
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

Test(e2e, user_removes_library,
     .init = story_import_setup, .fini = story_teardown, .timeout = 300) {

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
    cr_assert_eq(count_artist_name(artists, "Unknown Artist"), 0,
        "No 'Unknown Artist' should remain after lib_a removal");

    /* ODESZA was only in lib_a — should be gone */
    int odesza = count_artist_name(artists, "ODESZA");
    if (!odesza) odesza = count_artist_name(artists, "Odesza");
    cr_assert_eq(odesza, 0,
        "ODESZA (only in lib_a) should not appear after lib_a removal");

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

static void story_mb_credits_setup(void) {
    const char *pg = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    if (!pg)   cr_skip("MB_PG_PASSWORD not set");
    if (!solr) cr_skip("MB_SOLR_URL not set");

    pid_t pid = getpid();
    snprintf(lib_a_root, sizeof(lib_a_root), "/tmp/quad_e2e_%d_lib_a", pid);
    snprintf(lib_b_root, sizeof(lib_b_root), "/tmp/quad_e2e_%d_lib_b", pid);
    snprintf(lib_a_data, sizeof(lib_a_data), "/tmp/quad_e2e_%d_data_a", pid);
    snprintf(lib_b_data, sizeof(lib_b_data), "/tmp/quad_e2e_%d_data_b", pid);
    rm_rf(lib_a_root); rm_rf(lib_b_root);
    rm_rf(lib_a_data); rm_rf(lib_b_data);
    mkdirs(lib_a_data); mkdirs(lib_b_data);

    build_lib_a(lib_a_root);
    build_lib_b(lib_b_root);

    /* First index: lib_a is messy, ODESZA has "Bronson/Odesza" credits */
    index_library(lib_a_root, lib_a_data, pg, solr);
    index_library(lib_b_root, lib_b_data, pg, solr);

    /* Simulate Picard: add MB tags to ODESZA album files */
    char path[1024];
    snprintf(path, sizeof(path), "%s/ODESZA/The Last Goodbye Tour Live", lib_a_root);

    struct { int num; const char *title; const char *artist; int dur; } odesza_tracks[] = {
        { 1,  "This Version Of You (Live)", "Odesza",        186 },
        { 2,  "Behind the Sun (Live)",      "Odesza",        190 },
        { 3,  "Wide Awake (Live)",          "Odesza",        202 },
        { 17, "TENSE (Live)",               "Bronson/Odesza", 194 },
        { 18, "Keep Moving (Live)",         "Bronson/Odesza", 142 },
    };

    for (size_t i = 0; i < sizeof(odesza_tracks)/sizeof(odesza_tracks[0]); i++) {
        char fpath[1024], tracknum[32], title_tag[256];
        snprintf(fpath, sizeof(fpath), "%s/%02d - %s.flac",
                 path, odesza_tracks[i].num, odesza_tracks[i].title);
        snprintf(tracknum, sizeof(tracknum), "track=%d", odesza_tracks[i].num);
        snprintf(title_tag, sizeof(title_tag), "title=%s", odesza_tracks[i].title);

        char artist_tag[256], aa_tag[256];
        snprintf(artist_tag, sizeof(artist_tag), "artist=%s", odesza_tracks[i].artist);
        snprintf(aa_tag, sizeof(aa_tag), "album_artist=%s", odesza_tracks[i].artist);

        const char *tags[] = {
            title_tag, artist_tag, aa_tag,
            "album=The Last Goodbye Tour Live",
            tracknum, "date=2024", "genre=Electronic",
            "MUSICBRAINZ_ALBUMID=" MBID_ODESZA_LIVE_RELEASE,
            "MUSICBRAINZ_ARTISTID=" MBID_ODESZA_ARTIST,
            "MUSICBRAINZ_RELEASEGROUPID=" MBID_ODESZA_LIVE_RELEASE_GROUP,
            "MUSICBRAINZ_ALBUMARTISTID=" MBID_ODESZA_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, odesza_tracks[i].dur), 0);
    }

    /* Re-index lib_a — Phase 2 picks up MUSICBRAINZ_ALBUMID,
     * Phase 6 fetches from PG and rewrites track credits with MBIDs */
    index_library(lib_a_root, lib_a_data, pg, solr);

    /* Build cache */
    char db_a[512], db_b[512];
    snprintf(db_a, sizeof(db_a), "%s/quadrature.sqlite", lib_a_data);
    snprintf(db_b, sizeof(db_b), "%s/quadrature.sqlite", lib_b_data);
    library_cache_source_t sources[2] = {
        { .db_path = db_a, .music_base = lib_a_root,
          .display_name = "Elicb Music", .bitmap_index = 0 },
        { .db_path = db_b, .music_base = lib_b_root,
          .display_name = "Music", .bitmap_index = 1 },
    };
    cr_assert_eq(library_cache_create_multi(sources, 2, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);
    library_cache_warm_slot_blocking(cache, 1);
}

Test(e2e, mb_resolution_updates_featured_credits,
     .init = story_mb_credits_setup, .fini = story_teardown, .timeout = 300) {

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
    if (!bronson_count) bronson_count = count_artist_name(artists, "Bronson");
    cr_assert_eq(bronson_count, 1,
        "After MB resolution of ODESZA album, BRONSON should appear as 1 artist "
        "(Phase 6 replaced 'Bronson' credit with 'BRONSON' + MBID)");

    /* Find BRONSON and verify it has the correct MBID */
    const library_artist_info_t *bronson = find_artist(artists, "BRONSON");
    if (!bronson) bronson = find_artist(artists, "Bronson");
    cr_assert_not_null(bronson, "BRONSON artist not found after MB resolution");
    cr_assert(bronson->musicbrainz_id != NULL && bronson->musicbrainz_id[0],
        "BRONSON should have MBID after Phase 6 rewrote track credits");
    cr_assert_str_eq(bronson->musicbrainz_id, MBID_BRONSON_ARTIST,
        "BRONSON MBID should be correct");

    int64_t bronson_id = bronson->artist_id;

    /* ── BRONSON should have appearance tracks from ODESZA album ───── */

    GPtrArray *appearances = library_cache_get_artist_appearance_tracks(
        cache, bronson_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(appearances,
        "BRONSON should have appearance tracks after ODESZA MB resolution — "
        "Phase 6 should have written BRONSON (MBID) as track artist for "
        "tracks 17-18, enabling the appearance lookup");
    cr_assert(appearances->len >= 2,
        "BRONSON should appear on >= 2 ODESZA tracks (TENSE, KEEP MOVING), "
        "got %u", appearances->len);
    g_ptr_array_unref(appearances);

    /* ── ODESZA should also have MBID ──────────────────────────────── */

    const library_artist_info_t *odesza = find_artist(artists, "ODESZA");
    if (!odesza) odesza = find_artist(artists, "Odesza");
    cr_assert_not_null(odesza, "ODESZA should exist");
    cr_assert(odesza->musicbrainz_id != NULL && odesza->musicbrainz_id[0],
        "ODESZA should have MBID after album resolution");

    g_ptr_array_unref(artists);

    /* ── Verify ODESZA album has release_group_id ──────────────────── */

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, MASK_A);
    cr_assert_not_null(albums);
    bool found_odesza_album = false;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (strstr(a->title, "Last Goodbye") != NULL) {
            found_odesza_album = true;
            cr_assert(a->musicbrainz_release_group_id != NULL &&
                      a->musicbrainz_release_group_id[0],
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
            if (g_ascii_strcasecmp(a->name, "BRONSON") == 0 ||
                g_ascii_strcasecmp(a->name, "Bronson") == 0)
                search_artists++;
        }
    }
    cr_assert_eq(search_artists, 1,
        "Search 'Bronson' should return 1 artist after MB credit resolution, "
        "got %d (Phase 2 'Bronson' orphan leaking)", search_artists);
    library_search_results_free(results);
}
