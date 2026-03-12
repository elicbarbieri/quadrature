/**
 * AcoustID fingerprint lookup via acoustid-index HTTP + local PostgreSQL.
 *
 * Flow:
 *   1. Decode base64 chromaprint → raw uint32 hashes
 *   2. POST hashes to acoustid-index HTTP → matching track_ids
 *      (acoustid-index is indexed by track_id, not fingerprint_id)
 *   3. Query acoustid PG: fingerprint_ids → track_ids → recording MBIDs
 *      (via track_fingerprint + track_mbid tables)
 *   4. Query MB PG: recording MBIDs → release UUIDs
 *   5. Return (recording_id, release_id) pairs for release voting
 *
 * acoustid-index stores fingerprint.id; track_fingerprint maps fp_id → track_id.
 *
 * Performance:
 *   - HTTP connection persists across lookups (keep-alive, reconnect on failure)
 *   - PG queries use prepared statements (zero parse overhead after first call)
 *   - Both connections are per-thread via mb_pg_pool_t (no contention)
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <chromaprint.h>
#include <libpq-fe.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

// Maximum hashes to send per search query (matches import script's [:120] slice)
#define MAX_QUERY_HASHES 120

// Prepared statement names (must match between prepare and exec)
#define STMT_ACOUSTID_LOOKUP "aq"
#define STMT_MB_REC_TO_REL   "mrr"
#define STMT_MB_ISRC_LOOKUP  "isrc"
#define STMT_MB_SOLR_DUR    "sdur"

// =============================================================================
// Fingerprint Decoding
// =============================================================================

/**
 * Decode a base64 chromaprint fingerprint into a raw int32 array.
 * Returns the number of elements, or 0 on failure.
 */
static size_t decode_fingerprint(const char* encoded, int32_t** raw_out) {
    int32_t* raw = NULL;
    int size = 0;

    int ok = chromaprint_decode_fingerprint(encoded, strlen(encoded),
                                             (uint32_t**)&raw, &size,
                                             NULL, 1);
    if (!ok || !raw || size <= 0) {
        if (raw) chromaprint_dealloc(raw);
        return 0;
    }

    *raw_out = raw;
    return (size_t)size;
}

// =============================================================================
// HTTP helpers (POSIX sockets)
// =============================================================================

/**
 * Parse "http://host:port" into host (stack buffer, max 255 chars) and port.
 * Returns false if URL is not http://.
 */
static bool parse_http_url(const char* url, char host_out[256], int* port_out) {
    if (g_ascii_strncasecmp(url, "http://", 7) != 0) return false;
    const char* hp = url + 7;

    const char* colon = strchr(hp, ':');
    const char* slash = strchr(hp, '/');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - hp);
        if (hlen >= 255) return false;
        memcpy(host_out, hp, hlen);
        host_out[hlen] = '\0';
        *port_out = atoi(colon + 1);
    } else {
        size_t hlen = slash ? (size_t)(slash - hp) : strlen(hp);
        if (hlen >= 255) return false;
        memcpy(host_out, hp, hlen);
        host_out[hlen] = '\0';
        *port_out = 80;
    }
    return true;
}

/**
 * Send all bytes, handling partial writes. Returns false on error.
 */
static bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/**
 * Open a TCP connection with async DNS + non-blocking connect.
 * Returns fd >= 0 on success, -1 on failure.
 */
static int tcp_connect(const char* host, int port) {
    char port_str[8];
    g_snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = { .ai_socktype = SOCK_STREAM };

    // Async DNS with 15s timeout — blocking getaddrinfo() can stall 30s+
    struct gaicb req = { .ar_name = host, .ar_service = port_str, .ar_request = &hints };
    struct gaicb* reqs[1] = { &req };
    if (getaddrinfo_a(GAI_NOWAIT, reqs, 1, NULL) != 0) {
        g_warning("acoustid_http: getaddrinfo_a failed for %s", host);
        return -1;
    }
    struct timespec dns_timeout = { .tv_sec = 15, .tv_nsec = 0 };
    int gai_ret = gai_suspend((const struct gaicb* const*)reqs, 1, &dns_timeout);
    int gai_err = gai_error(&req);
    if (gai_ret != 0 || gai_err != 0) {
        if (gai_err == EAI_INPROGRESS) gai_cancel(&req);
        g_warning("acoustid_http: DNS resolve timeout/failed for %s", host);
        return -1;
    }
    struct addrinfo* res = req.ar_result;

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    // Non-blocking connect with 15s timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        g_warning("acoustid_http: connect to %s:%d failed: %s",
                  host, port, g_strerror(errno));
        return -1;
    }

    if (rc != 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, 15000) <= 0) {
            close(fd);
            g_debug("acoustid_http: connect timeout to %s:%d", host, port);
            return -1;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            g_warning("acoustid_http: connect to %s:%d failed: %s",
                      host, port, g_strerror(err));
            return -1;
        }
    }

    // Restore blocking mode; set 10s recv timeout
    fcntl(fd, F_SETFL, flags);
    struct timeval recv_tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));

    return fd;
}

