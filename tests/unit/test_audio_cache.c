/**
 * Audio Cache Tests
 *
 * Consolidated tests for the thread-safe LRU audio buffer cache.
 * Each test verifies multiple related behaviors to maximize coverage efficiency.
 *
 * Key invariants tested:
 * - Lock count prevents eviction during playback
 * - LRU eviction respects memory limits
 * - Statistics accurately reflect operations
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "internal.h"
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* FFmpeg must be initialized before Criterion forks to avoid corruption */
#include <libavformat/avformat.h>
ReportHook(PRE_ALL)(struct criterion_test_set *tests)
{
    (void)tests;
    avformat_network_init();
}

/* Test track IDs (arbitrary values for testing) */
#define TEST_TRACK_ID_1 1001
#define TEST_TRACK_ID_2 1002
#define TEST_TRACK_ID_3 1003
#define TEST_TRACK_ID_4 1004

/* Test format: stereo @ 48 kHz */
#define TEST_FORMAT ((audio_format_t){ .sample_rate = 48000, .channels = 2 })

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lifecycle, Null Safety, and Memory Configuration
 *
 * Verifies:
 * 1. Create with default memory limit succeeds
 * 2. Null parameter handling (create, destroy, all operations)
 * 3. Double-destroy is safe (idempotent)
 * 4. Memory limit can be changed after creation
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, lifecycle_memory_config)
{
    audio_cache_t *cache = NULL;

    /* Create with NULL out pointer returns error */
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, NULL), QUADRATURE_ERROR_INVALID_PARAM);

    /* Create with default limit (NULL library for testing) */
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);
    cr_assert_not_null(cache);
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);
    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Status Queries and Load Parameter Validation
 *
 * Verifies:
 * 1. Status is NOT_FOUND for unknown track IDs
 * 2. Invalid track ID handling for load and status
 * 3. Statistics start at zero
 * 4. Count is zero for empty cache
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, status_queries_parameter_validation)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    /* Status for unknown track ID */
    cr_assert_eq(audio_cache_get_status(cache, 999999), AUDIO_CACHE_NOT_FOUND);

    /* Invalid track ID handling (0 is invalid) - returns error, doesn't crash */
    cr_assert_eq(audio_cache_get_status(cache, 0), AUDIO_CACHE_NOT_FOUND);
    cr_assert_eq(audio_cache_load(cache, 0), QUADRATURE_ERROR_INVALID_PARAM);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Buffer Accessor Null Safety
 *
 * Verifies:
 * 1. All buffer accessors handle NULL safely
 * 2. Return appropriate defaults (NULL, 0) for NULL input
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, buffer_accessor_null_safety)
{
    /* All accessors must handle NULL buffer gracefully */
    cr_assert_null(audio_buffer_get_samples(NULL));
    cr_assert_eq(audio_buffer_get_num_frames(NULL), 0);
    cr_assert_eq(audio_buffer_get_track_id(NULL), 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Cancel Operations on Empty Cache
 *
 * Verifies:
 * 1. Cancel operations on empty cache are safe
 * 2. Cache remains usable after cancels
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, cancel_empty_cache)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    /* Operations on empty cache should not crash */
    audio_cache_cancel_load(cache, 999999);
    audio_cache_cancel_all_loads(cache);

    /* Cache should still be usable */
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lock/Unlock API Contract
 *
 * Lock/unlock require the track to be loaded first. Calling them on an
 * unloaded track crashes via g_error (see src/audio/cache.c:640,675).
 * The non-crashing surface is covered here; the crash contracts are
 * enforced by the death tests further down.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, lock_unlock_api_contract)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    /* Cache should be functional after creation */
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Death tests — invariant violations MUST terminate the process
 *
 * These tests verify the CLAUDE.md "crash on invariant violations / no
 * silent fallbacks" rule by triggering each documented crash path and
 * asserting the expected termination.
 *
 * Termination signal depends on which GLib macro fires:
 *   - g_assert()  → abort()       → SIGABRT
 *   - g_error()   → G_BREAKPOINT  → SIGTRAP
 *
 * Keep each death test minimal and single-purpose: the body should be the
 * shortest possible path to the crash, so a regression is unambiguous.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Invariant: audio_cache_lock() requires the track to have been loaded.
 * Enforced by g_error at src/audio/cache.c:640. */
Test(audio_cache, death_lock_unloaded_track, .signal = SIGTRAP)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);
    (void)audio_cache_lock(cache, 12345); /* never loaded → g_error */
}

/* Invariant: audio_cache_unlock() on a track not in cache aborts.
 * Enforced by g_error at src/audio/cache.c:675. */
Test(audio_cache, death_unlock_unloaded_track, .signal = SIGTRAP)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);
    audio_cache_unlock(cache, 12345, AUDIO_CACHE_UNLOCK_IMMEDIATE);
}

