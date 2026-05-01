/**
 * MusicBrainz HTTP backend — operation implementations.
 *
 * Talks to the public REST APIs:
 *   - musicbrainz.org/ws/2 for release/recording/artist metadata, ISRC,
 *     and Lucene text search (same Solr Picard uses)
 *   - api.acoustid.org/v2 for fingerprint matching
 *
 * All ops respect the rate limiters in mb_http_backend.c. JSON parsing
 * via json-glib produces the same mb_release_t / mb_acoustid_response_t
 * shapes as the PG backend so mb_resolver.c is unaware of the difference.
 *
 * Error semantics:
 *   - 404 on a single MBID → silent skip (matches PG "missing row" behavior)
 *   - 5xx, 429, network failure → QUADRATURE_ERROR_SERVICE_UNAVAILABLE
 *     (resolver's circuit breaker handles this exactly like PG failures)
 *   - 4xx other than 404 → log + skip
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
 * HTTP helpers
 * ---------------------------------------------------------------------------- */

/* Single SoupMessage send. Returns body on 2xx/3xx/4xx; NULL on transport
 * error (with *out_status=0) or non-2xx where we want the caller to inspect
 * status. *out_status is always written. */
static GBytes* http_send_once(SoupSession* session,
                               const char* method, const char* url,
                               const char* content_type, GBytes* req_body,
                               guint* out_status) {
    SoupMessage* msg = soup_message_new(method, url);
    if (req_body) {
        soup_message_set_request_body_from_bytes(msg, content_type, req_body);
    }
    GError* err = NULL;
    GBytes* body = soup_session_send_and_read(session, msg, NULL, &err);
    *out_status = soup_message_get_status(msg);
    g_object_unref(msg);
    if (err) {
        if (body) { g_bytes_unref(body); body = NULL; }
        /* Transport error (timeout, connection refused, DNS, etc.):
         * status is 0 — caller distinguishes from real HTTP statuses. */
        if (*out_status == 0) {
            g_debug("http_send_once: transport error: %s", err->message);
        } else {
            g_warning("http_send: %s", err->message);
        }
        g_error_free(err);
    }
    return body;
}

/* Send with one retry on transient failure. Retries trigger on:
 *   - transport error (status==0): socket timeout, DNS failure, connection
 *     refused. The public MB endpoint occasionally drops requests under load.
 *   - 5xx / 429: server-side throttling or transient error.
 * Does NOT retry 4xx (the request itself is wrong; retrying won't help).
 * `req_body` may be NULL for GETs; content_type ignored when body is NULL. */
static GBytes* http_send(SoupSession* session,
                          const char* method, const char* url,
                          const char* content_type, GBytes* req_body,
                          guint* out_status) {
    GBytes* body = http_send_once(session, method, url,
                                   content_type, req_body, out_status);
    const gboolean transient = (*out_status == 0)
                            || (*out_status == 429)
                            || (*out_status >= 500);
    if (transient) {
        if (body) { g_bytes_unref(body); body = NULL; }
        /* Brief backoff so the next attempt isn't sent into the same congestion
         * window. 1.5s comfortably clears MB's 1 req/sec ceiling. */
        g_usleep(1500 * 1000);
        body = http_send_once(session, method, url,
                               content_type, req_body, out_status);
    }
    return body;
}

/* Map HTTP status to quadrature_result_t. 404 → OK with no body; rest
 * follow the abort-vs-skip semantics in the file header. */
static quadrature_result_t status_to_result(guint status) {
    if (status >= 200 && status < 300) return QUADRATURE_OK;
    if (status == 404) return QUADRATURE_OK;          /* not found = empty result */
    if (status == 429 || status >= 500) return QUADRATURE_ERROR_SERVICE_UNAVAILABLE;
    return QUADRATURE_ERROR_INTERNAL;
}

static JsonObject* parse_json_body(GBytes* body) {
    if (!body) return NULL;
    gsize sz = 0;
    const char* data = g_bytes_get_data(body, &sz);
    JsonParser* parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, (gssize)sz, NULL)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonObject* obj = json_node_dup_object(root);  /* take ownership */
    g_object_unref(parser);
    return obj;
}

