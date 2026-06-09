/**
 * Integration tests for MusicBrainz/AcoustID resolution stack.
 *
 * All connection details come from environment variables (see .env.example):
 *   MB_HOST, MB_DBNAME, MB_USER, MB_PG_PASSWORD
 *   ACOUSTID_DBNAME, ACOUSTID_USER, ACOUSTID_PG_PASSWORD
 *   ACOUSTID_INDEX_URL
 *
 * Tests skip gracefully if passwords are absent.
 *
 * Run:
 *   source .env && make test
 */

#include <criterion/criterion.h>
#include "quadrature/indexer.h"
#include "test_helpers.h"
#include "mb_test_env.h"
#include "quadrature/database.h"
#include "internal.h" /* mb_pg_client_t, mb_pg_exec, mb_pg_client_create/destroy */
#include <libpq-fe.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// =============================================================================
// Helpers
// =============================================================================

static const char *
env_or(const char *name, const char *fallback)
{
    const char *val = getenv(name);
    return (val && val[0]) ? val : fallback;
}

static const char *
mb_pg_conninfo(void)
{
    /* HTTP mode: return NULL → mb_resolver dispatches to HTTP backend. */
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
acoustid_pg_conninfo(void)
{
    if (quad_test_use_http())
        return NULL; /* HTTP backend has no AcoustID PG */
    const char *pw = getenv("ACOUSTID_PG_PASSWORD");
    if (!pw || !pw[0])
        return NULL;
    static char buf[512];
    snprintf(buf,
             sizeof(buf),
             "host=%s dbname=%s user=%s password=%s connect_timeout=%s",
             env_or("MB_HOST", "localhost"),
             env_or("ACOUSTID_DBNAME", "acoustid"),
             env_or("ACOUSTID_USER", "acoustid"),
             pw,
             env_or("MB_PG_CONNECT_TIMEOUT", "5"));
    return buf;
}

static const char *
acoustid_index_url(void)
{
    return env_or("ACOUSTID_INDEX_URL", NULL);
}

// =============================================================================
// Test 1: acoustid-index HTTP — POST a real fingerprint, verify no crash
// =============================================================================
//
// tone_440hz.wav is a synthetic tone and won't match any real recording.
// The test verifies: mb_acoustid_lookup handles no-match gracefully (returns OK).
// It also exercises the HTTP POST path end-to-end and verifies PG connectivity.

Test(mb_resolve, acoustid_http_post)
{
    QUAD_TEST_REQUIRE(QUAD_TEST_NEEDS_DIRECT_PG);
    const char *ac_conninfo = acoustid_pg_conninfo();
    if (!ac_conninfo) {
        cr_skip("ACOUSTID_PG_PASSWORD not set");
    }
    const char *mb_conninfo = mb_pg_conninfo();
    if (!mb_conninfo) {
        cr_skip("MB_PG_PASSWORD not set");
    }

    mb_pg_client_t *mb_client = NULL;
    mb_pg_client_t *ac_client = NULL;
    cr_assert_eq(
        mb_pg_client_create(mb_conninfo, &mb_client), QUADRATURE_OK, "failed to connect to MB PG");
    mb_pg_set_schema(mb_client, "musicbrainz");
    cr_assert_eq(mb_pg_client_create(ac_conninfo, &ac_client),
                 QUADRATURE_OK,
                 "failed to connect to acoustid PG");

    // Generate fingerprint from a synthetic WAV (no real match expected)
    mb_fingerprint_t fp;
    quadrature_result_t res
        = mb_fingerprint_generate(SOURCE_DIR "/tests/assets/tone_440hz.wav", &fp);
    cr_assert_eq(res, QUADRATURE_OK, "fingerprint generation failed");

    const char *index_url = acoustid_index_url();
    if (!index_url) {
        mb_fingerprint_free(&fp);
        mb_pg_client_destroy(mb_client);
        mb_pg_client_destroy(ac_client);
        cr_skip("ACOUSTID_INDEX_URL not set");
    }

    // Prepare statements and create HTTP connection for the test
    mb_acoustid_prepare_stmts(mb_client, ac_client);
    mb_http_conn_t *http_conn = mb_http_conn_create(index_url);
    cr_assert_not_null(http_conn, "failed to create HTTP connection");

    mb_acoustid_response_t response;
    res = mb_acoustid_lookup(mb_client, ac_client, http_conn, &fp, &response);

    // Empty response (0 matches) is perfectly valid — just no crash
    cr_assert_eq(
        res, QUADRATURE_OK, "mb_acoustid_lookup must return OK even for no-match fingerprints");
    cr_assert_geq((int)response.count, 0);

    mb_acoustid_response_free(&response);
    mb_http_conn_destroy(http_conn);
    mb_fingerprint_free(&fp);
    mb_pg_client_destroy(mb_client);
    mb_pg_client_destroy(ac_client);
}

// =============================================================================
// Test 2: acoustid PG lookup — given known track_ids, verify recording MBIDs
// =============================================================================

Test(mb_resolve, acoustid_pg_lookup)
{
    QUAD_TEST_REQUIRE(QUAD_TEST_NEEDS_DIRECT_PG);
    const char *conninfo = acoustid_pg_conninfo();
    if (!conninfo) {
        cr_skip("ACOUSTID_PG_PASSWORD not set");
    }

    mb_pg_client_t *client = NULL;
    cr_assert_eq(
        mb_pg_client_create(conninfo, &client), QUADRATURE_OK, "failed to connect to acoustid PG");

    // Step 1: get the most-submitted track_id to use as test input
    PGresult *res = (PGresult *)mb_pg_exec(
        client, "SELECT track_id FROM track_mbid ORDER BY submission_count DESC LIMIT 1", 0, NULL);
    cr_assert_not_null(res, "NULL result from acoustid PG");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        mb_pg_client_destroy(client);
        cr_skip("acoustid PG query failed (schema not initialized?)");
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        mb_pg_client_destroy(client);
        cr_skip("acoustid PG has no data yet");
    }

    char tid_str[64];
    snprintf(tid_str, sizeof(tid_str), "{%s}", PQgetvalue(res, 0, 0));
    PQclear(res);

    // Step 2: query recording MBIDs for that track_id (same query as mb_acoustid.c)
    const char *params[1] = { tid_str };
    res = (PGresult *)mb_pg_exec(client,
                                 "SELECT tm.mbid::text, MAX(tm.submission_count) AS sc "
                                 "FROM track_mbid tm "
                                 "WHERE tm.track_id = ANY($1::int[]) "
                                 "GROUP BY tm.mbid "
                                 "ORDER BY sc DESC "
                                 "LIMIT 5",
                                 1,
                                 params);
    cr_assert_not_null(res, "NULL result");
    cr_assert_eq(PQresultStatus(res),
                 PGRES_TUPLES_OK,
                 "track_id→mbid query failed: %s",
                 PQresultErrorMessage(res));
    cr_assert_gt(PQntuples(res), 0, "no recording MBIDs for track %s", tid_str);

    // Validate UUID format (36 chars: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    const char *mbid = PQgetvalue(res, 0, 0);
    cr_assert_eq(strlen(mbid), 36U, "MBID wrong length: '%s'", mbid);
    cr_assert_eq(mbid[8], '-', "MBID format: '%s'", mbid);
    cr_assert_eq(mbid[13], '-', "MBID format: '%s'", mbid);
    cr_assert_eq(mbid[18], '-', "MBID format: '%s'", mbid);
    cr_assert_eq(mbid[23], '-', "MBID format: '%s'", mbid);
    PQclear(res);

    mb_pg_client_destroy(client);
}

