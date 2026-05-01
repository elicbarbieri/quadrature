/**
 * MusicBrainz backend dispatcher.
 *
 * URI-scheme dispatch + factory. Concrete backends live in:
 *   - mb_pg_backend.c   (libpq, self-hosted MB mirror, schema `pg://...`)
 *   - mb_http_backend.c (libsoup3, public REST APIs, scheme `mb+http://`)
 *
 * Phase 1 status: typedefs in place; both factories return NOT_SUPPORTED
 * until phase 3 (PG refactor) and phase 7 (HTTP skeleton) land. Existing
 * mb_resolver.c continues to call mb_pg_* directly during the migration.
 */

#include "internal.h"
#include <glib.h>
#include <string.h>

#ifdef QUADRATURE_USE_LIBPQ
/* Forward decl — implemented in mb_pg_backend.c (phase 3). */
quadrature_result_t mb_backend_pg_factory(const char* uri,
                                          const mb_backend_config_t* cfg,
                                          size_t slot_count,
                                          mb_backend_t** out);
#endif

/* Forward decl — implemented in mb_http_backend.c (phase 7). */
quadrature_result_t mb_backend_http_factory(const char* uri,
                                            const mb_backend_config_t* cfg,
                                            size_t slot_count,
                                            mb_backend_t** out);

quadrature_result_t mb_backend_create(const char* uri,
                                      const mb_backend_config_t* cfg,
                                      size_t slot_count,
                                      mb_backend_t** out)
{
    g_assert(uri != NULL);
    g_assert(cfg != NULL);
    g_assert(out != NULL);
    g_assert(slot_count > 0);

    *out = NULL;

    if (g_str_has_prefix(uri, "pg://")) {
#ifdef QUADRATURE_USE_LIBPQ
        return mb_backend_pg_factory(uri, cfg, slot_count, out);
#else
        g_warning("mb_backend_create: pg:// URI requested but PG backend not built "
                  "(QUADRATURE_USE_LIBPQ=OFF)");
        return QUADRATURE_ERROR_NOT_SUPPORTED;
#endif
    }

    if (g_str_has_prefix(uri, "mb+http://") || g_str_has_prefix(uri, "mb+https://")) {
        return mb_backend_http_factory(uri, cfg, slot_count, out);
    }

    g_warning("mb_backend_create: unknown URI scheme: %s", uri);
    return QUADRATURE_ERROR_INVALID_PARAM;
}

/* ----------------------------------------------------------------------------
 * Shared free functions for MB types
 *
 * These don't depend on any specific backend — they just g_free() fields.
 * Defined here so HTTP-only builds (QUADRATURE_USE_LIBPQ=OFF) link cleanly.
 * ---------------------------------------------------------------------------- */

void mb_artist_free(mb_artist_t* artist) {
    if (!artist) return;
    g_free(artist->id);
    g_free(artist->name);
    g_free(artist->credited_name);
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
    g_free(release->label);
    g_free(release->catalog_number);
    g_free(release->barcode);
    g_free(release->type);
    g_free(release->genres);
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
    memset(release, 0, sizeof(mb_release_t));
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

void mb_backend_destroy(mb_backend_t* backend) {
    if (!backend) return;
    g_assert(backend->vt != NULL);
    g_assert(backend->vt->pool_destroy != NULL);

    backend->vt->pool_destroy(backend->pool);
    g_free(backend->uri);
    g_free(backend);
}

/* Concrete factories live in mb_pg_backend.c and mb_http_backend.c. The
 * extern declarations above provide the link reference; the strong defs in
 * those files satisfy it. No weak stubs — both backends are now real. */