// =============================================================================
// Persistent HTTP Connection
// =============================================================================

mb_http_conn_t* mb_http_conn_create(const char* base_url) {
    mb_http_conn_t* conn = g_new0(mb_http_conn_t, 1);
    conn->fd = -1;
    conn->alive = false;
    conn->url = g_strdup(base_url);

    if (!parse_http_url(base_url, conn->host, &conn->port)) {
        g_warning("mb_http_conn_create: invalid URL: %s", base_url);
        g_free(conn->url);
        g_free(conn);
        return NULL;
    }

    // Attempt initial connection (non-fatal if it fails — will retry on use)
    conn->fd = tcp_connect(conn->host, conn->port);
    conn->alive = (conn->fd >= 0);
    return conn;
}

void mb_http_conn_destroy(mb_http_conn_t* conn) {
    if (!conn) return;
    if (conn->fd >= 0) close(conn->fd);
    g_free(conn->url);
    g_free(conn);
}

/**
 * Ensure the HTTP connection is open. Reconnects if needed.
 * Returns true if connection is usable.
 */
static bool http_conn_ensure(mb_http_conn_t* conn) {
    if (conn->fd >= 0 && conn->alive) return true;

    // Close stale fd
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }

    conn->fd = tcp_connect(conn->host, conn->port);
    conn->alive = (conn->fd >= 0);
    return conn->alive;
}

/**
 * HTTP POST json_body to {path} on a persistent connection.
 * Uses HTTP/1.1 keep-alive. Reconnects once on failure.
 *
 * Returns the response body as a newly-allocated string, or NULL on error.
 * Caller must g_free() the result.
 */
static char* http_post(mb_http_conn_t* conn, const char* path,
                        const char* json_body) {
    if (!http_conn_ensure(conn)) return NULL;

    size_t body_len = strlen(json_body);
    char* request = g_strdup_printf(
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        path, conn->host, conn->port, body_len, json_body);

    size_t req_len = strlen(request);

    // Try send; on failure, reconnect once and retry
    if (!send_all(conn->fd, request, req_len)) {
        g_free(request);
        conn->alive = false;
        if (!http_conn_ensure(conn)) return NULL;

        // Re-format after reconnect (same data)
        request = g_strdup_printf(
            "POST %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%s",
            path, conn->host, conn->port, body_len, json_body);
        req_len = strlen(request);

        if (!send_all(conn->fd, request, req_len)) {
            g_free(request);
            g_warning("acoustid_http: send failed after reconnect");
            conn->alive = false;
            return NULL;
        }
    }
    g_free(request);

    // Read response headers + body.
    // Parse Content-Length to know exactly how many body bytes to read
    // (enables keep-alive by knowing the body boundary).
    GString* hdr = g_string_sized_new(512);
    char buf[4096];
    char* hdr_end = NULL;

    while (!hdr_end) {
        ssize_t n = recv(conn->fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            g_string_free(hdr, TRUE);
            conn->alive = false;
            return NULL;
        }
        g_string_append_len(hdr, buf, n);
        hdr_end = strstr(hdr->str, "\r\n\r\n");
    }

    // Parse Content-Length
    size_t content_length = 0;
    const char* cl = strcasestr(hdr->str, "content-length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        content_length = (size_t)strtoul(cl, NULL, 10);
    }

    // Check Connection: close
    conn->alive = true;
    const char* conn_hdr = strcasestr(hdr->str, "connection:");
    if (conn_hdr) {
        conn_hdr += 11;
        while (*conn_hdr == ' ') conn_hdr++;
        if (g_ascii_strncasecmp(conn_hdr, "close", 5) == 0)
            conn->alive = false;
    }

    // Body starts after \r\n\r\n
    size_t hdr_len = (size_t)(hdr_end - hdr->str) + 4;
    size_t body_already = hdr->len - hdr_len;

    if (content_length == 0) {
        char* body = (body_already > 0) ? g_strndup(hdr_end + 4, body_already) : NULL;
        g_string_free(hdr, TRUE);
        conn->alive = false;  // can't reuse without knowing body boundary
        return body;
    }

    // Cap at 128 KB for safety
    if (content_length > 131072) content_length = 131072;

    char* body = g_malloc(content_length + 1);
    size_t body_have = body_already < content_length ? body_already : content_length;
    memcpy(body, hdr_end + 4, body_have);
    g_string_free(hdr, TRUE);

    // Read remaining body bytes
    while (body_have < content_length) {
        ssize_t n = recv(conn->fd, body + body_have, content_length - body_have, 0);
        if (n <= 0) {
            g_free(body);
            conn->alive = false;
            return NULL;
        }
        body_have += (size_t)n;
    }

    body[content_length] = '\0';
    return body;
}

// =============================================================================
// JSON response parsing
// =============================================================================