/* ----------------------------------------------------------------------------
 * MB ws/2: parse release JSON → mb_release_t
 * ----------------------------------------------------------------------------
 *
 * Endpoint: GET /release/{mbid}?inc=artist-credits+recordings+release-groups+
 *                                  isrcs+labels+artist-rels&fmt=json
 *
 * Maps the JSON object hierarchy into the existing mb_release_t shape so
 * the resolver's downstream voting logic is unchanged.
 */

static char* dup_str(JsonObject* obj, const char* key) {
    if (!obj || !json_object_has_member(obj, key)) return NULL;
    if (json_object_get_null_member(obj, key)) return NULL;
    const char* v = json_object_get_string_member(obj, key);
    return v ? g_strdup(v) : NULL;
}

static int get_int(JsonObject* obj, const char* key, int defv) {
    if (!obj || !json_object_has_member(obj, key)) return defv;
    if (json_object_get_null_member(obj, key)) return defv;
    return (int)json_object_get_int_member(obj, key);
}

/* relations[] of a recording → mb_recording_link_row_t entries appended
 * to `arr`. ws/2 returns each relation as:
 *   { "type": "producer", "type-id": "<gid>",
 *     "target-credit": "...", "attributes": ["..."],
 *     "artist": { "id": "...", "name": "...", "sort-name": "...", "type": "Person" } }
 * Non-artist relations (work, recording, place, etc.) are skipped — those
 * come from other inc= flags and have no `artist` member. */
static void parse_recording_relations(const char* rec_mbid,
                                      JsonArray* relations,
                                      GPtrArray* arr) {
    if (!relations) return;
    guint n = json_array_get_length(relations);
    for (guint i = 0; i < n; i++) {
        JsonObject* r = json_array_get_object_element(relations, i);
        if (!r) continue;
        JsonObject* artist = json_object_has_member(r, "artist")
                             ? json_object_get_object_member(r, "artist") : NULL;
        if (!artist) continue;  /* not an artist-rel */

        mb_recording_link_row_t* row = g_new0(mb_recording_link_row_t, 1);
        row->recording_mbid   = g_strdup(rec_mbid);
        row->link_type_gid    = dup_str(r, "type-id");
        row->link_type_name   = dup_str(r, "type");
        row->link_type_desc   = NULL;  /* ws/2 doesn't include this; PG has it from link_type.description */
        row->artist_mbid      = dup_str(artist, "id");
        row->artist_name      = dup_str(artist, "name");
        row->artist_sort_name = dup_str(artist, "sort-name");
        row->artist_type      = dup_str(artist, "type");
        row->entity0_credit   = dup_str(r, "target-credit");

        /* attributes[] (JSON array of strings) → comma-separated, matches
         * the PG path's GROUP_CONCAT(lav.name) format. */
        if (json_object_has_member(r, "attributes")) {
            JsonArray* attrs = json_object_get_array_member(r, "attributes");
            guint na = json_array_get_length(attrs);
            if (na > 0) {
                GString* s = g_string_new(NULL);
                for (guint k = 0; k < na; k++) {
                    if (k > 0) g_string_append_c(s, ',');
                    const char* v = json_array_get_string_element(attrs, k);
                    if (v) g_string_append(s, v);
                }
                row->attributes = g_string_free(s, FALSE);
            }
        }

        g_ptr_array_add(arr, row);
    }
}

/* artist-credit array → mb_artist_t[] */
static void parse_artist_credits(JsonArray* credits,
                                  mb_artist_t** out_artists, size_t* out_count) {
    if (!credits) { *out_artists = NULL; *out_count = 0; return; }
    guint n = json_array_get_length(credits);
    *out_artists = g_new0(mb_artist_t, n);
    *out_count = n;
    for (guint i = 0; i < n; i++) {
        JsonObject* ac = json_array_get_object_element(credits, i);
        JsonObject* artist = json_object_get_object_member(ac, "artist");
        (*out_artists)[i].id            = dup_str(artist, "id");
        (*out_artists)[i].name          = dup_str(artist, "name");
        (*out_artists)[i].sort_name     = dup_str(artist, "sort-name");
        (*out_artists)[i].credited_name = dup_str(ac, "name");
        (*out_artists)[i].joinphrase    = dup_str(ac, "joinphrase");
    }
}

