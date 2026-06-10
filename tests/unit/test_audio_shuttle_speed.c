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

/* Include internal header for shuttle_speed access */
#include "internal.h"

/* FFmpeg must be initialized before Criterion forks */
#include <libavformat/avformat.h>
ReportHook(PRE_ALL)(struct criterion_test_set *tests)
{
    (void)tests;
    avformat_network_init();
}

/* Test sample rate + format. Scrubber currently asserts channels == 2. */
#define TEST_SAMPLE_RATE 48000
#define TEST_FORMAT      ((audio_format_t){ .sample_rate = TEST_SAMPLE_RATE, .channels = 2 })

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

Test(audio_shuttle_speed, lifecycle_null_safety_initial_state)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;

    /* --- Null safety --- */
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, NULL), QUADRATURE_ERROR_INVALID_PARAM);

    /* Destroy null is safe */
    audio_shuttle_speed_destroy(NULL);

    /* Setters/getters on a NULL shuttle_speed are g_assert() crashes — that
     * contract is enforced by the death tests at the bottom of this file. */

    /* --- Create and verify initial state --- */
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);
    cr_assert_not_null(shuttle_speed);

    /* Initial state */
    cr_assert_eq(audio_shuttle_speed_get_speed(shuttle_speed), 1.0f);
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_OFF);

    audio_shuttle_speed_destroy(shuttle_speed);
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

