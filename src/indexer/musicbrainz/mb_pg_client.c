/**
 * MusicBrainz PostgreSQL client (formerly mb_postgres.c).
 *
 * Queries a self-hosted MusicBrainz PostgreSQL database directly via libpq.
 * Returns mb_release_t / mb_recording_t types for the resolver.
 *
 * Compiled only when QUADRATURE_USE_LIBPQ is defined (default ON; OFF for
 * Flatpak builds, which use the HTTP backend in mb_http_*.c instead).
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <libpq-fe.h>
#include <string.h>
#include <stdlib.h>

// =============================================================================
// PG Client Lifecycle
// =============================================================================

struct mb_pg_client {
    PGconn *conn;
};

quadrature_result_t
mb_pg_client_create(const char *conninfo, mb_pg_client_t **out)
{
    if (!conninfo || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    // Prepend connect_timeout so user's conninfo can override (libpq uses last value)
    char *full_conninfo = g_strdup_printf("connect_timeout=15 %s", conninfo);
    PGconn *conn = PQconnectdb(full_conninfo);
    g_free(full_conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        g_warning("MusicBrainz PG connection failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return QUADRATURE_ERROR_INTERNAL;
    }

    mb_pg_client_t *client = g_new0(mb_pg_client_t, 1);
    client->conn = conn;
    *out = client;
    return QUADRATURE_OK;
}

void
mb_pg_client_destroy(mb_pg_client_t *client)
{
    if (!client)
        return;
    if (client->conn)
        PQfinish(client->conn);
    g_free(client);
}

bool
mb_pg_client_reset(mb_pg_client_t *client)
{
    if (!client || !client->conn)
        return false;
    PQreset(client->conn);
    return PQstatus(client->conn) == CONNECTION_OK;
}

// =============================================================================
// Query Helper (used by mb_acoustid.c)
// =============================================================================

void *
mb_pg_exec(mb_pg_client_t *client, const char *query, int nparams, const char *const *params)
{
    if (!client || !client->conn || !query)
        return NULL;
    return PQexecParams(client->conn, query, nparams, NULL, params, NULL, NULL, 0);
}

quadrature_result_t
mb_pg_prepare(mb_pg_client_t *client, const char *stmt_name, const char *query, int nparams)
{
    if (!client || !client->conn || !stmt_name || !query)
        return QUADRATURE_ERROR_INVALID_PARAM;

    PGresult *res = PQprepare(client->conn, stmt_name, query, nparams, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        g_warning("mb_pg_prepare(%s): %s", stmt_name, PQerrorMessage(client->conn));
        PQclear(res);
        return QUADRATURE_ERROR_INTERNAL;
    }
    PQclear(res);
    return QUADRATURE_OK;
}

void *
mb_pg_exec_prepared(mb_pg_client_t *client,
                    const char *stmt_name,
                    int nparams,
                    const char *const *params)
{
    if (!client || !client->conn || !stmt_name)
        return NULL;
    return PQexecPrepared(client->conn, stmt_name, nparams, params, NULL, NULL, 0);
}

void
mb_pg_set_schema(mb_pg_client_t *client, const char *schema)
{
    if (!client || !client->conn || !schema)
        return;
    char *sql = g_strdup_printf("SET search_path TO %s", schema);
    PGresult *res = PQexec(client->conn, sql);
    g_free(sql);
    if (res)
        PQclear(res);
}

// =============================================================================
// Helper: safe string from PG result
// =============================================================================

static char *
pg_get_string(PGresult *res, int row, int col)
{
    if (PQgetisnull(res, row, col))
        return NULL;
    const char *val = PQgetvalue(res, row, col);
    return (val && val[0]) ? g_strdup(val) : NULL;
}

static int
pg_get_int(PGresult *res, int row, int col, int default_val)
{
    if (PQgetisnull(res, row, col))
        return default_val;
    return atoi(PQgetvalue(res, row, col));
}

// =============================================================================
// Free Functions
// =============================================================================

/* mb_artist_free / mb_recording_free / mb_release_free moved to mb_backend.c
 * (shared by both backends). */

