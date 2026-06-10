/**
 * Quadrature Rate Processor
 *
 * Variable-speed playback with user-controlled mode selection:
 *   - OFF (passthrough): direct copy, zero CPU
 *   - PITCHED (turntable): pitch shifts with speed, like vinyl
 *   - KEYLOCK (rubberband): pitch-preserved time stretch
 *
 * Mode is an atomic shuttle_mode_t; a short crossfade masks switches.
 *
 * ## Rubberband Integration
 *
 * Key implementation details for artifact-free rubberband processing:
 *   - Output ring buffer: RB doesn't produce 1:1 sample ratios
 *   - Start padding: Feed silence via getPreferredStartPad() on init
 *   - Start delay: Discard initial samples via getStartDelay()
 *   - RT safety: Call setMaxProcessSize() before first process
 *   - Options: TransientsMixed + SmoothingOn + WindowShort for quality
 */

#include "internal.h"
#include <rubberband/rubberband-c.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define WORK_BUFFER_FRAMES  4096 /* Pre-allocated buffer size */
#define RB_RING_BUFFER_SIZE 8192 /* Output ring buffer for rubberband */

/* Speed limits per mode */
#define RUBBERBAND_MIN 0.5f /* Pitch-preserved mode min */
#define RUBBERBAND_MAX 4.0f /* Pitch-preserved mode max */

/* Crossfade smooths mode transitions (prevents clicks) */
#define CROSSFADE_MS         10 /* 10ms crossfade */
#define CROSSFADE_MIN_FRAMES 64 /* Clamp so very low sample rates still fade */

/* ═══════════════════════════════════════════════════════════════════════════
 * Simple Ring Buffer for Rubberband Output
 *
 * Single-producer single-consumer lock-free ring buffer.
 * Only used within audio thread, so no atomics needed.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    float *buffer;      /* Interleaved samples (channels set at init) */
    uint32_t capacity;  /* Total frames capacity */
    uint32_t channels;  /* Channels per frame (interleave stride) */
    uint32_t write_idx; /* Write position (frames) */
    uint32_t read_idx;  /* Read position (frames) */
    uint32_t count;     /* Frames currently in buffer */
} rb_ring_t;

static void
rb_ring_init(rb_ring_t *r, uint32_t capacity, uint32_t channels)
{
    r->buffer = calloc((size_t)capacity * channels, sizeof(float));
    r->capacity = capacity;
    r->channels = channels;
    r->write_idx = 0;
    r->read_idx = 0;
    r->count = 0;
}

static void
rb_ring_free(rb_ring_t *r)
{
    free(r->buffer);
    r->buffer = NULL;
}

static void
rb_ring_clear(rb_ring_t *r)
{
    r->write_idx = 0;
    r->read_idx = 0;
    r->count = 0;
}

static uint32_t
rb_ring_available(const rb_ring_t *r)
{
    return r->count;
}

static uint32_t
rb_ring_space(const rb_ring_t *r)
{
    return r->capacity - r->count;
}

static void
rb_ring_write(rb_ring_t *r, const float *data, uint32_t frames)
{
    if (frames > rb_ring_space(r))
        frames = rb_ring_space(r);
    if (frames == 0)
        return;

    const uint32_t ch = r->channels;
    const size_t fsz = (size_t)ch * sizeof(float);

    /* Split into at most two memcpy's — libc memcpy uses best available SIMD */
    uint32_t first = r->capacity - r->write_idx;
    if (first > frames)
        first = frames;
    memcpy(&r->buffer[(size_t)r->write_idx * ch], data, first * fsz);
    if (frames > first)
        memcpy(r->buffer, data + (size_t)first * ch, (frames - first) * fsz);
    r->write_idx = (r->write_idx + frames) % r->capacity;
    r->count += frames;
}