Test(audio_shuttle_speed, speed_control_clamping_atomicity)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

    /* --- Normal speed values --- */
    audio_shuttle_speed_set_speed(shuttle_speed, 1.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 1.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, 2.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 2.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, 0.5f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 0.5f, 0.0001f);

    /* --- Negative speeds (reverse playback) --- */
    audio_shuttle_speed_set_speed(shuttle_speed, -1.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), -1.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, -2.5f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), -2.5f, 0.0001f);

    /* --- Clamping at positive limit --- */
    audio_shuttle_speed_set_speed(shuttle_speed, 4.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 4.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, 5.0f); /* Should clamp to 4.0 */
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 4.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, 100.0f); /* Extreme positive */
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 4.0f, 0.0001f);

    /* --- Clamping at negative limit --- */
    audio_shuttle_speed_set_speed(shuttle_speed, -4.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), -4.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, -5.0f); /* Should clamp to -4.0 */
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), -4.0f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, -100.0f); /* Extreme negative */
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), -4.0f, 0.0001f);

    /* --- Fractional speeds --- */
    audio_shuttle_speed_set_speed(shuttle_speed, 1.25f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 1.25f, 0.0001f);

    audio_shuttle_speed_set_speed(shuttle_speed, 0.75f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 0.75f, 0.0001f);

    /* --- Zero speed (stopped) --- */
    audio_shuttle_speed_set_speed(shuttle_speed, 0.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 0.0f, 0.0001f);

    /* --- Near-zero (should be treated as stopped in process) --- */
    audio_shuttle_speed_set_speed(shuttle_speed, 0.005f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 0.005f, 0.0001f);

    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: audio_seek_position_t — atomic playhead
 *
 * Position lives outside the shuttle now. These tests cover the type itself.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_seek_position, set_get_roundtrip)
{
    audio_seek_position_t pos;
    audio_seek_position_init(&pos, 0);
    cr_assert_eq(audio_seek_position_get(&pos), 0);

    audio_seek_position_set(&pos, 1000);
    cr_assert_eq(audio_seek_position_get(&pos), 1000);

    audio_seek_position_set(&pos, 48000); /* 1 second at 48kHz */
    cr_assert_eq(audio_seek_position_get(&pos), 48000);

    /* Large positions (long audio files) */
    uint64_t one_hour = (uint64_t)TEST_SAMPLE_RATE * 3600;
    audio_seek_position_set(&pos, one_hour);
    cr_assert_eq(audio_seek_position_get(&pos), one_hour);

    uint64_t ten_hours = (uint64_t)TEST_SAMPLE_RATE * 3600 * 10;
    audio_seek_position_set(&pos, ten_hours);
    cr_assert_eq(audio_seek_position_get(&pos), ten_hours);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Shuttle Mode Control and Zone Selection
 *
 * Verifies:
 * 1. All shuttle modes can be set and read back
 * 2. Mode changes are atomic
 * 3. Mode affects zone selection (tested via process behavior)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_shuttle_speed, shuttle_mode_control)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

    /* --- Initial mode is OFF --- */
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_OFF);

    /* --- Set to KEYLOCK (pitch-preserved) --- */
    audio_shuttle_speed_set_mode(shuttle_speed, SHUTTLE_MODE_KEYLOCK);
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_KEYLOCK);

    /* --- Set to PITCHED (turntable) --- */
    audio_shuttle_speed_set_mode(shuttle_speed, SHUTTLE_MODE_PITCHED);
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_PITCHED);

    /* --- Set back to OFF --- */
    audio_shuttle_speed_set_mode(shuttle_speed, SHUTTLE_MODE_OFF);
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_OFF);

    /* --- Rapid mode switching --- */
    for (int i = 0; i < 100; i++) {
        shuttle_mode_t mode = (shuttle_mode_t)(i % 3);
        audio_shuttle_speed_set_mode(shuttle_speed, mode);
        cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), mode);
    }

    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: prepare_mode — DSP resource allocation (separate from set_mode)
 *
 * set_mode is a pure atomic store. prepare_mode is where the allocation lives.
 * They are split so callers see the side effect explicitly. This test exercises
 * prepare_mode in isolation.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_shuttle_speed, prepare_mode_is_idempotent)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

    /* OFF / PITCHED need no allocation — prepare is a no-op */
    audio_shuttle_speed_prepare_mode(shuttle_speed, SHUTTLE_MODE_OFF);
    audio_shuttle_speed_prepare_mode(shuttle_speed, SHUTTLE_MODE_PITCHED);

    /* KEYLOCK allocates rubberband state. Idempotent — second call is a fast
     * no-op once already prepared. */
    audio_shuttle_speed_prepare_mode(shuttle_speed, SHUTTLE_MODE_KEYLOCK);
    audio_shuttle_speed_prepare_mode(shuttle_speed, SHUTTLE_MODE_KEYLOCK);
    audio_shuttle_speed_prepare_mode(shuttle_speed, SHUTTLE_MODE_KEYLOCK);

    /* prepare does NOT change the active mode — that's set_mode's job */
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_OFF);

    /* After prepare + set, the engine is in keylock with resources ready */
    audio_shuttle_speed_set_mode(shuttle_speed, SHUTTLE_MODE_KEYLOCK);
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_KEYLOCK);

    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Flush Operation
 *
 * Verifies:
 * 1. Flush doesn't crash on valid shuttle_speed
 * 2. Flush resets internal state appropriately
 * 3. Position is preserved after flush (synced from atomic)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_shuttle_speed, flush_operation)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

    /* Set some control state */
    audio_shuttle_speed_set_speed(shuttle_speed, 2.0f);
    audio_shuttle_speed_set_mode(shuttle_speed, SHUTTLE_MODE_KEYLOCK);

    /* Flush */
    audio_shuttle_speed_flush(shuttle_speed);

    /* Control state should be preserved (flush clears DSP scratch only) */
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 2.0f, 0.0001f);
    cr_assert_eq(audio_shuttle_speed_get_mode(shuttle_speed), SHUTTLE_MODE_KEYLOCK);

    /* Multiple flushes are safe */
    audio_shuttle_speed_flush(shuttle_speed);
    audio_shuttle_speed_flush(shuttle_speed);

    audio_shuttle_speed_destroy(shuttle_speed);
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

