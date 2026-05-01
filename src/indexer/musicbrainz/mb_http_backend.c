/**
 * MusicBrainz backend implementation: public REST APIs.
 *
 * Talks to musicbrainz.org/ws/2 and api.acoustid.org/v2 via libsoup3.
 * No PostgreSQL, no self-hosted infrastructure. Works inside a Flatpak
 * sandbox with `--share=network`.
 *
 * Rate limiting is process-wide and strict: MB API ≤ 1 req/sec per IP,
 * AcoustID API ≤ 3 req/sec. The limiters are shared across all slots —
 * pool slot count is not parallelism here; it's just buffer space.
 *
 * Provides a strong definition of mb_backend_http_factory(), replacing
 * the weak stub in mb_backend.c at link time.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>

/* AcoustID application key — identifies Quadrature to api.acoustid.org.
 * Defined in CMakeLists.txt under "Bundled application identifiers" alongside
 * FANART_TV_APPLICATION_KEY and MUSICBRAINZ_USER_AGENT. Override at build
 * time with `cmake -DACOUSTID_APPLICATION_KEY="..."` for forks. */
#ifndef ACOUSTID_APPLICATION_KEY
#error "ACOUSTID_APPLICATION_KEY must be defined by CMakeLists.txt"
#endif

/* ----------------------------------------------------------------------------
 * Rate limiter — process-wide token buckets per host.
 *
 * GMutex-protected last-request timestamps. mb_http_rate_limit_wait() blocks
 * the calling thread until the next slot is available, then stamps "now".
 *
 * Two limiters: one for musicbrainz.org (1 req/sec), one for
 * api.acoustid.org (3 req/sec). The HTTP backend respects published TOS.
 * ---------------------------------------------------------------------------- */

typedef struct {
    GMutex  mu;
    gint64  last_us;       /* monotonic time of last allowed request */
    gint64  min_interval_us;
    const char* host_label;  /* for logs */
} rate_limiter_t;

/* musicbrainz.org: published 1 req/sec per source IP.
 *   1100 ms interval → 0.909 req/sec. Strict-under, with 100 ms slack for
 *   clock drift, kernel scheduling jitter, and network re-ordering. */
static rate_limiter_t g_mb_rl = {
    .min_interval_us = 1100 * 1000,
    .host_label = "musicbrainz.org",
};
/* api.acoustid.org: published 3 req/sec per source IP.
 *   350 ms interval → 2.857 req/sec. Strict-under the 3/sec ceiling. The 17 ms
 *   slack vs the exact-3 boundary (333 ms) absorbs scheduling jitter and any
 *   transient queue backup that would otherwise spike us above the limit. */
static rate_limiter_t g_acoustid_rl = {
    .min_interval_us = 350 * 1000,
    .host_label = "api.acoustid.org",
};

static void rate_limiter_init_once(void) {
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        g_mutex_init(&g_mb_rl.mu);
        g_mutex_init(&g_acoustid_rl.mu);
        g_once_init_leave(&once, 1);
    }
}

static void rate_limiter_wait(rate_limiter_t* rl) {
    g_mutex_lock(&rl->mu);
    gint64 now = g_get_monotonic_time();
    gint64 wait_until = rl->last_us + rl->min_interval_us;
    if (now < wait_until) {
        gint64 sleep_us = wait_until - now;
        g_mutex_unlock(&rl->mu);
        g_usleep((gulong)sleep_us);
        g_mutex_lock(&rl->mu);
        now = g_get_monotonic_time();
    }
    rl->last_us = now;
    g_mutex_unlock(&rl->mu);
}

void mb_http_rate_limit_mb(void)       { rate_limiter_init_once(); rate_limiter_wait(&g_mb_rl); }
void mb_http_rate_limit_acoustid(void) { rate_limiter_init_once(); rate_limiter_wait(&g_acoustid_rl); }

/* ----------------------------------------------------------------------------
 * Pool / slot state
 * ---------------------------------------------------------------------------- */

typedef struct http_pool_state http_pool_state_t;

struct http_slot {
    SoupSession*       session;
    http_pool_state_t* pool;        /* back-pointer */
};