static uint32_t
rb_ring_read(rb_ring_t *r, float *data, uint32_t frames)
{
    if (frames > r->count)
        frames = r->count;
    if (frames == 0)
        return 0;

    const uint32_t ch = r->channels;
    const size_t fsz = (size_t)ch * sizeof(float);

    uint32_t first = r->capacity - r->read_idx;
    if (first > frames)
        first = frames;
    memcpy(data, &r->buffer[(size_t)r->read_idx * ch], first * fsz);
    if (frames > first)
        memcpy(data + (size_t)first * ch, r->buffer, (frames - first) * fsz);
    r->read_idx = (r->read_idx + frames) % r->capacity;
    r->count -= frames;
    return frames;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rate Processor Structure
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_shuttle_speed {
    /* Control state (UI thread writes, audio thread reads) */
    _Atomic float speed;      /* Playback speed: 1.0 = normal */
    _Atomic int shuttle_mode; /* shuttle_mode_t: OFF, KEYLOCK, PITCHED */

    /* Playhead is now external — see audio_seek_position_t passed to
     * audio_shuttle_speed_process(). The engine reads it at process entry
     * and writes back as it consumes samples. */

    /* Rubberband state (pre-allocated on mode change, NOT in RT callback) */
    RubberBandState rb_state;
    float **rb_input;  /* Deinterleaved input, one buffer per channel (format.channels) */
    float **rb_output; /* Deinterleaved output, one buffer per channel */
    double rb_ratio;   /* Current time ratio (1/speed) */
    bool rb_initialized;
    bool rb_primed;              /* Start padding fed, delay discarded */
    _Atomic bool rb_ready;       /* Set by UI thread after init+prime; checked by RT */
    uint32_t rb_start_delay;     /* Samples to discard from start */
    uint32_t rb_delay_remaining; /* Remaining samples to discard */
    rb_ring_t rb_ring;           /* Output ring buffer */

    /* Pre-allocated work buffers (audio thread only) */
    float *work_buffer; /* Temp interleaved buffer */
    uint32_t buffer_capacity;

    /* High-precision position tracking (audio thread only) */
    double fractional_position; /* Sub-sample accurate position */

    /* Crossfade state for smooth transitions */
    float *crossfade_buffer;   /* Previous output for crossfading */
    uint32_t crossfade_frames; /* Frames remaining in crossfade */
    uint32_t crossfade_length; /* Total crossfade length */
    shuttle_mode_t prev_mode;  /* Mode last processed — drives crossfade on change */

    /* Wire format — fixed at construction. Channels parameterizes every
     * memcpy/memset stride and the rubberband channel count. */
    audio_format_t format;

    /* Scrubber RT stat (atomic — written in callback, read from UI) */
    atomic_uint_fast64_t stats_underflows; /* Rubberband couldn't fill requested frames */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Rubberband Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
rubberband_cleanup(audio_shuttle_speed_t *s)
{
    if (s->rb_state) {
        rubberband_delete(s->rb_state);
        s->rb_state = NULL;
    }
    const uint32_t ch = s->format.channels;
    if (s->rb_input) {
        for (uint32_t c = 0; c < ch; c++)
            free(s->rb_input[c]);
        free(s->rb_input);
        s->rb_input = NULL;
    }
    if (s->rb_output) {
        for (uint32_t c = 0; c < ch; c++)
            free(s->rb_output[c]);
        free(s->rb_output);
        s->rb_output = NULL;
    }
    s->rb_initialized = false;
    s->rb_primed = false;
}

static void
rubberband_prime(audio_shuttle_speed_t *s)
{
    if (!s->rb_state || s->rb_primed)
        return;

    /* Get required start padding */
    unsigned int start_pad = rubberband_get_preferred_start_pad(s->rb_state);
    s->rb_start_delay = rubberband_get_start_delay(s->rb_state);
    s->rb_delay_remaining = s->rb_start_delay;

    /* Feed silence for start padding */
    if (start_pad > 0) {
        uint32_t pad_frames = (start_pad < s->buffer_capacity) ? start_pad : s->buffer_capacity;

        /* Clear input buffers (silence) */
        for (uint32_t c = 0; c < s->format.channels; c++)
            memset(s->rb_input[c], 0, pad_frames * sizeof(float));

        /* Feed silence in chunks */
        uint32_t fed = 0;
        while (fed < start_pad) {
            uint32_t chunk = start_pad - fed;
            if (chunk > s->buffer_capacity)
                chunk = s->buffer_capacity;

            rubberband_process(s->rb_state, (const float *const *)s->rb_input, chunk, 0);
            fed += chunk;
        }
    }

    s->rb_primed = true;
}

static void
ensure_rubberband(audio_shuttle_speed_t *s, float speed)
{
    double new_ratio = 1.0 / (double)fabsf(speed);

    /* Clamp ratio to valid range */
    if (new_ratio < 1.0 / RUBBERBAND_MAX)
        new_ratio = 1.0 / RUBBERBAND_MAX;
    if (new_ratio > 1.0 / RUBBERBAND_MIN)
        new_ratio = 1.0 / RUBBERBAND_MIN;

    if (s->rb_initialized) {
        /* Update ratio if changed significantly */
        if (fabs(new_ratio - s->rb_ratio) > 0.01) {
            rubberband_set_time_ratio(s->rb_state, new_ratio);
            s->rb_ratio = new_ratio;
        }
        return;
    }

    /*
     * Create rubberband with optimized options for real-time use:
     * - ProcessRealTime: Required for streaming
     * - EngineFaster: R2 engine for lower latency
     * - TransientsMixed: Better than Crisp (avoids "interruptions in stable sounds")
     * - SmoothingOn: Time-domain smoothing reduces artifacts
     * - WindowShort: Lower latency, faster processing
     * - PitchHighConsistency: Smooth ratio changes without discontinuities
     */
    RubberBandOptions opts = RubberBandOptionProcessRealTime | RubberBandOptionEngineFaster
                             | RubberBandOptionTransientsMixed | RubberBandOptionSmoothingOn
                             | RubberBandOptionWindowShort | RubberBandOptionPitchHighConsistency;

    s->rb_state = rubberband_new(s->format.sample_rate, s->format.channels, opts, new_ratio, 1.0);
    if (!s->rb_state)
        return;

    s->rb_ratio = new_ratio;

    /* Set max process size for RT safety (prevents internal reallocation) */
    rubberband_set_max_process_size(s->rb_state, WORK_BUFFER_FRAMES);

    /* Pre-allocate one deinterleaved buffer per channel. calloc the pointer
     * arrays so a mid-loop failure leaves cleanup with NULLs to free safely. */
    const uint32_t ch = s->format.channels;
    s->rb_input = calloc(ch, sizeof(float *));
    s->rb_output = calloc(ch, sizeof(float *));
    if (!s->rb_input || !s->rb_output) {
        rubberband_cleanup(s);
        return;
    }
    bool alloc_ok = true;
    for (uint32_t c = 0; c < ch; c++) {
        s->rb_input[c] = malloc(WORK_BUFFER_FRAMES * sizeof(float));
        s->rb_output[c] = malloc(WORK_BUFFER_FRAMES * sizeof(float));
        if (!s->rb_input[c] || !s->rb_output[c])
            alloc_ok = false;
    }
    if (!alloc_ok) {
        rubberband_cleanup(s);
        return;
    }

    /* Initialize output ring buffer */
    rb_ring_init(&s->rb_ring, RB_RING_BUFFER_SIZE, s->format.channels);

    s->rb_initialized = true;
    s->rb_primed = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
audio_shuttle_speed_create(audio_format_t format, audio_shuttle_speed_t **out)
{
    if (!out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    /* Channel-agnostic: rubberband, deinterleave buffers, and the hermite/
     * crossfade loops all iterate format.channels. Bound the count so the
     * per-channel allocations stay sane (covers mono..7.1.4). */
    g_assert(format.channels >= 1 && format.channels <= 16);

    audio_shuttle_speed_t *s = calloc(1, sizeof(audio_shuttle_speed_t));
    if (!s)
        return QUADRATURE_ERROR_OUT_OF_MEMORY;

    s->format = format;
    s->fractional_position = 0.0;
    s->prev_mode = SHUTTLE_MODE_OFF;

    /* Calculate crossfade length in frames */
    s->crossfade_length = (format.sample_rate * CROSSFADE_MS) / 1000;
    if (s->crossfade_length < CROSSFADE_MIN_FRAMES)
        s->crossfade_length = CROSSFADE_MIN_FRAMES;
    s->crossfade_frames = 0;

    /* Pre-allocate work buffers */
    const size_t bpf = audio_format_bytes_per_frame(&s->format);
    s->buffer_capacity = WORK_BUFFER_FRAMES;
    s->work_buffer = malloc((size_t)s->buffer_capacity * bpf);
    s->crossfade_buffer = malloc((size_t)s->buffer_capacity * bpf);
    if (!s->work_buffer || !s->crossfade_buffer) {
        free(s->work_buffer);
        free(s->crossfade_buffer);
        free(s);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }
    memset(s->crossfade_buffer, 0, (size_t)s->buffer_capacity * bpf);

    /* Initialize atomics */
    atomic_store(&s->speed, 1.0f);

    /* Rubberband is pre-allocated on mode change (not in RT callback) */
    s->rb_initialized = false;
    s->rb_primed = false;
    atomic_store(&s->rb_ready, false);
    s->rb_state = NULL;
    s->rb_input = NULL;
    s->rb_output = NULL;

    atomic_store(&s->stats_underflows, 0);

    *out = s;
    return QUADRATURE_OK;
}

void
audio_shuttle_speed_destroy(audio_shuttle_speed_t *s)
{
    if (!s)
        return;
    if (s->rb_initialized) {
        rb_ring_free(&s->rb_ring);
    }
    rubberband_cleanup(s);
    free(s->work_buffer);
    free(s->crossfade_buffer);
    free(s);
}

void
audio_shuttle_speed_flush(audio_shuttle_speed_t *s)
{
    g_assert(s != NULL);

    shuttle_mode_t mode = (shuttle_mode_t)atomic_load(&s->shuttle_mode);

    if (s->rb_state && mode == SHUTTLE_MODE_KEYLOCK) {
        /* Gate RT reads while we reset rubberband state */
        atomic_store(&s->rb_ready, false);
        rubberband_reset(s->rb_state);
        rb_ring_clear(&s->rb_ring);
        s->rb_primed = false;

        /* Re-prime immediately so RT path can resume */
        rubberband_prime(s);
        atomic_store(&s->rb_ready, s->rb_initialized && s->rb_primed);
    } else if (s->rb_state) {
        rb_ring_clear(&s->rb_ring);
    }

    /* Reset crossfade scratch. Fractional position resyncs to the (now
     * external) playhead on the next process() call via the mismatch check. */
    s->crossfade_frames = 0;
    s->prev_mode = SHUTTLE_MODE_OFF;

    const size_t bpf = audio_format_bytes_per_frame(&s->format);
    if (s->work_buffer)
        memset(s->work_buffer, 0, (size_t)s->buffer_capacity * bpf);
    if (s->crossfade_buffer)
        memset(s->crossfade_buffer, 0, (size_t)s->buffer_capacity * bpf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Control API (UI Thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

void
audio_shuttle_speed_set_speed(audio_shuttle_speed_t *s, float speed)
{
    g_assert(s != NULL);
    if (speed < -4.0f)
        speed = -4.0f;
    if (speed > 4.0f)
        speed = 4.0f;
    atomic_store(&s->speed, speed);
}

void
audio_shuttle_speed_set_mode(audio_shuttle_speed_t *s, shuttle_mode_t mode)
{
    g_assert(s != NULL);
    /* Pure atomic store. No allocation, no DSP setup. Safe from any thread.
     * Caller MUST have already called audio_shuttle_speed_prepare_mode(mode)
     * at least once on the UI/main thread if `mode` needs allocation
     * (currently: KEYLOCK). Otherwise the RT path outputs silence until the
     * resources are prepared. */
    atomic_store(&s->shuttle_mode, (int)mode);
}

void
audio_shuttle_speed_prepare_mode(audio_shuttle_speed_t *s, shuttle_mode_t mode)
{
    g_assert(s != NULL);
    /* UI/main thread only — may allocate. Idempotent: re-preparing an
     * already-prepared mode is a fast no-op. */
    if (mode != SHUTTLE_MODE_KEYLOCK)
        return; /* Only KEYLOCK needs allocation today (rubberband state). */

    float speed = atomic_load(&s->speed);
    if (fabsf(speed) < SPEED_STOPPED_EPSILON)
        speed = 1.0f; /* Default ratio for stopped state */

    ensure_rubberband(s, speed);
    if (s->rb_initialized && !s->rb_primed) {
        rubberband_prime(s);
    }
    /* Signal RT path that rubberband is ready */
    atomic_store(&s->rb_ready, s->rb_initialized && s->rb_primed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Query API (Thread-Safe)
 * ═══════════════════════════════════════════════════════════════════════════ */

float
audio_shuttle_speed_get_speed(const audio_shuttle_speed_t *s)
{
    g_assert(s != NULL);
    return atomic_load_explicit(&s->speed, memory_order_relaxed);
}

shuttle_mode_t
audio_shuttle_speed_get_mode(const audio_shuttle_speed_t *s)
{
    g_assert(s != NULL);
    return (shuttle_mode_t)atomic_load_explicit(&s->shuttle_mode, memory_order_relaxed);
}

uint64_t
audio_shuttle_speed_get_underflows(const audio_shuttle_speed_t *s)
{
    g_assert(s != NULL);
    return atomic_load_explicit(&s->stats_underflows, memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cubic Hermite Interpolation (4-point)
 *
 * Much higher quality than linear interpolation. Provides ~40dB of imaging
 * suppression vs ~26dB for linear. Essential for clean varispeed playback.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline float
cubic_hermite(float y0, float y1, float y2, float y3, float t)
{
    /* Catmull-Rom spline coefficients */
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rubberband Processing
 *
 * Pitch-preserved time stretch (KEYLOCK mode). The R2 engine emits variable
 * output sizes, so we accumulate into a ring buffer and pull fixed blocks.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t
process_rubberband(audio_shuttle_speed_t *s,
                   const float *samples,
                   uint64_t num_frames,
                   float *output,
                   uint32_t frames)
{
    /* RT-safe: only proceed if UI thread has finished init + prime */
    if (!atomic_load_explicit(&s->rb_ready, memory_order_acquire)) {
        memset(output, 0, (size_t)frames * audio_format_bytes_per_frame(&s->format));
        return frames;
    }

    /* Update ratio if speed changed (RT-safe: no allocation) */
    float speed = fabsf(atomic_load_explicit(&s->speed, memory_order_relaxed));
    ensure_rubberband(s, speed);

/*
     * Feed rubberband until we have enough output in ring buffer.
     * RB produces variable output sizes, so we accumulate in ring buffer
     * and pull from there for consistent output.
     *
     * Capped at MAX_RB_ITERATIONS to bound worst-case callback time.
     * At extreme ratios rubberband may need many iterations; if we
     * exhaust the cap, the shortfall is zero-padded below.
     */
#define MAX_RB_ITERATIONS 8
    const uint32_t ch = s->format.channels;
    int rb_iters = 0;
    while (rb_ring_available(&s->rb_ring) < frames && rb_iters < MAX_RB_ITERATIONS) {
        rb_iters++;
        /* Get how many samples rubberband needs */
        unsigned int required = rubberband_get_samples_required(s->rb_state);
        if (required == 0)
            required = 256; /* Minimum chunk */
        if (required > s->buffer_capacity)
            required = s->buffer_capacity;

        /* Get current source position */
        int64_t pos = (int64_t)s->fractional_position;
        uint64_t src_pos = (pos >= 0) ? (uint64_t)pos : 0;

        /* Calculate available source frames */
        uint32_t available = (src_pos < num_frames) ? (uint32_t)(num_frames - src_pos) : 0;
        uint32_t to_read = (required < available) ? required : available;

        /* Deinterleave source into per-channel rubberband input buffers */
        for (uint32_t i = 0; i < to_read; i++)
            for (uint32_t c = 0; c < ch; c++)
                s->rb_input[c][i] = samples[(size_t)(src_pos + i) * ch + c];

        /* Zero-pad if we ran out of source data */
        for (uint32_t i = to_read; i < required; i++)
            for (uint32_t c = 0; c < ch; c++)
                s->rb_input[c][i] = 0.0f;

        /* Process through rubberband */
        rubberband_process(s->rb_state, (const float *const *)s->rb_input, required, 0);

        /* Advance source position by what we actually consumed */
        s->fractional_position += (double)to_read;

        /* Retrieve all available output */
        int rb_avail = rubberband_available(s->rb_state);
        while (rb_avail > 0) {
            int chunk = (rb_avail < (int)s->buffer_capacity) ? rb_avail : (int)s->buffer_capacity;

            rubberband_retrieve(s->rb_state, s->rb_output, chunk);

            /* Handle start delay: discard initial samples */
            int usable_start = 0;
            if (s->rb_delay_remaining > 0) {
                if ((uint32_t)chunk <= s->rb_delay_remaining) {
                    s->rb_delay_remaining -= chunk;
                    rb_avail = rubberband_available(s->rb_state);
                    continue;
                } else {
                    usable_start = s->rb_delay_remaining;
                    s->rb_delay_remaining = 0;
                }
            }

            int usable_count = chunk - usable_start;

            /* Interleave and soft-limit into work buffer */
            for (int i = 0; i < usable_count; i++) {
                int src_i = usable_start + i;
                for (uint32_t c = 0; c < ch; c++)
                    s->work_buffer[(size_t)i * ch + c] = soft_limit(s->rb_output[c][src_i]);
            }

            /* Write to ring buffer */
            rb_ring_write(&s->rb_ring, s->work_buffer, usable_count);

            rb_avail = rubberband_available(s->rb_state);
        }

        /* Safety: if source exhausted, stop feeding */
        if (to_read == 0)
            break;
    }

    /* Pull requested frames from ring buffer */
    uint32_t got = rb_ring_read(&s->rb_ring, output, frames);

    /* Zero-pad if not enough (shouldn't happen in normal operation) */
    if (got < frames) {
        atomic_fetch_add_explicit(&s->stats_underflows, 1, memory_order_relaxed);
        const size_t spf = audio_format_samples_per_frame(&s->format);
        const size_t bpf = audio_format_bytes_per_frame(&s->format);
        memset(output + (size_t)got * spf, 0, (size_t)(frames - got) * bpf);
    }

    return frames;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Processing (Audio Thread Only)
 *
 * This is the ONLY playback path. Speed=1.0 is normal playback.
 * Uses high-precision position tracking and cubic interpolation for quality.
 * ═══════════════════════════════════════════════════════════════════════════ */

uint32_t
audio_shuttle_speed_process(audio_shuttle_speed_t *s,
                            audio_seek_position_t *pos,
                            const float *samples,
                            uint64_t num_frames,
                            float *output,
                            uint32_t frames)
{
    if (!s || !pos || !samples || !output || frames == 0)
        return 0;

    const uint32_t ch = s->format.channels;
    float speed = atomic_load_explicit(&s->speed, memory_order_relaxed);
    uint64_t playhead = audio_seek_position_get(pos);

    /* Sync fractional position if integer position was externally changed (seek) */
    if ((uint64_t)s->fractional_position != playhead) {
        s->fractional_position = (double)playhead;
        /* Ring buffer already cleared by flush with proper locking */
    }

    /* Clamp playhead to [0, num_frames-1]. Lower bound is structural — fractional
     * advance can't go negative from a non-negative start under any speed. */
    if (s->fractional_position >= (double)num_frames) {
        s->fractional_position = (double)(num_frames - 1);
    }

    /* Handle stopped state (speed near zero) */
    if (fabsf(speed) < SPEED_STOPPED_EPSILON) {
        memset(output, 0, (size_t)frames * audio_format_bytes_per_frame(&s->format));
        audio_seek_position_set(pos, (uint64_t)s->fractional_position);
        return frames;
    }

    /* Mode selects the DSP path directly; a crossfade on any change masks the
     * discontinuity between paths. */
    float rate = fabsf(speed);
    shuttle_mode_t mode
        = (shuttle_mode_t)atomic_load_explicit(&s->shuttle_mode, memory_order_relaxed);

    if (mode != s->prev_mode && s->crossfade_frames == 0)
        s->crossfade_frames = s->crossfade_length;

    /* ═══════════════════════════════════════════════════════════════════════
     * SHUTTLE_MODE_OFF — direct copy, no rate processing.
     * ═══════════════════════════════════════════════════════════════════════ */
    if (mode == SHUTTLE_MODE_OFF) {
        uint64_t src_frame = (uint64_t)s->fractional_position;
        uint32_t copy_frames = frames;
        uint64_t avail = (src_frame < num_frames) ? num_frames - src_frame : 0;
        if (copy_frames > avail)
            copy_frames = (uint32_t)avail;

        const size_t spf = audio_format_samples_per_frame(&s->format);
        const size_t bpf = audio_format_bytes_per_frame(&s->format);

        if (copy_frames > 0) {
            memcpy(output, samples + (size_t)src_frame * spf, (size_t)copy_frames * bpf);
        }
        if (copy_frames < frames) {
            memset(output + (size_t)copy_frames * spf, 0, (size_t)(frames - copy_frames) * bpf);
        }

        s->fractional_position += (double)copy_frames;
        goto apply_crossfade;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * SHUTTLE_MODE_PITCHED — cubic-hermite varispeed; pitch shifts with speed,
     * like vinyl.
     * ═══════════════════════════════════════════════════════════════════════ */
    if (mode == SHUTTLE_MODE_PITCHED) {
        uint32_t process_frames = frames;
        if (process_frames > s->buffer_capacity)
            process_frames = s->buffer_capacity;

        double fpos = s->fractional_position;
        double step = (double)rate;

        for (uint32_t i = 0; i < process_frames; i++) {
            int64_t idx = (int64_t)fpos;
            double frac = fpos - (double)idx;

            /* Clamp index to safe range for 4-sample window [idx-1 .. idx+2].
             * Eliminates per-sample branching in get_sample_safe. */
            if (idx < 1)
                idx = 1;
            if (idx >= (int64_t)num_frames - 2)
                idx = (int64_t)num_frames - 3;

            /* Read 4 contiguous frames directly — no bounds checks needed. Each
             * channel interpolates over its own samples at stride `ch`. */
            const float *p = samples + (size_t)(idx - 1) * ch;
            for (uint32_t c = 0; c < ch; c++)
                output[(size_t)i * ch + c]
                    = cubic_hermite(p[c], p[ch + c], p[2 * ch + c], p[3 * ch + c], (float)frac);

            fpos += step;
        }
        /* Bounds check once after loop — only matters at track edges */
        if (fpos < 0.0)
            fpos = 0.0;
        if (fpos >= (double)num_frames)
            fpos = (double)(num_frames - 1);

        /* Zero remaining frames if any */
        for (uint32_t i = process_frames; i < frames; i++)
            for (uint32_t c = 0; c < ch; c++)
                output[(size_t)i * ch + c] = 0.0f;

        s->fractional_position = fpos;
        goto apply_crossfade;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * SHUTTLE_MODE_KEYLOCK — pitch-preserved time stretch via rubberband.
     * ═══════════════════════════════════════════════════════════════════════ */
    process_rubberband(s, samples, num_frames, output, frames);

apply_crossfade:
    /* Apply crossfade if transitioning between modes */
    if (s->crossfade_frames > 0) {
        uint32_t fade_samples = (s->crossfade_frames < frames) ? s->crossfade_frames : frames;

        /* Pre-compute gain ramp — no loop-carried dependency → auto-vectorizable */
        float gain_step = 1.0f / (float)s->crossfade_length;
        float new_gain = 1.0f - (float)s->crossfade_frames / (float)s->crossfade_length;

        for (uint32_t i = 0; i < fade_samples; i++) {
            float old_gain = 1.0f - new_gain;
            for (uint32_t c = 0; c < ch; c++) {
                size_t k = (size_t)i * ch + c;
                output[k] = output[k] * new_gain + s->crossfade_buffer[k] * old_gain;
            }
            new_gain += gain_step;
        }
        s->crossfade_frames -= fade_samples;
    }

    /* Save output for potential future crossfade */
    uint32_t save_frames = (frames < s->buffer_capacity) ? frames : s->buffer_capacity;
    memcpy(s->crossfade_buffer,
           output,
           (size_t)save_frames * audio_format_bytes_per_frame(&s->format));

    s->prev_mode = mode;

    /* Write back the new playhead for external readers */
    audio_seek_position_set(pos, (uint64_t)s->fractional_position);

    return frames;
}