/* Parse one release JSON object → mb_release_t. If `links_out` is non-NULL,
 * also extracts artist-rels per recording and appends mb_recording_link_row_t
 * entries to it (caller owns the array and its elements). */
static mb_release_t* parse_release_with_links(JsonObject* root, GPtrArray* links_out) {
    if (!root) return NULL;
    mb_release_t* r = g_new0(mb_release_t, 1);
    r->id    = dup_str(root, "id");
    r->title = dup_str(root, "title");
    r->date  = dup_str(root, "date");
    r->barcode = dup_str(root, "barcode");

    /* release-group: id, primary-type */
    JsonObject* rg = json_object_get_object_member(root, "release-group");
    if (rg) {
        r->release_group_id = dup_str(rg, "id");
        r->type = dup_str(rg, "primary-type");
        /* genres (curated) */
        if (json_object_has_member(rg, "genres")) {
            JsonArray* genres = json_object_get_array_member(rg, "genres");
            GString* gs = g_string_new(NULL);
            for (guint i = 0; i < json_array_get_length(genres); i++) {
                JsonObject* g = json_array_get_object_element(genres, i);
                if (i > 0) g_string_append_c(gs, ';');
                const char* n = json_object_get_string_member(g, "name");
                if (n) g_string_append(gs, n);
            }
            r->genres = g_string_free(gs, FALSE);
        }
    }

    /* label-info[0] */
    if (json_object_has_member(root, "label-info")) {
        JsonArray* li = json_object_get_array_member(root, "label-info");
        if (json_array_get_length(li) > 0) {
            JsonObject* l0 = json_array_get_object_element(li, 0);
            r->catalog_number = dup_str(l0, "catalog-number");
            JsonObject* lbl = json_object_get_object_member(l0, "label");
            if (lbl) r->label = dup_str(lbl, "name");
        }
    }

    /* album artists */
    if (json_object_has_member(root, "artist-credit")) {
        parse_artist_credits(json_object_get_array_member(root, "artist-credit"),
                             &r->artists, &r->artist_count);
    }

    /* media[].tracks[] → recordings */
    GArray* recs = g_array_new(FALSE, TRUE, sizeof(mb_recording_t));
    if (json_object_has_member(root, "media")) {
        JsonArray* media = json_object_get_array_member(root, "media");
        for (guint mi = 0; mi < json_array_get_length(media); mi++) {
            JsonObject* m = json_array_get_object_element(media, mi);
            int disc = get_int(m, "position", 1);
            JsonArray* tracks = json_object_get_array_member(m, "tracks");
            if (!tracks) continue;
            for (guint ti = 0; ti < json_array_get_length(tracks); ti++) {
                JsonObject* t = json_array_get_object_element(tracks, ti);
                mb_recording_t rec = {0};
                rec.position    = get_int(t, "position", (int)ti + 1);
                rec.disc_number = disc;
                rec.duration_ms = get_int(t, "length", 0);
                JsonObject* recording = json_object_get_object_member(t, "recording");
                if (recording) {
                    rec.id    = dup_str(recording, "id");
                    rec.title = dup_str(recording, "title");
                    if (json_object_has_member(recording, "artist-credit")) {
                        parse_artist_credits(
                            json_object_get_array_member(recording, "artist-credit"),
                            &rec.artists, &rec.artist_count);
                    }
                    if (links_out && rec.id
                        && json_object_has_member(recording, "relations")) {
                        parse_recording_relations(rec.id,
                            json_object_get_array_member(recording, "relations"),
                            links_out);
                    }
                }
                /* Track-level title overrides recording title where present */
                char* tt = dup_str(t, "title");
                if (tt) { g_free(rec.title); rec.title = tt; }
                g_array_append_val(recs, rec);
            }
        }
    }
    r->recording_count = recs->len;
    r->recordings = (mb_recording_t*)g_array_free(recs, FALSE);

    return r;
}

