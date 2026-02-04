/**
 * AcoustID fingerprint lookup via local PostgreSQL (pg_acoustid).
 *
 * Replaces the HTTP-based AcoustID web service with a direct query
 * against a local PostgreSQL database containing the AcoustID dataset.
 *
 * Decodes base64 chromaprint fingerprint, formats as PG int4[] literal,
 * and uses the acoustid_compare2 function + GIN index for matching.
 */

#include "internal.h"
#include <chromaprint.h>
#include <libpq-fe.h>
#include <string.h>

// =============================================================================
// Fingerprint Decoding
// =============================================================================

/**
 * Decode a base64 chromaprint fingerprint into a raw int32 array.
 * Returns the number of elements, or 0 on failure.
 */
static size_t decode_fingerprint(const char* encoded, int duration,
                                  int32_t** raw_out) {
    int32_t* raw = NULL;
    int size = 0;

    int ok = chromaprint_decode_fingerprint(encoded, strlen(encoded),
                                             (uint32_t**)&raw, &size,
                                             NULL, 0);
    if (!ok || !raw || size <= 0) {
        if (raw) chromaprint_dealloc(raw);
        return 0;
    }

    (void)duration;  // Duration used for query filtering, not decoding

    *raw_out = raw;
    return (size_t)size;
}

/**
 * Format raw int32 fingerprint as a PostgreSQL int4[] literal.
 * Example output: "{12345,-67890,11111}"
 */
static char* format_pg_array(const int32_t* raw, size_t count) {
    // Estimate: each int32 is max 11 chars + comma + braces
    size_t buf_size = count * 13 + 3;
    char* buf = g_malloc(buf_size);
    size_t pos = 0;

    buf[pos++] = '{';
    for (size_t i = 0; i < count; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += (size_t)snprintf(buf + pos, buf_size - pos, "%d", raw[i]);
    }
    buf[pos++] = '}';
    buf[pos] = '\0';

    return buf;
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_acoustid_lookup(mb_pg_client_t* client,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response) {
    if (!client || !fingerprint || !response) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (!fingerprint->fingerprint || fingerprint->duration <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(response, 0, sizeof(mb_acoustid_response_t));

    // Decode base64 fingerprint to raw int32 array
    int32_t* raw = NULL;
    size_t raw_count = decode_fingerprint(fingerprint->fingerprint,
                                           fingerprint->duration, &raw);
    if (raw_count == 0) {
        g_warning("mb_acoustid_lookup: failed to decode fingerprint");
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Format as PG array literal
    char* pg_array = format_pg_array(raw, raw_count);
    chromaprint_dealloc(raw);

    // Duration string for query parameter
    char duration_str[16];
    snprintf(duration_str, sizeof(duration_str), "%d", fingerprint->duration);

    // Query: find fingerprint matches using acoustid_compare2,
    // then join to get recording and release IDs
    const char* query =
        "SELECT DISTINCT r.gid::text AS recording_id, "
        "       rl.gid::text AS release_id, "
        "       acoustid_compare2($1::int4[], f.fingerprint, 80) AS score "
        "FROM fingerprint f "
        "JOIN track t ON t.id = f.track_id "
        "JOIN track_mbid tm ON tm.track_id = t.id "
        "JOIN recording rec ON rec.id = tm.mbid "
        "JOIN musicbrainz.recording r ON r.id = rec.id "
        "LEFT JOIN musicbrainz.track mt ON mt.recording = r.id "
        "LEFT JOIN musicbrainz.medium m ON m.id = mt.medium "
        "LEFT JOIN musicbrainz.release rl ON rl.id = m.release "
        "WHERE f.length BETWEEN ($2::int - 7) AND ($2::int + 7) "
        "  AND acoustid_compare2($1::int4[], f.fingerprint, 80) > 0 "
        "ORDER BY score DESC "
        "LIMIT 50";

    const char* params[2] = { pg_array, duration_str };

    PGresult* result = (PGresult*)mb_pg_exec(client, query, 2, params);
    g_free(pg_array);

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        g_warning("mb_acoustid_lookup: query failed: %s",
                  result ? PQresultErrorMessage(result) : "NULL result");
        if (result) PQclear(result);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int nrows = PQntuples(result);
    if (nrows == 0) {
        PQclear(result);
        return QUADRATURE_OK;  // No matches, but not an error
    }

    // Parse results
    GPtrArray* all_results = g_ptr_array_new();

    for (int i = 0; i < nrows; i++) {
        const char* recording_id = PQgetvalue(result, i, 0);
        const char* release_id = PQgetisnull(result, i, 1) ? NULL : PQgetvalue(result, i, 1);
        const char* score_str = PQgetvalue(result, i, 2);

        mb_acoustid_result_t* entry = g_new0(mb_acoustid_result_t, 1);
        entry->recording_id = g_strdup(recording_id);
        entry->release_id = release_id ? g_strdup(release_id) : NULL;
        entry->score = score_str ? (float)g_ascii_strtod(score_str, NULL) / 100.0f : 0.0f;

        g_ptr_array_add(all_results, entry);
    }

    PQclear(result);

    // Convert to flat array
    if (all_results->len > 0) {
        response->results = g_new0(mb_acoustid_result_t, all_results->len);
        response->count = all_results->len;

        for (guint i = 0; i < all_results->len; i++) {
            mb_acoustid_result_t* entry = g_ptr_array_index(all_results, i);
            response->results[i] = *entry;
            g_free(entry);  // Free the container, not the strings
        }
    }

    g_ptr_array_free(all_results, TRUE);
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