// =============================================================================
// Test 3: mb_fetch_all_batch — given a release UUID, verify mb_release_t populated
// =============================================================================

Test(mb_resolve, mb_fetch_all_batch_test)
{
    QUAD_TEST_REQUIRE(QUAD_TEST_NEEDS_DIRECT_PG);
    const char *conninfo = mb_pg_conninfo();
    if (!conninfo) {
        cr_skip("MB_PG_PASSWORD not set");
    }

    mb_pg_client_t *client = NULL;
    cr_assert_eq(
        mb_pg_client_create(conninfo, &client), QUADRATURE_OK, "failed to connect to MB PG");
    mb_pg_set_schema(client, "musicbrainz");
    cr_assert_eq(
        mb_pg_install_batch_function(client), QUADRATURE_OK, "failed to install batch function");

    // Find a release that has at least one track (so recordings array is non-empty)
    PGresult *res = (PGresult *)mb_pg_exec(client,
                                           "SELECT r.gid::text, r.name "
                                           "FROM release r "
                                           "JOIN medium m ON m.release = r.id "
                                           "JOIN track t ON t.medium = m.id "
                                           "LIMIT 1",
                                           0,
                                           NULL);
    cr_assert_not_null(res, "NULL result from MB PG");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        mb_pg_client_destroy(client);
        cr_skip("MB PG query failed (schema not yet imported?)");
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        mb_pg_client_destroy(client);
        cr_skip("MB database has no releases yet");
    }

    char *release_uuid = g_strdup(PQgetvalue(res, 0, 0));
    char *expected_name = g_strdup(PQgetvalue(res, 0, 1));
    PQclear(res);

    GHashTable *releases = NULL;
    GHashTable *links = NULL;
    const char *ids[1] = { release_uuid };
    quadrature_result_t result = mb_fetch_all_batch(client, ids, 1, &releases, &links);
    cr_assert_eq(result, QUADRATURE_OK, "mb_fetch_all_batch failed for %s", release_uuid);

    mb_release_t *release = g_hash_table_lookup(releases, release_uuid);
    cr_assert_not_null(release, "release not found in batch results");
    cr_assert_not_null(release->id, "release.id is NULL");
    cr_assert_not_null(release->title, "release.title is NULL");
    cr_assert_str_eq(release->title,
                     expected_name,
                     "title mismatch: got '%s', expected '%s'",
                     release->title,
                     expected_name);
    cr_assert_gt((int)release->recording_count, 0, "release has no recordings");

    g_hash_table_destroy(releases);
    g_hash_table_destroy(links);
    g_free(release_uuid);
    g_free(expected_name);
    mb_pg_client_destroy(client);
}