/**
 * Parse acoustid-index _search response:
 *   {"results":[{"id":N,"score":M},...],...}
 *
 * Extracts "id" integers (track_ids) into a GArray of int.
 * Score field is an integer (number of matching hashes), not used here.
 */
static GArray* parse_search_ids(const char* json) {
    GArray* ids = g_array_new(FALSE, FALSE, sizeof(int));
    if (!json) return ids;

    const char* p = strstr(json, "\"results\"");
    if (!p) return ids;
    p = strchr(p, '[');
    if (!p) return ids;
    p++;  // skip '['

    // Track brace depth to find the matching ']'
    int depth = 1;
    const char* scan = p;
    const char* end = json + strlen(json);

    while (scan < end && depth > 0) {
        if (*scan == '[') depth++;
        else if (*scan == ']') { depth--; if (depth == 0) break; }
        scan++;
    }
    end = scan;

    while (p < end) {
        const char* id_key = strstr(p, "\"id\"");
        if (!id_key || id_key >= end) break;
        id_key += 4;

        while (id_key < end && (*id_key == ' ' || *id_key == ':')) id_key++;

        if (id_key < end && *id_key >= '0' && *id_key <= '9') {
            char* num_end;
            long val = strtol(id_key, &num_end, 10);
            if (val > 0 && val <= INT_MAX) {
                int id = (int)val;
                g_array_append_val(ids, id);
            }
            p = num_end;
        } else {
            p = id_key + 1;
        }
    }
    return ids;
}

// =============================================================================
// PG array parameter formatting
// =============================================================================

static char* format_int_array(const int* ids, guint len) {
    if (len == 0) return g_strdup("{}");
    GString* s = g_string_sized_new((gsize)len * 12 + 2);
    g_string_append_c(s, '{');
    for (guint i = 0; i < len; i++) {
        if (i > 0) g_string_append_c(s, ',');
        g_string_append_printf(s, "%d", ids[i]);
    }
    g_string_append_c(s, '}');
    return g_string_free(s, FALSE);
}

static char* format_uuid_array(const char** vals, int len) {
    if (len == 0) return g_strdup("{}");
    GString* s = g_string_sized_new((gsize)len * 37 + 2);
    g_string_append_c(s, '{');
    for (int i = 0; i < len; i++) {
        if (i > 0) g_string_append_c(s, ',');
        g_string_append(s, vals[i]);
    }
    g_string_append_c(s, '}');
    return g_string_free(s, FALSE);
}

// =============================================================================
// Prepared Statements
// =============================================================================

// AcoustID PG: fingerprint_ids → track_ids → recording MBIDs (ranked by evidence)
// acoustid-index returns fingerprint.id; track_fingerprint maps fp_id → track_id
static const char* ACOUSTID_QUERY_SQL =
    "SELECT tm.mbid::text "
    "FROM track_fingerprint tf "
    "JOIN track_mbid tm ON tm.track_id = tf.track_id "
    "WHERE tf.fingerprint_id = ANY($1::int[]) "
    "GROUP BY tm.mbid "
    "ORDER BY MAX(tm.submission_count) DESC "
    "LIMIT 20";

// MB PG: recording MBIDs → (release UUID, release_group UUID)
static const char* MB_REC_TO_REL_SQL =
    "SELECT DISTINCT r.gid::text, rl.gid::text, rg.gid::text "
    "FROM recording r "
    "JOIN track mt ON mt.recording = r.id "
    "JOIN medium m ON m.id = mt.medium "
    "JOIN release rl ON rl.id = m.release "
    "JOIN release_group rg ON rg.id = rl.release_group "
    "WHERE r.gid = ANY($1::uuid[])";

// MB PG: ISRCs → (release UUID, release_group UUID) pairs for voting
static const char* MB_ISRC_LOOKUP_SQL =
    "SELECT DISTINCT r.gid::text, rg.gid::text "
    "FROM isrc i "
    "JOIN recording rec ON rec.id = i.recording "
    "JOIN track t ON t.recording = rec.id "
    "JOIN medium m ON m.id = t.medium "
    "JOIN release r ON r.id = m.release "
    "JOIN release_group rg ON rg.id = r.release_group "
    "WHERE i.isrc = ANY($1::text[])";

// MB PG: get scoring data for a Solr candidate release (by UUID).
// Returns (total_duration_ms, track_count, release_type, release_title, artist_credit)
// in one roundtrip — all fields needed for Picard-style weighted scoring.
static const char* MB_SOLR_RELEASE_INFO_SQL =
    "SELECT "
    "  (SELECT COALESCE(SUM(rec.length), 0) FROM track t "
    "   JOIN medium m ON m.id = t.medium "
    "   JOIN recording rec ON rec.id = t.recording WHERE m.release = r.id), "
    "  (SELECT COUNT(*) FROM track t "
    "   JOIN medium m ON m.id = t.medium WHERE m.release = r.id), "
    "  rgt.name, "
    "  r.name, "
    "  (SELECT string_agg(a.name, ', ' ORDER BY acn.position) "
    "   FROM artist_credit_name acn "
    "   JOIN artist a ON a.id = acn.artist "
    "   WHERE acn.artist_credit = r.artist_credit) "
    "FROM release r "
    "JOIN release_group rg ON rg.id = r.release_group "
    "LEFT JOIN release_group_primary_type rgt ON rgt.id = rg.type "
    "WHERE r.gid = $1::uuid";