// =============================================================================
// UUID Array Formatting
// =============================================================================

char *
mb_format_uuid_array(const char **uuids, size_t count)
{
    if (count == 0)
        return g_strdup("{}");
    GString *s = g_string_new("{");
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            g_string_append_c(s, ',');
        g_string_append(s, uuids[i]);
    }
    g_string_append_c(s, '}');
    return g_string_free(s, FALSE);
}

// =============================================================================
// Consolidated Batch Fetch via Session-Local PG Function
// =============================================================================

/**
 * SQL body for pg_temp.mb_batch_fetch(uuid[]).
 *
 * Returns a tagged result set ordered by section integer (column 1) so the C
 * parser always sees rows in dependency order:
 *
 *   section=1  R: release metadata + genres
 *   section=2  A: album artist credits   <- must follow R so the release is in
 *                                           the hash table when we attach artists
 *   section=3  T: recording + track artist credits
 *   section=4  L: artist-recording links (CTE forces index on entity1)
 *
 * Using integers rather than chars makes the ordering contract explicit:
 * ORDER BY 1 gives 1,2,3,4 without relying on ASCII collation of letters.
 */
static const char *MB_BATCH_FUNCTION_SQL
    = "CREATE OR REPLACE FUNCTION pg_temp.mb_batch_fetch(release_gids uuid[])\n"
      "RETURNS TABLE(\n"
      "  section      int,\n"
      "  release_gid  text,\n"
      "  title        text,\n"
      "  rg_gid       text,\n"
      "  type         text,\n"
      "  barcode      text,\n"
      "  label        text,\n"
      "  catalog      text,\n"
      "  date         text,\n"
      "  genres       text,\n"
      "  artist_gid   text,\n"
      "  artist_name  text,\n"
      "  credited     text,\n"
      "  sort_name    text,\n"
      "  joinphrase   text,\n"
      "  artist_pos   int,\n"
      "  rec_gid      text,\n"
      "  rec_title    text,\n"
      "  track_pos    int,\n"
      "  disc         int,\n"
      "  duration_ms  int,\n"
      "  lt_gid       text,\n"
      "  lt_name      text,\n"
      "  lt_desc      text,\n"
      "  artist_type  text,\n"
      "  e0_credit    text,\n"
      "  attributes   text\n"
      ") AS $$\n"
      "WITH target AS (\n"
      "  SELECT r.id, r.gid, r.name, r.release_group, r.artist_credit,\n"
      "         rg.gid AS rg_gid, rgpt.name AS type, r.barcode\n"
      "  FROM release r\n"
      "  JOIN release_group rg ON r.release_group = rg.id\n"
      "  LEFT JOIN release_group_primary_type rgpt ON rg.type = rgpt.id\n"
      "  WHERE r.gid = ANY(release_gids)\n"
      "),\n"
      "target_recs AS (\n"
      "  SELECT DISTINCT t.recording, tgt.gid AS release_gid,\n"
      "         m.position AS disc, t.position AS track_pos\n"
      "  FROM target tgt\n"
      "  JOIN medium m ON m.release = tgt.id\n"
      "  JOIN track t ON t.medium = m.id\n"
      ")\n"
      "\n"
      "-- section=1: release metadata + genres\n"
      "SELECT 1,\n"
      "  tgt.gid::text,\n"
      "  tgt.name,\n"
      "  tgt.rg_gid::text,\n"
      "  tgt.type,\n"
      "  tgt.barcode,\n"
      "  (SELECT li.name FROM label li\n"
      "   JOIN release_label rl ON rl.label = li.id\n"
      "   WHERE rl.release = tgt.id LIMIT 1),\n"
      "  (SELECT rl.catalog_number FROM release_label rl\n"
      "   WHERE rl.release = tgt.id AND rl.catalog_number IS NOT NULL LIMIT 1),\n"
      "  COALESCE(to_char(re.date_year, 'FM0000') ||\n"
      "    CASE WHEN re.date_month IS NOT NULL\n"
      "      THEN '-' || to_char(re.date_month, 'FM00') ELSE '' END ||\n"
      "    CASE WHEN re.date_day IS NOT NULL\n"
      "      THEN '-' || to_char(re.date_day, 'FM00') ELSE '' END, ''),\n"
      "  (SELECT string_agg(t.name, ';' ORDER BY rgt.count DESC)\n"
      "   FROM release_group_tag rgt\n"
      "   JOIN tag t ON t.id = rgt.tag\n"
      "   JOIN genre g ON g.name = t.name\n"
      "   WHERE rgt.release_group = tgt.release_group AND rgt.count > 0\n"
      "   ),\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::int,\n"
      "  NULL::text, NULL::text, NULL::int, NULL::int, NULL::int,\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text\n"
      "FROM target tgt\n"
      "LEFT JOIN release_country re ON re.release = tgt.id\n"
      "\n"
      "UNION ALL\n"
      "\n"
      "-- section=2: album artist credits\n"
      "SELECT 2,\n"
      "  tgt.gid::text,\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, "
      "NULL::text,\n"
      "  a.gid::text, a.name, acn.name, a.sort_name, acn.join_phrase,\n"
      "  acn.position,\n"
      "  NULL::text, NULL::text, NULL::int, NULL::int, NULL::int,\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text\n"
      "FROM target tgt\n"
      "JOIN artist_credit_name acn ON acn.artist_credit = tgt.artist_credit\n"
      "JOIN artist a ON a.id = acn.artist\n"
      "\n"
      "UNION ALL\n"
      "\n"
      "-- section=3: recordings + track artist credits\n"
      "SELECT 3,\n"
      "  tr.release_gid::text,\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, "
      "NULL::text,\n"
      "  a.gid::text, a.name, acn.name, a.sort_name, acn.join_phrase,\n"
      "  acn.position,\n"
      "  rec.gid::text, rec.name, tr.track_pos, tr.disc, rec.length,\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text\n"
      "FROM target_recs tr\n"
      "JOIN recording rec ON rec.id = tr.recording\n"
      "LEFT JOIN artist_credit_name acn ON acn.artist_credit = rec.artist_credit\n"
      "LEFT JOIN artist a ON a.id = acn.artist\n"
      "\n"
      "UNION ALL\n"
      "\n"
      "-- section=4: artist-recording links (CTE forces index on l_artist_recording.entity1)\n"
      "SELECT 4,\n"
      "  tr.release_gid::text,\n"
      "  NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, NULL::text, "
      "NULL::text,\n"
      "  a.gid::text, a.name, NULL::text, a.sort_name, NULL::text, NULL::int,\n"
      "  rec.gid::text, NULL::text, NULL::int, NULL::int, NULL::int,\n"
      "  lt.gid::text, lt.name, lt.description,\n"
      "  at.name, lar.entity0_credit,\n"
      "  string_agg(lav.name, ',' ORDER BY lav.name)\n"
      "FROM target_recs tr\n"
      "JOIN l_artist_recording lar ON lar.entity1 = tr.recording\n"
      "JOIN link l ON l.id = lar.link\n"
      "JOIN link_type lt ON lt.id = l.link_type\n"
      "JOIN artist a ON a.id = lar.entity0\n"
      "JOIN recording rec ON rec.id = lar.entity1\n"
      "LEFT JOIN artist_type at ON at.id = a.type\n"
      "LEFT JOIN link_attribute la ON la.link = l.id\n"
      "LEFT JOIN link_attribute_type lav ON lav.id = la.attribute_type\n"
      "GROUP BY tr.release_gid, rec.gid, lt.gid, lt.name, lt.description,\n"
      "         a.gid, a.name, a.sort_name, at.name, lar.entity0_credit\n"
      "\n"
      "ORDER BY 1, 2, 20, 19, 16, 17, 23, 14;\n"
      "\n"
      "$$ LANGUAGE SQL STABLE;\n";

