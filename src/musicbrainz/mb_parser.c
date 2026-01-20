/**
 * MusicBrainz JSON response parser.
 *
 * Fetches and parses release metadata from the MusicBrainz API.
 */

#include "internal.h"
#include <json-glib/json-glib.h>
#include <string.h>

// =============================================================================
// JSON Helpers
// =============================================================================

static char* json_get_string(JsonObject* obj, const char* key) {
    if (!json_object_has_member(obj, key)) {
        return NULL;
    }
    JsonNode* node = json_object_get_member(obj, key);
    if (json_node_is_null(node)) {
        return NULL;
    }
    const char* val = json_object_get_string_member(obj, key);
    return val ? g_strdup(val) : NULL;
}

static int json_get_int(JsonObject* obj, const char* key, int default_val) {
    if (!json_object_has_member(obj, key)) {
        return default_val;
    }
    return (int)json_object_get_int_member(obj, key);
}

// =============================================================================
// Artist Parsing
// =============================================================================

static void parse_artist(JsonObject* artist_obj, mb_artist_t* artist) {
    artist->id = json_get_string(artist_obj, "id");
    artist->name = json_get_string(artist_obj, "name");
    artist->sort_name = json_get_string(artist_obj, "sort-name");
}

static void parse_artist_credit(JsonArray* artist_credit, mb_artist_t** artists_out, size_t* count_out) {
    if (!artist_credit) {
        *artists_out = NULL;
        *count_out = 0;
        return;
    }

    guint len = json_array_get_length(artist_credit);
    if (len == 0) {
        *artists_out = NULL;
        *count_out = 0;
        return;
    }

    *artists_out = g_new0(mb_artist_t, len);
    *count_out = len;

    for (guint i = 0; i < len; i++) {
        JsonNode* node = json_array_get_element(artist_credit, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) continue;

        JsonObject* credit_obj = json_node_get_object(node);
        if (!json_object_has_member(credit_obj, "artist")) continue;

        JsonObject* artist_obj = json_object_get_object_member(credit_obj, "artist");
        if (artist_obj) {
            parse_artist(artist_obj, &(*artists_out)[i]);
        }
    }
}

// =============================================================================
// Recording/Track Parsing
// =============================================================================

static void parse_recording(JsonObject* track_obj, mb_recording_t* recording, int disc_number) {
    // Track position
    recording->position = json_get_int(track_obj, "position", 0);
    recording->disc_number = disc_number;

    // Get the actual recording object
    if (!json_object_has_member(track_obj, "recording")) {
        return;
    }

    JsonObject* rec_obj = json_object_get_object_member(track_obj, "recording");
    if (!rec_obj) return;

    recording->id = json_get_string(rec_obj, "id");
    recording->title = json_get_string(rec_obj, "title");
    recording->duration_ms = json_get_int(rec_obj, "length", 0);

    // Artist credit
    if (json_object_has_member(rec_obj, "artist-credit")) {
        JsonArray* artist_credit = json_object_get_array_member(rec_obj, "artist-credit");
        parse_artist_credit(artist_credit, &recording->artists, &recording->artist_count);
    }
}

// =============================================================================
// Release Parsing
// =============================================================================

