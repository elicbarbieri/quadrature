/**
 * End-to-end integration test: metadata-mode credit search must not duplicate.
 *
 * Bug being targeted (visible in production today, see chat screenshot):
 * metadata-mode search returns DUPLICATE rows for albums and tracks.
 * "Random Access Memories" appears twice; "GL (early take)" appears twice.
 *
 * REPRO MECHANISM (read directly from the user's actual library state on
 * disk — not synthesised):
 *
 *   ~/Music/Daft Punk/Random Access Memories/                  (Picard-tagged)
 *     → resolves to release_mbid    = 8ecfafd1-89a8-423a-968f-3fff47f0b0f9
 *                  release_group_id = aa997ea0-2936-40bd-884d-3af8a0e064dc
 *
 *   ~/elicb_music/Daft Punk/Random Access Memories (2013)/      (tag-less)
 *     → resolves to release_mbid    = 7d1b2d38-97e8-4fc7-b0fa-f275dcbea77a
 *                  release_group_id = aa997ea0-2936-40bd-884d-3af8a0e064dc
 *
 * Same release-group, different release MBIDs. Normal browse view dedupes
 * by RGID via library_cache_get_album(). Credit search does NOT — it
 * resolves credits per-library, keys tracks by global id, and slips the
 * duplicates past `&t->album_id` dedup.
 *
 * Pharrell Williams (MBID 149f91ef-1287-46da-9a8e-87fee02f1471) is credited
 * on RAM disc 1 tracks 6 (Lose Yourself to Dance) and 8 (Get Lucky) per
 * the user's actual meta DB. Both libraries contain those recordings.
 * Correct result = 2 distinct recordings, 1 distinct release-group.
 * Bug result    = 4 tracks, 2 albums.
 *
 * STORIES:
 *   1. Tag-based path:   index ~/Music/Daft Punk only        (Picard-tagged).
 *   2. Fingerprint path: index ~/elicb_music/Daft Punk only  (no MB tags).
 *   3. Cross-library:    index BOTH — reproduces the screenshot.
 *
 * All three call the same credit-search primitives the UI uses
 * (build_credit_track_set in src/ui/search/search_view.c) and assert
 * dedup against the meta DB's recording_mbid / release_group_mbid axes.
 *
 * Story 1 (tagged) synthesizes a tiny FLAC fixture in /tmp with real RAM
 * MBIDs baked as Picard tags — no personal-library dependency.
 *
 * Stories 2 and 3 still depend on real tag-less audio (fingerprint → AcoustID
 * only matches real recordings). On machines without the path below, those
 * stories will fail the setup cr_assert.
 *
 * Required env vars (test skips if absent):
 *   MB_PG_PASSWORD       — MusicBrainz PostgreSQL password (all stories)
 *   MB_SOLR_URL          — SOLR endpoint (all stories)
 *   ACOUSTID_PG_PASSWORD — AcoustID PostgreSQL password (Story 2 + 3)
 *   ACOUSTID_INDEX_URL   — acoustid-index HTTP endpoint (Story 2 + 3)
 *
 * Required filesystem (Stories 2 + 3 only):
 *   ~/elicb_music/Daft Punk/Random Access Memories (2013)/  (tag-less FLACs
 *     that fingerprint-match against the local AcoustID index)
 *
 * Run:
 *   source .env && cd build && ninja test_metadata_search
 *   ./test_metadata_search --verbose
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "test_helpers.h"
#include "quadrature/indexer.h"
#include "quadrature/database.h"
#include "quadrature/library.h"
#include "quadrature/metadata.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>

ReportHook(PRE_ALL)(struct criterion_test_set *tests) {
    (void)tests;
    avformat_network_init();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Real data — read directly from the user's library state.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Daft Punk — Random Access Memories (10th Anniversary release group).
 * Both library copies resolve to this RGID. */
