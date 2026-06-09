/**
 * Reconciler tests — the BRONSON scenario and other diff behaviors.
 *
 * The BRONSON case: ten FLAC files with no TRACKNUMBER / TITLE tags. Phase 2
 * writes them with track_num=0. Phase 6 matches them against MB (by title
 * similarity + duration, Pass 2) and must write the canonical track
 * positions back. The reconciler is the single writer making that happen.
 *
 * These tests bypass the full indexer pipeline and drive the reconciler
 * directly against a synthetic DB state. This isolates the reconciler's
 * correctness from FFmpeg/MB PG availability.
 */

#include <criterion/criterion.h>
#include "quadrature/database.h"
#include "quadrature/indexer.h"
#include "quadrature/library.h"
#include "../../src/database/internal.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

/* ----------------------------------------------------------------------------
 * Fixture: bare-bones DB with an album + N untagged tracks
 *
 * Returns the album_id; out_db_path is filled with the on-disk DB path for
 * subsequent library_cache mounting.
 * -------------------------------------------------------------------------- */
static int64_t
build_untagged_album(quadrature_db_t *db,
                     const char *album_path,
                     const char *album_title,
                     const char *artist_name,
                     const char *const *filenames,
                     size_t track_count,
                     int64_t *artist_id_out)
{
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);

    /* Artist */
    int64_t artist_id = db_get_or_create_artist(db, artist_name, NULL, NULL);
    cr_assert_gt(artist_id, 0);
    if (artist_id_out)
        *artist_id_out = artist_id;

    /* Album — create via reconciler's canonical API (no legacy calls) */
    int64_t album_id = 0;
    cr_assert_eq(
        db_create_or_get_album_by_path(db, album_path, album_title, artist_id, 0, &album_id),
        QUADRATURE_OK);
    cr_assert_gt(album_id, 0);

    /* Tracks — raw INSERT each with track_num=0 (the BRONSON pre-state:
     * untagged FLACs written by Phase 2 before any MB resolution). We use
     * direct SQL rather than the reconciler because the reconciler's
     * normal insert path would populate track_num from tag data — and the
     * whole point of the test is to start from track_num=0. */
    for (size_t i = 0; i < track_count; i++) {
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db->db,
                           "INSERT INTO tracks(title, album_id, path, duration_ms, "
                           "track_num, disc_num, mtime, year) "
                           "VALUES('', ?, ?, ?, 0, 1, 0, 0)",
                           -1,
                           &ins,
                           NULL);
        sqlite3_bind_int64(ins, 1, album_id);
        sqlite3_bind_text(ins, 2, filenames[i], -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 3, (int)(180000 + i * 1000));
        cr_assert_eq(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_finalize(ins);
    }

    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    return album_id;
}

/* ----------------------------------------------------------------------------
 * Build a desired_album_state_t as if MB had matched all tracks in order
 * with Pass 2 (fuzzy) confidence — the realistic BRONSON case.
 * -------------------------------------------------------------------------- */
typedef struct {
    desired_track_t *tracks;
    desired_track_artist_t **credits;
    size_t n;
} mb_mock_t;

static mb_mock_t
build_mb_mock(const char *const *paths,
              const char *const *titles,
              size_t n,
              int64_t artist_id,
              const char *artist_name,
              reconcile_confidence_t conf)
{
    mb_mock_t m = { .tracks = g_new0(desired_track_t, n),
                    .credits = g_new0(desired_track_artist_t *, n),
                    .n = n };
    for (size_t i = 0; i < n; i++) {
        m.credits[i] = g_new0(desired_track_artist_t, 1);
        m.credits[i][0] = (desired_track_artist_t){
            .artist_id = artist_id,
            .name = g_strdup(artist_name),
            .join_phrase = g_strdup(""),
            .position = 0,
        };
        m.tracks[i] = (desired_track_t){
            .path = paths[i],
            .present_fields
            = DESIRED_TRACK_TITLE | DESIRED_TRACK_NUM | DESIRED_TRACK_DISC | DESIRED_TRACK_ARTISTS,
            .title = titles[i],
            .track_num = (uint16_t)(i + 1),
            .disc_num = 1,
            .artists = m.credits[i],
            .artist_count = 1,
            .position_confidence = conf,
        };
    }
    return m;
}