/* ----------------------------------------------------------------------------
 * batch_fetch: serial GETs over rate-limited HTTP
 * ---------------------------------------------------------------------------- */

static void batch_release_free(gpointer data) {
    mb_release_t* r = data;
    if (!r) return;
    mb_release_free(r);
    g_free(r);
}

/* Free one mb_recording_link_row_t (used as GPtrArray element-free). */
static void link_row_free(gpointer data) {
    mb_recording_link_row_t* l = data;
    if (!l) return;
    g_free(l->recording_mbid);
    g_free(l->link_type_gid);
    g_free(l->link_type_name);
    g_free(l->link_type_desc);
    g_free(l->artist_mbid);
    g_free(l->artist_name);
    g_free(l->artist_sort_name);
    g_free(l->artist_type);
    g_free(l->entity0_credit);
    g_free(l->attributes);
    g_free(l);
}

static void link_arr_free(gpointer data) {
    GPtrArray* arr = data;
    if (!arr) return;
    for (guint i = 0; i < arr->len; i++) link_row_free(g_ptr_array_index(arr, i));
    g_ptr_array_free(arr, TRUE);
}

quadrature_result_t mb_http_batch_fetch(mb_conn_t* conn,
                                         const char** ids, size_t n,
                                         GHashTable** out_releases,
                                         GHashTable** out_links)
{
    g_assert(conn != NULL);
    g_assert(out_releases != NULL);
    g_assert(out_links != NULL);

    http_slot_t* slot = (http_slot_t*)conn;
    SoupSession* session = (SoupSession*)mb_http_slot_session(slot);
    const char* base = mb_http_pool_mb_base(mb_http_slot_pool(slot));

    *out_releases = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, batch_release_free);
    *out_links = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, link_arr_free);

    for (size_t i = 0; i < n; i++) {
        if (!ids[i]) continue;
        mb_http_rate_limit_mb();

        /* recording-level-rels + artist-rels makes ws/2 embed every artist
         * relation (producer, vocal, instrument, etc.) under each recording's
         * `relations` array — equivalent to the PG path's l_artist_recording
         * join, in a single HTTP call. */
        char* url = g_strdup_printf(
            "%s/release/%s"
            "?inc=artist-credits+recordings+release-groups+labels+isrcs+genres"
            "+recording-level-rels+artist-rels"
            "&fmt=json",
            base, ids[i]);
        guint status = 0;
        GBytes* body = http_send(session, "GET", url, NULL, NULL, &status);
        g_free(url);

        quadrature_result_t res = status_to_result(status);
        if (res == QUADRATURE_ERROR_SERVICE_UNAVAILABLE) {
            if (body) g_bytes_unref(body);
            return res;  /* abort batch on 5xx/429 */
        }
        if (status == 404 || res != QUADRATURE_OK) {
            if (body) g_bytes_unref(body);
            continue;     /* not found / other 4xx → skip */
        }

        JsonObject* root = parse_json_body(body);
        g_bytes_unref(body);
        if (!root) {
            g_warning("mb_http_batch_fetch: failed to parse JSON for %s", ids[i]);
            continue;
        }

        GPtrArray* links_arr = g_ptr_array_new();
        mb_release_t* rel = parse_release_with_links(root, links_arr);
        json_object_unref(root);
        if (rel && rel->id) {
            g_hash_table_insert(*out_releases, g_strdup(ids[i]), rel);
            if (links_arr->len > 0) {
                g_hash_table_insert(*out_links, g_strdup(ids[i]), links_arr);
            } else {
                g_ptr_array_free(links_arr, TRUE);
            }
        } else {
            if (rel) { mb_release_free(rel); g_free(rel); }
            for (guint k = 0; k < links_arr->len; k++)
                link_row_free(g_ptr_array_index(links_arr, k));
            g_ptr_array_free(links_arr, TRUE);
        }
    }

    return QUADRATURE_OK;
}

/* ----------------------------------------------------------------------------
 * isrc_lookup: GET /isrc/{isrc}?inc=releases&fmt=json per ISRC
 *
 * Returns a flat list of (recording_id, release_id, release_group_id, score)
 * triples ready for the resolver's voting logic.
 * ---------------------------------------------------------------------------- */

