/**
 * AcoustID fingerprint lookup via acoustid-index HTTP + local PostgreSQL.
 *
 * Flow:
 *   1. Decode base64 chromaprint → raw uint32 hashes
 *   2. POST hashes to acoustid-index HTTP → matching track_ids
 *      (acoustid-index is indexed by track_id, not fingerprint_id)
 *   3. Query acoustid PG: track_ids → recording MBIDs (via track_mbid table)
 *   4. Query MB PG: recording MBIDs → release UUIDs
 *   5. Return (recording_id, release_id) pairs for release voting
 *
 * The acoustid-index and PG schema use track_id as the linking key.
 * No fingerprint table is needed in PostgreSQL.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <chromaprint.h>
#include <libpq-fe.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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
                                             NULL, 0);
    if (!ok || !raw || size <= 0) {
        if (raw) chromaprint_dealloc(raw);
        return 0;
    }

    *raw_out = raw;
    return (size_t)size;
}

// =============================================================================
// HTTP POST helper (POSIX sockets, no extra dependencies)
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
 * HTTP POST json_body to {base_url}{path}.
 * Returns the response body as a newly-allocated string, or NULL on error.
 * Caller must g_free() the result.
 */
static char* http_post_json(const char* base_url, const char* path,
                             const char* json_body) {
    char host[256];
    int port = 80;
    if (!parse_http_url(base_url, host, &port)) {
        g_warning("acoustid_http: invalid URL: %s", base_url);
        return NULL;
    }

    char port_str[8];
    g_snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    // Async DNS with 15s timeout — blocking getaddrinfo() can stall 30s+
    struct gaicb req = { .ar_name = host, .ar_service = port_str, .ar_request = &hints };
    struct gaicb* reqs[1] = { &req };
    if (getaddrinfo_a(GAI_NOWAIT, reqs, 1, NULL) != 0) {
        g_warning("acoustid_http: getaddrinfo_a failed for %s", host);
        return NULL;
    }
    struct timespec dns_timeout = { .tv_sec = 15, .tv_nsec = 0 };
    int gai_ret = gai_suspend((const struct gaicb* const*)reqs, 1, &dns_timeout);
    int gai_err = gai_error(&req);
    if (gai_ret != 0 || gai_err != 0) {
        if (gai_err == EAI_INPROGRESS) gai_cancel(&req);
        g_warning("acoustid_http: DNS resolve timeout/failed for %s", host);
        return NULL;
    }
    struct addrinfo* res = req.ar_result;

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return NULL; }

    // Non-blocking connect with 15s timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        g_warning("acoustid_http: connect to %s:%d failed", host, port);
        return NULL;
    }

    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(fd);
            g_debug("acoustid_http: connect timeout to %s:%d", host, port);
            return NULL;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(fd);
            g_warning("acoustid_http: connect to %s:%d failed", host, port);
            return NULL;
        }
    }

    // Restore blocking mode; set 10s recv timeout
    fcntl(fd, F_SETFL, flags);
    struct timeval recv_tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));

    size_t body_len = strlen(json_body);
    char* request = g_strdup_printf(
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, body_len, json_body);

    ssize_t sent = send(fd, request, strlen(request), 0);
    g_free(request);

    if (sent < 0) {
        close(fd);
        g_warning("acoustid_http: send failed");
        return NULL;
    }

    // Read full response (cap at 128 KB — acoustid results are small)
    GString* resp = g_string_sized_new(4096);
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        g_string_append_len(resp, buf, n);
        if (resp->len > 131072) break;
    }
    close(fd);

    // Extract HTTP body (everything after the blank header line)
    char* sep = strstr(resp->str, "\r\n\r\n");
    char* body = sep ? g_strdup(sep + 4) : NULL;
    g_string_free(resp, TRUE);
    return body;
}

// =============================================================================
// JSON response parsing
// =============================================================================

/**
 * Parse acoustid-index _search response:
 *   {"results":[{"id":N,"score":0.9},...],...}
 *
 * Extracts "id" integers (track_ids) into a GArray of int.
 */
static GArray* parse_search_ids(const char* json) {
    GArray* ids = g_array_new(FALSE, FALSE, sizeof(int));
    if (!json) return ids;

    const char* p = strstr(json, "\"results\"");
    if (!p) return ids;
    p = strchr(p, '[');
    if (!p) return ids;

    const char* end = strchr(p, ']');
    if (!end) end = json + strlen(json);

    while (p < end) {
        const char* id_key = strstr(p, "\"id\"");
        if (!id_key || id_key >= end) break;
        id_key += 4;  // past "id"

        while (*id_key == ' ' || *id_key == ':') id_key++;

        if (*id_key >= '0' && *id_key <= '9') {
            int id = atoi(id_key);
            if (id > 0) g_array_append_val(ids, id);
        }
        p = id_key + 1;
    }
    return ids;
}

// =============================================================================
// PG array parameter formatting
// =============================================================================

