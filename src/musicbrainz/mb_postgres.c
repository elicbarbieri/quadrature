/**
 * MusicBrainz PostgreSQL client.
 *
 * Queries a self-hosted MusicBrainz PostgreSQL database directly via libpq.
 * Returns mb_release_t / mb_recording_t types for the resolver.
 */

#include "internal.h"
#include <libpq-fe.h>
#include <string.h>
#include <stdlib.h>

// =============================================================================
// PG Client Lifecycle
// =============================================================================

struct mb_pg_client {
    PGconn* conn;
};

quadrature_result_t mb_pg_client_create(const char* conninfo, mb_pg_client_t** out) {
    if (!conninfo || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    PGconn* conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        g_warning("MusicBrainz PG connection failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return QUADRATURE_ERROR_INTERNAL;
    }

    mb_pg_client_t* client = g_new0(mb_pg_client_t, 1);
    client->conn = conn;
    *out = client;
    return QUADRATURE_OK;
}

void mb_pg_client_destroy(mb_pg_client_t* client) {
    if (!client) return;
    if (client->conn) PQfinish(client->conn);
    g_free(client);
}

// =============================================================================
// Query Helper (used by mb_acoustid.c)
// =============================================================================

void* mb_pg_exec(mb_pg_client_t* client, const char* query,
                  int nparams, const char* const* params) {
    if (!client || !client->conn || !query) return NULL;
    return PQexecParams(client->conn, query, nparams, NULL, params, NULL, NULL, 0);
}

// =============================================================================
// Helper: safe string from PG result
// =============================================================================

static char* pg_get_string(PGresult* res, int row, int col) {
    if (PQgetisnull(res, row, col)) return NULL;
    const char* val = PQgetvalue(res, row, col);
    return (val && val[0]) ? g_strdup(val) : NULL;
}

static int pg_get_int(PGresult* res, int row, int col, int default_val) {
    if (PQgetisnull(res, row, col)) return default_val;
    return atoi(PQgetvalue(res, row, col));
}

// =============================================================================
// Fetch Release from MusicBrainz PostgreSQL
// =============================================================================

quadrature_result_t mb_fetch_release(mb_pg_client_t* client,
                                      const char* release_id,
                                      mb_release_t* release) {
    if (!client || !release_id || !release) return QUADRATURE_ERROR_INVALID_PARAM;

    memset(release, 0, sizeof(mb_release_t));

    // --- Release metadata ---
    const char* release_params[1] = { release_id };
    PGresult* res = PQexecParams(client->conn,
        "SELECT r.gid::text, r.name, "
        "  rg.gid::text, rgpt.name, "
        "  r.barcode, rs.name, "
        "  (SELECT li.name FROM label li "
        "   JOIN release_label rl ON rl.label = li.id "
        "   WHERE rl.release = r.id LIMIT 1), "
        "  COALESCE(to_char(re.date_year, 'FM0000') || "
        "    CASE WHEN re.date_month IS NOT NULL "
        "      THEN '-' || to_char(re.date_month, 'FM00') ELSE '' END || "
        "    CASE WHEN re.date_day IS NOT NULL "
        "      THEN '-' || to_char(re.date_day, 'FM00') ELSE '' END, ''), "
        "  a.name "
        "FROM release r "
        "JOIN release_group rg ON r.release_group = rg.id "
        "LEFT JOIN release_group_primary_type rgpt ON rg.type = rgpt.id "
        "LEFT JOIN release_status rs ON r.status = rs.id "
        "LEFT JOIN release_country re ON re.release = r.id "
        "LEFT JOIN area a ON re.country = a.id "
        "WHERE r.gid = $1::uuid "
        "LIMIT 1",
        1, NULL, release_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            g_warning("MB PG release query failed: %s", PQerrorMessage(client->conn));
        }
        PQclear(res);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    release->id = pg_get_string(res, 0, 0);
    release->title = pg_get_string(res, 0, 1);
    release->release_group_id = pg_get_string(res, 0, 2);
    release->type = pg_get_string(res, 0, 3);
    release->barcode = pg_get_string(res, 0, 4);
    release->status = pg_get_string(res, 0, 5);
    release->label = pg_get_string(res, 0, 6);
    release->date = pg_get_string(res, 0, 7);
    release->country = pg_get_string(res, 0, 8);
    PQclear(res);

    // --- Album artist credits ---
    res = PQexecParams(client->conn,
        "SELECT a.gid::text, a.name, a.sort_name, acn.join_phrase "
        "FROM release r "
        "JOIN artist_credit_name acn ON acn.artist_credit = r.artist_credit "
        "JOIN artist a ON a.id = acn.artist "
        "WHERE r.gid = $1::uuid "
        "ORDER BY acn.position",
        1, NULL, release_params, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int n = PQntuples(res);
        if (n > 0) {
            release->artists = g_new0(mb_artist_t, n);
            release->artist_count = (size_t)n;
            for (int i = 0; i < n; i++) {
                release->artists[i].id = pg_get_string(res, i, 0);
                release->artists[i].name = pg_get_string(res, i, 1);
                release->artists[i].sort_name = pg_get_string(res, i, 2);
                release->artists[i].joinphrase = pg_get_string(res, i, 3);
            }
        }
    }
    PQclear(res);

    // --- Recordings (tracks across all media) ---
    res = PQexecParams(client->conn,
        "SELECT rec.gid::text, rec.name, t.position, m.position, rec.length "
        "FROM release r "
        "JOIN medium m ON m.release = r.id "
        "JOIN track t ON t.medium = m.id "
        "JOIN recording rec ON rec.id = t.recording "
        "WHERE r.gid = $1::uuid "
        "ORDER BY m.position, t.position",
        1, NULL, release_params, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int n = PQntuples(res);
        if (n > 0) {
            release->recordings = g_new0(mb_recording_t, n);
            release->recording_count = (size_t)n;
            for (int i = 0; i < n; i++) {
                release->recordings[i].id = pg_get_string(res, i, 0);
                release->recordings[i].title = pg_get_string(res, i, 1);
                release->recordings[i].position = pg_get_int(res, i, 2, 0);
                release->recordings[i].disc_number = pg_get_int(res, i, 3, 1);
                release->recordings[i].duration_ms = pg_get_int(res, i, 4, 0);
            }
        }
    }
    PQclear(res);

    // --- Per-recording artist credits (prepared statement for N executions) ---
    res = PQprepare(client->conn, "rec_artists",
        "SELECT a.gid::text, a.name, a.sort_name, acn.join_phrase "
        "FROM recording rec "
        "JOIN artist_credit_name acn ON acn.artist_credit = rec.artist_credit "
        "JOIN artist a ON a.id = acn.artist "
        "WHERE rec.gid = $1::uuid "
        "ORDER BY acn.position",
        1, NULL);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        g_warning("MB PG prepare rec_artists failed: %s", PQerrorMessage(client->conn));
        PQclear(res);
        return QUADRATURE_OK; // release metadata is still valid
    }
    PQclear(res);

    for (size_t i = 0; i < release->recording_count; i++) {
        if (!release->recordings[i].id) continue;

        const char* rec_params[1] = { release->recordings[i].id };
        res = PQexecPrepared(client->conn, "rec_artists",
            1, rec_params, NULL, NULL, 0);

        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            int n = PQntuples(res);
            if (n > 0) {
                release->recordings[i].artists = g_new0(mb_artist_t, n);
                release->recordings[i].artist_count = (size_t)n;
                for (int j = 0; j < n; j++) {
                    release->recordings[i].artists[j].id = pg_get_string(res, j, 0);
                    release->recordings[i].artists[j].name = pg_get_string(res, j, 1);
                    release->recordings[i].artists[j].sort_name = pg_get_string(res, j, 2);
                    release->recordings[i].artists[j].joinphrase = pg_get_string(res, j, 3);
                }
            }
        }
        PQclear(res);
    }

    // Deallocate prepared statement
    res = PQexec(client->conn, "DEALLOCATE rec_artists");
    PQclear(res);

    // --- Genres (from release group tags) ---
    res = PQexecParams(client->conn,
        "SELECT t.name "
        "FROM release r "
        "JOIN release_group rg ON r.release_group = rg.id "
        "JOIN release_group_tag rgt ON rgt.release_group = rg.id "
        "JOIN tag t ON t.id = rgt.tag "
        "WHERE r.gid = $1::uuid "
        "ORDER BY rgt.count DESC",
        1, NULL, release_params, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int n = PQntuples(res);
        if (n > 0) {
            release->genres = g_new0(char*, n);
            release->genre_count = (size_t)n;
            for (int i = 0; i < n; i++) {
                release->genres[i] = pg_get_string(res, i, 0);
            }
        }
    }
    PQclear(res);

    return QUADRATURE_OK;
}