#define MBID_DAFT_PUNK_ARTIST       "056e4f3e-d505-4dad-8ec1-d04f521cbb56"
#define MBID_RAM_RELEASE_GROUP      "aa997ea0-2936-40bd-884d-3af8a0e064dc"
#define MBID_RAM_RELEASE_PICARD     "8ecfafd1-89a8-423a-968f-3fff47f0b0f9"
#define MBID_RAM_RELEASE_TAGLESS    "7d1b2d38-97e8-4fc7-b0fa-f275dcbea77a"

/* Pharrell Williams — credited on RAM disc 1 t6 (Lose Yourself to Dance)
 * + disc 1 t8 (Get Lucky). Both confirmed in the user's actual meta DB. */
#define CREDIT_QUERY                "Pharrell"
#define MBID_PHARRELL_WILLIAMS      "149f91ef-1287-46da-9a8e-87fee02f1471"
#define EXPECTED_PHARRELL_RAM_RECORDINGS  2

/* Source path in the user's home directory — Stories 2 + 3 reflink-copy this
 * tag-less RAM into the fixture root so Chromaprint can fingerprint against
 * the local AcoustID index. */
#define SRC_TAGLESS_RAM_PARENT  "/home/elicb/elicb_music/Daft Punk"
#define SRC_TAGLESS_RAM_FOLDER  "Random Access Memories (2013)"

/* ═══════════════════════════════════════════════════════════════════════════
 * Connection helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : fallback;
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

static const char *mb_solr_url(void) { return env_or("MB_SOLR_URL", NULL); }

static const char *acoustid_pg_conninfo(void) {
    const char *pw = getenv("ACOUSTID_PG_PASSWORD");
    if (!pw || !pw[0]) return NULL;
    static char buf[512];
    snprintf(buf, sizeof(buf),
             "host=%s dbname=%s user=%s password=%s connect_timeout=%s",
             env_or("ACOUSTID_HOST", "localhost"),
             env_or("ACOUSTID_DBNAME", "acoustid"),
             env_or("ACOUSTID_USER", "acoustid"),
             pw,
             env_or("MB_PG_CONNECT_TIMEOUT", "5"));
    return buf;
}

static const char *acoustid_index_url(void) {
    return env_or("ACOUSTID_INDEX_URL", NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fixture helpers — Story 1 synthesizes tagged FLACs from real RAM MBIDs
 * (hermetic, no audio required). Stories 2 + 3 reflink-copy the user's real
 * tag-less RAM so Chromaprint/AcoustID has a real signal to match against.
 *
 * Reflink (cp --reflink=auto) uses CoW on btrfs/xfs for near-zero-cost
 * cloning; falls back to full copy on other filesystems. Symlinks can't be
 * used — the indexer deliberately skips them (src/indexer/indexer.c:108) to
 * prevent cycles.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void run_shell(const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    (void)system(cmd);
}

static void mkdirs(const char *path)  { run_shell("mkdir -p '%s'", path); }
static void rm_rf(const char *path)   { run_shell("rm -rf '%s'", path); }

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Reflink-copy the user's real RAM album folder under a fresh fixture root.
 * Layout mirrors prod: <fixture_root>/<artist>/<album>/<files...>. Used by
 * Stories 2 + 3, which need real audio for Chromaprint fingerprinting. */
static void copy_ram_fixture(const char *fixture_root,
                             const char *src_parent,
                             const char *src_album) {
    char artist_dir[512], src_full[1024], dst_album[1024];
    snprintf(artist_dir, sizeof(artist_dir), "%s/Daft Punk", fixture_root);
    snprintf(src_full,   sizeof(src_full),   "%s/%s", src_parent, src_album);
    snprintf(dst_album,  sizeof(dst_album),  "%s/%s", artist_dir, src_album);
    cr_assert(path_exists(src_full),
        "Source album not found on disk: %s\n"
        "Stories 2 + 3 require real audio for AcoustID fingerprinting.\n"
        "Expected album at: %s",
        src_full, src_full);
    mkdirs(artist_dir);
    /* --reflink=auto: CoW clone on btrfs/xfs, full copy otherwise.
     * Preserves mtime/xattrs so Phase 1 delta detection and tag reads match prod. */
    run_shell("cp -a --reflink=auto '%s' '%s'", src_full, dst_album);
}