quadrature_result_t mb_http_isrc_lookup(mb_conn_t* conn,
                                         const char** isrcs, size_t n,
                                         mb_acoustid_response_t* out)
{
    g_assert(conn != NULL);
    g_assert(out != NULL);
    out->results = NULL;
    out->count = 0;

    http_slot_t* slot = (http_slot_t*)conn;
    SoupSession* session = (SoupSession*)mb_http_slot_session(slot);
    const char* base = mb_http_pool_mb_base(mb_http_slot_pool(slot));

    GArray* results = g_array_new(FALSE, TRUE, sizeof(mb_acoustid_result_t));

    for (size_t i = 0; i < n; i++) {
        if (!isrcs[i] || !isrcs[i][0]) continue;
        mb_http_rate_limit_mb();

        char* url = g_strdup_printf("%s/isrc/%s?inc=releases+release-groups&fmt=json",
                                     base, isrcs[i]);
        guint status = 0;
        GBytes* body = http_send(session, "GET", url, NULL, NULL, &status);
        g_free(url);

        quadrature_result_t res = status_to_result(status);
        if (res == QUADRATURE_ERROR_SERVICE_UNAVAILABLE) {
            if (body) g_bytes_unref(body);
            g_array_free(results, TRUE);
            return res;
        }
        if (status == 404 || res != QUADRATURE_OK) {
            if (body) g_bytes_unref(body);
            continue;
        }

        JsonObject* root = parse_json_body(body);
        g_bytes_unref(body);
        if (!root) continue;

        /* Response shape: { "isrc": "...", "recordings": [{ "id":..., "releases": [...] }] } */
        if (json_object_has_member(root, "recordings")) {
            JsonArray* recs = json_object_get_array_member(root, "recordings");
            for (guint ri = 0; ri < json_array_get_length(recs); ri++) {
                JsonObject* rec = json_array_get_object_element(recs, ri);
                const char* rec_id = json_object_get_string_member_with_default(rec, "id", NULL);
                if (!rec_id) continue;
                JsonArray* rels = json_object_has_member(rec, "releases")
                                  ? json_object_get_array_member(rec, "releases") : NULL;
                if (rels) {
                    for (guint xi = 0; xi < json_array_get_length(rels); xi++) {
                        JsonObject* rel = json_array_get_object_element(rels, xi);
                        mb_acoustid_result_t r = {0};
                        r.recording_id = g_strdup(rec_id);
                        r.release_id   = dup_str(rel, "id");
                        JsonObject* rg = json_object_get_object_member(rel, "release-group");
                        r.release_group_id = rg ? dup_str(rg, "id") : NULL;
                        r.score = 1.0f;
                        g_array_append_val(results, r);
                    }
                }
            }
        }
        json_object_unref(root);
    }

    out->count = results->len;
    out->results = (mb_acoustid_result_t*)g_array_free(results, FALSE);
    return QUADRATURE_OK;
}

/* ----------------------------------------------------------------------------
 * fingerprint_lookup: POST api.acoustid.org/v2/lookup
 *
 * Single round-trip replaces the PG path's 3-hop flow.
 * ---------------------------------------------------------------------------- */