static void
mb_mock_free(mb_mock_t *m)
{
    for (size_t i = 0; i < m->n; i++) {
        g_free((char *)m->credits[i][0].name);
        g_free((char *)m->credits[i][0].join_phrase);
        g_free(m->credits[i]);
    }
    g_free(m->credits);
    g_free(m->tracks);
}

/* ════════════════════════════════════════════════════════════════════════════
 * TEST 1: BRONSON — untagged tracks get track_num from MB
 * ════════════════════════════════════════════════════════════════════════════ */

Test(reconciler, bronson_untagged_tracks_get_positions_from_mb)
{
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "/tmp/quad_reconciler_%d.sqlite", getpid());
    unlink(db_path);

    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path, false, &db), QUADRATURE_OK);

    /* Canonical BRONSON tracklist (release 5ed617d7-898f-4e05-82a1-bfc586a4b013) */
    const char *filenames[] = {
        "01 - FOUNDATION (FLAC 828 kbps).flac",  "02 - HEART ATTACK (FLAC 840 kbps).flac",
        "03 - BLINE (FLAC 871 kbps).flac",       "04 - KNOW ME (FLAC 892 kbps).flac",
        "05 - VAULTS (FLAC 901 kbps).flac",      "06 - TENSE (FLAC 913 kbps).flac",
        "07 - CALL OUT (FLAC 872 kbps).flac",    "08 - CONTACT (FLAC 1043 kbps).flac",
        "09 - KEEP MOVING (FLAC 912 kbps).flac", "10 - DAWN (FLAC 886 kbps).flac",
    };
    const char *mb_titles[] = {
        "Foundation", "Heart Attack", "Bline",   "KNOW ME",     "Vaults",
        "Tense",      "Call Out",     "Contact", "Keep Moving", "Dawn",
    };
    const size_t N = G_N_ELEMENTS(filenames);

    int64_t artist_id = 0;
    int64_t album_id = build_untagged_album(
        db, "BRONSON/BRONSON (2020)", "BRONSON", "BRONSON", filenames, N, &artist_id);

    /* --- Sanity check: all tracks currently have track_num=0 (the bug) --- */
    {
        db_track_t *tracks = NULL;
        size_t count = 0;
        cr_assert_eq(db_get_tracks_by_album(db, album_id, &tracks, &count), QUADRATURE_OK);
        cr_assert_eq(count, N);
        for (size_t i = 0; i < count; i++) {
            cr_assert_eq(tracks[i].track_num, 0, "Pre-reconcile track_num must be 0 (untagged)");
        }
        db_tracks_free(tracks, count);
    }

    /* --- Build desired state simulating an MB Pass 2 match --- */
    mb_mock_t mock
        = build_mb_mock(filenames, mb_titles, N, artist_id, "BRONSON", RECONCILE_CONFIDENCE_FUZZY);

    desired_album_state_t desired = {
        .source = RECONCILE_SOURCE_MB,
        .present_fields = DESIRED_ALBUM_TITLE | DESIRED_ALBUM_MB_RELEASE_ID
                          | DESIRED_ALBUM_MB_STATUS | DESIRED_ALBUM_MB_RESOLVED_AT,
        .title = "BRONSON",
        .musicbrainz_release_id = "5ed617d7-898f-4e05-82a1-bfc586a4b013",
        .mb_status = MB_STATUS_RESOLVED,
        .mb_resolved_at = 1700000000,
        .tracks = mock.tracks,
        .track_count = mock.n,
    };

    /* --- Reconcile --- */
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    reconcile_summary_t summary = { 0 };
    cr_assert_eq(db_reconcile_albums(db, &album_id, &desired, 1, &RECONCILE_POLICY_MB, &summary),
                 QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    cr_assert_gt(summary.tracks_updated, 0, "Reconciler should have updated every track");
    cr_assert_eq(summary.track_positions_changed,
                 (int)N,
                 "All %zu track positions should have been written back",
                 N);
    cr_assert_eq(
        summary.track_titles_changed, (int)N, "All %zu track titles should have been written", N);
    cr_assert(summary.fts_synced, "FTS must be synced when titles changed");

    /* --- Verify via DB direct read --- */
    {
        db_track_t *tracks = NULL;
        size_t count = 0;
        cr_assert_eq(db_get_tracks_by_album(db, album_id, &tracks, &count), QUADRATURE_OK);
        cr_assert_eq(count, N);
        /* Tracks come back ordered by (disc, track_num) */
        for (size_t i = 0; i < count; i++) {
            cr_assert_eq(tracks[i].track_num,
                         (uint16_t)(i + 1),
                         "Track at position %zu should have track_num=%zu (got %u)",
                         i,
                         i + 1,
                         tracks[i].track_num);
            cr_assert_str_eq(tracks[i].title,
                             mb_titles[i],
                             "Track %zu title should be '%s' (got '%s')",
                             i,
                             mb_titles[i],
                             tracks[i].title);
        }
        db_tracks_free(tracks, count);
    }

    db_close(db);

    /* --- Verify via library_cache --- */
    library_cache_source_t src = {
        .db_path = db_path,
        .music_base = "/tmp",
        .display_name = "Test",
        .bitmap_index = 0,
    };
    library_cache_t *cache = NULL;
    cr_assert_eq(library_cache_create_multi(&src, 1, &cache), QUADRATURE_OK);
    library_cache_warm_slot_blocking(cache, 0);

    GPtrArray *albums = library_cache_get_albums_filtered(
        cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(albums);
    cr_assert_eq(albums->len, 1, "Exactly one album in cache");
    const library_album_info_t *album = g_ptr_array_index(albums, 0);
    cr_assert_str_eq(album->title, "BRONSON");

    GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album->album_id, LIBRARY_MASK_ALL);
    cr_assert_not_null(tracks);
    cr_assert_eq(tracks->len, N, "Library cache should expose all %zu tracks", N);

    for (size_t i = 0; i < N; i++) {
        const library_track_info_t *t = g_ptr_array_index(tracks, i);
        cr_assert_eq(t->track_num,
                     (uint16_t)(i + 1),
                     "library_cache: track %zu should have track_num=%zu (got %u)",
                     i,
                     i + 1,
                     t->track_num);
        cr_assert_str_eq(t->title, mb_titles[i]);
    }

    g_ptr_array_unref(tracks);
    g_ptr_array_unref(albums);
    library_cache_destroy(cache);
    mb_mock_free(&mock);
    unlink(db_path);
}

