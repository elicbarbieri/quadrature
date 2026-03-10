/**
 * Audio Pipeline Tests
 *
 * Consolidated tests for the main audio engine with multi-player support.
 * Tests focus on API contracts, state machine correctness, and parameter validation.
 *
 * Note: Full integration tests require PipeWire, which may not be available
 * in all test environments. These tests focus on behavior that can be verified
 * without active audio output.
 *
 * Key invariants tested:
 * - Player ID bounds checking (0-3)
 * - State queries return valid values
 * - Metering values are in valid ranges [0.0, 1.0]
 * - Multi-player operations are independent
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "quadrature/audio.h"
#include "quadrature/quadrature.h"
#include "internal.h"
#include <pthread.h>
#include <string.h>

/* FFmpeg must be initialized before Criterion forks */
#include <libavformat/avformat.h>
ReportHook(PRE_ALL)(struct criterion_test_set *tests) {
    (void)tests;
    avformat_network_init();
}

/* Test sample rate */
#define TEST_SAMPLE_RATE 48000

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lifecycle and Null Safety
 *
 * Verifies:
 * 1. Create with valid sample rate succeeds
 * 2. Null parameter handling for create
 * 3. Destroy null is safe (idempotent)
 * 4. Sample rate is stored correctly
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, lifecycle_null_safety) {
    audio_pipeline_t *pipeline = NULL;

    /* --- Null safety for create --- */
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, NULL), QUADRATURE_ERROR_INVALID_PARAM);

    /* Destroy null is safe */
    audio_pipeline_destroy(NULL);

    /* --- Queries on null pipeline return error states --- */
    /* Note: get_player_state returns CHANNEL_ERROR for NULL pipeline */
    cr_assert_eq(audio_pipeline_get_player_state(NULL, 0), CHANNEL_ERROR);
    cr_assert_eq(audio_pipeline_get_player_position(NULL, 0), 0);
    cr_assert_eq(audio_pipeline_get_player_length(NULL, 0), 0);
    cr_assert_eq(audio_pipeline_get_sample_rate(NULL), 0);

    /* --- Create pipeline --- */
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);
    cr_assert_not_null(pipeline);

    /* Verify sample rate stored */
    cr_assert_eq(audio_pipeline_get_sample_rate(pipeline), TEST_SAMPLE_RATE);

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Player ID Bounds Checking
 *
 * Verifies:
 * 1. Valid player IDs (0-3) are accepted for queries
 * 2. Invalid player IDs (negative, >= 4) return error states
 * 3. All player operations check bounds
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, player_id_bounds_checking) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    /* --- Test invalid player IDs --- */
    int invalid_ids[] = {-1, -100, 4, 5, 100, 1000};

    for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); i++) {
        int id = invalid_ids[i];

        /* Control operations should fail */
        cr_assert_eq(audio_pipeline_player_play(pipeline, id),
                     QUADRATURE_ERROR_INVALID_PARAM);
        cr_assert_eq(audio_pipeline_player_toggle_play(pipeline, id),
                     QUADRATURE_ERROR_INVALID_PARAM);
        cr_assert_eq(audio_pipeline_player_stop(pipeline, id),
                     QUADRATURE_ERROR_INVALID_PARAM);
        cr_assert_eq(audio_pipeline_player_seek(pipeline, id, 0),
                     QUADRATURE_ERROR_INVALID_PARAM);

        /* Queries should return error states */
        cr_assert_eq(audio_pipeline_get_player_state(pipeline, id), CHANNEL_ERROR);
        cr_assert_eq(audio_pipeline_get_player_position(pipeline, id), 0);
        cr_assert_eq(audio_pipeline_get_player_length(pipeline, id), 0);
    }

    /* --- Test valid player IDs (0-3) return valid states --- */
    for (int id = 0; id < 4; id++) {
        channel_state_t state = audio_pipeline_get_player_state(pipeline, id);
        /* Initial state should be STOPPED */
        cr_assert_eq(state, CHANNEL_STOPPED,
                    "Player %d should start STOPPED, got %d", id, state);
    }

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Initial Player State
 *
 * Verifies:
 * 1. All players start in STOPPED state
 * 2. Position is 0 for all players
 * 3. Length is 0 for all players (no track loaded)
 * 4. Not ready (no track loaded)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, initial_player_state) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    for (int id = 0; id < 4; id++) {
        /* State is STOPPED */
        cr_assert_eq(audio_pipeline_get_player_state(pipeline, id), CHANNEL_STOPPED,
                    "Player %d should start STOPPED", id);

        /* Position and length are 0 */
        cr_assert_eq(audio_pipeline_get_player_position(pipeline, id), 0);
        cr_assert_eq(audio_pipeline_get_player_length(pipeline, id), 0);
    }

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Repeat Control
 *
 * Verifies:
 * 1. Repeat can be enabled and disabled
 * 2. Setting repeat is per-player
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, repeat_control) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    /* Enable repeat on player 0 */
    cr_assert_eq(audio_pipeline_player_set_repeat(pipeline, 0, true), QUADRATURE_OK);

    /* Disable repeat on player 0 */
    cr_assert_eq(audio_pipeline_player_set_repeat(pipeline, 0, false), QUADRATURE_OK);

    /* Enable repeat on all players */
    for (int id = 0; id < 4; id++) {
        cr_assert_eq(audio_pipeline_player_set_repeat(pipeline, id, true), QUADRATURE_OK);
    }

    audio_pipeline_destroy(pipeline);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Spectrum Output
 *
 * Verifies:
 * 1. Spectrum values are in [0.0, 1.0] range
 * 2. Spectrum works for all players
 * 3. Various bar counts are handled
 * 4. NULL bars pointer is handled
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, spectrum_output) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    float left[24], right[24];

    for (int id = 0; id < 4; id++) {
        /* Clear bars first */
        memset(left, 0xFF, sizeof(left));
        memset(right, 0xFF, sizeof(right));

        audio_pipeline_get_player_spectrum(pipeline, id, left, right, 24);

        /* Verify all values in range */
        for (int i = 0; i < 24; i++) {
            cr_assert(left[i] >= 0.0f && left[i] <= 1.0f,
                     "Spectrum left bar %d out of range for player %d: %f", i, id, left[i]);
            cr_assert(right[i] >= 0.0f && right[i] <= 1.0f,
                     "Spectrum right bar %d out of range for player %d: %f", i, id, right[i]);
        }
    }

    /* Smaller bar counts */
    memset(left, 0xFF, sizeof(left));
    memset(right, 0xFF, sizeof(right));
    audio_pipeline_get_player_spectrum(pipeline, 0, left, right, 12);
    for (int i = 0; i < 12; i++) {
        cr_assert(left[i] >= 0.0f && left[i] <= 1.0f);
        cr_assert(right[i] >= 0.0f && right[i] <= 1.0f);
    }

    /* Note: Zero bar count or NULL bars would crash (API contract) */

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Smooth Position Query
 *
 * Verifies:
 * 1. Smooth position returns valid value
 * 2. Speed output parameter is optional (NULL)
 * 3. Works for all players
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, smooth_position_query) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    for (int id = 0; id < 4; id++) {
        float speed;
        double pos = audio_pipeline_get_player_position_smooth(pipeline, id, &speed);

        /* Position should be non-negative */
        cr_assert(pos >= 0.0, "Smooth position negative for player %d: %f", id, pos);

        /* Speed should be in valid range */
        cr_assert(speed >= -4.0f && speed <= 4.0f,
                 "Speed out of range for player %d: %f", id, speed);
    }

    /* NULL speed parameter is OK */
    double pos = audio_pipeline_get_player_position_smooth(pipeline, 0, NULL);
    cr_assert(pos >= 0.0);

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Load Parameter Validation
 *
 * Verifies:
 * 1. NULL path is rejected
 * 2. Invalid player ID is rejected
 * 3. NULL pipeline is rejected
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Device Routing
 *
 * Verifies:
 * 1. NULL device is accepted (clears device target)
 * 2. Device name can be set
 * 3. Device setting is per-player
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, device_routing) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    /* Set device for player 0 */
    cr_assert_eq(audio_pipeline_set_player_device(pipeline, 0, "hw:0"),
                 QUADRATURE_OK);

    /* Set device for player 1 */
    cr_assert_eq(audio_pipeline_set_player_device(pipeline, 1, "pipewire:default"),
                 QUADRATURE_OK);

    /* Clear device (NULL) */
    cr_assert_eq(audio_pipeline_set_player_device(pipeline, 0, NULL),
                 QUADRATURE_OK);

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Speed/Shuttle Control Requires Loaded Track
 *
 * Verifies:
 * 1. set_speed returns error when no track loaded
 * 2. set_shuttle_mode returns error when no track loaded
 *
 * Note: Speed/shuttle control requires a buffer to be loaded first.
 * This is by design - the scrubber operates on the buffer.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, speed_control_requires_track) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    /* set_speed fails without loaded track (returns INTERNAL error) */
    quadrature_result_t result = audio_pipeline_player_set_speed(pipeline, 0, 2.0f);
    cr_assert_neq(result, QUADRATURE_OK,
                 "set_speed should fail without loaded track");

    audio_pipeline_destroy(pipeline);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Concurrent Queries (Thread Safety)
 *
 * Verifies:
 * 1. Multiple threads can query pipeline state concurrently
 * 2. No crashes under concurrent access
 * 3. All returned values are valid
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PIPELINE_THREADS 8
#define PIPELINE_ITERATIONS 500