quadrature_result_t mb_http_fingerprint_lookup(mb_conn_t* conn,
                                                const mb_fingerprint_t* fp,
                                                mb_acoustid_response_t* out)
{
    g_assert(conn != NULL);
    g_assert(fp != NULL);
    g_assert(out != NULL);
    out->results = NULL;
    out->count = 0;

    http_slot_t* slot = (http_slot_t*)conn;
    http_pool_state_t* pool = mb_http_slot_pool(slot);
    const char* api_key = mb_http_pool_acoustid_key(pool);
    if (!api_key || !api_key[0]) {
        /* No key: caller should not have reached here (cap bit unset).
         * Defensive return — empty response = no match. */
        return QUADRATURE_OK;
    }

    SoupSession* session = (SoupSession*)mb_http_slot_session(slot);
    const char* base = mb_http_pool_acoustid_base(pool);

    mb_http_rate_limit_acoustid();

    /* AcoustID accepts URL-encoded form data via POST. We need:
     *   client=<api_key>, format=json, meta=releases+releasegroups,
     *   duration=<seconds>, fingerprint=<base64> */
    char* duration = g_strdup_printf("%d", fp->duration);
    char* form = soup_form_encode(
        "client",      api_key,
        "format",      "json",
        "meta",        "releases+releasegroups",
        "duration",    duration,
        "fingerprint", fp->fingerprint ? fp->fingerprint : "",
        NULL);
    g_free(duration);

    char* url = g_strdup_printf("%s/lookup", base);
    GBytes* form_bytes = g_bytes_new_take(form, strlen(form));
    guint status = 0;
    GBytes* body = http_send(session, "POST", url,
                              "application/x-www-form-urlencoded", form_bytes,
                              &status);
    g_bytes_unref(form_bytes);
    g_free(url);

    quadrature_result_t res = status_to_result(status);
    if (res != QUADRATURE_OK) {
        if (body) g_bytes_unref(body);
        return res;
    }

    JsonObject* root = parse_json_body(body);
    g_bytes_unref(body);
    if (!root) return QUADRATURE_OK;

    /* { "status": "ok", "results": [{ "id": ..., "score": 0.99,
     *                                  "recordings": [{ "id": ..., "releases": [{...}] }] }] } */
    GArray* arr = g_array_new(FALSE, TRUE, sizeof(mb_acoustid_result_t));
    const char* status_str = json_object_get_string_member_with_default(root, "status", "");
    if (g_strcmp0(status_str, "ok") == 0 && json_object_has_member(root, "results")) {
        JsonArray* results = json_object_get_array_member(root, "results");
        for (guint i = 0; i < json_array_get_length(results); i++) {
            JsonObject* res_obj = json_array_get_object_element(results, i);
            double score = json_object_get_double_member_with_default(res_obj, "score", 0.0);
            if (!json_object_has_member(res_obj, "recordings")) continue;
            JsonArray* recs = json_object_get_array_member(res_obj, "recordings");
            for (guint ri = 0; ri < json_array_get_length(recs); ri++) {
                JsonObject* rec = json_array_get_object_element(recs, ri);
                const char* rec_id = json_object_get_string_member_with_default(rec, "id", NULL);
                if (!rec_id) continue;
                JsonArray* rels = json_object_has_member(rec, "releases")
                                  ? json_object_get_array_member(rec, "releases") : NULL;
                if (rels && json_array_get_length(rels) > 0) {
                    for (guint xi = 0; xi < json_array_get_length(rels); xi++) {
                        JsonObject* rel = json_array_get_object_element(rels, xi);
                        mb_acoustid_result_t r = {0};
                        r.recording_id = g_strdup(rec_id);
                        r.release_id   = dup_str(rel, "id");
                        JsonObject* rg = json_object_get_object_member(rel, "releasegroup");
                        r.release_group_id = rg ? dup_str(rg, "id") : NULL;
                        r.score = (float)score;
                        g_array_append_val(arr, r);
                    }
                } else {
                    mb_acoustid_result_t r = {0};
                    r.recording_id = g_strdup(rec_id);
                    r.score = (float)score;
                    g_array_append_val(arr, r);
                }
            }
        }
    }
    json_object_unref(root);

    out->count = arr->len;
    out->results = (mb_acoustid_result_t*)g_array_free(arr, FALSE);
    return QUADRATURE_OK;
}

/* ----------------------------------------------------------------------------
 * solr_search: Lucene query against the public ws/2 search endpoint
 *
 * Same engine Picard uses. The public search response gives us per-release
 * track-count and artist-credit, but NOT per-track durations (only the PG
 * mirror has those). So we score on title/artist string match + Solr's
 * own relevance + track-count exact filter; duration check is the
 * resolver's downstream batch_fetch validation step, not here.
 *
 * Returns the best release_id. NULL if no candidate clears the threshold.
 * ---------------------------------------------------------------------------- */

