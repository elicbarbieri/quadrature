/**
 * Phase 8 — Artist Bio Fetch (Wikipedia via Wikidata)
 *
 * For each artist with a MusicBrainz ID, fetches a biographical summary
 * from Wikipedia via the Wikidata SPARQL bridge:
 *   MBID → Wikidata P434 search → Q-ID → enwiki sitelink → Wikipedia REST summary
 *
 * Results are written to quadrature-bios.sqlite (standalone bios DB).
 * Uses 2 parallel worker threads with per-worker rate limiting (~8 req/s total).
 * Artists without a Wikipedia page get a sentinel row to avoid re-fetching.
 */

#include "internal.h"
#include "quadrature/metadata.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib.h>
#include <string.h>

#define BIO_USER_AGENT "Quadrature/1.0 (https://github.com/elicb/quadrature)"
#define BIO_NUM_WORKERS 2
#define BIO_BATCH_SIZE 20
#define BIO_DEFAULT_RATE_LIMIT_MS 250

#define INITIAL_BACKOFF_MS 1000
#define MAX_BACKOFF_MS 30000
#define CONSECUTIVE_ERROR_LIMIT 3
#define PHASE_TIMEOUT_US ((int64_t)30 * 60 * G_USEC_PER_SEC)  // 30 minutes

// =============================================================================
// Rate Limiter (with exponential backoff, matches artist_art.c)
// =============================================================================

typedef struct {
    int64_t last_request_us;
    int base_interval_ms;
    int current_backoff_ms;
} rate_limiter_t;

static void rate_limiter_init(rate_limiter_t *rl, int interval_ms) {
    rl->last_request_us = 0;
    rl->base_interval_ms = interval_ms;
    rl->current_backoff_ms = 0;
}

static void rate_limiter_wait(rate_limiter_t *rl) {
    int wait_ms = rl->base_interval_ms + rl->current_backoff_ms;
    int64_t now = g_get_monotonic_time();
    int64_t elapsed_us = now - rl->last_request_us;
    int64_t interval_us = (int64_t)wait_ms * 1000;
    if (elapsed_us < interval_us) {
        g_usleep((useconds_t)(interval_us - elapsed_us));
    }
    rl->last_request_us = g_get_monotonic_time();
}

static void rate_limiter_backoff(rate_limiter_t *rl) {
    if (rl->current_backoff_ms == 0) {
        rl->current_backoff_ms = INITIAL_BACKOFF_MS;
    } else {
        rl->current_backoff_ms = MIN(rl->current_backoff_ms * 2, MAX_BACKOFF_MS);
    }
    g_debug("artist_bio: backoff increased to %d ms", rl->current_backoff_ms);
}

static void rate_limiter_reset_backoff(rate_limiter_t *rl) {
    rl->current_backoff_ms = 0;
}

// =============================================================================
// Wikipedia Fetch Pipeline
// =============================================================================

typedef struct {
    char *bio_text;    /* Owned, may be NULL */
    char *wiki_url;    /* Owned, may be NULL */
} bio_result_t;

/**
 * Make an HTTP GET request with retry on 429/5xx.
 * Returns response body (caller must unref) or NULL on failure.
 * Updates rate limiter backoff state based on response status.
 */
static GBytes *http_get_with_retry(SoupSession *session, const char *url,
                                    rate_limiter_t *rl, atomic_int *cancel_flag) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (atomic_load(cancel_flag)) return NULL;

        rate_limiter_wait(rl);

        SoupMessage *msg = soup_message_new("GET", url);
        if (!msg) return NULL;

        GError *error = NULL;
        GBytes *body = soup_session_send_and_read(session, msg, NULL, &error);
        guint status = soup_message_get_status(msg);
        g_object_unref(msg);

        if (error) {
            g_clear_error(&error);
            if (body) g_bytes_unref(body);
            return NULL;
        }

        if (status == 200) {
            rate_limiter_reset_backoff(rl);
            return body;
        }

        if (body) { g_bytes_unref(body); body = NULL; }

        if (status == 429 || status >= 500) {
            rate_limiter_backoff(rl);
            continue;
        }

        /* 4xx (not 429) — permanent failure, don't retry */
        return NULL;
    }
    return NULL;
}

/**
 * Fetch artist bio from Wikipedia via Wikidata.
 * Returns bio_text and wiki_url (caller must free).
 * Returns {NULL, NULL} if no Wikipedia article found or on failure.
 */