/* Synthesize a tagged RAM fixture — silent FLACs with real Picard tags.
 * No audio data in repo, no user-library dependency. Real MBIDs mean
 * Phase 6 resolves the release straight from the tag (no fingerprint /
 * Solr search needed), and meta.sqlite gets populated from the live MB PG
 * just like it would for the real album. */
typedef struct { int disc, num, dur; const char *title; } ram_track_t;
static const ram_track_t RAM_TRACKS[] = {
    { 1,  1, 274, "Give Life Back to Music" },
    { 1,  2, 321, "The Game of Love" },
    { 1,  3, 548, "Giorgio by Moroder" },
    { 1,  4, 228, "Within" },
    { 1,  5, 337, "Instant Crush" },
    { 1,  6, 353, "Lose Yourself to Dance" },
    { 1,  7, 496, "Touch" },
    { 1,  8, 367, "Get Lucky" },
    { 1,  9, 290, "Beyond" },
    { 1, 10, 341, "Motherboard" },
    { 1, 11, 279, "Fragments of Time" },
    { 1, 12, 251, "Doin' It Right" },
    { 1, 13, 383, "Contact" },
};

static void build_ram_tagged_fixture(const char *fixture_root) {
    char album_dir[1024];
    snprintf(album_dir, sizeof(album_dir),
             "%s/Daft Punk/Random Access Memories", fixture_root);
    mkdirs(album_dir);

    for (size_t i = 0; i < sizeof(RAM_TRACKS) / sizeof(RAM_TRACKS[0]); i++) {
        const ram_track_t *t = &RAM_TRACKS[i];
        char fpath[1536], title_tag[256], track_tag[32], disc_tag[32];
        snprintf(fpath, sizeof(fpath), "%s/%d-%02d %s.flac",
                 album_dir, t->disc, t->num, t->title);
        snprintf(title_tag, sizeof(title_tag), "title=%s", t->title);
        snprintf(track_tag, sizeof(track_tag), "track=%d", t->num);
        snprintf(disc_tag,  sizeof(disc_tag),  "disc=%d",  t->disc);
        const char *tags[] = {
            title_tag, track_tag, disc_tag,
            "artist=Daft Punk",
            "album=Random Access Memories",
            "album_artist=Daft Punk",
            "date=2013",
            "MUSICBRAINZ_ALBUMID="          MBID_RAM_RELEASE_PICARD,
            "MUSICBRAINZ_RELEASEGROUPID="   MBID_RAM_RELEASE_GROUP,
            "MUSICBRAINZ_ARTISTID="         MBID_DAFT_PUNK_ARTIST,
            "MUSICBRAINZ_ALBUMARTISTID="    MBID_DAFT_PUNK_ARTIST,
            NULL
        };
        cr_assert_eq(create_flac(fpath, tags, t->dur), 0,
            "failed to synthesize %s", fpath);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Prod-parity indexer harness (pattern from test_e2e_library_resolution.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_LIBS 2
static char  lib_root[MAX_LIBS][256];
static char  lib_data[MAX_LIBS][256];
static library_cache_t *cache = NULL;

typedef struct {
    int        bitmap_index;
    atomic_int lib_updated;
    atomic_int completed;
    atomic_int errored;
} ProdParityTracker;

static void prod_parity_cb(indexer_event_t event,
                           const indexer_progress_t *progress,
                           const library_cache_changeset_t *changeset,
                           void *user_data) {
    (void)progress; (void)changeset;
    ProdParityTracker *t = user_data;
    switch (event) {
        case INDEXER_LIBRARY_UPDATED: atomic_fetch_add(&t->lib_updated, 1); break;
        case INDEXER_COMPLETED:
        case INDEXER_CANCELLED:       atomic_store(&t->completed, 1); break;
        case INDEXER_ERROR:           atomic_store(&t->errored, 1);
                                      atomic_store(&t->completed, 1); break;
        default: break;
    }
}

static void bootstrap_empty_db(const char *data_dir) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/quadrature.sqlite", data_dir);
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path, false, &db), QUADRATURE_OK,
        "bootstrap_empty_db: failed at %s", db_path);
    db_close(db);
}