// =============================================================================
// Test 4: full_resolve — end-to-end using tag-based release ID path
// =============================================================================
//
// Creates an in-memory SQLite DB with one album, seeds it with a real MB release
// UUID (via db_set_album_release_id_from_tags, as Phase 2 would do), then runs
// mb_resolver_run and verifies the album transitions from unresolved → resolved.

Test(mb_resolve, full_resolve)
{
    /* Test seeds release_uuid via raw mb_pg_exec — direct-PG only. */
    QUAD_TEST_REQUIRE(QUAD_TEST_NEEDS_DIRECT_PG);
    const char *mb_conninfo = mb_pg_conninfo();
    if (!mb_conninfo) {
        cr_skip("MB_PG_PASSWORD not set");
    }

    // Pick a release UUID with at least 3 tracks from MB PG
    mb_pg_client_t *client = NULL;
    cr_assert_eq(mb_pg_client_create(mb_conninfo, &client), QUADRATURE_OK);
    mb_pg_set_schema(client, "musicbrainz");

    PGresult *res = (PGresult *)mb_pg_exec(client,
                                           "SELECT r.gid::text "
                                           "FROM release r "
                                           "JOIN medium m ON m.release = r.id "
                                           "JOIN track t ON t.medium = m.id "
                                           "GROUP BY r.id "
                                           "HAVING COUNT(t.id) >= 3 "
                                           "LIMIT 1",
                                           0,
                                           NULL);
    cr_assert_not_null(res);
    cr_assert_eq(PQresultStatus(res),
                 PGRES_TUPLES_OK,
                 "release query failed: %s",
                 PQresultErrorMessage(res));
    cr_assert_gt(PQntuples(res), 0, "MB database has no releases with 3+ tracks");

    char *release_uuid = g_strdup(PQgetvalue(res, 0, 0));
    PQclear(res);
    mb_pg_client_destroy(client);

    // Build in-memory SQLite DB with one album
    quadrature_db_t *db = NULL;
    cr_assert_eq(db_open(NULL, false, &db), QUADRATURE_OK);

    int64_t artist_id = db_get_or_create_artist(db, "Test Artist", NULL, NULL);
    cr_assert_gt(artist_id, 0LL, "failed to create artist");

    int64_t album_id = test_insert_album(db, "/fake/test", "Test Album", artist_id, 2020);
    int64_t track_id = test_insert_track_full(db,
                                              album_id,
                                              "/fake/test/01-track.flac",
                                              "Test Track 1",
                                              1,
                                              1,
                                              180000,
                                              NULL,
                                              NULL,
                                              NULL,
                                              0);
    (void)track_id;

    // Simulate Phase 2: stamp the MB release UUID and HAS_RELEASE_ID status
    // onto the album via the reconciler.
    {
        desired_album_state_t desired = {
            .source = RECONCILE_SOURCE_TAGS,
            .present_fields = DESIRED_ALBUM_MB_RELEASE_ID | DESIRED_ALBUM_MB_STATUS,
            .musicbrainz_release_id = release_uuid,
            .mb_status = MB_STATUS_HAS_RELEASE_ID,
        };
        reconcile_policy_t p = { 0 };
        cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
        cr_assert_eq(db_reconcile_albums(db, &album_id, &desired, 1, &p, NULL), QUADRATURE_OK);
        cr_assert_eq(db_commit(db), QUADRATURE_OK);
    }

    // Verify album is initially unresolved
    int64_t *unresolved = NULL;
    size_t unresolved_count = 0;
    cr_assert_eq(db_get_unresolved_albums(db, 0, &unresolved, &unresolved_count), QUADRATURE_OK);
    cr_assert_eq(unresolved_count, 1U, "expected 1 unresolved album before resolve");
    g_free(unresolved);

    // Run the resolver (tag path — no fingerprint required)
    mb_resolver_options_t opts = {
        .pg_conninfo = mb_conninfo,
        .acoustid_pg_conninfo = NULL,
        .acoustid_index_url = NULL,
        .parallelism = 1,
        .library_root = "/fake/test",
    };
    mb_resolver_t *resolver = NULL;
    cr_assert_eq(mb_resolver_create(&resolver, db, &opts, NULL, NULL),
                 QUADRATURE_OK,
                 "mb_resolver_create failed");
    cr_assert_eq(mb_resolver_run(resolver), QUADRATURE_OK, "mb_resolver_run failed");
    mb_resolver_destroy(resolver);

    // After resolution the album must no longer appear as unresolved
    cr_assert_eq(db_get_unresolved_albums(db, 0, &unresolved, &unresolved_count), QUADRATURE_OK);
    cr_assert_eq(unresolved_count, 0U, "album not resolved: still in unresolved list");
    g_free(unresolved);

    g_free(release_uuid);
    db_close(db);
}