static bio_result_t fetch_artist_bio(SoupSession *session, const char *mbid,
                                      rate_limiter_t *rl, atomic_int *cancel_flag,
                                      bool *http_error_out) {
    bio_result_t result = {0};
    if (http_error_out) *http_error_out = false;

    /* Step 1: MBID → Wikidata Q-ID via P434 property search */
    char *wikidata_url = g_strdup_printf(
        "https://www.wikidata.org/w/api.php?"
        "action=query&list=search&srsearch=haswbstatement:P434=%s"
        "&srlimit=1&format=json",
        mbid);

    GBytes *body = http_get_with_retry(session, wikidata_url, rl, cancel_flag);
    g_free(wikidata_url);
    if (!body) {
        if (http_error_out) *http_error_out = true;
        return result;
    }

    gsize body_size;
    const char *body_data = g_bytes_get_data(body, &body_size);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body_data, (gssize)body_size, NULL)) {
        g_object_unref(parser);
        g_bytes_unref(body);
        return result;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *root_obj = json_node_get_object(root);
    JsonObject *query_obj = json_object_get_object_member(root_obj, "query");
    JsonArray *search_arr = query_obj ? json_object_get_array_member(query_obj, "search") : NULL;

    char *qid = NULL;
    if (search_arr && json_array_get_length(search_arr) > 0) {
        JsonObject *first = json_array_get_object_element(search_arr, 0);
        const char *title = json_object_get_string_member_with_default(first, "title", NULL);
        if (title) qid = g_strdup(title);
    }

    g_object_unref(parser);
    g_bytes_unref(body);

    if (!qid) return result;

    /* Step 2: Q-ID → enwiki article title via sitelinks */
    char *entity_url = g_strdup_printf(
        "https://www.wikidata.org/w/api.php?"
        "action=wbgetentities&ids=%s&props=sitelinks&sitefilter=enwiki&format=json",
        qid);

    body = http_get_with_retry(session, entity_url, rl, cancel_flag);
    g_free(entity_url);
    if (!body) { g_free(qid); return result; }

    body_data = g_bytes_get_data(body, &body_size);
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body_data, (gssize)body_size, NULL)) {
        g_object_unref(parser);
        g_bytes_unref(body);
        g_free(qid);
        return result;
    }

    root = json_parser_get_root(parser);
    root_obj = json_node_get_object(root);
    JsonObject *entities = json_object_has_member(root_obj, "entities")
        ? json_object_get_object_member(root_obj, "entities") : NULL;
    JsonObject *entity = (entities && json_object_has_member(entities, qid))
        ? json_object_get_object_member(entities, qid) : NULL;
    JsonObject *sitelinks = (entity && json_object_has_member(entity, "sitelinks"))
        ? json_object_get_object_member(entity, "sitelinks") : NULL;
    JsonObject *enwiki = (sitelinks && json_object_has_member(sitelinks, "enwiki"))
        ? json_object_get_object_member(sitelinks, "enwiki") : NULL;

    char *wiki_title = NULL;
    if (enwiki) {
        const char *t = json_object_get_string_member_with_default(enwiki, "title", NULL);
        if (t) wiki_title = g_strdup(t);
    }

    g_object_unref(parser);
    g_bytes_unref(body);
    g_free(qid);

    if (!wiki_title) return result;

    /* Step 3: Wikipedia article title → summary extract */
    char *encoded_title = g_uri_escape_string(wiki_title, NULL, FALSE);
    char *summary_url = g_strdup_printf(
        "https://en.wikipedia.org/api/rest_v1/page/summary/%s", encoded_title);

    result.wiki_url = g_strdup_printf("https://en.wikipedia.org/wiki/%s", encoded_title);
    g_free(encoded_title);
    g_free(wiki_title);

    body = http_get_with_retry(session, summary_url, rl, cancel_flag);
    g_free(summary_url);
    if (!body) return result;

    body_data = g_bytes_get_data(body, &body_size);
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body_data, (gssize)body_size, NULL)) {
        g_object_unref(parser);
        g_bytes_unref(body);
        return result;
    }

    root = json_parser_get_root(parser);
    root_obj = json_node_get_object(root);
    const char *extract = json_object_get_string_member_with_default(root_obj, "extract", NULL);

    if (extract && extract[0])
        result.bio_text = g_strdup(extract);

    g_object_unref(parser);
    g_bytes_unref(body);

    return result;
}

// =============================================================================
// Thread Pool Types
// =============================================================================

/* Work item pushed to thread pool */
typedef struct {
    char *mbid;             /* Owned — worker must free */
} bio_work_item_t;