static void pump_until_done(library_cache_t *c, ProdParityTracker *trackers, int n) {
    int *seen = g_new0(int, n);
    for (;;) {
        int all_done = 1;
        for (int i = 0; i < n; i++) {
            int cur = atomic_load(&trackers[i].lib_updated);
            while (seen[i] < cur) {
                library_cache_refresh_slot(c, trackers[i].bitmap_index, NULL);
                library_cache_await_slot(c, trackers[i].bitmap_index);
                seen[i]++;
            }
            if (!atomic_load(&trackers[i].completed)) all_done = 0;
        }
        if (all_done) {
            int pending = 0;
            for (int i = 0; i < n; i++) {
                int cur = atomic_load(&trackers[i].lib_updated);
                while (seen[i] < cur) {
                    library_cache_refresh_slot(c, trackers[i].bitmap_index, NULL);
                    library_cache_await_slot(c, trackers[i].bitmap_index);
                    seen[i]++; pending++;
                }
            }
            if (!pending) break;
        }
        g_usleep(10000);
    }
    g_free(seen);
}

static void setup_prod_cache(int lib_count) {
    library_cache_source_t srcs[MAX_LIBS] = {0};
    char db_paths[MAX_LIBS][512];
    for (int i = 0; i < lib_count; i++) {
        bootstrap_empty_db(lib_data[i]);
        snprintf(db_paths[i], sizeof(db_paths[i]), "%s/quadrature.sqlite", lib_data[i]);
        srcs[i].db_path      = db_paths[i];
        srcs[i].music_base   = lib_root[i];
        srcs[i].display_name = (i == 0) ? "Music" : "Elicb";
        srcs[i].bitmap_index = i;
    }
    cr_assert_eq(library_cache_create_multi(srcs, lib_count, &cache), QUADRATURE_OK);
    for (int i = 0; i < lib_count; i++) {
        library_cache_warm_slot(cache, i);
        library_cache_await_slot(cache, i);
    }
}

static void run_prod_indexers(int lib_count, bool with_acoustid) {
    const char *pg   = mb_pg_conninfo();
    const char *solr = mb_solr_url();
    const char *acpg  = with_acoustid ? acoustid_pg_conninfo() : NULL;
    const char *acidx = with_acoustid ? acoustid_index_url()   : NULL;

    ProdParityTracker trackers[MAX_LIBS] = {0};
    indexer_t *ix[MAX_LIBS] = {0};

    for (int i = 0; i < lib_count; i++) {
        trackers[i].bitmap_index = i;
        indexer_config_t cfg = {
            .thread_count = 2, .process_artwork = false, .mb_resolve = true,
            .pg_conninfo = pg, .mb_solr_url = solr,
            .acoustid_pg_conninfo = acpg, .acoustid_index_url = acidx,
            .fetch_artist_art = false, .fanart_api_key = NULL,
            .fetch_artist_bios = false,
            .callback = prod_parity_cb, .user_data = &trackers[i],
        };
        cr_assert_eq(indexer_create(&ix[i], &cfg), QUADRATURE_OK);
        cr_assert_eq(indexer_scan(ix[i], lib_root[i], lib_data[i]), QUADRATURE_OK);
    }

    pump_until_done(cache, trackers, lib_count);

    for (int i = 0; i < lib_count; i++) {
        cr_assert(!atomic_load(&trackers[i].errored),
            "lib %d indexer reported INDEXER_ERROR", i);
        cr_assert(atomic_load(&trackers[i].lib_updated) >= 1,
            "lib %d indexer emitted no LIBRARY_UPDATED events", i);
        indexer_destroy(ix[i]);
    }
}

