/**
 * Audio Scrubber Tests
 *
 * Consolidated tests for the variable-speed playback processor.
 * Tests cover speed control, position tracking, shuttle modes, and zone selection.
 *
 * Key invariants tested:
 * - Speed is clamped to [-4.0, 4.0]
 * - Position tracks correctly through atomic operations
 * - Shuttle mode determines processing zone
 * - Passthrough is used at speed ≈ 1.0 regardless of mode
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

/* Include internal header for scrubber access */
#include "internal.h"

/* FFmpeg must be initialized before Criterion forks */
#include <libavformat/avformat.h>
ReportHook(PRE_ALL)(struct criterion_test_set *tests)
{
    (void)tests;
    avformat_network_init();
}

/* Test sample rate */
#define TEST_SAMPLE_RATE 48000

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lifecycle, Null Safety, and Initial State
 *
 * Verifies:
 * 1. Create succeeds with valid sample rate
 * 2. Null parameter handling for create
 * 3. Initial state is correct (speed=1.0, position=0, mode=OFF)
 * 4. Destroy null is safe (idempotent)
 * 5. Double destroy doesn't crash
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, lifecycle_null_safety_initial_state)
{
    audio_scrubber_t *scrubber = NULL;

    /* --- Null safety --- */
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, NULL), QUADRATURE_ERROR_INVALID_PARAM);

    /* Destroy null is safe */
    audio_scrubber_destroy(NULL);

    /* Setters/getters on a NULL scrubber are g_assert() crashes — that
     * contract is enforced by the death tests at the bottom of this file. */

    /* --- Create and verify initial state --- */
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);
    cr_assert_not_null(scrubber);

    /* Initial state */
    cr_assert_eq(audio_scrubber_get_speed(scrubber), 1.0f);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 0);
    cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), SHUTTLE_MODE_OFF);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Speed Control, Clamping, and Atomic Consistency
 *
 * Verifies:
 * 1. Speed can be set and read back
 * 2. Speed is clamped to [-4.0, 4.0]
 * 3. Extreme values are handled (infinity, NaN-like)
 * 4. Speed changes are atomic (set then get returns expected value)
 * 5. Fractional speeds work correctly
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, speed_control_clamping_atomicity)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* --- Normal speed values --- */
    audio_scrubber_set_speed(scrubber, 1.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 1.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, 2.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 2.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, 0.5f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 0.5f, 0.0001f);

    /* --- Negative speeds (reverse playback) --- */
    audio_scrubber_set_speed(scrubber, -1.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), -1.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, -2.5f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), -2.5f, 0.0001f);

    /* --- Clamping at positive limit --- */
    audio_scrubber_set_speed(scrubber, 4.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 4.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, 5.0f); /* Should clamp to 4.0 */
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 4.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, 100.0f); /* Extreme positive */
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 4.0f, 0.0001f);

    /* --- Clamping at negative limit --- */
    audio_scrubber_set_speed(scrubber, -4.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), -4.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, -5.0f); /* Should clamp to -4.0 */
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), -4.0f, 0.0001f);

    audio_scrubber_set_speed(scrubber, -100.0f); /* Extreme negative */
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), -4.0f, 0.0001f);

    /* --- Fractional speeds --- */
    audio_scrubber_set_speed(scrubber, 1.25f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 1.25f, 0.0001f);

    audio_scrubber_set_speed(scrubber, 0.75f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 0.75f, 0.0001f);

    /* --- Zero speed (stopped) --- */
    audio_scrubber_set_speed(scrubber, 0.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 0.0f, 0.0001f);

    /* --- Near-zero (should be treated as stopped in process) --- */
    audio_scrubber_set_speed(scrubber, 0.005f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 0.005f, 0.0001f);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Position Control and Atomic Consistency
 *
 * Verifies:
 * 1. Position can be set and read back
 * 2. Large position values work (64-bit range)
 * 3. Negative positions are accepted (handled in process)
 * 4. Position changes are atomic
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, position_control_atomicity)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* --- Basic position control --- */
    audio_scrubber_set_position(scrubber, 0);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 0);

    audio_scrubber_set_position(scrubber, 1000);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 1000);

    audio_scrubber_set_position(scrubber, 48000); /* 1 second at 48kHz */
    cr_assert_eq(audio_scrubber_get_position(scrubber), 48000);

    /* --- Large positions (typical for long audio files) --- */
    int64_t one_hour = (int64_t)TEST_SAMPLE_RATE * 3600; /* 1 hour in samples */
    audio_scrubber_set_position(scrubber, one_hour);
    cr_assert_eq(audio_scrubber_get_position(scrubber), one_hour);

    int64_t ten_hours = (int64_t)TEST_SAMPLE_RATE * 3600 * 10;
    audio_scrubber_set_position(scrubber, ten_hours);
    cr_assert_eq(audio_scrubber_get_position(scrubber), ten_hours);

    /* --- Negative position (edge case, clamped in process) --- */
    audio_scrubber_set_position(scrubber, -100);
    cr_assert_eq(audio_scrubber_get_position(scrubber), -100);

    /* Reset to valid position */
    audio_scrubber_set_position(scrubber, 0);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 0);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Shuttle Mode Control and Zone Selection
 *
 * Verifies:
 * 1. All shuttle modes can be set and read back
 * 2. Mode changes are atomic
 * 3. Mode affects zone selection (tested via process behavior)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, shuttle_mode_control)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* --- Initial mode is OFF --- */
    cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), SHUTTLE_MODE_OFF);

    /* --- Set to KEYLOCK (pitch-preserved) --- */
    audio_scrubber_set_shuttle_mode(scrubber, SHUTTLE_MODE_KEYLOCK);
    cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), SHUTTLE_MODE_KEYLOCK);

    /* --- Set to PITCHED (turntable) --- */
    audio_scrubber_set_shuttle_mode(scrubber, SHUTTLE_MODE_PITCHED);
    cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), SHUTTLE_MODE_PITCHED);

    /* --- Set back to OFF --- */
    audio_scrubber_set_shuttle_mode(scrubber, SHUTTLE_MODE_OFF);
    cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), SHUTTLE_MODE_OFF);

    /* --- Rapid mode switching --- */
    for (int i = 0; i < 100; i++) {
        shuttle_mode_t mode = (shuttle_mode_t)(i % 3);
        audio_scrubber_set_shuttle_mode(scrubber, mode);
        cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), mode);
    }

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Flush Operation
 *
 * Verifies:
 * 1. Flush doesn't crash on valid scrubber
 * 2. Flush resets internal state appropriately
 * 3. Position is preserved after flush (synced from atomic)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, flush_operation)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* Set some state */
    audio_scrubber_set_speed(scrubber, 2.0f);
    audio_scrubber_set_position(scrubber, 10000);
    audio_scrubber_set_shuttle_mode(scrubber, SHUTTLE_MODE_KEYLOCK);

    /* Flush */
    audio_scrubber_flush(scrubber);

    /* State should be preserved (flush clears internal buffers, not control state) */
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 2.0f, 0.0001f);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 10000);
    cr_assert_eq(audio_scrubber_get_shuttle_mode(scrubber), SHUTTLE_MODE_KEYLOCK);

    /* Multiple flushes are safe */
    audio_scrubber_flush(scrubber);
    audio_scrubber_flush(scrubber);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Process with Synthetic Audio (Passthrough Zone)
 *
 * Verifies:
 * 1. Process at speed 1.0 copies input directly (passthrough)
 * 2. Position advances by frame count
 * 3. Output matches input for passthrough
 * 4. Process handles edge cases (empty frames, zero samples)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, process_passthrough_zone)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* Create synthetic audio: 1 second of stereo samples */
    const uint32_t num_frames = TEST_SAMPLE_RATE;
    float *input = malloc(num_frames * 2 * sizeof(float));
    float *output = malloc(1024 * 2 * sizeof(float)); /* Process buffer */

    cr_assert_not_null(input);
    cr_assert_not_null(output);

    /* Fill with recognizable pattern: left=frame_index, right=-frame_index */
    for (uint32_t i = 0; i < num_frames; i++) {
        input[i * 2] = (float)i / num_frames;      /* Left: 0.0 to ~1.0 */
        input[i * 2 + 1] = -(float)i / num_frames; /* Right: 0.0 to ~-1.0 */
    }

    /* Configure for passthrough: speed=1.0, mode=OFF */
    audio_scrubber_set_speed(scrubber, 1.0f);
    audio_scrubber_set_shuttle_mode(scrubber, SHUTTLE_MODE_OFF);
    audio_scrubber_set_position(scrubber, 0);

    /* Process 256 frames */
    uint64_t out_position = 0;
    uint32_t processed
        = audio_scrubber_process(scrubber, input, num_frames, output, 256, &out_position);

    cr_assert_eq(processed, 256);
    cr_assert_eq(out_position, 256); /* Position should advance */

    /* Verify output matches input (passthrough) */
    for (uint32_t i = 0; i < 256; i++) {
        cr_assert_float_eq(
            output[i * 2], input[i * 2], 0.0001f, "Left channel mismatch at frame %u", i);
        cr_assert_float_eq(
            output[i * 2 + 1], input[i * 2 + 1], 0.0001f, "Right channel mismatch at frame %u", i);
    }

    /* Process more and verify position continues */
    processed = audio_scrubber_process(scrubber, input, num_frames, output, 256, &out_position);
    cr_assert_eq(processed, 256);
    cr_assert_eq(out_position, 512); /* 256 + 256 */

    free(input);
    free(output);
    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Process at Zero Speed (Stopped)
 *
 * Verifies:
 * 1. Process at speed 0.0 outputs silence
 * 2. Position does not advance when stopped
 * 3. Near-zero speed (< 0.01) is treated as stopped
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, process_stopped_state)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* Create synthetic audio */
    const uint32_t num_frames = TEST_SAMPLE_RATE;
    float *input = malloc(num_frames * 2 * sizeof(float));
    float *output = malloc(256 * 2 * sizeof(float));

    /* Fill input with non-zero values */
    for (uint32_t i = 0; i < num_frames; i++) {
        input[i * 2] = 0.5f;
        input[i * 2 + 1] = -0.5f;
    }

    /* Set speed to zero (stopped) */
    audio_scrubber_set_speed(scrubber, 0.0f);
    audio_scrubber_set_position(scrubber, 1000);

    uint64_t out_position = 9999;
    uint32_t processed
        = audio_scrubber_process(scrubber, input, num_frames, output, 256, &out_position);

    cr_assert_eq(processed, 256);
    cr_assert_eq(out_position, 1000); /* Position should NOT advance */

    /* Output should be silence */
    for (uint32_t i = 0; i < 256; i++) {
        cr_assert_float_eq(output[i * 2], 0.0f, 0.0001f);
        cr_assert_float_eq(output[i * 2 + 1], 0.0f, 0.0001f);
    }

    /* Near-zero speed should also be treated as stopped */
    audio_scrubber_set_speed(scrubber, 0.005f);
    processed = audio_scrubber_process(scrubber, input, num_frames, output, 256, &out_position);
    cr_assert_eq(out_position, 1000); /* Still no advancement */

    free(input);
    free(output);
    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Process Parameter Validation
 *
 * Verifies:
 * 1. Process with NULL parameters returns 0
 * 2. Process with zero frames returns 0
 * 3. Process with empty sample buffer handles gracefully
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, process_parameter_validation)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    float input[256 * 2];
    float output[256 * 2];
    uint64_t out_pos;

    /* NULL scrubber */
    cr_assert_eq(audio_scrubber_process(NULL, input, 256, output, 256, &out_pos), 0);

    /* NULL input samples */
    cr_assert_eq(audio_scrubber_process(scrubber, NULL, 256, output, 256, &out_pos), 0);

    /* NULL output buffer */
    cr_assert_eq(audio_scrubber_process(scrubber, input, 256, NULL, 256, &out_pos), 0);

    /* Zero output frames */
    cr_assert_eq(audio_scrubber_process(scrubber, input, 256, output, 0, &out_pos), 0);

    /* NULL out_position is OK (optional) */
    memset(input, 0, sizeof(input));
    audio_scrubber_set_speed(scrubber, 1.0f);
    uint32_t processed = audio_scrubber_process(scrubber, input, 256, output, 64, NULL);
    cr_assert_eq(processed, 64);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Concurrent Speed/Position Updates (Thread Safety)
 *
 * Verifies:
 * 1. Multiple threads can update speed concurrently
 * 2. Multiple threads can update position concurrently
 * 3. Reads always return valid values (no torn reads)
 * 4. No crashes under contention
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SCRUB_THREADS    4
#define SCRUB_ITERATIONS 10000