/* Result pushed to GAsyncQueue by workers */
typedef struct {
    char *mbid;             /* Owned */
    char *bio_text;         /* Owned, may be NULL */
    char *wiki_url;         /* Owned, may be NULL */
    bool has_bio;           /* true if bio_text is non-empty */
    bool http_error;        /* true if all HTTP requests for this artist failed */
} bio_fetch_result_t;

/* Shared context for worker threads */
typedef struct {
    GAsyncQueue *result_queue;  /* Workers push results here */
    atomic_int *cancel_flag;
    int rate_limit_ms;
} bio_worker_ctx_t;

static void bio_fetch_result_free(bio_fetch_result_t *r) {
    if (!r) return;
    g_free(r->mbid);
    g_free(r->bio_text);
    g_free(r->wiki_url);
    g_free(r);
}

/* Thread pool worker function — each invocation handles one artist */
static void bio_worker_func(gpointer data, gpointer user_data) {
    bio_work_item_t *item = data;
    bio_worker_ctx_t *ctx = user_data;

    if (atomic_load(ctx->cancel_flag)) {
        g_free(item->mbid);
        g_free(item);
        return;
    }

    /* Each worker call creates a fresh session — SoupSession is not thread-safe,
     * but GThreadPool reuses threads. Use thread-local session via GPrivate. */
    static GPrivate session_key = G_PRIVATE_INIT(g_object_unref);
    static GPrivate rl_key = G_PRIVATE_INIT(g_free);

    SoupSession *session = g_private_get(&session_key);
    if (!session) {
        session = soup_session_new();
        soup_session_set_user_agent(session, BIO_USER_AGENT);
        g_private_set(&session_key, session);
    }

    rate_limiter_t *rl = g_private_get(&rl_key);
    if (!rl) {
        rl = g_malloc(sizeof(rate_limiter_t));
        rate_limiter_init(rl, ctx->rate_limit_ms);
        g_private_set(&rl_key, rl);
    }

    bool http_error = false;
    bio_result_t bio = fetch_artist_bio(session, item->mbid, rl, ctx->cancel_flag,
                                         &http_error);

    bio_fetch_result_t *result = g_malloc(sizeof(bio_fetch_result_t));
    result->mbid = item->mbid;  /* Transfer ownership */
    result->bio_text = bio.bio_text;
    result->wiki_url = bio.wiki_url;
    result->has_bio = (bio.bio_text && bio.bio_text[0]);
    result->http_error = http_error;

    g_async_queue_push(ctx->result_queue, result);

    g_free(item);
}

// =============================================================================
// Phase Entry Point
// =============================================================================

