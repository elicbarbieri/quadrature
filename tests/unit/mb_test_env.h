/**
 * Shared test environment for MusicBrainz/AcoustID integration tests.
 *
 * Backend selection: when QUADRATURE_TEST_HTTP=1 is set in the environment,
 * test helpers return NULL for PG conninfo strings, which causes
 * mb_resolver_create() (and indexer_create() under the hood) to dispatch to
 * the HTTP backend instead of the libpq one. The HTTP backend ships a
 * bundled AcoustID app key (QUADRATURE_BUNDLED_ACOUSTID_KEY); setting
 * ACOUSTID_API_KEY in the env overrides it for tests that want their own.
 *
 * Skip policy (find bugs, not be fast):
 *   - HTTP mode + direct-PG test            → skip (structurally impossible)
 *   - PG mode  + MB_PG_PASSWORD unset       → skip
 *   - PG mode  + ACOUSTID_PG_PASSWORD unset → skip tests that need fingerprinting
 *
 * Tests using mb_resolver_t / indexer_t (the backend-polymorphic layer) run
 * unchanged in either mode. Only direct mb_pg_* call sites need PG.
 */
#pragma once

#include <criterion/criterion.h>
#include <stdbool.h>
#include <stdlib.h>

/* Returns true when QUADRATURE_TEST_HTTP=1 (or y/Y/t/T) is set. */
static inline bool quad_test_use_http(void) {
    const char* v = getenv("QUADRATURE_TEST_HTTP");
    return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y'
                              || v[0] == 't' || v[0] == 'T');
}

/* Capability flags — what a given test needs to run. */
typedef enum {
    QUAD_TEST_NEEDS_MB           = 1 << 0,  /* needs ANY MB backend (resolver-level) */
    QUAD_TEST_NEEDS_FINGERPRINT  = 1 << 1,  /* needs AcoustID fingerprint matching */
    QUAD_TEST_NEEDS_DIRECT_PG    = 1 << 2,  /* uses raw mb_pg_* — PG-only by construction */
    QUAD_TEST_NEEDS_SOLR         = 1 << 3,  /* needs Solr-equivalent text search */
} quad_test_needs_t;

/* Returns a skip reason string if the requested needs cannot be met by the
 * current environment, or NULL if the test can run. */
static inline const char* quad_test_skip_reason(int needs) {
    if (quad_test_use_http()) {
        if (needs & QUAD_TEST_NEEDS_DIRECT_PG)
            return "QUADRATURE_TEST_HTTP=1 set: skipping direct-PG test";
        /* MB / fingerprint / Solr are all satisfiable on the public APIs
         * (fingerprint uses the bundled QUADRATURE_BUNDLED_ACOUSTID_KEY). */
        return NULL;
    }
    /* PG mode: defer to legacy env-var requirements. */
    if ((needs & QUAD_TEST_NEEDS_MB) && !(getenv("MB_PG_PASSWORD") && getenv("MB_PG_PASSWORD")[0]))
        return "MB_PG_PASSWORD not set";
    if ((needs & QUAD_TEST_NEEDS_FINGERPRINT)
        && !(getenv("ACOUSTID_PG_PASSWORD") && getenv("ACOUSTID_PG_PASSWORD")[0]))
        return "ACOUSTID_PG_PASSWORD not set";
    return NULL;
}

/* Skip the current test if the environment can't satisfy `needs`.
 * cr_skip's underlying macro concatenates "" with its arg, so it requires
 * string-literal varargs — pass via "%s" + runtime string. */
#define QUAD_TEST_REQUIRE(needs) do { \
    const char* _qreason = quad_test_skip_reason(needs); \
    if (_qreason) cr_skip("%s", _qreason); \
} while (0)