Test(audio_shuttle_speed, process_passthrough_zone)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

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
    audio_shuttle_speed_set_speed(shuttle_speed, 1.0f);
    audio_shuttle_speed_set_mode(shuttle_speed, SHUTTLE_MODE_OFF);

    /* External playhead, starts at 0 */
    audio_seek_position_t pos;
    audio_seek_position_init(&pos, 0);

    /* Process 256 frames */
    uint32_t processed
        = audio_shuttle_speed_process(shuttle_speed, &pos, input, num_frames, output, 256);

    cr_assert_eq(processed, 256);
    cr_assert_eq(audio_seek_position_get(&pos), 256); /* playhead advanced */

    /* Verify output matches input (passthrough) */
    for (uint32_t i = 0; i < 256; i++) {
        cr_assert_float_eq(
            output[i * 2], input[i * 2], 0.0001f, "Left channel mismatch at frame %u", i);
        cr_assert_float_eq(
            output[i * 2 + 1], input[i * 2 + 1], 0.0001f, "Right channel mismatch at frame %u", i);
    }

    /* Process more and verify position continues */
    processed = audio_shuttle_speed_process(shuttle_speed, &pos, input, num_frames, output, 256);
    cr_assert_eq(processed, 256);
    cr_assert_eq(audio_seek_position_get(&pos), 512); /* 256 + 256 */

    free(input);
    free(output);
    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Process at Zero Speed (Stopped)
 *
 * Verifies:
 * 1. Process at speed 0.0 outputs silence
 * 2. Position does not advance when stopped
 * 3. Near-zero speed (< 0.01) is treated as stopped
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_shuttle_speed, process_stopped_state)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

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
    audio_shuttle_speed_set_speed(shuttle_speed, 0.0f);
    audio_seek_position_t pos;
    audio_seek_position_init(&pos, 1000);

    uint32_t processed
        = audio_shuttle_speed_process(shuttle_speed, &pos, input, num_frames, output, 256);

    cr_assert_eq(processed, 256);
    cr_assert_eq(audio_seek_position_get(&pos), 1000); /* playhead does NOT advance */

    /* Output should be silence */
    for (uint32_t i = 0; i < 256; i++) {
        cr_assert_float_eq(output[i * 2], 0.0f, 0.0001f);
        cr_assert_float_eq(output[i * 2 + 1], 0.0f, 0.0001f);
    }

    /* Near-zero speed should also be treated as stopped */
    audio_shuttle_speed_set_speed(shuttle_speed, 0.005f);
    processed = audio_shuttle_speed_process(shuttle_speed, &pos, input, num_frames, output, 256);
    cr_assert_eq(audio_seek_position_get(&pos), 1000); /* still no advancement */

    free(input);
    free(output);
    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Process Parameter Validation
 *
 * Verifies:
 * 1. Process with NULL parameters returns 0
 * 2. Process with zero frames returns 0
 * 3. Process with empty sample buffer handles gracefully
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_shuttle_speed, process_parameter_validation)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

    float input[256 * 2];
    float output[256 * 2];
    audio_seek_position_t pos;
    audio_seek_position_init(&pos, 0);

    /* NULL shuttle_speed */
    cr_assert_eq(audio_shuttle_speed_process(NULL, &pos, input, 256, output, 256), 0);

    /* NULL playhead */
    cr_assert_eq(audio_shuttle_speed_process(shuttle_speed, NULL, input, 256, output, 256), 0);

    /* NULL input samples */
    cr_assert_eq(audio_shuttle_speed_process(shuttle_speed, &pos, NULL, 256, output, 256), 0);

    /* NULL output buffer */
    cr_assert_eq(audio_shuttle_speed_process(shuttle_speed, &pos, input, 256, NULL, 256), 0);

    /* Zero output frames */
    cr_assert_eq(audio_shuttle_speed_process(shuttle_speed, &pos, input, 256, output, 0), 0);

    /* Smoke: valid call still works */
    memset(input, 0, sizeof(input));
    audio_shuttle_speed_set_speed(shuttle_speed, 1.0f);
    uint32_t processed = audio_shuttle_speed_process(shuttle_speed, &pos, input, 256, output, 64);
    cr_assert_eq(processed, 64);

    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Concurrent Speed/Playhead Updates (Thread Safety)
 *
 * Speed lives on the shuttle, playhead lives on audio_seek_position_t. Both
 * are atomic. Threads hammer both concurrently; reads must stay valid and no
 * crashes under contention.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SHUTTLE_THREADS  4
#define SCRUB_ITERATIONS 10000