quadrature_result_t artist_bio_fetch_all(const artist_bio_config_t *config,
                                          artist_bio_progress_cb cb, void *user_data) {
    if (!config || !config->db || !config->library_root)
        return QUADRATURE_ERROR_INVALID_PARAM;

    /* Get all artists with MBIDs from the main library DB */
    int64_t *artist_ids = NULL;
    char **mbids = NULL;
    size_t count = 0;

    quadrature_result_t res = db_get_artists_with_mbid(config->db,
        &artist_ids, &mbids, &count);
    if (res != QUADRATURE_OK || count == 0) {
        g_free(artist_ids);
        g_strfreev(mbids);
        return QUADRATURE_OK;
    }

    /* Open bios DB for writing */
    quadrature_bios_db_t *bios_db = NULL;
    res = db_bios_open(config->library_root, &bios_db);
    if (res != QUADRATURE_OK) {
        g_free(artist_ids);
        g_strfreev(mbids);
        return res;
    }

    /* Pre-filter: build work list of artists that need fetching.
     * Any existing row (including empty sentinel) means skip. */
    char **work_mbids = g_malloc(count * sizeof(char *));
    size_t work_count = 0;
    size_t cached_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (atomic_load(config->cancel_flag)) break;

        const char *mbid = mbids[i];
        if (!mbid || !mbid[0]) continue;

        bool exists = false;
        db_bios_exists(bios_db, mbid, &exists);

        if (exists) {
            /* Row exists (real bio or sentinel) — skip */
            cached_count++;
            continue;
        }

        work_mbids[work_count] = mbids[i];  /* Borrows from mbids[] */
        work_count++;
    }

    g_message("Phase 8: %zu artists with MBIDs (%zu cached, %zu to fetch)",
              count, cached_count, work_count);

    if (work_count == 0 || atomic_load(config->cancel_flag)) {
        g_free(work_mbids);
        db_bios_close(bios_db);
        g_free(artist_ids);
        g_strfreev(mbids);
        return QUADRATURE_OK;
    }

    artist_bio_progress_t progress = { .total = work_count };

    int rate_limit_ms = config->rate_limit_ms > 0
        ? config->rate_limit_ms : BIO_DEFAULT_RATE_LIMIT_MS;

    /* Set up producer-consumer: thread pool (producers) → async queue → main thread (consumer) */
    bio_worker_ctx_t worker_ctx = {
        .result_queue = g_async_queue_new(),
        .cancel_flag = config->cancel_flag,
        .rate_limit_ms = rate_limit_ms,
    };

    GError *pool_error = NULL;
    GThreadPool *pool = g_thread_pool_new(bio_worker_func, &worker_ctx,
                                           BIO_NUM_WORKERS, FALSE, &pool_error);
    if (!pool) {
        g_warning("Phase 8: failed to create thread pool: %s",
                  pool_error ? pool_error->message : "unknown");
        g_clear_error(&pool_error);
        g_async_queue_unref(worker_ctx.result_queue);
        g_free(work_mbids);
        db_bios_close(bios_db);
        g_free(artist_ids);
        g_strfreev(mbids);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Push all work items to the thread pool */
    for (size_t i = 0; i < work_count; i++) {
        bio_work_item_t *item = g_malloc(sizeof(bio_work_item_t));
        item->mbid = g_strdup(work_mbids[i]);
        g_thread_pool_push(pool, item, NULL);
    }

    /* Consume results from workers, write to DB on main thread */
    db_bios_begin(bios_db);
    size_t batch_count = 0;
    size_t results_received = 0;
    int consecutive_errors = 0;
    int64_t phase_start = g_get_monotonic_time();
    bool abort_phase = false;

    while (results_received < work_count && !abort_phase) {
        /* Pop with 200ms timeout so we can check cancel_flag and timeout */
        bio_fetch_result_t *result = g_async_queue_timeout_pop(
            worker_ctx.result_queue, 200 * 1000);

        if (atomic_load(config->cancel_flag)) {
            if (result) bio_fetch_result_free(result);
            /* Commit what we have before breaking */
            db_bios_commit(bios_db);
            db_bios_begin(bios_db);
            batch_count = 0;
            break;
        }

        /* Phase timeout */
        if (g_get_monotonic_time() - phase_start > PHASE_TIMEOUT_US) {
            g_warning("Phase 8: timeout after 30 minutes — %zu/%zu artists processed",
                      results_received, work_count);
            if (result) bio_fetch_result_free(result);
            db_bios_commit(bios_db);
            db_bios_begin(bios_db);
            batch_count = 0;
            break;
        }

        if (!result) continue;  /* Timeout — check cancel and loop */

        results_received++;

        /* Consecutive error tracking and telemetry */
        if (result->http_error) {
            if (config->http_errors)
                atomic_fetch_add(config->http_errors, 1);
            consecutive_errors++;
            if (consecutive_errors >= CONSECUTIVE_ERROR_LIMIT) {
                g_warning("Phase 8: aborting after %d consecutive network errors",
                          CONSECUTIVE_ERROR_LIMIT);
                bio_fetch_result_free(result);
                db_bios_commit(bios_db);
                db_bios_begin(bios_db);
                batch_count = 0;
                abort_phase = true;
                break;
            }
        } else {
            consecutive_errors = 0;
        }

        if (result->has_bio) {
            db_bios_upsert(bios_db, result->mbid,
                            result->bio_text, result->wiki_url);
            progress.fetched++;
        } else {
            /* No Wikipedia article — write sentinel (empty bio_text)
             * so we skip this artist on future runs */
            db_bios_upsert(bios_db, result->mbid, "", NULL);
            progress.no_bio++;
        }

        batch_count++;

        if (batch_count >= BIO_BATCH_SIZE) {
            db_bios_commit(bios_db);
            db_bios_begin(bios_db);
            batch_count = 0;
        }

        bio_fetch_result_free(result);

        progress.processed++;
        if (cb) cb(&progress, user_data);
    }

    /* Wait for all workers to finish (some may still be in-flight after cancel) */
    g_thread_pool_free(pool, TRUE /* immediate */, TRUE /* wait */);

    /* Drain any remaining results from the queue */
    bio_fetch_result_t *leftover;
    while ((leftover = g_async_queue_try_pop(worker_ctx.result_queue))) {
        bio_fetch_result_free(leftover);
    }

    /* Final commit */
    db_bios_commit(bios_db);
    db_bios_checkpoint(bios_db);
    db_bios_close(bios_db);

    g_async_queue_unref(worker_ctx.result_queue);
    g_free(work_mbids);
    g_free(artist_ids);
    g_strfreev(mbids);

    return QUADRATURE_OK;
}