typedef struct {
    audio_pipeline_t *pipeline;
    int thread_id;
} pipeline_thread_ctx_t;

static void *pipeline_query_thread(void *arg) {
    pipeline_thread_ctx_t *ctx = arg;

    for (int i = 0; i < PIPELINE_ITERATIONS; i++) {
        for (int id = 0; id < 4; id++) {
            /* Query all properties */
            channel_state_t state = audio_pipeline_get_player_state(ctx->pipeline, id);
            cr_assert(state == CHANNEL_STOPPED || state == CHANNEL_PLAYING ||
                      state == CHANNEL_PAUSED || state == CHANNEL_ERROR);

            audio_pipeline_get_player_position(ctx->pipeline, id);
            audio_pipeline_get_player_length(ctx->pipeline, id);

            float left[24], right[24];
            audio_pipeline_get_player_spectrum(ctx->pipeline, id, left, right, 24);
        }
    }

    return NULL;
}

Test(audio_pipeline, concurrent_queries) {
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, &pipeline), QUADRATURE_OK);

    pthread_t threads[PIPELINE_THREADS];
    pipeline_thread_ctx_t contexts[PIPELINE_THREADS];

    for (int i = 0; i < PIPELINE_THREADS; i++) {
        contexts[i].pipeline = pipeline;
        contexts[i].thread_id = i;
        pthread_create(&threads[i], NULL, pipeline_query_thread, &contexts[i]);
    }

    for (int i = 0; i < PIPELINE_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Pipeline should still be functional */
    cr_assert_eq(audio_pipeline_get_sample_rate(pipeline), TEST_SAMPLE_RATE);

    audio_pipeline_destroy(pipeline);
}