/* Case-insensitive UTF-8 equality via Unicode casefold. */
static gboolean ci_equal(const char* a, const char* b) {
    if (!a || !b) return FALSE;
    char* af = g_utf8_casefold(a, -1);
    char* bf = g_utf8_casefold(b, -1);
    gboolean eq = g_str_equal(af, bf);
    g_free(af);
    g_free(bf);
    return eq;
}

/* Strip trailing " (YYYY)" or " (... Edition/Remaster/Mix/etc)" parenthetical
 * suffixes that users commonly add to folder names but MB titles don't carry.
 * Returns a newly-allocated cleaned string. */
static char* strip_edition_suffix(const char* s) {
    if (!s) return NULL;
    char* dup = g_strdup(s);
    char* paren = strrchr(dup, '(');
    if (paren && paren > dup && *(paren - 1) == ' ') {
        char* close = strrchr(paren, ')');
        if (close && *(close + 1) == '\0') {
            *(paren - 1) = '\0';
        }
    }
    g_strstrip(dup);
    return dup;
}

/* Score a candidate release against the query.
 *   title:  exact +1.0, casefold-equal +0.7, edition-stripped casefold +0.5
 *   artist: any credit exact +1.0, casefold +0.7
 *   tracks: equal +1.0, local<candidate +0.3, candidate==0 +0.3, local>candidate 0
 * Weighted: title*0.5 + artist*0.3 + tracks*0.2, then * solr_score. */
static double score_candidate(JsonObject* rel,
                               const char* q_album_raw, const char* q_artist,
                               size_t local_track_count,
                               double solr_score) {
    const char* title = json_object_get_string_member_with_default(rel, "title", "");
    char* q_album_clean = strip_edition_suffix(q_album_raw);

    double title_s = 0.0;
    if (g_strcmp0(title, q_album_raw) == 0)              title_s = 1.0;
    else if (ci_equal(title, q_album_raw))               title_s = 0.7;
    else if (ci_equal(title, q_album_clean))             title_s = 0.5;
    g_free(q_album_clean);

    double artist_s = 0.0;
    if (json_object_has_member(rel, "artist-credit")) {
        JsonArray* ac = json_object_get_array_member(rel, "artist-credit");
        for (guint i = 0; i < json_array_get_length(ac); i++) {
            JsonObject* c = json_array_get_object_element(ac, i);
            JsonObject* a = json_object_get_object_member(c, "artist");
            const char* an = a ? json_object_get_string_member_with_default(a, "name", "") : "";
            if (g_strcmp0(an, q_artist) == 0)    { artist_s = 1.0; break; }
            if (ci_equal(an, q_artist))          artist_s = MAX(artist_s, 0.7);
        }
    }

    int candidate_tracks = get_int(rel, "track-count", 0);
    double tracks_s;
    if (candidate_tracks == 0)                              tracks_s = 0.3;
    else if ((int)local_track_count == candidate_tracks)    tracks_s = 1.0;
    else if ((int)local_track_count < candidate_tracks)     tracks_s = 0.3;
    else                                                    tracks_s = 0.0;

    /* Single-disc tiebreaker: when title/artist match well, prefer releases
     * with one media item. Most user folders are single-disc rips; the public
     * search API returns multi-disc editions alongside flat ones at equal
     * Solr score. Small bonus (0.05) breaks ties without overriding real
     * signal differences. */
    int disc_count = 0;
    if (json_object_has_member(rel, "media")) {
        JsonArray* media = json_object_get_array_member(rel, "media");
        disc_count = (int)json_array_get_length(media);
    }
    double disc_bonus = (disc_count == 1) ? 0.05 : 0.0;

    double local = title_s * 0.5 + artist_s * 0.3 + tracks_s * 0.2 + disc_bonus;
    return local * solr_score;
}

/* Escape Lucene special chars in user-supplied strings to keep the query
 * well-formed. Public ws/2 honors the same syntax as the PG path. */
static char* http_escape_lucene(const char* input) {
    static gsize once = 0;
    static GRegex* re = NULL;
    if (g_once_init_enter(&once)) {
        re = g_regex_new("[+\\-&|!(){}\\[\\]^\"~*?:\\\\/]", 0, 0, NULL);
        g_once_init_leave(&once, 1);
    }
    return g_regex_replace(re, input, -1, 0, "\\\\\\0", 0, NULL);
}