/* ════════════════════════════════════════════════════════════════════════════
 * TEST 2: Idempotency — reconciling with identical desired state is a no-op
 * ════════════════════════════════════════════════════════════════════════════ */

Test(reconciler, idempotent_when_already_consistent)
{
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "/tmp/quad_reconciler_idem_%d.sqlite", getpid());
    unlink(db_path);

    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path, false, &db), QUADRATURE_OK);

    const char *filenames[] = { "01.flac", "02.flac", "03.flac" };
    const char *titles[] = { "A", "B", "C" };
    const size_t N = G_N_ELEMENTS(filenames);

    int64_t artist_id = 0;
    int64_t album_id
        = build_untagged_album(db, "test/album", "Album", "Artist", filenames, N, &artist_id);

    mb_mock_t mock
        = build_mb_mock(filenames, titles, N, artist_id, "Artist", RECONCILE_CONFIDENCE_EXACT);
    desired_album_state_t desired = {
        .source = RECONCILE_SOURCE_MB,
        .present_fields
        = DESIRED_ALBUM_TITLE | DESIRED_ALBUM_MB_STATUS | DESIRED_ALBUM_MB_RESOLVED_AT,
        .title = "Album",
        .mb_status = MB_STATUS_RESOLVED,
        .mb_resolved_at = 1700000000,
        .tracks = mock.tracks,
        .track_count = mock.n,
    };

    /* First reconcile: everything changes. */
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    reconcile_summary_t s1 = { 0 };
    cr_assert_eq(db_reconcile_albums(db, &album_id, &desired, 1, &RECONCILE_POLICY_MB, &s1),
                 QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);
    cr_assert_gt(s1.tracks_updated, 0);

    /* Second reconcile with identical desired state: zero writes. */
    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    reconcile_summary_t s2 = { 0 };
    cr_assert_eq(db_reconcile_albums(db, &album_id, &desired, 1, &RECONCILE_POLICY_MB, &s2),
                 QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    cr_assert_eq(s2.tracks_updated, 0, "Second reconcile must be a no-op");
    cr_assert_eq(s2.track_positions_changed, 0);
    cr_assert_eq(s2.track_titles_changed, 0);
    cr_assert_eq(s2.track_artists_changed, 0);
    cr_assert_eq(s2.album_fields_changed, 0);
    cr_assert(!s2.fts_synced, "FTS sync must not fire when nothing changed");

    db_close(db);
    mb_mock_free(&mock);
    unlink(db_path);
}

