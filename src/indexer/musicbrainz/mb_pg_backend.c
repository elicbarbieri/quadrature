/**
 * MusicBrainz backend implementation: PostgreSQL.
 *
 * Wraps the existing mb_pg_client / mb_*_lookup functions into the
 * mb_backend_t vtable. No behavior change — this is structural glue so
 * mb_resolver.c can call backends polymorphically (phase 4 migration).
 *
 * Compiled only when QUADRATURE_USE_LIBPQ is defined. Provides a strong
 * definition of mb_backend_pg_factory(), replacing the weak stub in
 * mb_backend.c at link time.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <glib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
 * Internal pool / slot state
 *
 * mb_pool_t and mb_conn_t are opaque to callers — they're cast through to
 * these private types only inside this file.
 * ---------------------------------------------------------------------------- */

typedef struct pg_pool_state pg_pool_state_t;

typedef struct {
    mb_pg_client_t *mb_conn;       /* MB PG client (always present) */
    mb_pg_client_t *acoustid_conn; /* acoustid PG client (NULL if unconfigured) */
    mb_http_conn_t *fp_index_conn; /* persistent HTTP to acoustid-index (NULL if unconfigured) */
    pg_pool_state_t *pool;         /* back-pointer for ops that need pool-scoped config */
} pg_slot_t;

struct pg_pool_state {
    pg_slot_t *slots;
    size_t count;
    volatile gint next_slot; /* round-robin counter */
    char *mb_solr_url;       /* heap-owned, may be NULL */
};

/* ----------------------------------------------------------------------------
 * Vtable ops
 * ---------------------------------------------------------------------------- */