quadrature_result_t
mb_pg_install_batch_function(mb_pg_client_t *client)
{
    if (!client || !client->conn)
        return QUADRATURE_ERROR_INVALID_PARAM;

    PGresult *res = PQexec(client->conn, MB_BATCH_FUNCTION_SQL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        g_warning("mb_pg_install_batch_function: %s", PQerrorMessage(client->conn));
        PQclear(res);
        return QUADRATURE_ERROR_INTERNAL;
    }
    PQclear(res);
    return QUADRATURE_OK;
}

// =============================================================================
// Helpers for mb_fetch_all_batch
// =============================================================================

static void
batch_release_free(gpointer data)
{
    mb_release_t *r = data;
    mb_release_free(r);
    g_free(r);
}

static mb_artist_t *
flatten_artists(GPtrArray *ptrs)
{
    if (!ptrs || ptrs->len == 0) {
        if (ptrs)
            g_ptr_array_free(ptrs, TRUE);
        return NULL;
    }
    mb_artist_t *flat = g_new(mb_artist_t, ptrs->len);
    for (guint i = 0; i < ptrs->len; i++) {
        mb_artist_t *src = g_ptr_array_index(ptrs, i);
        flat[i] = *src;
        g_free(src);
    }
    g_ptr_array_free(ptrs, TRUE);
    return flat;
}

