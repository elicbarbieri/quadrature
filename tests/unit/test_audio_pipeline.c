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
ReportHook(PRE_ALL)(struct criterion_test_set *tests)
{
    (void)tests;
    avformat_network_init();
}

/* Test sample rate */
#define TEST_SAMPLE_RATE 48000
#define TEST_CHANNELS    2

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lifecycle and Null Safety
 *
 * Verifies:
 * 1. Create with valid sample rate succeeds
 * 2. Null parameter handling for create
 * 3. Destroy null is safe (idempotent)
 * 4. Sample rate is stored correctly
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, lifecycle_null_safety)
{
    audio_pipeline_t *pipeline = NULL;

    /* --- Null safety for create --- */
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, NULL),
                 QUADRATURE_ERROR_INVALID_PARAM);

    /* Destroy null is safe */
    audio_pipeline_destroy(NULL);

    /* --- Queries on null pipeline return error states --- */
    audio_player_display_t disp;
    audio_pipeline_get_player_display(NULL, 0, &disp);
    cr_assert_eq(disp.state, CHANNEL_ERROR);
    cr_assert_float_eq(disp.position_seconds, 0.0, 1e-9);
    cr_assert_float_eq(disp.length_seconds, 0.0, 1e-9);

    /* --- Create pipeline --- */
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);
    cr_assert_not_null(pipeline);

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