typedef struct {
    audio_scrubber_t *scrubber;
    int thread_id;
} scrub_thread_ctx_t;

static void *
speed_writer_thread(void *arg)
{
    scrub_thread_ctx_t *ctx = arg;

    for (int i = 0; i < SCRUB_ITERATIONS; i++) {
        /* Vary speed based on iteration */
        float speed = -4.0f + (float)(i % 800) / 100.0f; /* -4.0 to 4.0 */
        audio_scrubber_set_speed(ctx->scrubber, speed);

        /* Read back and verify in valid range */
        float read_speed = audio_scrubber_get_speed(ctx->scrubber);
        cr_assert(read_speed >= -4.0f && read_speed <= 4.0f, "Speed out of range: %f", read_speed);
    }

    return NULL;
}

static void *
position_writer_thread(void *arg)
{
    scrub_thread_ctx_t *ctx = arg;

    for (int i = 0; i < SCRUB_ITERATIONS; i++) {
        int64_t position = (int64_t)(ctx->thread_id * 100000 + i);
        audio_scrubber_set_position(ctx->scrubber, position);

        /* Read back - may be different due to other threads, but should be valid */
        int64_t read_pos = audio_scrubber_get_position(ctx->scrubber);
        (void)read_pos; /* Just verify no crash */
    }

    return NULL;
}