static void
link_row_free(gpointer data)
{
    mb_recording_link_row_t *row = data;
    if (!row)
        return;
    g_free(row->recording_mbid);
    g_free(row->link_type_gid);
    g_free(row->link_type_name);
    g_free(row->link_type_desc);
    g_free(row->artist_mbid);
    g_free(row->artist_name);
    g_free(row->artist_sort_name);
    g_free(row->artist_type);
    g_free(row->entity0_credit);
    g_free(row->attributes);
    g_free(row);
}

// =============================================================================
// mb_fetch_all_batch — Single PG call returns releases + links
// =============================================================================

/* Column indices in pg_temp.mb_batch_fetch result set */
enum {
    COL_SECTION = 0,
    COL_RELEASE_GID,
    COL_TITLE,
    COL_RG_GID,
    COL_TYPE,
    COL_BARCODE,
    COL_LABEL,
    COL_CATALOG,
    COL_DATE,
    COL_GENRES,
    COL_ARTIST_GID,
    COL_ARTIST_NAME,
    COL_CREDITED,
    COL_SORT_NAME,
    COL_JOINPHRASE,
    COL_POSITION,
    COL_REC_GID,
    COL_REC_TITLE,
    COL_TRACK_POS,
    COL_DISC,
    COL_DURATION_MS,
    COL_LT_GID,
    COL_LT_NAME,
    COL_LT_DESC,
    COL_ARTIST_TYPE,
    COL_E0_CREDIT,
    COL_ATTRIBUTES,
};

/**
 * Flush accumulated artist and recording state when transitioning
 * to a new recording or finishing the T section.
 */
static void
flush_recording(mb_recording_t *rec, GPtrArray *artists, GPtrArray *recs_arr)
{
    if (!rec)
        return;
    if (artists) {
        rec->artist_count = artists->len;
        rec->artists = flatten_artists(artists);
    }
    if (recs_arr)
        g_ptr_array_add(recs_arr, rec);
}

/**
 * Finalize recordings: move from per-release GPtrArray into release struct.
 */
static void
finalize_recordings(GHashTable *releases, GHashTable *rec_arrays)
{
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, rec_arrays);
    while (g_hash_table_iter_next(&iter, &key, &val)) {
        mb_release_t *rel = g_hash_table_lookup(releases, (const char *)key);
        GPtrArray *recs = val;
        if (rel && recs->len > 0) {
            rel->recording_count = recs->len;
            rel->recordings = g_new0(mb_recording_t, recs->len);
            for (guint i = 0; i < recs->len; i++) {
                mb_recording_t *src = g_ptr_array_index(recs, i);
                rel->recordings[i] = *src;
                g_free(src);
            }
        }
        g_ptr_array_free(recs, TRUE);
    }
}