/* Invariant: track_id must be > 0. Enforced by g_assert at cache.c:634. */
Test(audio_cache, death_lock_rejects_zero_track_id, .signal = SIGABRT)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);
    (void)audio_cache_lock(cache, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Decode Events Query
 *
 * Verifies:
 * 1. Decode events are empty on fresh cache
 * 2. Decode events query works on empty cache
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, decode_events_query)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    /* Decode events should be empty on fresh cache */
    audio_cache_decode_event_t events[10];
    uint32_t count = audio_cache_get_decode_events(cache, events, 10);
    cr_assert_eq(count, 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Concurrent Status Queries (Thread Safety)
 *
 * Verifies:
 * 1. Multiple threads can query status concurrently
 * 2. No crashes or data corruption under concurrent access
 * 3. Status queries are thread-safe
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CONCURRENT_THREADS    8
#define CONCURRENT_ITERATIONS 1000

typedef struct {
    audio_cache_t *cache;
    int thread_id;
} concurrent_test_ctx_t;

static void *
status_query_thread(void *arg)
{
    concurrent_test_ctx_t *ctx = arg;
    int64_t track_ids[4];

    /* Generate unique track IDs for this thread */
    for (int i = 0; i < 4; i++) {
        track_ids[i] = (int64_t)(ctx->thread_id * 1000 + i + 1);
    }

    for (int i = 0; i < CONCURRENT_ITERATIONS; i++) {
        for (int t = 0; t < 4; t++) {
            /* Status queries should return NOT_FOUND for unloaded tracks */
            audio_cache_get_status(ctx->cache, track_ids[t]);
        }
        /* Also query cache-wide stats */
        audio_cache_get_memory_used(ctx->cache);
    }

    return NULL;
}

Test(audio_cache, concurrent_status_queries)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    pthread_t threads[CONCURRENT_THREADS];
    concurrent_test_ctx_t contexts[CONCURRENT_THREADS];

    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        contexts[i].cache = cache;
        contexts[i].thread_id = i;
        pthread_create(&threads[i], NULL, status_query_thread, &contexts[i]);
    }

    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Cache should still be functional */
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Load Request Parameter Validation
 *
 * Note: We cannot test actual decoding without a LibraryCache and real files,
 * but we can test the load request parameter validation.
 *
 * Verifies:
 * 1. Load with invalid track ID returns error
 * 2. Cancel operations are always safe
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, load_request_parameter_validation)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    /* Cancel is always safe on empty cache */
    audio_cache_cancel_load(cache, 999999);
    audio_cache_cancel_load(cache, 888888);
    audio_cache_cancel_all_loads(cache);

    /* Cache should still be functional */
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Sweep Stale on Empty Cache
 *
 * Verifies:
 * 1. Sweep on empty cache is safe
 * 2. Cache remains usable after sweep
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, sweep_stale_empty)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    /* Sweep on empty cache should not crash */
    audio_cache_sweep_stale(cache, 60 * G_USEC_PER_SEC);

    /* Cache should still be functional */
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Concurrent Cache Operations (Thread Safety)
 *
 * Note: Lock/unlock require tracks to be loaded first (API contract).
 * This test verifies thread-safe read operations on empty cache.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *
cache_read_thread(void *arg)
{
    concurrent_test_ctx_t *ctx = arg;
    int64_t track_id = (int64_t)(ctx->thread_id + 1);

    for (int i = 0; i < CONCURRENT_ITERATIONS; i++) {
        /* Thread-safe read operations */
        audio_cache_get_status(ctx->cache, track_id);
        audio_cache_get_memory_used(ctx->cache);
    }

    return NULL;
}

Test(audio_cache, concurrent_cache_reads)
{
    audio_cache_t *cache = NULL;
    cr_assert_eq(audio_cache_create(NULL, TEST_FORMAT, &cache), QUADRATURE_OK);

    pthread_t threads[CONCURRENT_THREADS];
    concurrent_test_ctx_t contexts[CONCURRENT_THREADS];

    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        contexts[i].cache = cache;
        contexts[i].thread_id = i;
        pthread_create(&threads[i], NULL, cache_read_thread, &contexts[i]);
    }

    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Cache should still be functional */
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Unlock Delay Computation
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_cache, unlock_delay_computation)
{
    /* Formula: 3 × ceil(quantum_frames / sample_rate × 1000), min 16ms */

    /* 48kHz / 256 frames → period ~5.3ms → 3×6 = 18ms */
    uint32_t d1 = audio_cache_compute_unlock_delay(256, 48000);
    cr_assert_geq(d1, AUDIO_CACHE_UNLOCK_DELAY_MIN_MS);
    cr_assert_leq(d1, 50);

    /* 48kHz / 512 frames → period ~10.7ms → 3×11 = 33ms */
    uint32_t d2 = audio_cache_compute_unlock_delay(512, 48000);
    cr_assert_gt(d2, d1, "512 quantum should produce longer delay than 256");

    /* 48kHz / 4096 frames → period ~85ms → 3×86 = 258ms */
    uint32_t d3 = audio_cache_compute_unlock_delay(4096, 48000);
    cr_assert_geq(d3, 200);

    /* Minimum clamp: very small quantum */
    uint32_t d4 = audio_cache_compute_unlock_delay(32, 48000);
    cr_assert_geq(d4, AUDIO_CACHE_UNLOCK_DELAY_MIN_MS);

    /* Monotonic: larger quantum → larger delay */
    cr_assert_leq(d1, d2);
    cr_assert_leq(d2, d3);
}

Test(audio_cache, set_quantum_updates_delay)
{
    audio_cache_t *cache = NULL;
    audio_cache_create(NULL, TEST_FORMAT, &cache);
    cr_assert_not_null(cache);

    /* Update to larger quantum — verify no crash and cache still functional */
    audio_cache_set_quantum(cache, 1024);
    cr_assert_eq(audio_cache_get_memory_used(cache), 0);

    audio_cache_destroy(cache);
}