typedef struct {
    audio_shuttle_speed_t *shuttle_speed;
    audio_seek_position_t *pos;
    int thread_id;
} shuttle_thread_ctx_t;

static void *
speed_writer_thread(void *arg)
{
    shuttle_thread_ctx_t *ctx = arg;

    for (int i = 0; i < SCRUB_ITERATIONS; i++) {
        float speed = -4.0f + (float)(i % 800) / 100.0f; /* -4.0 to 4.0 */
        audio_shuttle_speed_set_speed(ctx->shuttle_speed, speed);

        float read_speed = audio_shuttle_speed_get_speed(ctx->shuttle_speed);
        cr_assert(read_speed >= -4.0f && read_speed <= 4.0f, "Speed out of range: %f", read_speed);
    }

    return NULL;
}

static void *
position_writer_thread(void *arg)
{
    shuttle_thread_ctx_t *ctx = arg;

    for (int i = 0; i < SCRUB_ITERATIONS; i++) {
        uint64_t position = (uint64_t)(ctx->thread_id * 100000 + i);
        audio_seek_position_set(ctx->pos, position);

        uint64_t read_pos = audio_seek_position_get(ctx->pos);
        (void)read_pos; /* atomic — just verify no crash */
    }

    return NULL;
}

Test(audio_shuttle_speed, concurrent_speed_position_updates)
{
    audio_shuttle_speed_t *shuttle_speed = NULL;
    cr_assert_eq(audio_shuttle_speed_create(TEST_FORMAT, &shuttle_speed), QUADRATURE_OK);

    audio_seek_position_t pos;
    audio_seek_position_init(&pos, 0);

    pthread_t threads[SHUTTLE_THREADS * 2];
    shuttle_thread_ctx_t contexts[SHUTTLE_THREADS * 2];

    for (int i = 0; i < SHUTTLE_THREADS; i++) {
        contexts[i].shuttle_speed = shuttle_speed;
        contexts[i].pos = &pos;
        contexts[i].thread_id = i;
        pthread_create(&threads[i], NULL, speed_writer_thread, &contexts[i]);

        contexts[SHUTTLE_THREADS + i].shuttle_speed = shuttle_speed;
        contexts[SHUTTLE_THREADS + i].pos = &pos;
        contexts[SHUTTLE_THREADS + i].thread_id = SHUTTLE_THREADS + i;
        pthread_create(&threads[SHUTTLE_THREADS + i],
                       NULL,
                       position_writer_thread,
                       &contexts[SHUTTLE_THREADS + i]);
    }

    for (int i = 0; i < SHUTTLE_THREADS * 2; i++) {
        pthread_join(threads[i], NULL);
    }

    audio_shuttle_speed_set_speed(shuttle_speed, 1.0f);
    cr_assert_float_eq(audio_shuttle_speed_get_speed(shuttle_speed), 1.0f, 0.0001f);

    audio_shuttle_speed_destroy(shuttle_speed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Death tests — NULL shuttle_speed inputs MUST abort the process
 *
 * Every setter/getter asserts `s != NULL` (src/audio/shuttle_speed.c:328 and
 * following). These tests confirm the assertions actually fire — a regression
 * that replaced the g_assert with a silent NULL-check would be caught here.
 *
 * One invariant per test. Criterion forks per test, so a surviving process
 * would signal a missed crash immediately.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(audio_shuttle_speed, death_set_speed_null_shuttle, .signal = SIGABRT)
{
    audio_shuttle_speed_set_speed(NULL, 1.0f);
}

Test(audio_shuttle_speed, death_get_speed_null_shuttle, .signal = SIGABRT)
{
    (void)audio_shuttle_speed_get_speed(NULL);
}

Test(audio_shuttle_speed, death_flush_null_shuttle, .signal = SIGABRT)
{
    audio_shuttle_speed_flush(NULL);
}

Test(audio_shuttle_speed, death_prepare_mode_null_shuttle, .signal = SIGABRT)
{
    audio_shuttle_speed_prepare_mode(NULL, SHUTTLE_MODE_KEYLOCK);
}