quadrature_result_t
mb_fetch_all_batch(mb_pg_client_t *client,
                   const char **release_ids,
                   size_t count,
                   GHashTable **out_releases,
                   GHashTable **out_links)
{
    if (!client || !release_ids || count == 0 || !out_releases || !out_links)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *out_releases = NULL;
    *out_links = NULL;

    char *uuid_array = mb_format_uuid_array(release_ids, count);
    const char *params[1] = { uuid_array };

    PGresult *res = PQexecParams(client->conn,
                                 "SELECT * FROM pg_temp.mb_batch_fetch($1::uuid[])",
                                 1,
                                 NULL,
                                 params,
                                 NULL,
                                 NULL,
                                 0);

    g_free(uuid_array);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        g_warning("mb_fetch_all_batch: PG query failed: %s", PQerrorMessage(client->conn));
        PQclear(res);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int nrows = PQntuples(res);
    /* Output containers */
    GHashTable *releases = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, batch_release_free);
    GHashTable *links
        = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    GHashTable *rec_arrays = g_hash_table_new(g_str_hash, g_str_equal);

    /* Per-release artist accumulator (A section) */
    GPtrArray *cur_album_artists = NULL;
    char *cur_album_gid = NULL;

    /* Per-recording state (T section) */
    mb_recording_t *cur_rec = NULL;
    GPtrArray *cur_rec_artists = NULL;
    GPtrArray *cur_recs_arr = NULL;
    char *t_release_gid = NULL;
    char *t_rec_gid = NULL;

    for (int row = 0; row < nrows; row++) {
        int section = atoi(PQgetvalue(res, row, COL_SECTION));
        const char *rel_gid = PQgetvalue(res, row, COL_RELEASE_GID);

        switch (section) {
        case 1: {
            /* Flush previous album artists if switching releases */
            if (cur_album_gid && cur_album_artists) {
                mb_release_t *prev = g_hash_table_lookup(releases, cur_album_gid);
                if (prev) {
                    prev->artist_count = cur_album_artists->len;
                    prev->artists = flatten_artists(cur_album_artists);
                    cur_album_artists = NULL;
                }
            }

            /* Deduplicate: LEFT JOIN release_country can produce multiple
             * section=1 rows per release (one per country). Skip if already seen. */
            if (g_hash_table_contains(releases, rel_gid)) {
                g_free(cur_album_gid);
                cur_album_gid = NULL;
                break;
            }

            mb_release_t *r = g_new0(mb_release_t, 1);
            r->id = pg_get_string(res, row, COL_RELEASE_GID);
            r->title = pg_get_string(res, row, COL_TITLE);
            r->release_group_id = pg_get_string(res, row, COL_RG_GID);
            r->type = pg_get_string(res, row, COL_TYPE);
            r->barcode = pg_get_string(res, row, COL_BARCODE);
            r->label = pg_get_string(res, row, COL_LABEL);
            r->catalog_number = pg_get_string(res, row, COL_CATALOG);
            r->date = pg_get_string(res, row, COL_DATE);
            r->genres = pg_get_string(res, row, COL_GENRES);

            g_hash_table_insert(releases, r->id, r);
            g_free(cur_album_gid);
            cur_album_gid = NULL;
            break;
        }

        case 2: {
            mb_release_t *r = g_hash_table_lookup(releases, rel_gid);
            if (!r)
                break;

            /* Track which release we're accumulating artists for */
            if (!cur_album_gid || g_strcmp0(rel_gid, cur_album_gid) != 0) {
                /* Flush previous */
                if (cur_album_gid && cur_album_artists) {
                    mb_release_t *prev = g_hash_table_lookup(releases, cur_album_gid);
                    if (prev) {
                        prev->artist_count = cur_album_artists->len;
                        prev->artists = flatten_artists(cur_album_artists);
                    }
                }
                cur_album_artists = g_ptr_array_new();
                g_free(cur_album_gid);
                cur_album_gid = g_strdup(rel_gid);
            }

            mb_artist_t *a = g_new0(mb_artist_t, 1);
            a->id = pg_get_string(res, row, COL_ARTIST_GID);
            a->name = pg_get_string(res, row, COL_ARTIST_NAME);
            a->credited_name = pg_get_string(res, row, COL_CREDITED);
            a->sort_name = pg_get_string(res, row, COL_SORT_NAME);
            a->joinphrase = pg_get_string(res, row, COL_JOINPHRASE);
            g_ptr_array_add(cur_album_artists, a);
            break;
        }

        case 3: {
            const char *rec_gid = PQgetvalue(res, row, COL_REC_GID);

            /* Detect new (release, recording) pair */
            bool new_recording = !t_rec_gid || g_strcmp0(rec_gid, t_rec_gid) != 0 || !t_release_gid
                                 || g_strcmp0(rel_gid, t_release_gid) != 0;

            if (new_recording) {
                /* Flush previous recording */
                flush_recording(cur_rec, cur_rec_artists, cur_recs_arr);
                cur_rec_artists = NULL;

                /* Ensure we have a recs array for this release */
                if (!t_release_gid || g_strcmp0(rel_gid, t_release_gid) != 0) {
                    cur_recs_arr = g_hash_table_lookup(rec_arrays, rel_gid);
                    if (!cur_recs_arr) {
                        cur_recs_arr = g_ptr_array_new();
                        mb_release_t *r = g_hash_table_lookup(releases, rel_gid);
                        if (r)
                            g_hash_table_insert(rec_arrays, r->id, cur_recs_arr);
                    }
                    g_free(t_release_gid);
                    t_release_gid = g_strdup(rel_gid);
                }

                cur_rec = g_new0(mb_recording_t, 1);
                cur_rec->id = pg_get_string(res, row, COL_REC_GID);
                cur_rec->title = pg_get_string(res, row, COL_REC_TITLE);
                cur_rec->position = pg_get_int(res, row, COL_TRACK_POS, 0);
                cur_rec->disc_number = pg_get_int(res, row, COL_DISC, 1);
                cur_rec->duration_ms = pg_get_int(res, row, COL_DURATION_MS, 0);
                cur_rec_artists = g_ptr_array_new();

                g_free(t_rec_gid);
                t_rec_gid = g_strdup(rec_gid);
            }

            /* Accumulate track artist */
            char *artist_id = pg_get_string(res, row, COL_ARTIST_GID);
            if (artist_id && cur_rec_artists) {
                mb_artist_t *a = g_new0(mb_artist_t, 1);
                a->id = artist_id;
                a->name = pg_get_string(res, row, COL_ARTIST_NAME);
                a->credited_name = pg_get_string(res, row, COL_CREDITED);
                a->sort_name = pg_get_string(res, row, COL_SORT_NAME);
                a->joinphrase = pg_get_string(res, row, COL_JOINPHRASE);
                g_ptr_array_add(cur_rec_artists, a);
            }
            break;
        }

        case 4: {
            char *release_gid_owned = pg_get_string(res, row, COL_RELEASE_GID);
            if (!release_gid_owned)
                break;

            GPtrArray *arr = g_hash_table_lookup(links, release_gid_owned);
            if (!arr) {
                arr = g_ptr_array_new_with_free_func(link_row_free);
                g_hash_table_insert(links, release_gid_owned, arr);
            } else {
                g_free(release_gid_owned);
            }

            mb_recording_link_row_t *lrow = g_new0(mb_recording_link_row_t, 1);
            lrow->recording_mbid = pg_get_string(res, row, COL_REC_GID);
            lrow->link_type_gid = pg_get_string(res, row, COL_LT_GID);
            lrow->link_type_name = pg_get_string(res, row, COL_LT_NAME);
            lrow->link_type_desc = pg_get_string(res, row, COL_LT_DESC);
            lrow->artist_mbid = pg_get_string(res, row, COL_ARTIST_GID);
            lrow->artist_name = pg_get_string(res, row, COL_ARTIST_NAME);
            lrow->artist_sort_name = pg_get_string(res, row, COL_SORT_NAME);
            lrow->artist_type = pg_get_string(res, row, COL_ARTIST_TYPE);
            lrow->entity0_credit = pg_get_string(res, row, COL_E0_CREDIT);
            lrow->attributes = pg_get_string(res, row, COL_ATTRIBUTES);
            g_ptr_array_add(arr, lrow);
            break;
        }

        default:
            g_warning("mb_fetch_all_batch: unknown section %d", section);
            break;
        }
    }

    /* Flush trailing state */
    if (cur_album_gid && cur_album_artists) {
        mb_release_t *prev = g_hash_table_lookup(releases, cur_album_gid);
        if (prev) {
            prev->artist_count = cur_album_artists->len;
            prev->artists = flatten_artists(cur_album_artists);
        } else if (cur_album_artists) {
            /* Orphaned artists — free them */
            for (guint i = 0; i < cur_album_artists->len; i++) {
                mb_artist_t *a = g_ptr_array_index(cur_album_artists, i);
                mb_artist_free(a);
                g_free(a);
            }
            g_ptr_array_free(cur_album_artists, TRUE);
        }
    }
    flush_recording(cur_rec, cur_rec_artists, cur_recs_arr);

    g_free(cur_album_gid);
    g_free(t_release_gid);
    g_free(t_rec_gid);
    PQclear(res);

    /* Move recordings into release structs */
    finalize_recordings(releases, rec_arrays);
    g_hash_table_destroy(rec_arrays);

    *out_releases = releases;
    *out_links = links;
    return QUADRATURE_OK;
}