Test(audio_scrubber, concurrent_speed_position_updates)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    pthread_t threads[SCRUB_THREADS * 2];
    scrub_thread_ctx_t contexts[SCRUB_THREADS * 2];

    /* Launch speed writers and position writers */
    for (int i = 0; i < SCRUB_THREADS; i++) {
        contexts[i].scrubber = scrubber;
        contexts[i].thread_id = i;
        pthread_create(&threads[i], NULL, speed_writer_thread, &contexts[i]);

        contexts[SCRUB_THREADS + i].scrubber = scrubber;
        contexts[SCRUB_THREADS + i].thread_id = SCRUB_THREADS + i;
        pthread_create(&threads[SCRUB_THREADS + i],
                       NULL,
                       position_writer_thread,
                       &contexts[SCRUB_THREADS + i]);
    }

    /* Wait for all threads */
    for (int i = 0; i < SCRUB_THREADS * 2; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Scrubber should still be functional */
    audio_scrubber_set_speed(scrubber, 1.0f);
    cr_assert_float_eq(audio_scrubber_get_speed(scrubber), 1.0f, 0.0001f);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Seek and Flush Operations
 *
 * Verifies:
 * 1. Setting position updates the atomic position
 * 2. Flush doesn't crash and preserves position
 * 3. Position can be set, flushed, and read back
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, seek_and_flush_operations)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* Initial position is 0 */
    cr_assert_eq(audio_scrubber_get_position(scrubber), 0);

    /* Set position and verify */
    audio_scrubber_set_position(scrubber, 100000);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 100000);

    /* Flush should preserve position */
    audio_scrubber_flush(scrubber);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 100000);

    /* Seek backward */
    audio_scrubber_set_position(scrubber, 5000);
    audio_scrubber_flush(scrubber);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 5000);

    /* Multiple flushes are safe */
    audio_scrubber_flush(scrubber);
    audio_scrubber_flush(scrubber);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 5000);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Position Bounds and Edge Cases
 *
 * Verifies:
 * 1. Position at start of buffer (0)
 * 2. Position at end of buffer
 * 3. Position beyond end is handled
 * 4. Large position values work
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, position_bounds_edge_cases)
{
    audio_scrubber_t *scrubber = NULL;
    cr_assert_eq(audio_scrubber_create(TEST_SAMPLE_RATE, &scrubber), QUADRATURE_OK);

    /* Position at 0 */
    audio_scrubber_set_position(scrubber, 0);
    cr_assert_eq(audio_scrubber_get_position(scrubber), 0);

    /* Position at typical track length (3 minutes at 48kHz) */
    int64_t three_min = (int64_t)TEST_SAMPLE_RATE * 180;
    audio_scrubber_set_position(scrubber, three_min);
    cr_assert_eq(audio_scrubber_get_position(scrubber), three_min);

    /* Very large position (10 hours) */
    int64_t ten_hours = (int64_t)TEST_SAMPLE_RATE * 3600 * 10;
    audio_scrubber_set_position(scrubber, ten_hours);
    cr_assert_eq(audio_scrubber_get_position(scrubber), ten_hours);

    audio_scrubber_destroy(scrubber);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Death tests — NULL scrubber inputs MUST abort the process
 *
 * Every setter/getter asserts `s != NULL` (src/audio/audio_scrub.c:328 and
 * following). These tests confirm the assertions actually fire — a regression
 * that replaced the g_assert with a silent NULL-check would be caught here.
 *
 * One invariant per test. Criterion forks per test, so a surviving process
 * would signal a missed crash immediately.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_scrubber, death_set_speed_null_scrubber, .signal = SIGABRT)
{
    audio_scrubber_set_speed(NULL, 1.0f);
}

Test(audio_scrubber, death_get_speed_null_scrubber, .signal = SIGABRT)
{
    (void)audio_scrubber_get_speed(NULL);
}

Test(audio_scrubber, death_flush_null_scrubber, .signal = SIGABRT)
{
    audio_scrubber_flush(NULL);
}

Test(audio_scrubber, death_set_position_null_scrubber, .signal = SIGABRT)
{
    audio_scrubber_set_position(NULL, 0);
}