quadrature_result_t mb_acoustid_prepare_stmts(mb_pg_client_t* mb_client,
                                               mb_pg_client_t* acoustid_client) {
    if (acoustid_client) {
        quadrature_result_t res = mb_pg_prepare(
            acoustid_client, STMT_ACOUSTID_LOOKUP, ACOUSTID_QUERY_SQL, 1);
        if (res != QUADRATURE_OK) {
            g_warning("mb_acoustid_prepare_stmts: failed to prepare acoustid query");
            return res;
        }
    }

    if (mb_client) {
        quadrature_result_t res = mb_pg_prepare(
            mb_client, STMT_MB_REC_TO_REL, MB_REC_TO_REL_SQL, 1);
        if (res != QUADRATURE_OK) {
            g_warning("mb_acoustid_prepare_stmts: failed to prepare MB query");
            return res;
        }

        res = mb_pg_prepare(mb_client, STMT_MB_ISRC_LOOKUP, MB_ISRC_LOOKUP_SQL, 1);
        if (res != QUADRATURE_OK) {
            g_warning("mb_acoustid_prepare_stmts: failed to prepare ISRC lookup");
            return res;
        }

        res = mb_pg_prepare(mb_client, STMT_MB_SOLR_DUR, MB_SOLR_RELEASE_INFO_SQL, 1);
        if (res != QUADRATURE_OK) {
            g_warning("mb_acoustid_prepare_stmts: failed to prepare Solr release info query");
            return res;
        }
    }

    return QUADRATURE_OK;
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_acoustid_lookup(mb_pg_client_t* mb_client,
                                        mb_pg_client_t* acoustid_client,
                                        mb_http_conn_t* http_conn,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response) {
    if (!mb_client || !fingerprint || !response) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }
    if (!fingerprint->fingerprint || fingerprint->duration <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Both acoustid_client and http_conn are required for fingerprint lookup
    if (!acoustid_client || !http_conn) {
        memset(response, 0, sizeof(*response));
        return QUADRATURE_ERROR_SERVICE_UNAVAILABLE;
    }

    memset(response, 0, sizeof(*response));

    // Step 1: Decode fingerprint to raw int32 hashes
    int32_t* raw = NULL;
    size_t raw_count = decode_fingerprint(fingerprint->fingerprint, &raw);
    if (raw_count == 0) {
        g_warning("mb_acoustid_lookup: failed to decode fingerprint");
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Step 2: Build JSON query body (up to 120 hashes as uint32)
    size_t nhashes = raw_count < MAX_QUERY_HASHES ? raw_count : MAX_QUERY_HASHES;
    GString* json = g_string_sized_new(nhashes * 11 + 40);
    g_string_append(json, "{\"query\":[");
    for (size_t i = 0; i < nhashes; i++) {
        if (i > 0) g_string_append_c(json, ',');
        g_string_append_printf(json, "%u", (uint32_t)raw[i]);
    }
    g_string_append(json, "],\"limit\":20}");
    chromaprint_dealloc(raw);
    char* json_body = g_string_free(json, FALSE);

    // Step 3: POST to acoustid-index → get track_ids back (persistent connection)
    char* http_resp = http_post(http_conn, "/acoustid/_search", json_body);
    g_free(json_body);

    if (!http_resp) {
        g_warning("mb_acoustid_lookup: no response from acoustid-index at %s",
                  http_conn->url);
        return QUADRATURE_ERROR_SERVICE_UNAVAILABLE;
    }

    GArray* track_ids = parse_search_ids(http_resp);
    g_free(http_resp);

    if (track_ids->len == 0) {
        g_array_free(track_ids, TRUE);
        return QUADRATURE_OK;
    }

    // Step 4: Query acoustid PG: track_ids → recording MBIDs (prepared statement)
    char* fpid_array = format_int_array((const int*)track_ids->data, track_ids->len);
    g_array_free(track_ids, TRUE);

    const char* acoustid_params[1] = { fpid_array };
    PGresult* acoustid_res = (PGresult*)mb_pg_exec_prepared(
        acoustid_client, STMT_ACOUSTID_LOOKUP, 1, acoustid_params);
    g_free(fpid_array);

    if (!acoustid_res || PQresultStatus(acoustid_res) != PGRES_TUPLES_OK) {
        g_warning("mb_acoustid_lookup: acoustid PG query failed: %s",
                  acoustid_res ? PQresultErrorMessage(acoustid_res) : "NULL");
        if (acoustid_res) PQclear(acoustid_res);
        return QUADRATURE_ERROR_SERVICE_UNAVAILABLE;
    }

    int nmbids = PQntuples(acoustid_res);
    if (nmbids == 0) {
        PQclear(acoustid_res);
        return QUADRATURE_OK;
    }

    // Collect recording MBIDs as array for MB PG query
    const char** mbid_vals = g_new(const char*, nmbids);
    for (int i = 0; i < nmbids; i++) {
        mbid_vals[i] = PQgetvalue(acoustid_res, i, 0);
    }
    char* mbid_array = format_uuid_array(mbid_vals, nmbids);
    g_free(mbid_vals);
    PQclear(acoustid_res);

    // Step 5: Query MB PG: recording MBIDs → release UUIDs (prepared statement)
    const char* mb_params[1] = { mbid_array };
    PGresult* mb_res = (PGresult*)mb_pg_exec_prepared(
        mb_client, STMT_MB_REC_TO_REL, 1, mb_params);
    g_free(mbid_array);

    if (!mb_res || PQresultStatus(mb_res) != PGRES_TUPLES_OK) {
        g_warning("mb_acoustid_lookup: MB PG recording→release query failed: %s",
                  mb_res ? PQresultErrorMessage(mb_res) : "NULL");
        if (mb_res) PQclear(mb_res);
        return QUADRATURE_ERROR_SERVICE_UNAVAILABLE;
    }

    int nrows = PQntuples(mb_res);
    if (nrows == 0) {
        PQclear(mb_res);
        return QUADRATURE_OK;
    }

    // Step 6: Build response array
    response->results = g_new0(mb_acoustid_result_t, (size_t)nrows);
    response->count   = (size_t)nrows;
    for (int i = 0; i < nrows; i++) {
        response->results[i].recording_id    = g_strdup(PQgetvalue(mb_res, i, 0));
        response->results[i].release_id      = g_strdup(PQgetvalue(mb_res, i, 1));
        response->results[i].release_group_id = g_strdup(PQgetvalue(mb_res, i, 2));
        response->results[i].score           = 1.0f;
    }
    PQclear(mb_res);

    return QUADRATURE_OK;
}

// =============================================================================
// ISRC Lookup
// =============================================================================

quadrature_result_t mb_isrc_lookup(mb_pg_client_t* mb_client,
                                    const char** isrcs, size_t count,
                                    mb_acoustid_response_t* response) {
    if (!mb_client || !isrcs || count == 0 || !response)
        return QUADRATURE_ERROR_INVALID_PARAM;

    memset(response, 0, sizeof(*response));

    // Format ISRCs as PG text array: {ISRC1,ISRC2,...}
    GString* arr = g_string_sized_new(count * 14 + 2);
    g_string_append_c(arr, '{');
    for (size_t i = 0; i < count; i++) {
        if (i > 0) g_string_append_c(arr, ',');
        g_string_append(arr, isrcs[i]);
    }
    g_string_append_c(arr, '}');
    char* isrc_array = g_string_free(arr, FALSE);

    const char* params[1] = { isrc_array };
    PGresult* res = (PGresult*)mb_pg_exec_prepared(
        mb_client, STMT_MB_ISRC_LOOKUP, 1, params);
    g_free(isrc_array);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        g_warning("mb_isrc_lookup: PG query failed: %s",
                  res ? PQresultErrorMessage(res) : "NULL");
        if (res) PQclear(res);
        return QUADRATURE_ERROR_SERVICE_UNAVAILABLE;
    }

    int nrows = PQntuples(res);
    if (nrows == 0) {
        PQclear(res);
        return QUADRATURE_OK;
    }

    response->results = g_new0(mb_acoustid_result_t, (size_t)nrows);
    response->count   = (size_t)nrows;
    for (int i = 0; i < nrows; i++) {
        response->results[i].recording_id    = NULL;
        response->results[i].release_id      = g_strdup(PQgetvalue(res, i, 0));
        response->results[i].release_group_id = g_strdup(PQgetvalue(res, i, 1));
        response->results[i].score           = 1.0f;
    }
    PQclear(res);

    return QUADRATURE_OK;
}

// =============================================================================
// Solr-Based Release Search (diacritics + Unicode via Lucene)
// =============================================================================

// Minimum weighted score to accept a Solr match.
// Below this threshold, the candidate is rejected as too dissimilar.
#define SOLR_MATCH_THRESHOLD 0.60

// --------------------------------------------------------------------------
// String similarity — port of Picard's similarity.py + util/astrcmp.py
// --------------------------------------------------------------------------

// Levenshtein distance normalized to [0.0, 1.0].
// Port of Picard's astrcmp(): 1.0 - edit_distance / max(len_a, len_b)
// Operates on Unicode codepoints (not bytes) for correct multi-byte handling.
static double astrcmp_score(const char* a, const char* b) {
    glong a_len = 0, b_len = 0;
    gunichar* a_ucs = g_utf8_to_ucs4_fast(a, -1, &a_len);
    gunichar* b_ucs = g_utf8_to_ucs4_fast(b, -1, &b_len);

    if (a_len == 0 || b_len == 0) {
        g_free(a_ucs); g_free(b_ucs);
        return 0.0;
    }

    // Ensure a is the shorter string (O(min(n,m)) space)
    if (a_len > b_len) {
        gunichar* tu = a_ucs; a_ucs = b_ucs; b_ucs = tu;
        glong tl = a_len; a_len = b_len; b_len = tl;
    }

    glong* prev = g_malloc((a_len + 1) * sizeof(glong));
    glong* curr = g_malloc((a_len + 1) * sizeof(glong));
    for (glong j = 0; j <= a_len; j++) prev[j] = j;

    for (glong i = 1; i <= b_len; i++) {
        curr[0] = i;
        for (glong j = 1; j <= a_len; j++) {
            glong cost_add = prev[j] + 1;
            glong cost_del = curr[j - 1] + 1;
            glong cost_chg = prev[j - 1] + (a_ucs[j - 1] != b_ucs[i - 1] ? 1 : 0);
            curr[j] = MIN(MIN(cost_add, cost_del), cost_chg);
        }
        glong* tmp = prev; prev = curr; curr = tmp;
    }

    double result = 1.0 - (double)prev[a_len] / (double)(a_len > b_len ? a_len : b_len);
    g_free(prev); g_free(curr);
    g_free(a_ucs); g_free(b_ucs);
    return result;
}

// Multi-word string similarity — port of Picard's similarity2().
// Splits on \W+, lowercases, matches words greedily by Levenshtein.
// Matched words (score > 0.6) are removed to avoid double-counting.
// Returns: sum_of_best_scores / (shorter_len + remaining_longer_len * 0.4)
static double similarity2(const char* a, const char* b) {
    if (!a || !b || !*a || !*b) return 0.0;
    if (strcmp(a, b) == 0) return 1.0;

    static GRegex* split_re = NULL;
    if (G_UNLIKELY(!split_re))
        split_re = g_regex_new("\\W+", G_REGEX_OPTIMIZE, 0, NULL);

    char* a_low = g_utf8_strdown(a, -1);
    char* b_low = g_utf8_strdown(b, -1);
    char** a_parts = g_regex_split(split_re, a_low, 0);
    char** b_parts = g_regex_split(split_re, b_low, 0);

    // Collect non-empty words
    GPtrArray* alist = g_ptr_array_new();
    GPtrArray* blist = g_ptr_array_new();
    for (char** w = a_parts; *w; w++) if (**w) g_ptr_array_add(alist, *w);
    for (char** w = b_parts; *w; w++) if (**w) g_ptr_array_add(blist, *w);

    guint alen = alist->len, blen = blist->len;
    if (alen == 0 || blen == 0) {
        g_ptr_array_free(alist, TRUE); g_ptr_array_free(blist, TRUE);
        g_strfreev(a_parts); g_strfreev(b_parts);
        g_free(a_low); g_free(b_low);
        return 0.0;
    }

    // alist = shorter
    if (alen > blen) {
        GPtrArray* tp = alist; alist = blist; blist = tp;
        guint tl = alen; alen = blen; blen = tl;
    }

    double score = 0.0;
    for (guint i = 0; i < alen; i++) {
        double best_s = 0.0;
        int best_j = -1;
        for (guint j = 0; j < blist->len; j++) {
            double s = astrcmp_score((const char*)alist->pdata[i],
                                     (const char*)blist->pdata[j]);
            if (s > best_s) { best_s = s; best_j = (int)j; }
        }
        score += best_s;
        if (best_j >= 0 && best_s > 0.6)
            g_ptr_array_remove_index(blist, (guint)best_j);
    }

    double result = score / ((double)alen + (double)blist->len * 0.4);

    g_ptr_array_free(alist, TRUE); g_ptr_array_free(blist, TRUE);
    g_strfreev(a_parts); g_strfreev(b_parts);
    g_free(a_low); g_free(b_low);
    return result;
}

// Release type score — Picard default: DEFAULT_RELEASE_SCORE = 0.5 for ALL types.
// Picard's release_type_scores is user-configurable, but defaults to 0.5 across
// the board (Album, EP, Single, Compilation, etc. all equal).
// We match the default here. Weight in CLUSTER_COMPARISON_WEIGHTS = 10.
static double release_type_score(const char* type) {
    (void)type;
    return 0.5;
}

// --------------------------------------------------------------------------
// Lucene escaping + Solr preprocessing
// --------------------------------------------------------------------------

// Escape Lucene special characters — exact port of Picard's escape_lucene_query():
//   re.sub(r'([+\-&|!(){}\[\]\^"~*?:\\/])', r'\\\1', text)
static char* escape_lucene(const char* input) {
    static GRegex* re = NULL;
    if (G_UNLIKELY(!re))
        re = g_regex_new("[+\\-&|!(){}\\[\\]^\"~*?:\\\\/]", 0, 0, NULL);
    return g_regex_replace(re, input, -1, 0, "\\\\\\0", 0, NULL);
}

char* mb_solr_search_release(mb_pg_client_t* mb_client,
                              const char* solr_url,
                              const char* album_title,
                              const char* artist_name,
                              size_t local_track_count,
                              int64_t local_total_duration_ms) {
    if (!mb_client || !solr_url || !album_title || !artist_name)
        return NULL;

    // Build Solr query — matches Picard's build_lucene_query() exactly:
    //   artist:(escaped_artist) release:(escaped_album) tracks:(N)
    //
    // Key design choices matching Picard:
    // - NO q.op=AND: Picard uses default OR. Extra terms (parentheticals,
    //   possessives) just lower relevance instead of causing 0 results.
    //   similarity2() post-scoring rejects bad matches via threshold.
    // - NO text preprocessing: Picard sends raw escaped text. The Solr analyzer
    //   handles tokenization, lowercasing, and stop words.
    // - tracks:(N) as a query term: boosts results with matching track count
    //   in the Solr relevance score (same as Picard via ws/2).
    // - fl=mbid,score: request Solr relevance score for each result.
    //   Picard multiplies similarity by get_score(release) — we do the same.
    // - rows=25: matches Picard's default query_limit.
    char* lucene_album = escape_lucene(album_title);
    char* lucene_artist = escape_lucene(artist_name);
    char* escaped_album = g_uri_escape_string(lucene_album, NULL, FALSE);
    char* escaped_artist = g_uri_escape_string(lucene_artist, NULL, FALSE);
    g_free(lucene_album);
    g_free(lucene_artist);

    char* url = g_strdup_printf(
        "%s/solr/release/select"
        "?q=artist:(%s)+release:(%s)+tracks:%zu"
        "&fl=mbid,score&rows=25&wt=json",
        solr_url, escaped_artist, escaped_album, local_track_count);
    g_free(escaped_album);
    g_free(escaped_artist);

    // HTTP GET via libsoup (thread-local session reuses TCP connections)
    static __thread SoupSession* session = NULL;
    if (!session) session = soup_session_new();

    SoupMessage* msg = soup_message_new("GET", url);
    g_free(url);

    if (!msg) {
        g_warning("mb_solr_search: invalid URL for '%s' by '%s'",
                  album_title, artist_name);
        return NULL;
    }

    GError* error = NULL;
    GBytes* body = soup_session_send_and_read(session, msg, NULL, &error);

    guint status = soup_message_get_status(msg);
    g_object_unref(msg);

    if (error || !body || status != 200) {
        if (status != 200) {
            g_warning("mb_solr_search: HTTP %u for '%s' by '%s'",
                      status, album_title, artist_name);
        }
        if (error) {
            g_warning("mb_solr_search: %s", error->message);
            g_error_free(error);
        }
        if (body) g_bytes_unref(body);
        return NULL;
    }

    // Parse JSON response
    gsize body_len;
    const char* body_data = g_bytes_get_data(body, &body_len);

    JsonParser* parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body_data, (gssize)body_len, NULL)) {
        g_warning("mb_solr_search: failed to parse JSON response");
        g_object_unref(parser);
        g_bytes_unref(body);
        return NULL;
    }

    JsonNode* root = json_parser_get_root(parser);
    JsonObject* root_obj = json_node_get_object(root);
    JsonObject* response_obj = json_object_get_object_member(root_obj, "response");
    if (!response_obj) {
        g_object_unref(parser);
        g_bytes_unref(body);
        return NULL;
    }

    JsonArray* docs = json_object_get_array_member(response_obj, "docs");
    if (!docs || json_array_get_length(docs) == 0) {
        g_object_unref(parser);
        g_bytes_unref(body);
        return NULL;
    }

    // Score candidates using Picard's exact algorithm:
    //
    // Picard CLUSTER_COMPARISON_WEIGHTS:
    //   album=17, releasetype=10, albumartist=6, totalalbumtracks=5,
    //   date=4, format=2, releasecountry=2    (total=46)
    //
    // We implement: album=17, releasetype=10, albumartist=6, totalalbumtracks=5,
    //   duration=3 (tiebreaker between editions — Picard doesn't use this)
    //   (total=41)
    //
    // Picard's final formula (metadata.py:268):
    //   similarity = linear_combination_of_weights(parts) * get_score(release)
    //
    // get_score() returns the ws/2 search score (0-100) normalized to 0.0-1.0.
    // ws/2 normalizes Solr scores: top result = 100, others relative.
    // We replicate this by dividing each Solr score by max_score.
    //
    // PG query returns: (duration, track_count, release_type, title, artist)

    guint ndocs = json_array_get_length(docs);

    // Find max Solr score for normalization (replicates ws/2's 0-100 scaling)
    double max_solr_score = 0.0;
    for (guint i = 0; i < ndocs; i++) {
        JsonObject* doc = json_array_get_object_element(docs, i);
        if (!doc) continue;
        if (json_object_has_member(doc, "score")) {
            double s = json_object_get_double_member(doc, "score");
            if (s > max_solr_score) max_solr_score = s;
        }
    }

    char* best_release_id = NULL;
    double best_score = -1.0;

    for (guint i = 0; i < ndocs; i++) {
        JsonObject* doc = json_array_get_object_element(docs, i);
        if (!doc) continue;

        if (!json_object_has_member(doc, "mbid")) continue;
        const char* release_mbid = json_object_get_string_member(doc, "mbid");
        if (!release_mbid) continue;

        // Picard get_score(): normalize search relevance to 0.0-1.0
        double solr_score = 1.0;
        if (json_object_has_member(doc, "score") && max_solr_score > 0.0)
            solr_score = json_object_get_double_member(doc, "score") / max_solr_score;

        // One PG roundtrip: (duration, tracks, type, title, artist)
        const char* info_params[1] = { release_mbid };
        PGresult* info_res = (PGresult*)mb_pg_exec_prepared(
            mb_client, STMT_MB_SOLR_DUR, 1, info_params);

        if (!info_res || PQresultStatus(info_res) != PGRES_TUPLES_OK
            || PQntuples(info_res) == 0) {
            if (info_res) PQclear(info_res);
            continue;
        }

        int64_t candidate_dur    = strtoll(PQgetvalue(info_res, 0, 0), NULL, 10);
        int64_t candidate_tracks = strtoll(PQgetvalue(info_res, 0, 1), NULL, 10);
        const char* mb_type   = PQgetisnull(info_res, 0, 2) ? NULL : PQgetvalue(info_res, 0, 2);
        const char* mb_title  = PQgetisnull(info_res, 0, 3) ? "" : PQgetvalue(info_res, 0, 3);
        const char* mb_artist = PQgetisnull(info_res, 0, 4) ? "" : PQgetvalue(info_res, 0, 4);

        // 1. Album title similarity (weight 17) — Picard similarity2()
        double title_sim = similarity2(album_title, mb_title);

        // 2. Artist similarity (weight 6) — Picard similarity2()
        double artist_sim = similarity2(artist_name, mb_artist);

        // 3. Release type (weight 10) — Picard default: all types = 0.5
        double type_score = release_type_score(mb_type);

        // 4. Track count (weight 5) — Picard trackcount_score()
        double track_score;
        if (candidate_tracks == 0) {
            track_score = 0.3;
        } else if ((int64_t)local_track_count == candidate_tracks) {
            track_score = 1.0;
        } else if ((int64_t)local_track_count < candidate_tracks) {
            track_score = 0.3;
        } else {
            track_score = 0.0;
        }

        // 5. Duration (weight 3) — tiebreaker between editions
        double dur_score = 0.5;
        if (candidate_dur > 0 && local_total_duration_ms > 0) {
            int64_t thresh = local_total_duration_ms / 10;
            if (thresh < 300000) thresh = 300000;
            int64_t delta = llabs(candidate_dur - local_total_duration_ms);
            dur_score = 1.0 - (double)(delta < thresh ? delta : thresh) / (double)thresh;
        }

        // Picard formula: linear_combination_of_weights(parts) * get_score(release)
        double local_sim = (title_sim   * 17.0 +
                            type_score  * 10.0 +
                            artist_sim  *  6.0 +
                            track_score *  5.0 +
                            dur_score   *  3.0) / 41.0;
        double score = local_sim * solr_score;

        PQclear(info_res);

        if (score > best_score) {
            best_score = score;
            g_free(best_release_id);
            best_release_id = g_strdup(release_mbid);
        }
    }

    g_object_unref(parser);
    g_bytes_unref(body);

    // Reject if best score is below threshold
    if (best_release_id && best_score < SOLR_MATCH_THRESHOLD) {
        g_debug("mb_solr_search: rejected '%s' by '%s' — best score %.3f < %.2f threshold (%u candidates)",
                album_title, artist_name, best_score, (double)SOLR_MATCH_THRESHOLD, ndocs);
        g_free(best_release_id);
        return NULL;
    }

    if (best_release_id) {
        g_debug("mb_solr_search: matched '%s' by '%s' → %s (score %.3f, %u candidates)",
                album_title, artist_name, best_release_id, best_score, ndocs);
    }

    return best_release_id;
}

void mb_acoustid_response_free(mb_acoustid_response_t* response) {
    if (!response) return;

    if (response->results) {
        for (size_t i = 0; i < response->count; i++) {
            g_free(response->results[i].recording_id);
            g_free(response->results[i].release_id);
            g_free(response->results[i].release_group_id);
        }
        g_free(response->results);
    }

    memset(response, 0, sizeof(mb_acoustid_response_t));
}