struct http_pool_state {
    http_slot_t*  slots;
    size_t        count;
    volatile gint next_slot;

    /* Endpoints + auth, owned strings. */
    char* mb_base_url;          /* e.g. https://musicbrainz.org/ws/2 */
    char* acoustid_base_url;    /* e.g. https://api.acoustid.org/v2 */
    char* mb_user_agent;        /* e.g. quadrature/0.1.0 ( https://github.com/... ) */
    char* acoustid_api_key;     /* may be NULL → fingerprinting disabled */
};

http_pool_state_t* mb_http_slot_pool(http_slot_t* s) { return s->pool; }
void*              mb_http_slot_session(http_slot_t* s) { return s->session; }
const char*        mb_http_pool_mb_base(const http_pool_state_t* p) { return p->mb_base_url; }
const char*        mb_http_pool_acoustid_base(const http_pool_state_t* p) { return p->acoustid_base_url; }
const char*        mb_http_pool_acoustid_key(const http_pool_state_t* p) { return p->acoustid_api_key; }

/* ----------------------------------------------------------------------------
 * Vtable ops
 * ---------------------------------------------------------------------------- */

static quadrature_result_t http_pool_create(const mb_backend_config_t* cfg,
                                             size_t slot_count,
                                             mb_pool_t** out)
{
    g_assert(cfg != NULL);
    g_assert(out != NULL);
    g_assert(slot_count > 0);

    if (!cfg->mb_user_agent || !cfg->mb_user_agent[0]) {
        g_warning("http_pool_create: mb_user_agent is required by MusicBrainz TOS");
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    http_pool_state_t* state = g_new0(http_pool_state_t, 1);
    state->slots = g_new0(http_slot_t, slot_count);
    state->count = slot_count;
    state->next_slot = 0;

    state->mb_base_url = g_strdup(
        (cfg->mb_base_url && cfg->mb_base_url[0]) ? cfg->mb_base_url
                                                  : "https://musicbrainz.org/ws/2");
    state->acoustid_base_url = g_strdup(
        (cfg->acoustid_base_url && cfg->acoustid_base_url[0]) ? cfg->acoustid_base_url
                                                              : "https://api.acoustid.org/v2");
    state->mb_user_agent = g_strdup(cfg->mb_user_agent);
    /* Application key from cfg (CMake/test override) wins; otherwise use the
     * default identifier baked at build time. */
    state->acoustid_api_key = (cfg->acoustid_api_key && cfg->acoustid_api_key[0])
                              ? g_strdup(cfg->acoustid_api_key)
                              : g_strdup(ACOUSTID_APPLICATION_KEY);

    for (size_t i = 0; i < slot_count; i++) {
        state->slots[i].pool = state;
        state->slots[i].session = soup_session_new();
        soup_session_set_user_agent(state->slots[i].session, state->mb_user_agent);
        /* 60s: MB ws/2 occasionally takes 30-40s for release lookups with
         * many inc= flags (recording-level-rels + artist-rels can add 25+
         * additional joins server-side). 30s was too tight under load. */
        soup_session_set_timeout(state->slots[i].session, 60);
    }

    *out = (mb_pool_t*)state;
    return QUADRATURE_OK;
}

static void http_pool_destroy(mb_pool_t* pool) {
    if (!pool) return;
    http_pool_state_t* state = (http_pool_state_t*)pool;
    for (size_t i = 0; i < state->count; i++) {
        if (state->slots[i].session) {
            soup_session_abort(state->slots[i].session);
            g_object_unref(state->slots[i].session);
        }
    }
    g_free(state->slots);
    g_free(state->mb_base_url);
    g_free(state->acoustid_base_url);
    g_free(state->mb_user_agent);
    g_free(state->acoustid_api_key);
    g_free(state);
}

static mb_conn_t* http_pool_claim_slot(mb_pool_t* pool, int slot) {
    g_assert(pool != NULL);
    http_pool_state_t* state = (http_pool_state_t*)pool;
    g_assert(slot >= 0 && (size_t)slot < state->count);
    return (mb_conn_t*)&state->slots[slot];
}

static bool http_conn_reset(mb_conn_t* conn) {
    g_assert(conn != NULL);
    http_slot_t* s = (http_slot_t*)conn;
    /* libsoup3 sessions auto-recover; abort idle conns to force fresh ones. */
    soup_session_abort(s->session);
    return true;
}

static size_t http_pool_count(const mb_pool_t* pool) {
    g_assert(pool != NULL);
    return ((const http_pool_state_t*)pool)->count;
}

static int http_pool_claim_round_robin(mb_pool_t* pool) {
    g_assert(pool != NULL);
    http_pool_state_t* s = (http_pool_state_t*)pool;
    return g_atomic_int_add(&s->next_slot, 1) % (int)s->count;
}

static const char* http_name(const mb_pool_t* pool) {
    (void)pool;
    return "http";
}

/* The actual op implementations live in mb_http_ops.c — they use HTTP
 * helpers shared between batch_fetch / isrc_lookup / solr_search. */
quadrature_result_t mb_http_batch_fetch(mb_conn_t* conn,
                                         const char** ids, size_t n,
                                         GHashTable** out_releases,
                                         GHashTable** out_links);
quadrature_result_t mb_http_isrc_lookup(mb_conn_t* conn,
                                         const char** isrcs, size_t n,
                                         mb_acoustid_response_t* out);
quadrature_result_t mb_http_fingerprint_lookup(mb_conn_t* conn,
                                                const mb_fingerprint_t* fp,
                                                mb_acoustid_response_t* out);
quadrature_result_t mb_http_solr_search(mb_conn_t* conn,
                                         const char* album_title,
                                         const char* artist_name,
                                         size_t local_track_count,
                                         int64_t local_total_duration_ms,
                                         char** out_release_id);

static const mb_backend_vtable_t HTTP_VTABLE = {
    .pool_create        = http_pool_create,
    .pool_destroy       = http_pool_destroy,
    .pool_claim_slot    = http_pool_claim_slot,
    .conn_reset         = http_conn_reset,
    .batch_fetch        = mb_http_batch_fetch,
    .isrc_lookup        = mb_http_isrc_lookup,
    .fingerprint_lookup = mb_http_fingerprint_lookup,
    .solr_search        = mb_http_solr_search,
    .pool_count         = http_pool_count,
    .pool_claim_round_robin = http_pool_claim_round_robin,
    .name               = http_name,
};

/* ----------------------------------------------------------------------------
 * Factory — strong symbol replaces weak stub in mb_backend.c.
 *
 * URI form: `mb+http://` or `mb+https://`. Endpoints come from cfg or
 * sensible defaults. The URI itself carries no information beyond scheme.
 * ---------------------------------------------------------------------------- */

quadrature_result_t mb_backend_http_factory(const char* uri,
                                            const mb_backend_config_t* cfg,
                                            size_t slot_count,
                                            mb_backend_t** out)
{
    g_assert(uri != NULL);
    g_assert(cfg != NULL);
    g_assert(out != NULL);
    g_assert(slot_count > 0);

    /* HTTP rate limit means slot count > 1 doesn't help — clamp to 1
     * to make resource accounting honest. */
    if (slot_count > 1) {
        g_debug("mb_backend_http_factory: slot_count %zu clamped to 1 (rate limit)",
                slot_count);
        slot_count = 1;
    }

    mb_pool_t* pool = NULL;
    quadrature_result_t res = http_pool_create(cfg, slot_count, &pool);
    if (res != QUADRATURE_OK) {
        if (pool) http_pool_destroy(pool);
        return res;
    }

    /* Fingerprint is always available — the bundled AcoustID key is the
     * default; a user-supplied key only overrides quota source. */
    mb_caps_t caps = MB_CAP_BATCH_FETCH | MB_CAP_ISRC_LOOKUP
                   | MB_CAP_SOLR_SEARCH | MB_CAP_FINGERPRINT;
    /* No MB_CAP_PREFETCH — rate limit serializes anyway. */

    mb_backend_t* be = g_new0(mb_backend_t, 1);
    be->vt   = &HTTP_VTABLE;
    be->pool = pool;
    be->caps = caps;
    be->uri  = g_strdup(uri);

    *out = be;
    return QUADRATURE_OK;
}