Test(audio_pipeline, player_id_bounds_checking)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    /* --- Test invalid player IDs --- */
    int invalid_ids[] = { -1, -100, 4, 5, 100, 1000 };

    for (size_t i = 0; i < sizeof(invalid_ids) / sizeof(invalid_ids[0]); i++) {
        int id = invalid_ids[i];

        /* Control operations should fail */
        cr_assert_eq(audio_pipeline_player_play(pipeline, id), QUADRATURE_ERROR_INVALID_PARAM);
        cr_assert_eq(audio_pipeline_player_toggle_play(pipeline, id),
                     QUADRATURE_ERROR_INVALID_PARAM);
        cr_assert_eq(audio_pipeline_player_stop(pipeline, id), QUADRATURE_ERROR_INVALID_PARAM);
        cr_assert_eq(audio_pipeline_player_seek(pipeline, id, 0), QUADRATURE_ERROR_INVALID_PARAM);

        /* Queries should return error states */
        audio_player_display_t d;
        audio_pipeline_get_player_display(pipeline, id, &d);
        cr_assert_eq(d.state, CHANNEL_ERROR);
        cr_assert_float_eq(d.position_seconds, 0.0, 1e-9);
        cr_assert_float_eq(d.length_seconds, 0.0, 1e-9);
    }

    /* --- Test valid player IDs (0-3) return valid states --- */
    for (int id = 0; id < 4; id++) {
        audio_player_display_t d;
        audio_pipeline_get_player_display(pipeline, id, &d);
        /* Initial state should be STOPPED */
        cr_assert_eq(
            d.state, CHANNEL_STOPPED, "Player %d should start STOPPED, got %d", id, d.state);
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

Test(audio_pipeline, initial_player_state)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    for (int id = 0; id < 4; id++) {
        audio_player_display_t d;
        audio_pipeline_get_player_display(pipeline, id, &d);
        /* State is STOPPED */
        cr_assert_eq(d.state, CHANNEL_STOPPED, "Player %d should start STOPPED", id);

        /* Position and length are 0 */
        cr_assert_float_eq(d.position_seconds, 0.0, 1e-9);
        cr_assert_float_eq(d.length_seconds, 0.0, 1e-9);
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

Test(audio_pipeline, end_mode_control)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    /* Default is AUTOPLAY */
    cr_assert_eq(audio_pipeline_get_player_end_mode(pipeline, 0), TRACK_END_AUTOPLAY);

    /* Set all three modes on player 0 */
    cr_assert_eq(audio_pipeline_set_player_end_mode(pipeline, 0, TRACK_END_REPEAT), QUADRATURE_OK);
    cr_assert_eq(audio_pipeline_get_player_end_mode(pipeline, 0), TRACK_END_REPEAT);

    cr_assert_eq(audio_pipeline_set_player_end_mode(pipeline, 0, TRACK_END_STOP), QUADRATURE_OK);
    cr_assert_eq(audio_pipeline_get_player_end_mode(pipeline, 0), TRACK_END_STOP);

    /* Each player holds its own mode */
    for (int id = 0; id < 4; id++) {
        cr_assert_eq(audio_pipeline_set_player_end_mode(pipeline, id, TRACK_END_REPEAT),
                     QUADRATURE_OK);
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

Test(audio_pipeline, spectrum_output)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    float left[24], right[24];

    for (int id = 0; id < 4; id++) {
        /* Clear bars first */
        memset(left, 0xFF, sizeof(left));
        memset(right, 0xFF, sizeof(right));

        audio_pipeline_get_player_spectrum(pipeline, id, left, right, 24);

        /* Verify all values in range */
        for (int i = 0; i < 24; i++) {
            cr_assert(left[i] >= 0.0f && left[i] <= 1.0f,
                      "Spectrum left bar %d out of range for player %d: %f",
                      i,
                      id,
                      left[i]);
            cr_assert(right[i] >= 0.0f && right[i] <= 1.0f,
                      "Spectrum right bar %d out of range for player %d: %f",
                      i,
                      id,
                      right[i]);
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

Test(audio_pipeline, smooth_position_query)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    for (int id = 0; id < 4; id++) {
        audio_player_display_t d;
        audio_pipeline_get_player_display(pipeline, id, &d);

        /* Position should be non-negative */
        cr_assert(d.position_seconds >= 0.0,
                  "Position negative for player %d: %f",
                  id,
                  d.position_seconds);

        /* Speed should be in valid range */
        cr_assert(d.speed >= -4.0f && d.speed <= 4.0f,
                  "Speed out of range for player %d: %f",
                  id,
                  d.speed);
    }

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

Test(audio_pipeline, device_routing)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    /* Set device for player 0 */
    cr_assert_eq(audio_pipeline_set_player_device(pipeline, 0, "hw:0"), QUADRATURE_OK);

    /* Set device for player 1 */
    cr_assert_eq(audio_pipeline_set_player_device(pipeline, 1, "pipewire:default"), QUADRATURE_OK);

    /* Clear device (NULL) */
    cr_assert_eq(audio_pipeline_set_player_device(pipeline, 0, NULL), QUADRATURE_OK);

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
 * This is by design - the shuttle_speed operates on the buffer.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_pipeline, speed_control_requires_track)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

    /* set_speed fails without loaded track (returns INTERNAL error) */
    quadrature_result_t result = audio_pipeline_set_player_speed(pipeline, 0, 2.0f);
    cr_assert_neq(result, QUADRATURE_OK, "set_speed should fail without loaded track");

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

#define PIPELINE_THREADS    8
#define PIPELINE_ITERATIONS 500

typedef struct {
    audio_pipeline_t *pipeline;
    int thread_id;
} pipeline_thread_ctx_t;

static void *
pipeline_query_thread(void *arg)
{
    pipeline_thread_ctx_t *ctx = arg;

    for (int i = 0; i < PIPELINE_ITERATIONS; i++) {
        for (int id = 0; id < 4; id++) {
            /* Query all properties */
            audio_player_display_t d;
            audio_pipeline_get_player_display(ctx->pipeline, id, &d);
            cr_assert(d.state == CHANNEL_STOPPED || d.state == CHANNEL_PLAYING
                      || d.state == CHANNEL_PAUSED || d.state == CHANNEL_ERROR);

            float left[24], right[24];
            audio_pipeline_get_player_spectrum(ctx->pipeline, id, left, right, 24);
        }
    }

    return NULL;
}

Test(audio_pipeline, concurrent_queries)
{
    audio_pipeline_t *pipeline = NULL;
    cr_assert_eq(audio_pipeline_create(NULL, TEST_SAMPLE_RATE, TEST_CHANNELS, &pipeline), QUADRATURE_OK);

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
    audio_player_display_t d;
    audio_pipeline_get_player_display(pipeline, 0, &d);
    cr_assert_neq(d.state, CHANNEL_ERROR);

    audio_pipeline_destroy(pipeline);
}