// =============================================================================
// Free Functions (moved from mb_parser.c)
// =============================================================================

void mb_artist_free(mb_artist_t* artist) {
    if (!artist) return;
    g_free(artist->id);
    g_free(artist->name);
    g_free(artist->sort_name);
    g_free(artist->joinphrase);
}

void mb_recording_free(mb_recording_t* recording) {
    if (!recording) return;
    g_free(recording->id);
    g_free(recording->title);

    if (recording->artists) {
        for (size_t i = 0; i < recording->artist_count; i++) {
            mb_artist_free(&recording->artists[i]);
        }
        g_free(recording->artists);
    }
}

void mb_release_free(mb_release_t* release) {
    if (!release) return;

    g_free(release->id);
    g_free(release->release_group_id);
    g_free(release->title);
    g_free(release->date);
    g_free(release->country);
    g_free(release->label);
    g_free(release->barcode);
    g_free(release->status);
    g_free(release->type);

    if (release->artists) {
        for (size_t i = 0; i < release->artist_count; i++) {
            mb_artist_free(&release->artists[i]);
        }
        g_free(release->artists);
    }

    if (release->recordings) {
        for (size_t i = 0; i < release->recording_count; i++) {
            mb_recording_free(&release->recordings[i]);
        }
        g_free(release->recordings);
    }

    if (release->genres) {
        for (size_t i = 0; i < release->genre_count; i++) {
            g_free(release->genres[i]);
        }
        g_free(release->genres);
    }

    memset(release, 0, sizeof(mb_release_t));
}
