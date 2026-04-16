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
 * The test points the indexer at the user's real ~/Music and
 * ~/elicb_music paths so REAL Picard tags + REAL audio drive the
 * resolver. A temporary data dir is used for output so the user's live
 * DBs are never touched.
 *
 * Required env vars (test skips if absent):
 *   MB_PG_PASSWORD       — MusicBrainz PostgreSQL password
 *   MB_SOLR_URL          — SOLR endpoint
 *   ACOUSTID_PG_PASSWORD — AcoustID PostgreSQL password (Story 2 + 3)
 *   ACOUSTID_INDEX_URL   — acoustid-index HTTP endpoint (Story 2 + 3)
 *
 * Required filesystem:
 *   ~/Music/Daft Punk/Random Access Memories/        (must exist, Picard-tagged)
 *   ~/elicb_music/Daft Punk/Random Access Memories (2013)/   (must exist, tag-less)
 *
 * Run:
 *   source .env && cd build && ninja test_metadata_search
 *   ./test_metadata_search --verbose
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
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

/* Source paths in the user's home directory — fixtures symlink into these
 * so the indexer scans real audio + real Picard tags. */
#define SRC_TAGGED_RAM_PARENT   "/home/elicb/Music/Daft Punk"
#define SRC_TAGGED_RAM_FOLDER   "Random Access Memories"
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
 * Filesystem helpers — fixtures symlink into the user's real library so
 * REAL audio + REAL Picard tags drive the indexer. Temp data dir keeps
 * the user's live DBs untouched.
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

/* Symlink the user's real RAM album folder under a fresh fixture root.
 * Layout mirrors prod: <fixture_root>/<artist>/<album>/<files...>. */
static void link_ram_fixture(const char *fixture_root,
                             const char *src_parent,
                             const char *src_album) {
    char artist_dir[512], src_full[1024];
    snprintf(artist_dir, sizeof(artist_dir), "%s/Daft Punk", fixture_root);
    snprintf(src_full,   sizeof(src_full),   "%s/%s", src_parent, src_album);
    cr_assert(path_exists(src_full),
        "Source album not found on disk: %s\n"
        "This test reads the user's real library; that path must exist.",
        src_full);
    mkdirs(artist_dir);
    run_shell("ln -sfn '%s' '%s/%s'", src_full, artist_dir, src_album);
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
    cr_assert_eq(db_open(db_path, &db), QUADRATURE_OK,
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
    for (int i = 0; i < MAX_LIBS; i++) {
        if (lib_root[i][0]) rm_rf(lib_root[i]);
        if (lib_data[i][0]) rm_rf(lib_data[i]);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Credit-search helper — duplicates `build_credit_track_set` in
 * src/ui/search/search_view.c (currently `static` inside the UI module).
 * Same primitives, same dedup pattern as production.
 *
 * TODO(refactor): extract the prod symbol into a non-UI module so this
 * test can call it directly and any dedup fix lands in exactly one place.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void run_credit_search(library_cache_t *c, const char *credit_query,
                              GArray **out_track_ids, GArray **out_album_ids) {
    GHashTable *track_set = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                   g_free, NULL);

    int lib_count = library_cache_get_library_count(c);
    for (int li = 0; li < lib_count; li++) {
        int bi = library_cache_get_bitmap_index(c, li);
        if (!library_cache_get_available(c, bi)) continue;
        library_cache_dbs_t dbs = library_cache_get_dbs(c, bi);
        if (!dbs.meta || !dbs.db) continue;

        db_meta_artist_search_result_t *artists = NULL;
        size_t artist_count = 0;
        if (db_meta_search_artists(dbs.meta, credit_query, 50,
                                    &artists, &artist_count) != QUADRATURE_OK)
            continue;

        for (size_t ai = 0; ai < artist_count; ai++) {
            db_meta_artist_credit_t *credits = NULL;
            size_t credit_count = 0;
            if (db_meta_get_credits_by_artist(dbs.meta, artists[ai].artist_mbid,
                                               NULL, &credits, &credit_count)
                != QUADRATURE_OK) continue;

            if (credit_count > 0) {
                db_track_position_t *pos = g_new0(db_track_position_t, credit_count);
                int64_t *tids = g_new0(int64_t, credit_count);
                for (size_t ci = 0; ci < credit_count; ci++) {
                    pos[ci].release_mbid = credits[ci].release_mbid;
                    pos[ci].disc_num     = credits[ci].disc_num;
                    pos[ci].track_num    = credits[ci].track_num;
                }
                db_resolve_track_positions_batch(dbs.db, pos, credit_count, tids);
                for (size_t ci = 0; ci < credit_count; ci++) {
                    if (tids[ci] == 0) continue;
                    int64_t gid = LIBRARY_MAKE_GLOBAL_ID(bi, tids[ci]);
                    int64_t *key = g_new(int64_t, 1); *key = gid;
                    if (!g_hash_table_lookup(track_set, key))
                        g_hash_table_insert(track_set, key, GINT_TO_POINTER(1));
                    else
                        g_free(key);
                }
                g_free(pos); g_free(tids);
            }
            db_meta_artist_credits_free(credits, credit_count);
        }
        db_meta_artist_search_results_free(artists, artist_count);
    }

    /* Mirror apply_search_with_credits's album derivation: dedup by
     * raw `t->album_id` of resolved tracks. (Suspected source of the bug.) */
    GArray *tracks = g_array_new(FALSE, FALSE, sizeof(int64_t));
    GArray *albums = g_array_new(FALSE, FALSE, sizeof(int64_t));
    GHashTable *seen_albums = g_hash_table_new(g_int64_hash, g_int64_equal);

    GHashTableIter it; gpointer k;
    g_hash_table_iter_init(&it, track_set);
    while (g_hash_table_iter_next(&it, &k, NULL)) {
        int64_t tid = *(int64_t *)k;
        g_array_append_val(tracks, tid);
        const library_track_info_t *t = library_cache_get_track(c, tid);
        if (!t) continue;
        if (!g_hash_table_contains(seen_albums, &t->album_id)) {
            g_hash_table_add(seen_albums, (gpointer)&t->album_id);
            g_array_append_val(albums, t->album_id);
        }
    }
    g_hash_table_unref(seen_albums);
    g_hash_table_unref(track_set);

    *out_track_ids = tracks;
    *out_album_ids = albums;
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
    link_ram_fixture(lib_root[0], SRC_TAGGED_RAM_PARENT, SRC_TAGGED_RAM_FOLDER);
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
    link_ram_fixture(lib_root[0], SRC_TAGLESS_RAM_PARENT, SRC_TAGLESS_RAM_FOLDER);
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
    link_ram_fixture(lib_root[0], SRC_TAGGED_RAM_PARENT,  SRC_TAGGED_RAM_FOLDER);
    link_ram_fixture(lib_root[1], SRC_TAGLESS_RAM_PARENT, SRC_TAGLESS_RAM_FOLDER);
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