static void story_paths_init(int lib_count) {
    pid_t pid = getpid();
    for (int i = 0; i < lib_count; i++) {
        snprintf(lib_root[i], sizeof(lib_root[i]),
                 "/tmp/quad_meta_%d_lib%d", pid, i);
        snprintf(lib_data[i], sizeof(lib_data[i]),
                 "/tmp/quad_meta_%d_data%d", pid, i);
        rm_rf(lib_root[i]); rm_rf(lib_data[i]);
        mkdirs(lib_root[i]); mkdirs(lib_data[i]);
    }
}

static void story_teardown(void) {
    if (cache) { library_cache_destroy(cache); cache = NULL; }
    if (getenv("QUAD_KEEP_FIXTURES")) return;
    for (int i = 0; i < MAX_LIBS; i++) {
        if (lib_root[i][0]) rm_rf(lib_root[i]);
        if (lib_data[i][0]) rm_rf(lib_data[i]);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Credit-search helper — thin wrapper around library_credit_search() so the
 * test matches production's exact dedup semantics. Any dedup change made for
 * the UI flows through here automatically.
 *
 * The caller owns the returned GArrays (test keeps ownership symmetry with
 * the old local impl). The rest of library_credit_search_result_t is
 * discarded for brevity — we're only asserting on counts / MBIDs.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void run_credit_search(library_cache_t *c, const char *credit_query,
                              GArray **out_track_ids, GArray **out_album_ids) {
    library_credit_search_result_t *r =
        library_credit_search(c, credit_query, NULL, LIBRARY_MASK_ALL);
    /* Steal the GArrays — swap NULLs in so result_free doesn't double-unref. */
    *out_track_ids = r->track_ids; r->track_ids = NULL;
    *out_album_ids = r->album_ids; r->album_ids = NULL;
    library_credit_search_result_free(r);
}

/* ── Dedup-axis helpers ───────────────────────────────────────────────── */

static int distinct_release_groups(library_cache_t *c, GArray *album_ids,
                                    uint32_t mask) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int unknown = 0;
    for (guint i = 0; i < album_ids->len; i++) {
        int64_t aid = g_array_index(album_ids, int64_t, i);
        const library_album_info_t *a = library_cache_get_album(c, aid, mask);
        if (!a || !a->musicbrainz_release_group_id ||
            !a->musicbrainz_release_group_id[0]) { unknown++; continue; }
        if (!g_hash_table_contains(seen, a->musicbrainz_release_group_id))
            g_hash_table_add(seen, g_strdup(a->musicbrainz_release_group_id));
    }
    int n = (int)g_hash_table_size(seen) + unknown;
    g_hash_table_unref(seen);
    return n;
}

static int distinct_recording_mbids(library_cache_t *c, GArray *track_ids,
                                     uint32_t mask) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int unknown = 0;
    for (guint i = 0; i < track_ids->len; i++) {
        int64_t tid = g_array_index(track_ids, int64_t, i);
        const library_track_info_t *t = library_cache_get_track(c, tid);
        if (!t) { unknown++; continue; }

        const library_album_info_t *a = library_cache_get_album(c, t->album_id, mask);
        if (!a || !a->musicbrainz_release_id) { unknown++; continue; }

        /* Resolve recording MBID via the meta DB of the track's source slot. */
        int slot_bi = LIBRARY_GLOBAL_ID_LIB(tid);
        library_cache_dbs_t dbs = library_cache_get_dbs(c, slot_bi);
        if (!dbs.meta) { unknown++; continue; }

        char *rec_mbid = NULL;
        if (db_meta_get_recording_mbid(dbs.meta, a->musicbrainz_release_id,
                                        t->disc_num, t->track_num,
                                        &rec_mbid) != QUADRATURE_OK || !rec_mbid) {
            unknown++; g_free(rec_mbid); continue;
        }
        if (!g_hash_table_contains(seen, rec_mbid))
            g_hash_table_add(seen, rec_mbid);
        else
            g_free(rec_mbid);
    }
    int n = (int)g_hash_table_size(seen) + unknown;
    g_hash_table_unref(seen);
    return n;
}