static char* format_int_array(const int* ids, guint len) {
    if (len == 0) return g_strdup("{}");
    GString* s = g_string_new("{");
    for (guint i = 0; i < len; i++) {
        if (i > 0) g_string_append_c(s, ',');
        g_string_append_printf(s, "%d", ids[i]);
    }
    g_string_append_c(s, '}');
    return g_string_free(s, FALSE);
}

static char* format_str_array(const char** vals, int len) {
    if (len == 0) return g_strdup("{}");
    GString* s = g_string_new("{");
    for (int i = 0; i < len; i++) {
        if (i > 0) g_string_append_c(s, ',');
        g_string_append(s, vals[i]);
    }
    g_string_append_c(s, '}');
    return g_string_free(s, FALSE);
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_acoustid_lookup(mb_pg_client_t* mb_client,
                                        mb_pg_client_t* acoustid_client,
                                        const char* acoustid_index_url,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response) {
    if (!mb_client || !fingerprint || !response) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }
    if (!fingerprint->fingerprint || fingerprint->duration <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Both acoustid_client and acoustid_index_url are required for fingerprint tier
    if (!acoustid_client || !acoustid_index_url) {
        memset(response, 0, sizeof(*response));
        return QUADRATURE_OK;
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
    size_t nhashes = raw_count < 120 ? raw_count : 120;
    GString* json = g_string_sized_new(nhashes * 12 + 32);
    g_string_append(json, "{\"query\":[");
    for (size_t i = 0; i < nhashes; i++) {
        if (i > 0) g_string_append_c(json, ',');
        g_string_append_printf(json, "%u", (uint32_t)raw[i]);
    }
    g_string_append(json, "],\"limit\":20}");
    chromaprint_dealloc(raw);
    char* json_body = g_string_free(json, FALSE);

    // Step 3: POST to acoustid-index → get track_ids back
    char* http_resp = http_post_json(acoustid_index_url, "/acoustid/_search", json_body);
    g_free(json_body);

    if (!http_resp) {
        g_debug("mb_acoustid_lookup: no response from acoustid-index at %s",
                acoustid_index_url);
        return QUADRATURE_OK;
    }

    GArray* track_ids = parse_search_ids(http_resp);
    g_free(http_resp);

    if (track_ids->len == 0) {
        g_array_free(track_ids, TRUE);
        return QUADRATURE_OK;
    }

    // Step 4: Query acoustid PG: track_ids → recording MBIDs
    char* tid_array = format_int_array((const int*)track_ids->data, track_ids->len);
    g_array_free(track_ids, TRUE);

    const char* acoustid_query =
        "SELECT DISTINCT tm.mbid::text "
        "FROM track_mbid tm "
        "WHERE tm.track_id = ANY($1::int[]) "
        "ORDER BY tm.submission_count DESC "
        "LIMIT 20";

    const char* acoustid_params[1] = { tid_array };
    PGresult* acoustid_res = (PGresult*)mb_pg_exec(
        acoustid_client, acoustid_query, 1, acoustid_params);
    g_free(tid_array);

    if (!acoustid_res || PQresultStatus(acoustid_res) != PGRES_TUPLES_OK) {
        g_warning("mb_acoustid_lookup: acoustid PG query failed: %s",
                  acoustid_res ? PQresultErrorMessage(acoustid_res) : "NULL");
        if (acoustid_res) PQclear(acoustid_res);
        return QUADRATURE_ERROR_INTERNAL;
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
    char* mbid_array = format_str_array(mbid_vals, nmbids);
    g_free(mbid_vals);
    PQclear(acoustid_res);

    // Step 5: Query MB PG: recording MBIDs → release UUIDs
    const char* mb_query =
        "SELECT DISTINCT r.gid::text AS recording_mbid, "
        "                rl.gid::text AS release_mbid "
        "FROM recording r "
        "JOIN track mt ON mt.recording = r.id "
        "JOIN medium m ON m.id = mt.medium "
        "JOIN release rl ON rl.id = m.release "
        "WHERE r.gid = ANY($1::uuid[])";

    const char* mb_params[1] = { mbid_array };
    PGresult* mb_res = (PGresult*)mb_pg_exec(mb_client, mb_query, 1, mb_params);
    g_free(mbid_array);

    if (!mb_res || PQresultStatus(mb_res) != PGRES_TUPLES_OK) {
        g_warning("mb_acoustid_lookup: MB PG recording→release query failed: %s",
                  mb_res ? PQresultErrorMessage(mb_res) : "NULL");
        if (mb_res) PQclear(mb_res);
        return QUADRATURE_ERROR_INTERNAL;
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
        response->results[i].recording_id = g_strdup(PQgetvalue(mb_res, i, 0));
        response->results[i].release_id   = g_strdup(PQgetvalue(mb_res, i, 1));
        response->results[i].score        = 1.0f;
    }
    PQclear(mb_res);

    return QUADRATURE_OK;
}

void mb_acoustid_response_free(mb_acoustid_response_t* response) {
    if (!response) return;

    if (response->results) {
        for (size_t i = 0; i < response->count; i++) {
            g_free(response->results[i].recording_id);
            g_free(response->results[i].release_id);
        }
        g_free(response->results);
    }

    memset(response, 0, sizeof(mb_acoustid_response_t));
}