quadrature_result_t mb_http_solr_search(mb_conn_t* conn,
                                         const char* album_title,
                                         const char* artist_name,
                                         size_t local_track_count,
                                         int64_t local_total_duration_ms,
                                         char** out_release_id)
{
    g_assert(conn != NULL);
    g_assert(out_release_id != NULL);
    (void)local_total_duration_ms;  /* not validatable here — public search response lacks per-track lengths */
    *out_release_id = NULL;

    if (!album_title || !artist_name || !album_title[0] || !artist_name[0]) {
        return QUADRATURE_OK;
    }

    http_slot_t* slot = (http_slot_t*)conn;
    SoupSession* session = (SoupSession*)mb_http_slot_session(slot);
    const char* base = mb_http_pool_mb_base(mb_http_slot_pool(slot));

    mb_http_rate_limit_mb();

    /* Picard-style query: parenthesized fields with default OR.
     * tracks:N acts as a relevance boost on top of the post-filter. */
    char* esc_album  = http_escape_lucene(album_title);
    char* esc_artist = http_escape_lucene(artist_name);
    char* query = g_strdup_printf("release:(%s) artist:(%s) tracks:%zu",
                                   esc_album, esc_artist, local_track_count);
    g_free(esc_album);
    g_free(esc_artist);
    char* enc_query = g_uri_escape_string(query, NULL, FALSE);
    g_free(query);

    char* url = g_strdup_printf("%s/release/?query=%s&limit=25&fmt=json",
                                 base, enc_query);
    g_free(enc_query);

    guint status = 0;
    GBytes* body = http_send(session, "GET", url, NULL, NULL, &status);
    g_free(url);

    quadrature_result_t res = status_to_result(status);
    if (res == QUADRATURE_ERROR_SERVICE_UNAVAILABLE) {
        if (body) g_bytes_unref(body);
        return res;
    }
    if (res != QUADRATURE_OK) {
        if (body) g_bytes_unref(body);
        return QUADRATURE_OK;
    }

    JsonObject* root = parse_json_body(body);
    g_bytes_unref(body);
    if (!root) return QUADRATURE_OK;

    if (!json_object_has_member(root, "releases")) {
        json_object_unref(root);
        return QUADRATURE_OK;
    }

    JsonArray* rels = json_object_get_array_member(root, "releases");
    guint n = json_array_get_length(rels);

    /* Pass 1: find max raw Solr score for normalization (ws/2 already
     * normalizes to 0..100 in most cases, but be defensive). */
    double max_solr = 0.0;
    for (guint i = 0; i < n; i++) {
        JsonObject* rel = json_array_get_object_element(rels, i);
        double s = json_object_get_double_member_with_default(rel, "score", 0.0);
        if (s > max_solr) max_solr = s;
    }
    if (max_solr <= 0.0) max_solr = 1.0;

    /* Pass 2: score every candidate, pick best above threshold. */
    const double THRESHOLD = 0.55;
    char* best_id = NULL;
    double best_score = -1.0;
    for (guint i = 0; i < n; i++) {
        JsonObject* rel = json_array_get_object_element(rels, i);
        double solr_norm = json_object_get_double_member_with_default(rel, "score", 0.0) / max_solr;
        double sc = score_candidate(rel, album_title, artist_name, local_track_count, solr_norm);
        if (sc > best_score) {
            best_score = sc;
            g_free(best_id);
            best_id = dup_str(rel, "id");
        }
    }

    if (best_id && best_score >= THRESHOLD) {
        *out_release_id = best_id;
        g_debug("mb_http_solr_search: '%s' by '%s' → %s (score %.3f, %u candidates)",
                album_title, artist_name, best_id, best_score, n);
    } else {
        if (best_id) {
            g_debug("mb_http_solr_search: rejected '%s' by '%s' — best %.3f < %.2f (%u candidates)",
                    album_title, artist_name, best_score, THRESHOLD, n);
            g_free(best_id);
        }
    }

    json_object_unref(root);
    return QUADRATURE_OK;
}