/* ── Common skip-guards ───────────────────────────────────────────────── */

static void require_mb_env(void) {
    if (!mb_pg_conninfo()) cr_skip("MB_PG_PASSWORD not set");
    if (!mb_solr_url())    cr_skip("MB_SOLR_URL not set");
}
static void require_acoustid_env(void) {
    if (!acoustid_pg_conninfo()) cr_skip("ACOUSTID_PG_PASSWORD not set");
    if (!acoustid_index_url())   cr_skip("ACOUSTID_INDEX_URL not set");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 1: Tag-based MB resolve, single library
 *
 * Index ~/Music/Daft Punk/Random Access Memories (Picard-tagged) into a
 * fresh fixture. Credit search for "Pharrell" must return exactly 2
 * recordings, 1 album. Baseline: with one library and one physical
 * release this should already pass.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void story1_setup(void) {
    require_mb_env();
    story_paths_init(1);
    build_ram_tagged_fixture(lib_root[0]);
    setup_prod_cache(1);
    run_prod_indexers(1, false);
}

Test(metadata_search, tagged_single_library_baseline,
     .init = story1_setup, .fini = story_teardown, .timeout = 600) {

    GArray *tracks = NULL, *albums = NULL;
    run_credit_search(cache, CREDIT_QUERY, &tracks, &albums);

    cr_assert_gt(tracks->len, 0,
        "credit search returned zero tracks — meta DB likely missing "
        "recording_links for Pharrell (MBID %s)", MBID_PHARRELL_WILLIAMS);

    int distinct_albums = distinct_release_groups(cache, albums, 1u << 0);
    int distinct_recs   = distinct_recording_mbids(cache, tracks, 1u << 0);

    cr_assert_eq((int)tracks->len, EXPECTED_PHARRELL_RAM_RECORDINGS,
        "Pharrell credited on exactly %d RAM recordings (Get Lucky + "
        "Lose Yourself to Dance) — got %u tracks",
        EXPECTED_PHARRELL_RAM_RECORDINGS, tracks->len);
    cr_assert_eq((int)albums->len, 1,
        "single library, single album — got %u albums", albums->len);
    cr_assert_eq(distinct_recs, EXPECTED_PHARRELL_RAM_RECORDINGS);
    cr_assert_eq(distinct_albums, 1);

    g_array_unref(tracks);
    g_array_unref(albums);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 2: Fingerprint-based MB resolve, single library
 *
 * Index ~/elicb_music/Daft Punk/Random Access Memories (2013) — tag-less
 * FLACs. Indexer must derive everything from Chromaprint → AcoustID → MB.
 * Same expectation as Story 1 once meta DB is populated.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void story2_setup(void) {
    require_mb_env();
    require_acoustid_env();
    story_paths_init(1);
    copy_ram_fixture(lib_root[0], SRC_TAGLESS_RAM_PARENT, SRC_TAGLESS_RAM_FOLDER);
    setup_prod_cache(1);
    run_prod_indexers(1, true);
}

Test(metadata_search, fingerprinted_single_library_baseline,
     .init = story2_setup, .fini = story_teardown, .timeout = 1200) {

    GArray *tracks = NULL, *albums = NULL;
    run_credit_search(cache, CREDIT_QUERY, &tracks, &albums);

    cr_assert_gt(tracks->len, 0,
        "credit search returned zero tracks after fingerprint resolution "
        "— AcoustID may not have matched (check ACOUSTID_INDEX_URL is "
        "reachable and the user's audio fingerprints exist in the local "
        "AcoustID PG)");

    int distinct_albums = distinct_release_groups(cache, albums, 1u << 0);
    int distinct_recs   = distinct_recording_mbids(cache, tracks, 1u << 0);

    cr_assert_eq((int)tracks->len, EXPECTED_PHARRELL_RAM_RECORDINGS,
        "Pharrell credited on exactly %d RAM recordings — got %u tracks",
        EXPECTED_PHARRELL_RAM_RECORDINGS, tracks->len);
    cr_assert_eq((int)albums->len, 1, "got %u albums", albums->len);
    cr_assert_eq(distinct_recs, EXPECTED_PHARRELL_RAM_RECORDINGS);
    cr_assert_eq(distinct_albums, 1);

    g_array_unref(tracks);
    g_array_unref(albums);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STORY 3: Cross-library duplication — the actual production bug.
 *
 * Mirrors the user's real setup: lib_a = ~/Music (Picard-tagged),
 * lib_b = ~/elicb_music (tag-less). Both contain RAM. After resolution
 * both albums share release-group MBID aa997ea0 but have different
 * release MBIDs (8ecfafd1 vs 7d1b2d38).
 *
 * Browse view dedupes via library_cache_get_album → 1 RAM card.
 * Metadata-mode search dedupes by raw global id → 2 cards (the bug).
 *
 * Correct behavior post-fix: 2 distinct recordings, 1 distinct
 * release-group. Bug today: 4 tracks, 2 albums.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void story3_setup(void) {
    require_mb_env();
    require_acoustid_env();
    story_paths_init(2);
    build_ram_tagged_fixture(lib_root[0]);
    copy_ram_fixture(lib_root[1], SRC_TAGLESS_RAM_PARENT, SRC_TAGLESS_RAM_FOLDER);
    setup_prod_cache(2);
    run_prod_indexers(2, true);
}

Test(metadata_search, cross_library_credit_search_no_duplicates,
     .init = story3_setup, .fini = story_teardown, .timeout = 1800) {

    uint32_t mask = (1u << 0) | (1u << 1);
    GArray *tracks = NULL, *albums = NULL;
    run_credit_search(cache, CREDIT_QUERY, &tracks, &albums);

    cr_assert_gt(tracks->len, 0, "credit search returned zero tracks");

    int distinct_albums = distinct_release_groups(cache, albums, mask);
    int distinct_recs   = distinct_recording_mbids(cache, tracks, mask);

    /* Bug-repro contract:
     *   - Both libraries contain RAM under one release-group.
     *   - Both libraries contain the SAME 2 Pharrell-credited recordings.
     *   - Correct dedup → 2 tracks, 1 album.
     *   - Today's bug    → 4 tracks, 2 albums. */

    cr_assert_eq((int)albums->len, 1,
        "ALBUM DUP: %u album rows for one RAM release-group — "
        "metadata search not deduping across libraries", albums->len);
    cr_assert_eq(distinct_albums, 1,
        "expected 1 distinct release-group, got %d", distinct_albums);

    cr_assert_eq((int)tracks->len, EXPECTED_PHARRELL_RAM_RECORDINGS,
        "TRACK DUP: %u track rows for %d unique Pharrell-credited "
        "recordings — same recording appearing once per library",
        tracks->len, EXPECTED_PHARRELL_RAM_RECORDINGS);
    cr_assert_eq(distinct_recs, EXPECTED_PHARRELL_RAM_RECORDINGS,
        "expected %d distinct recording MBIDs, got %d",
        EXPECTED_PHARRELL_RAM_RECORDINGS, distinct_recs);

    g_array_unref(tracks);
    g_array_unref(albums);
}