/* ════════════════════════════════════════════════════════════════════════════
 * TEST 3: Confidence gate — MB position below threshold is NOT applied
 * ════════════════════════════════════════════════════════════════════════════ */

Test(reconciler, confidence_gate_blocks_low_confidence_position_writeback)
{
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "/tmp/quad_reconciler_conf_%d.sqlite", getpid());
    unlink(db_path);

    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(db_path, false, &db), QUADRATURE_OK);

    const char *filenames[] = { "01.flac", "02.flac" };
    const char *titles[] = { "One", "Two" };

    int64_t artist_id = 0;
    int64_t album_id
        = build_untagged_album(db, "conf/album", "Album", "Artist", filenames, 2, &artist_id);

    /* Desired with CONFIDENCE_NONE (no match) — positions must NOT be applied */
    mb_mock_t mock
        = build_mb_mock(filenames, titles, 2, artist_id, "Artist", RECONCILE_CONFIDENCE_NONE);
    desired_album_state_t desired = {
        .source = RECONCILE_SOURCE_MB,
        .present_fields = DESIRED_ALBUM_TITLE,
        .title = "Album",
        .tracks = mock.tracks,
        .track_count = mock.n,
    };

    /* Policy requires FUZZY or higher — NONE is below threshold. */
    reconcile_policy_t strict = RECONCILE_POLICY_MB;
    strict.mb_position_min_confidence = RECONCILE_CONFIDENCE_FUZZY;

    cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    reconcile_summary_t s = { 0 };
    cr_assert_eq(db_reconcile_albums(db, &album_id, &desired, 1, &strict, &s), QUADRATURE_OK);
    cr_assert_eq(db_commit(db), QUADRATURE_OK);

    cr_assert_eq(s.track_positions_changed,
                 0,
                 "Positions with CONFIDENCE_NONE must not be written under FUZZY policy");

    /* Verify track_nums are still 0. */
    db_track_t *tracks = NULL;
    size_t count = 0;
    cr_assert_eq(db_get_tracks_by_album(db, album_id, &tracks, &count), QUADRATURE_OK);
    for (size_t i = 0; i < count; i++)
        cr_assert_eq(
            tracks[i].track_num, 0, "track_num must remain 0 (confidence gate blocked writeback)");
    db_tracks_free(tracks, count);

    db_close(db);
    mb_mock_free(&mock);
    unlink(db_path);
}