// =============================================================================
// PG Connection Pool
// =============================================================================

quadrature_result_t
mb_pg_pool_create(const char *mb_conninfo,
                  const char *acoustid_conninfo,
                  const char *acoustid_index_url,
                  size_t count,
                  mb_pg_pool_t **out)
{
    if (!mb_conninfo || count == 0 || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    mb_pg_pool_t *pool = g_new0(mb_pg_pool_t, 1);
    pool->mb_conns = g_new0(mb_pg_client_t *, count);
    pool->acoustid_conns = g_new0(mb_pg_client_t *, count);
    pool->http_conns = g_new0(mb_http_conn_t *, count);
    pool->count = count;
    pool->next_slot = 0;

    for (size_t i = 0; i < count; i++) {
        quadrature_result_t res = mb_pg_client_create(mb_conninfo, &pool->mb_conns[i]);
        if (res != QUADRATURE_OK) {
            g_warning("mb_pg_pool_create: failed to create MB conn %zu", i);
            mb_pg_pool_destroy(pool);
            return res;
        }
        mb_pg_set_schema(pool->mb_conns[i], "musicbrainz");

        if (acoustid_conninfo && acoustid_conninfo[0]) {
            res = mb_pg_client_create(acoustid_conninfo, &pool->acoustid_conns[i]);
            if (res != QUADRATURE_OK) {
                g_warning("mb_pg_pool_create: failed to create acoustid conn %zu (non-fatal)", i);
                pool->acoustid_conns[i] = NULL;
            }
        }

        // Prepare acoustid lookup statements on both PG connections
        mb_acoustid_prepare_stmts(pool->mb_conns[i], pool->acoustid_conns[i]);

        // Persistent HTTP connection to acoustid-index
        if (acoustid_index_url && acoustid_index_url[0]) {
            pool->http_conns[i] = mb_http_conn_create(acoustid_index_url);
            // Non-fatal if initial connect fails — will retry on first use
        }
    }

    *out = pool;
    return QUADRATURE_OK;
}

void
mb_pg_pool_destroy(mb_pg_pool_t *pool)
{
    if (!pool)
        return;
    for (size_t i = 0; i < pool->count; i++) {
        mb_pg_client_destroy(pool->mb_conns[i]);
        if (pool->acoustid_conns[i])
            mb_pg_client_destroy(pool->acoustid_conns[i]);
        if (pool->http_conns[i])
            mb_http_conn_destroy(pool->http_conns[i]);
    }
    g_free(pool->mb_conns);
    g_free(pool->acoustid_conns);
    g_free(pool->http_conns);
    g_free(pool);
}