static void parse_release(JsonObject* release_obj, mb_release_t* release) {
    release->id = json_get_string(release_obj, "id");
    release->title = json_get_string(release_obj, "title");
    release->date = json_get_string(release_obj, "date");
    release->country = json_get_string(release_obj, "country");
    release->barcode = json_get_string(release_obj, "barcode");
    release->status = json_get_string(release_obj, "status");

    // Release group
    if (json_object_has_member(release_obj, "release-group")) {
        JsonObject* rg = json_object_get_object_member(release_obj, "release-group");
        if (rg) {
            release->release_group_id = json_get_string(rg, "id");
            release->type = json_get_string(rg, "primary-type");
        }
    }

    // Label info
    if (json_object_has_member(release_obj, "label-info")) {
        JsonArray* label_info = json_object_get_array_member(release_obj, "label-info");
        if (label_info && json_array_get_length(label_info) > 0) {
            JsonNode* first = json_array_get_element(label_info, 0);
            if (JSON_NODE_HOLDS_OBJECT(first)) {
                JsonObject* li = json_node_get_object(first);
                if (json_object_has_member(li, "label")) {
                    JsonObject* label = json_object_get_object_member(li, "label");
                    if (label) {
                        release->label = json_get_string(label, "name");
                    }
                }
            }
        }
    }

    // Artist credit
    if (json_object_has_member(release_obj, "artist-credit")) {
        JsonArray* artist_credit = json_object_get_array_member(release_obj, "artist-credit");
        parse_artist_credit(artist_credit, &release->artists, &release->artist_count);
    }

    // Media/tracks
    if (json_object_has_member(release_obj, "media")) {
        JsonArray* media = json_object_get_array_member(release_obj, "media");
        if (media) {
            // Count total tracks across all media
            size_t total_tracks = 0;
            guint media_count = json_array_get_length(media);

            for (guint m = 0; m < media_count; m++) {
                JsonNode* media_node = json_array_get_element(media, m);
                if (!JSON_NODE_HOLDS_OBJECT(media_node)) continue;

                JsonObject* media_obj = json_node_get_object(media_node);
                if (json_object_has_member(media_obj, "tracks")) {
                    JsonArray* tracks = json_object_get_array_member(media_obj, "tracks");
                    if (tracks) {
                        total_tracks += json_array_get_length(tracks);
                    }
                }
            }

            if (total_tracks > 0) {
                release->recordings = g_new0(mb_recording_t, total_tracks);
                release->recording_count = total_tracks;

                size_t track_idx = 0;
                for (guint m = 0; m < media_count; m++) {
                    JsonNode* media_node = json_array_get_element(media, m);
                    if (!JSON_NODE_HOLDS_OBJECT(media_node)) continue;

                    JsonObject* media_obj = json_node_get_object(media_node);
                    int disc_number = json_get_int(media_obj, "position", m + 1);

                    if (json_object_has_member(media_obj, "tracks")) {
                        JsonArray* tracks = json_object_get_array_member(media_obj, "tracks");
                        if (!tracks) continue;

                        guint track_count = json_array_get_length(tracks);
                        for (guint t = 0; t < track_count && track_idx < total_tracks; t++) {
                            JsonNode* track_node = json_array_get_element(tracks, t);
                            if (!JSON_NODE_HOLDS_OBJECT(track_node)) continue;

                            JsonObject* track_obj = json_node_get_object(track_node);
                            parse_recording(track_obj, &release->recordings[track_idx], disc_number);
                            track_idx++;
                        }
                    }
                }
            }
        }
    }

    // Genres/tags
    if (json_object_has_member(release_obj, "genres")) {
        JsonArray* genres = json_object_get_array_member(release_obj, "genres");
        if (genres) {
            guint genre_count = json_array_get_length(genres);
            if (genre_count > 0) {
                release->genres = g_new0(char*, genre_count);
                release->genre_count = genre_count;

                for (guint i = 0; i < genre_count; i++) {
                    JsonNode* genre_node = json_array_get_element(genres, i);
                    if (JSON_NODE_HOLDS_OBJECT(genre_node)) {
                        JsonObject* genre_obj = json_node_get_object(genre_node);
                        release->genres[i] = json_get_string(genre_obj, "name");
                    }
                }
            }
        }
    }
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_fetch_release(mb_http_client_t* client,
                                      const char* release_id,
                                      mb_release_t* release) {
    if (!client || !release_id || !release) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(release, 0, sizeof(mb_release_t));

    // Build URL with includes
    char* url = g_strdup_printf(
        "%s/release/%s?fmt=json&inc=artists+recordings+labels+release-groups+genres+artist-credits",
        MUSICBRAINZ_API_URL,
        release_id
    );

    // Make request
    char* json_response = NULL;
    size_t response_len = 0;
    quadrature_result_t result = mb_http_get(client, url, MB_RATE_MUSICBRAINZ,
                                              &json_response, &response_len);
    g_free(url);

    if (result != QUADRATURE_OK) {
        return result;
    }

    // Parse JSON
    JsonParser* parser = json_parser_new();
    GError* error = NULL;

    if (!json_parser_load_from_data(parser, json_response, response_len, &error)) {
        g_warning("Failed to parse MusicBrainz JSON: %s", error->message);
        g_error_free(error);
        g_free(json_response);
        g_object_unref(parser);
        return QUADRATURE_ERROR_INTERNAL;
    }
    g_free(json_response);

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return QUADRATURE_ERROR_INTERNAL;
    }

    JsonObject* root_obj = json_node_get_object(root);

    // Check for error response
    if (json_object_has_member(root_obj, "error")) {
        const char* err_msg = json_object_get_string_member(root_obj, "error");
        g_warning("MusicBrainz API error: %s", err_msg ? err_msg : "unknown");
        g_object_unref(parser);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    // Parse the release
    parse_release(root_obj, release);

    g_object_unref(parser);
    return QUADRATURE_OK;
}

void mb_artist_free(mb_artist_t* artist) {
    if (!artist) return;
    g_free(artist->id);
    g_free(artist->name);
    g_free(artist->sort_name);
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