static quadrature_result_t
pg_pool_create(const mb_backend_config_t *cfg, size_t slot_count, mb_pool_t **out)
{
    g_assert(cfg != NULL);
    g_assert(out != NULL);
    g_assert(slot_count > 0);

    if (!cfg->mb_conninfo || !cfg->mb_conninfo[0]) {
        g_warning("pg_pool_create: mb_conninfo is required for pg:// backend");
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    pg_pool_state_t *state = g_new0(pg_pool_state_t, 1);
    state->slots = g_new0(pg_slot_t, slot_count);
    state->count = slot_count;
    state->next_slot = 0;
    state->mb_solr_url
        = (cfg->mb_solr_url && cfg->mb_solr_url[0]) ? g_strdup(cfg->mb_solr_url) : NULL;

    for (size_t i = 0; i < slot_count; i++) {
        state->slots[i].pool = state;
        quadrature_result_t res = mb_pg_client_create(cfg->mb_conninfo, &state->slots[i].mb_conn);
        if (res != QUADRATURE_OK) {
            g_warning("pg_pool_create: failed to create MB conn %zu", i);
            *out = (mb_pool_t *)state;
            /* Caller will invoke pool_destroy via mb_backend_destroy on failure. */
            return res;
        }
        mb_pg_set_schema(state->slots[i].mb_conn, "musicbrainz");
        mb_pg_install_batch_function(state->slots[i].mb_conn);

        if (cfg->acoustid_conninfo && cfg->acoustid_conninfo[0]) {
            res = mb_pg_client_create(cfg->acoustid_conninfo, &state->slots[i].acoustid_conn);
            if (res != QUADRATURE_OK) {
                g_warning("pg_pool_create: failed to create acoustid conn %zu (non-fatal)", i);
                state->slots[i].acoustid_conn = NULL;
            }
        }

        /* Prepare AcoustID lookup statements on both PG connections. */
        mb_acoustid_prepare_stmts(state->slots[i].mb_conn, state->slots[i].acoustid_conn);

        if (cfg->acoustid_index_url && cfg->acoustid_index_url[0]) {
            state->slots[i].fp_index_conn = mb_http_conn_create(cfg->acoustid_index_url);
            /* Non-fatal if initial connect fails — retried on first use. */
        }
    }

    *out = (mb_pool_t *)state;
    return QUADRATURE_OK;
}

static void
pg_pool_destroy(mb_pool_t *pool)
{
    if (!pool)
        return;
    pg_pool_state_t *state = (pg_pool_state_t *)pool;

    for (size_t i = 0; i < state->count; i++) {
        mb_pg_client_destroy(state->slots[i].mb_conn);
        if (state->slots[i].acoustid_conn)
            mb_pg_client_destroy(state->slots[i].acoustid_conn);
        if (state->slots[i].fp_index_conn)
            mb_http_conn_destroy(state->slots[i].fp_index_conn);
    }
    g_free(state->slots);
    g_free(state->mb_solr_url);
    g_free(state);
}

static mb_conn_t *
pg_pool_claim_slot(mb_pool_t *pool, int slot)
{
    g_assert(pool != NULL);
    pg_pool_state_t *state = (pg_pool_state_t *)pool;
    g_assert(slot >= 0 && (size_t)slot < state->count);
    return (mb_conn_t *)&state->slots[slot];
}

static bool
pg_conn_reset(mb_conn_t *conn)
{
    g_assert(conn != NULL);
    pg_slot_t *s = (pg_slot_t *)conn;
    bool ok = mb_pg_client_reset(s->mb_conn);
    if (s->acoustid_conn && !mb_pg_client_reset(s->acoustid_conn))
        ok = false;
    return ok;
}

static quadrature_result_t
pg_batch_fetch(
    mb_conn_t *conn, const char **ids, size_t n, GHashTable **out_releases, GHashTable **out_links)
{
    g_assert(conn != NULL);
    pg_slot_t *s = (pg_slot_t *)conn;
    return mb_fetch_all_batch(s->mb_conn, ids, n, out_releases, out_links);
}

static quadrature_result_t
pg_isrc_lookup(mb_conn_t *conn, const char **isrcs, size_t n, mb_acoustid_response_t *out)
{
    g_assert(conn != NULL);
    pg_slot_t *s = (pg_slot_t *)conn;
    return mb_isrc_lookup(s->mb_conn, isrcs, n, out);
}

static quadrature_result_t
pg_fingerprint_lookup(mb_conn_t *conn, const mb_fingerprint_t *fp, mb_acoustid_response_t *out)
{
    g_assert(conn != NULL);
    pg_slot_t *s = (pg_slot_t *)conn;
    return mb_acoustid_lookup(s->mb_conn, s->acoustid_conn, s->fp_index_conn, fp, out);
}

static quadrature_result_t
pg_solr_search(mb_conn_t *conn,
               const char *album_title,
               const char *artist_name,
               size_t local_track_count,
               int64_t local_total_duration_ms,
               char **out_release_id)
{
    g_assert(conn != NULL);
    g_assert(out_release_id != NULL);
    pg_slot_t *s = (pg_slot_t *)conn;
    g_assert(s->pool != NULL);

    /* Solr URL must have been provided at pool create time, otherwise the
     * MB_CAP_SOLR_SEARCH bit would not be set and the resolver would not
     * have reached this op. */
    g_assert(s->pool->mb_solr_url != NULL);

    *out_release_id = mb_solr_search_release(s->mb_conn,
                                             s->pool->mb_solr_url,
                                             album_title,
                                             artist_name,
                                             local_track_count,
                                             local_total_duration_ms);
    return QUADRATURE_OK;
}

static size_t
pg_pool_count(const mb_pool_t *pool)
{
    g_assert(pool != NULL);
    return ((const pg_pool_state_t *)pool)->count;
}

static int
pg_pool_claim_round_robin(mb_pool_t *pool)
{
    g_assert(pool != NULL);
    pg_pool_state_t *s = (pg_pool_state_t *)pool;
    return g_atomic_int_add(&s->next_slot, 1) % (int)s->count;
}

static const char *
pg_name(const mb_pool_t *pool)
{
    (void)pool;
    return "pg";
}

static const mb_backend_vtable_t PG_VTABLE = {
    .pool_create = pg_pool_create,
    .pool_destroy = pg_pool_destroy,
    .pool_claim_slot = pg_pool_claim_slot,
    .conn_reset = pg_conn_reset,
    .batch_fetch = pg_batch_fetch,
    .isrc_lookup = pg_isrc_lookup,
    .fingerprint_lookup = pg_fingerprint_lookup,
    .solr_search = pg_solr_search,
    .pool_count = pg_pool_count,
    .pool_claim_round_robin = pg_pool_claim_round_robin,
    .name = pg_name,
};

/* ----------------------------------------------------------------------------
 * Factory
 *
 * Strong definition — replaces the weak stub in mb_backend.c at link time.
 * URI form: `pg://<libpq conninfo>` (the conninfo follows the slashes
 * verbatim, e.g. `pg://host=localhost dbname=musicbrainz user=...`).
 * ---------------------------------------------------------------------------- */

quadrature_result_t
mb_backend_pg_factory(const char *uri,
                      const mb_backend_config_t *cfg,
                      size_t slot_count,
                      mb_backend_t **out)
{
    g_assert(uri != NULL);
    g_assert(cfg != NULL);
    g_assert(out != NULL);
    g_assert(slot_count > 0);

    /* Strip `pg://` prefix; remainder is the libpq conninfo string.
     * If cfg->mb_conninfo is already populated, prefer it (lets callers pass
     * conninfo through config rather than embedding it in the URI). */
    const char *uri_conninfo = uri + strlen("pg://");
    mb_backend_config_t local_cfg = *cfg;
    if (!local_cfg.mb_conninfo || !local_cfg.mb_conninfo[0]) {
        local_cfg.mb_conninfo = uri_conninfo;
    }

    mb_pool_t *pool = NULL;
    quadrature_result_t res = pg_pool_create(&local_cfg, slot_count, &pool);
    if (res != QUADRATURE_OK) {
        if (pool)
            pg_pool_destroy(pool);
        return res;
    }

    mb_caps_t caps = MB_CAP_BATCH_FETCH | MB_CAP_ISRC_LOOKUP | MB_CAP_FINGERPRINT | MB_CAP_PREFETCH;
    if (local_cfg.mb_solr_url && local_cfg.mb_solr_url[0]) {
        caps |= MB_CAP_SOLR_SEARCH;
    }

    mb_backend_t *be = g_new0(mb_backend_t, 1);
    be->vt = &PG_VTABLE;
    be->pool = pool;
    be->caps = caps;
    be->uri = g_strdup(uri);

    *out = be;
    return QUADRATURE_OK;
}
