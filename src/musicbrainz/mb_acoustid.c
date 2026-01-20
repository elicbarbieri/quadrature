/**
 * AcoustID fingerprint lookup.
 *
 * Queries the AcoustID web service to match audio fingerprints
 * to MusicBrainz recording/release IDs.
 */

#include "internal.h"
#include <json-glib/json-glib.h>
#include <string.h>

// =============================================================================
// URL Building
// =============================================================================

static char* build_lookup_url(const char* api_key,
                               const mb_fingerprint_t* fingerprint) {
    // URL-encode the fingerprint (it's base64, so mostly safe, but be careful)
    char* encoded_fp = g_uri_escape_string(fingerprint->fingerprint, NULL, FALSE);

    char* url = g_strdup_printf(
        "%s?client=%s&duration=%d&fingerprint=%s&meta=recordings+releases",
        ACOUSTID_API_URL,
        api_key,
        fingerprint->duration,
        encoded_fp
    );

    g_free(encoded_fp);
    return url;
}

// =============================================================================
// Response Parsing
// =============================================================================

static void parse_acoustid_response(const char* json_str,
                                     mb_acoustid_response_t* response) {
    JsonParser* parser = json_parser_new();

    GError* error = NULL;
    if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
        g_warning("Failed to parse AcoustID JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    JsonObject* root_obj = json_node_get_object(root);

    // Check status
    if (json_object_has_member(root_obj, "status")) {
        const char* status = json_object_get_string_member(root_obj, "status");
        if (g_strcmp0(status, "ok") != 0) {
            g_warning("AcoustID returned status: %s", status);
            g_object_unref(parser);
            return;
        }
    }

    // Get results array
    if (!json_object_has_member(root_obj, "results")) {
        g_object_unref(parser);
        return;
    }

    JsonArray* results = json_object_get_array_member(root_obj, "results");
    if (!results) {
        g_object_unref(parser);
        return;
    }

    // Collect all recordings from all results
    GPtrArray* all_results = g_ptr_array_new();

    guint result_count = json_array_get_length(results);
    for (guint i = 0; i < result_count; i++) {
        JsonNode* result_node = json_array_get_element(results, i);
        if (!JSON_NODE_HOLDS_OBJECT(result_node)) continue;

        JsonObject* result_obj = json_node_get_object(result_node);

        // Get score for this result
        double score = 0.0;
        if (json_object_has_member(result_obj, "score")) {
            score = json_object_get_double_member(result_obj, "score");
        }

        // Get recordings
        if (!json_object_has_member(result_obj, "recordings")) continue;

        JsonArray* recordings = json_object_get_array_member(result_obj, "recordings");
        if (!recordings) continue;

        guint rec_count = json_array_get_length(recordings);
        for (guint j = 0; j < rec_count; j++) {
            JsonNode* rec_node = json_array_get_element(recordings, j);
            if (!JSON_NODE_HOLDS_OBJECT(rec_node)) continue;

            JsonObject* rec_obj = json_node_get_object(rec_node);

            // Get recording ID
            if (!json_object_has_member(rec_obj, "id")) continue;
            const char* recording_id = json_object_get_string_member(rec_obj, "id");
            if (!recording_id) continue;

            // Get first release ID if available
            const char* release_id = NULL;
            if (json_object_has_member(rec_obj, "releases")) {
                JsonArray* releases = json_object_get_array_member(rec_obj, "releases");
                if (releases && json_array_get_length(releases) > 0) {
                    JsonNode* rel_node = json_array_get_element(releases, 0);
                    if (JSON_NODE_HOLDS_OBJECT(rel_node)) {
                        JsonObject* rel_obj = json_node_get_object(rel_node);
                        if (json_object_has_member(rel_obj, "id")) {
                            release_id = json_object_get_string_member(rel_obj, "id");
                        }
                    }
                }
            }

            // Create result entry
            mb_acoustid_result_t* entry = g_new0(mb_acoustid_result_t, 1);
            entry->recording_id = g_strdup(recording_id);
            entry->release_id = release_id ? g_strdup(release_id) : NULL;
            entry->score = (float)score;

            g_ptr_array_add(all_results, entry);
        }
    }

    // Convert to array
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
    g_object_unref(parser);
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_acoustid_lookup(mb_http_client_t* client,
                                        const char* api_key,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response) {
    if (!client || !api_key || !fingerprint || !response) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (!fingerprint->fingerprint || fingerprint->duration <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(response, 0, sizeof(mb_acoustid_response_t));

    // Build URL
    char* url = build_lookup_url(api_key, fingerprint);

    // Make request
    char* json_response = NULL;
    size_t response_len = 0;
    quadrature_result_t result = mb_http_get(client, url, MB_RATE_ACOUSTID,
                                              &json_response, &response_len);
    g_free(url);

    if (result != QUADRATURE_OK) {
        return result;
    }

    // Parse response
    parse_acoustid_response(json_response, response);
    g_free(json_response);

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
