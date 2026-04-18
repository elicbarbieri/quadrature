/**
 * Quadrature Rate Processor
 *
 * Variable-speed playback with user-controlled mode selection:
 *   - Passthrough: speed ≈ 1.0 (direct copy, zero CPU) - always active
 *   - Rubberband: pitch-preserved time stretching (default mode, 0.5x-4.0x)
 *   - Turntable: pitch shifts with speed (pitched mode, 0.5x-2.0x)
 *
 * Mode is controlled via pitched_mode flag (atomic, lock-free).
 * Crossfade handles smooth transitions when switching modes.
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

#define WORK_BUFFER_FRAMES   4096    /* Pre-allocated buffer size */
#define RB_RING_BUFFER_SIZE  8192    /* Output ring buffer for rubberband */

/* Speed limits per mode */
#define RUBBERBAND_MIN       0.5f    /* Pitch-preserved mode min */
#define RUBBERBAND_MAX       4.0f    /* Pitch-preserved mode max */

/* Crossfade for smooth zone transitions (prevents clicks) */
#define CROSSFADE_MS            10   /* 10ms crossfade */
#define CROSSFADE_MIN_FRAMES    64   /* Clamp so very low sample rates still fade */
#define ZONE_PASSTHROUGH     0
#define ZONE_TURNTABLE       1
#define ZONE_RUBBERBAND      2

/* ═══════════════════════════════════════════════════════════════════════════
 * Simple Ring Buffer for Rubberband Output
 *
 * Single-producer single-consumer lock-free ring buffer.
 * Only used within audio thread, so no atomics needed.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    float *buffer;          /* Interleaved stereo samples */
    uint32_t capacity;      /* Total frames capacity */
    uint32_t write_idx;     /* Write position (frames) */
    uint32_t read_idx;      /* Read position (frames) */
    uint32_t count;         /* Frames currently in buffer */
} rb_ring_t;

static void rb_ring_init(rb_ring_t *r, uint32_t capacity) {
    r->buffer = calloc(capacity * 2, sizeof(float));
    r->capacity = capacity;
    r->write_idx = 0;
    r->read_idx = 0;
    r->count = 0;
}

static void rb_ring_free(rb_ring_t *r) {
    free(r->buffer);
    r->buffer = NULL;
}

static void rb_ring_clear(rb_ring_t *r) {
    r->write_idx = 0;
    r->read_idx = 0;
    r->count = 0;
}

static uint32_t rb_ring_available(const rb_ring_t *r) {
    return r->count;
}

static uint32_t rb_ring_space(const rb_ring_t *r) {
    return r->capacity - r->count;
}

static void rb_ring_write(rb_ring_t *r, const float *data, uint32_t frames) {
    if (frames > rb_ring_space(r)) frames = rb_ring_space(r);
    if (frames == 0) return;

    /* Split into at most two memcpy's — libc memcpy uses best available SIMD */
    uint32_t first = r->capacity - r->write_idx;
    if (first > frames) first = frames;
    memcpy(&r->buffer[r->write_idx * 2], data, first * 2 * sizeof(float));
    if (frames > first)
        memcpy(r->buffer, data + first * 2, (frames - first) * 2 * sizeof(float));
    r->write_idx = (r->write_idx + frames) % r->capacity;
    r->count += frames;
}

static uint32_t rb_ring_read(rb_ring_t *r, float *data, uint32_t frames) {
    if (frames > r->count) frames = r->count;
    if (frames == 0) return 0;

    uint32_t first = r->capacity - r->read_idx;
    if (first > frames) first = frames;
    memcpy(data, &r->buffer[r->read_idx * 2], first * 2 * sizeof(float));
    if (frames > first)
        memcpy(data + first * 2, r->buffer, (frames - first) * 2 * sizeof(float));
    r->read_idx = (r->read_idx + frames) % r->capacity;
    r->count -= frames;
    return frames;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rate Processor Structure
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_scrubber {
    /* Control state (UI thread writes, audio thread reads) */
    _Atomic float speed;              /* Playback speed: 1.0 = normal */
    _Atomic int64_t position;         /* Current position in samples */
    _Atomic int shuttle_mode;         /* shuttle_mode_t: OFF, KEYLOCK, PITCHED */

    /* Rubberband state (pre-allocated on mode change, NOT in RT callback) */
    RubberBandState rb_state;
    float *rb_input[2];               /* Deinterleaved input buffers */
    float *rb_output[2];              /* Deinterleaved output buffers */
    double rb_ratio;                  /* Current time ratio (1/speed) */
    bool rb_initialized;
    bool rb_primed;                   /* Start padding fed, delay discarded */
    _Atomic bool rb_ready;            /* Set by UI thread after init+prime; checked by RT */
    uint32_t rb_start_delay;          /* Samples to discard from start */
    uint32_t rb_delay_remaining;      /* Remaining samples to discard */
    rb_ring_t rb_ring;                /* Output ring buffer */

    /* Pre-allocated work buffers (audio thread only) */
    float *work_buffer;               /* Temp interleaved buffer */
    uint32_t buffer_capacity;

    /* High-precision position tracking (audio thread only) */
    double fractional_position;       /* Sub-sample accurate position */

    /* Crossfade state for smooth transitions */
    float *crossfade_buffer;          /* Previous output for crossfading */
    uint32_t crossfade_frames;        /* Frames remaining in crossfade */
    uint32_t crossfade_length;        /* Total crossfade length */
    int prev_zone;                    /* 0=passthrough, 1=turntable, 2=rubberband */

    /* Configuration */
    uint32_t sample_rate;

    /* Scrubber RT stat (atomic — written in callback, read from UI) */
    atomic_uint_fast64_t stats_underflows;   /* Rubberband couldn't fill requested frames */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Rubberband Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rubberband_cleanup(audio_scrubber_t *s) {
    if (s->rb_state) {
        rubberband_delete(s->rb_state);
        s->rb_state = NULL;
    }
    free(s->rb_input[0]);
    free(s->rb_input[1]);
    free(s->rb_output[0]);
    free(s->rb_output[1]);
    s->rb_input[0] = s->rb_input[1] = NULL;
    s->rb_output[0] = s->rb_output[1] = NULL;
    s->rb_initialized = false;
    s->rb_primed = false;
}

static void rubberband_prime(audio_scrubber_t *s) {
    if (!s->rb_state || s->rb_primed) return;

    /* Get required start padding */
    unsigned int start_pad = rubberband_get_preferred_start_pad(s->rb_state);
    s->rb_start_delay = rubberband_get_start_delay(s->rb_state);
    s->rb_delay_remaining = s->rb_start_delay;

    /* Feed silence for start padding */
    if (start_pad > 0) {
        uint32_t pad_frames = (start_pad < s->buffer_capacity) ? start_pad : s->buffer_capacity;

        /* Clear input buffers (silence) */
        memset(s->rb_input[0], 0, pad_frames * sizeof(float));
        memset(s->rb_input[1], 0, pad_frames * sizeof(float));

        /* Feed silence in chunks */
        uint32_t fed = 0;
        while (fed < start_pad) {
            uint32_t chunk = start_pad - fed;
            if (chunk > s->buffer_capacity) chunk = s->buffer_capacity;

            rubberband_process(s->rb_state,
                              (const float *const *)s->rb_input,
                              chunk, 0);
            fed += chunk;
        }
    }

    s->rb_primed = true;
}

static void ensure_rubberband(audio_scrubber_t *s, float speed) {
    double new_ratio = 1.0 / (double)fabsf(speed);

    /* Clamp ratio to valid range */
    if (new_ratio < 1.0 / RUBBERBAND_MAX) new_ratio = 1.0 / RUBBERBAND_MAX;
    if (new_ratio > 1.0 / RUBBERBAND_MIN) new_ratio = 1.0 / RUBBERBAND_MIN;

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
    RubberBandOptions opts =
        RubberBandOptionProcessRealTime |
        RubberBandOptionEngineFaster |
        RubberBandOptionTransientsMixed |
        RubberBandOptionSmoothingOn |
        RubberBandOptionWindowShort |
        RubberBandOptionPitchHighConsistency;

    s->rb_state = rubberband_new(s->sample_rate, 2, opts, new_ratio, 1.0);
    if (!s->rb_state) return;

    s->rb_ratio = new_ratio;

    /* Set max process size for RT safety (prevents internal reallocation) */
    rubberband_set_max_process_size(s->rb_state, WORK_BUFFER_FRAMES);

    /* Pre-allocate deinterleaved buffers */
    s->rb_input[0] = malloc(WORK_BUFFER_FRAMES * sizeof(float));
    s->rb_input[1] = malloc(WORK_BUFFER_FRAMES * sizeof(float));
    s->rb_output[0] = malloc(WORK_BUFFER_FRAMES * sizeof(float));
    s->rb_output[1] = malloc(WORK_BUFFER_FRAMES * sizeof(float));

    if (!s->rb_input[0] || !s->rb_input[1] ||
        !s->rb_output[0] || !s->rb_output[1]) {
        rubberband_cleanup(s);
        return;
    }

    /* Initialize output ring buffer */
    rb_ring_init(&s->rb_ring, RB_RING_BUFFER_SIZE);

    s->rb_initialized = true;
    s->rb_primed = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_scrubber_create(uint32_t sample_rate, audio_scrubber_t **out) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    audio_scrubber_t *s = calloc(1, sizeof(audio_scrubber_t));
    if (!s) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    s->sample_rate = sample_rate;
    s->fractional_position = 0.0;
    s->prev_zone = ZONE_PASSTHROUGH;

    /* Calculate crossfade length in frames */
    s->crossfade_length = (sample_rate * CROSSFADE_MS) / 1000;
    if (s->crossfade_length < CROSSFADE_MIN_FRAMES) s->crossfade_length = CROSSFADE_MIN_FRAMES;
    s->crossfade_frames = 0;

    /* Pre-allocate work buffers */
    s->buffer_capacity = WORK_BUFFER_FRAMES;
    s->work_buffer = malloc(s->buffer_capacity * 2 * sizeof(float));
    s->crossfade_buffer = malloc(s->buffer_capacity * 2 * sizeof(float));
    if (!s->work_buffer || !s->crossfade_buffer) {
        free(s->work_buffer);
        free(s->crossfade_buffer);
        free(s);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }
    memset(s->crossfade_buffer, 0, s->buffer_capacity * 2 * sizeof(float));

    /* Initialize atomics */
    atomic_store(&s->speed, 1.0f);
    atomic_store(&s->position, 0);

    /* Rubberband is pre-allocated on mode change (not in RT callback) */
    s->rb_initialized = false;
    s->rb_primed = false;
    atomic_store(&s->rb_ready, false);
    s->rb_state = NULL;
    s->rb_input[0] = s->rb_input[1] = NULL;
    s->rb_output[0] = s->rb_output[1] = NULL;

    atomic_store(&s->stats_underflows, 0);

    *out = s;
    return QUADRATURE_OK;
}

void audio_scrubber_destroy(audio_scrubber_t *s) {
    if (!s) return;
    if (s->rb_initialized) {
        rb_ring_free(&s->rb_ring);
    }
    rubberband_cleanup(s);
    free(s->work_buffer);
    free(s->crossfade_buffer);
    free(s);
}

void audio_scrubber_flush(audio_scrubber_t *s) {
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

    /* Reset high-precision position and crossfade state */
    s->fractional_position = (double)atomic_load(&s->position);
    s->crossfade_frames = 0;
    s->prev_zone = ZONE_PASSTHROUGH;

    if (s->work_buffer)
        memset(s->work_buffer, 0, s->buffer_capacity * 2 * sizeof(float));
    if (s->crossfade_buffer)
        memset(s->crossfade_buffer, 0, s->buffer_capacity * 2 * sizeof(float));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Control API (UI Thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

void audio_scrubber_set_speed(audio_scrubber_t *s, float speed) {
    g_assert(s != NULL);
    if (speed < -4.0f) speed = -4.0f;
    if (speed > 4.0f) speed = 4.0f;
    atomic_store(&s->speed, speed);
}

void audio_scrubber_set_position(audio_scrubber_t *s, int64_t position) {
    g_assert(s != NULL);
    atomic_store(&s->position, position);
}

void audio_scrubber_set_shuttle_mode(audio_scrubber_t *s, shuttle_mode_t mode) {
    g_assert(s != NULL);

    shuttle_mode_t old_mode = (shuttle_mode_t)atomic_load(&s->shuttle_mode);
    atomic_store(&s->shuttle_mode, (int)mode);

    /*
     * Pre-allocate rubberband on UI thread when switching TO keylock mode.
     * This avoids malloc + unbounded rubberband_prime() in the RT callback.
     * Once allocated, rubberband stays alive (reusable across mode toggles).
     */
    if (mode == SHUTTLE_MODE_KEYLOCK && old_mode != SHUTTLE_MODE_KEYLOCK) {
        float speed = atomic_load(&s->speed);
        if (fabsf(speed) < 0.01f) speed = 1.0f;  /* Default ratio for stopped state */

        ensure_rubberband(s, speed);
        if (s->rb_initialized && !s->rb_primed) {
            rubberband_prime(s);
        }
        /* Signal RT path that rubberband is ready */
        atomic_store(&s->rb_ready, s->rb_initialized && s->rb_primed);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Query API (Thread-Safe)
 * ═══════════════════════════════════════════════════════════════════════════ */

float audio_scrubber_get_speed(const audio_scrubber_t *s) {
    g_assert(s != NULL);
    return atomic_load(&s->speed);
}

int64_t audio_scrubber_get_position(const audio_scrubber_t *s) {
    g_assert(s != NULL);
    return atomic_load(&s->position);
}

shuttle_mode_t audio_scrubber_get_shuttle_mode(const audio_scrubber_t *s) {
    g_assert(s != NULL);
    return (shuttle_mode_t)atomic_load(&s->shuttle_mode);
}

uint64_t audio_scrubber_get_underflows(const audio_scrubber_t *s) {
    g_assert(s != NULL);
    return atomic_load_explicit(&s->stats_underflows, memory_order_relaxed);
}

int audio_scrubber_get_zone(const audio_scrubber_t *s) {
    g_assert(s != NULL);

    shuttle_mode_t mode = (shuttle_mode_t)atomic_load(&s->shuttle_mode);

    if (mode == SHUTTLE_MODE_OFF) {
        return ZONE_PASSTHROUGH;
    } else if (mode == SHUTTLE_MODE_PITCHED) {
        return ZONE_TURNTABLE;
    } else {
        return ZONE_RUBBERBAND;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cubic Hermite Interpolation (4-point)
 *
 * Much higher quality than linear interpolation. Provides ~40dB of imaging
 * suppression vs ~26dB for linear. Essential for clean varispeed playback.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline float cubic_hermite(float y0, float y1, float y2, float y3, float t) {
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
 * Pitch-preserved time stretching for extreme speeds (<0.8x or >1.2x).
 * Uses R2 engine with output ring buffer to handle variable output sizes.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t process_rubberband(audio_scrubber_t *s,
                                   const float *samples, uint64_t num_frames,
                                   float *output, uint32_t frames) {
    /* RT-safe: only proceed if UI thread has finished init + prime */
    if (!atomic_load_explicit(&s->rb_ready, memory_order_acquire)) {
        memset(output, 0, frames * 2 * sizeof(float));
        return frames;
    }

    /* Update ratio if speed changed (RT-safe: no allocation) */
    float speed = fabsf(atomic_load(&s->speed));
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
    int rb_iters = 0;
    while (rb_ring_available(&s->rb_ring) < frames && rb_iters < MAX_RB_ITERATIONS) {
        rb_iters++;
        /* Get how many samples rubberband needs */
        unsigned int required = rubberband_get_samples_required(s->rb_state);
        if (required == 0) required = 256;  /* Minimum chunk */
        if (required > s->buffer_capacity) required = s->buffer_capacity;

        /* Get current source position */
        int64_t pos = (int64_t)s->fractional_position;
        uint64_t src_pos = (pos >= 0) ? (uint64_t)pos : 0;

        /* Calculate available source frames */
        uint32_t available = (src_pos < num_frames) ? (uint32_t)(num_frames - src_pos) : 0;
        uint32_t to_read = (required < available) ? required : available;

        /* Deinterleave source into rubberband input buffers */
        for (uint32_t i = 0; i < to_read; i++) {
            s->rb_input[0][i] = samples[(src_pos + i) * 2];
            s->rb_input[1][i] = samples[(src_pos + i) * 2 + 1];
        }

        /* Zero-pad if we ran out of source data */
        for (uint32_t i = to_read; i < required; i++) {
            s->rb_input[0][i] = 0.0f;
            s->rb_input[1][i] = 0.0f;
        }

        /* Process through rubberband */
        rubberband_process(s->rb_state,
                          (const float *const *)s->rb_input,
                          required, 0);

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
                s->work_buffer[i * 2]     = soft_limit(s->rb_output[0][src_i]);
                s->work_buffer[i * 2 + 1] = soft_limit(s->rb_output[1][src_i]);
            }

            /* Write to ring buffer */
            rb_ring_write(&s->rb_ring, s->work_buffer, usable_count);

            rb_avail = rubberband_available(s->rb_state);
        }

        /* Safety: if source exhausted, stop feeding */
        if (to_read == 0) break;
    }

    /* Pull requested frames from ring buffer */
    uint32_t got = rb_ring_read(&s->rb_ring, output, frames);

    /* Zero-pad if not enough (shouldn't happen in normal operation) */
    if (got < frames) {
        atomic_fetch_add_explicit(&s->stats_underflows, 1, memory_order_relaxed);
        memset(output + got * 2, 0, (frames - got) * 2 * sizeof(float));
    }

    return frames;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Processing (Audio Thread Only)
 *
 * This is the ONLY playback path. Speed=1.0 is normal playback.
 * Uses high-precision position tracking and cubic interpolation for quality.
 * ═══════════════════════════════════════════════════════════════════════════ */

uint32_t audio_scrubber_process(audio_scrubber_t *s,
                                 const float *samples, uint64_t num_frames,
                                 float *output, uint32_t frames,
                                 uint64_t *out_position) {
    if (!s || !samples || !output || frames == 0) return 0;

    float speed = atomic_load(&s->speed);
    int64_t int_position = atomic_load(&s->position);

    /* Sync fractional position if integer position was externally changed (seek) */
    int64_t expected_int = (int64_t)s->fractional_position;
    if (expected_int != int_position) {
        s->fractional_position = (double)int_position;
        /* Ring buffer already cleared by flush with proper locking */
    }

    /* Clamp position */
    if (s->fractional_position < 0.0) s->fractional_position = 0.0;
    if (s->fractional_position >= (double)num_frames) {
        s->fractional_position = (double)(num_frames - 1);
    }

    /* Handle stopped state (speed near zero) */
    if (fabsf(speed) < 0.01f) {
        memset(output, 0, frames * 2 * sizeof(float));
        int_position = (int64_t)s->fractional_position;
        atomic_store(&s->position, int_position);
        if (out_position) *out_position = (uint64_t)int_position;
        return frames;
    }

    /* Determine current zone based on shuttle mode only */
    float rate = fabsf(speed);
    shuttle_mode_t mode = (shuttle_mode_t)atomic_load(&s->shuttle_mode);
    int current_zone;

    if (mode == SHUTTLE_MODE_OFF) {
        current_zone = ZONE_PASSTHROUGH;
    } else if (mode == SHUTTLE_MODE_PITCHED) {
        current_zone = ZONE_TURNTABLE;
    } else {
        current_zone = ZONE_RUBBERBAND;
    }

    /* Detect zone transition - start crossfade */
    if (current_zone != s->prev_zone && s->crossfade_frames == 0) {
        s->crossfade_frames = s->crossfade_length;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * DIRECT PASSTHROUGH: No processing at exactly 1.0x speed
     * ═══════════════════════════════════════════════════════════════════════ */
    if (current_zone == ZONE_PASSTHROUGH) {
        int64_t pos = (int64_t)s->fractional_position;
        uint32_t copy_frames = frames;
        uint64_t avail = (pos >= 0 && (uint64_t)pos < num_frames)
                       ? num_frames - (uint64_t)pos : 0;
        if (copy_frames > avail) copy_frames = (uint32_t)avail;

        if (copy_frames > 0) {
            memcpy(output, samples + pos * 2, copy_frames * 2 * sizeof(float));
        }
        if (copy_frames < frames) {
            memset(output + copy_frames * 2, 0, (frames - copy_frames) * 2 * sizeof(float));
        }

        s->fractional_position += (double)copy_frames;
        goto apply_crossfade;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * TURNTABLE ZONE: Cubic Hermite interpolation varispeed (pitched mode)
     *
     * When pitched_mode is ON, use high-quality cubic interpolation.
     * Pitch shifts with speed (like vinyl). Quality range: 0.5x-2.0x.
     * ═══════════════════════════════════════════════════════════════════════ */
    if (current_zone == ZONE_TURNTABLE) {
        uint32_t process_frames = frames;
        if (process_frames > s->buffer_capacity) process_frames = s->buffer_capacity;

        double pos = s->fractional_position;
        double step = (double)rate;

        for (uint32_t i = 0; i < process_frames; i++) {
            int64_t idx = (int64_t)pos;
            double frac = pos - (double)idx;

            /* Clamp index to safe range for 4-sample window [idx-1 .. idx+2].
             * Eliminates per-sample branching in get_sample_safe. */
            if (idx < 1) idx = 1;
            if (idx >= (int64_t)num_frames - 2) idx = (int64_t)num_frames - 3;

            /* Read 4 contiguous stereo pairs directly — no bounds checks needed */
            const float *p = samples + (idx - 1) * 2;
            output[i * 2]     = cubic_hermite(p[0], p[2], p[4], p[6], (float)frac);
            output[i * 2 + 1] = cubic_hermite(p[1], p[3], p[5], p[7], (float)frac);

            pos += step;
        }
        /* Bounds check once after loop — only matters at track edges */
        if (pos < 0.0) pos = 0.0;
        if (pos >= (double)num_frames) pos = (double)(num_frames - 1);

        /* Zero remaining frames if any */
        for (uint32_t i = process_frames; i < frames; i++) {
            output[i * 2] = 0.0f;
            output[i * 2 + 1] = 0.0f;
        }

        s->fractional_position = pos;
        goto apply_crossfade;
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * RUBBERBAND ZONE: Pitch-preserved time stretching (default mode)
     *
     * When pitched_mode is OFF (default), use librubberband R2 engine.
     * Pitch is preserved across all speeds. Range: 0.5x-4.0x.
     * ═══════════════════════════════════════════════════════════════════════ */

    /* Process through rubberband */
    process_rubberband(s, samples, num_frames, output, frames);

    /* Fall through to crossfade application */

apply_crossfade:
    /* Apply crossfade if transitioning between zones */
    if (s->crossfade_frames > 0) {
        uint32_t fade_samples = (s->crossfade_frames < frames) ? s->crossfade_frames : frames;

        /* Pre-compute gain ramp — no loop-carried dependency → auto-vectorizable */
        float gain_step = 1.0f / (float)s->crossfade_length;
        float new_gain = 1.0f - (float)s->crossfade_frames / (float)s->crossfade_length;

        for (uint32_t i = 0; i < fade_samples; i++) {
            float old_gain = 1.0f - new_gain;
            output[i * 2]     = output[i * 2] * new_gain + s->crossfade_buffer[i * 2] * old_gain;
            output[i * 2 + 1] = output[i * 2 + 1] * new_gain + s->crossfade_buffer[i * 2 + 1] * old_gain;
            new_gain += gain_step;
        }
        s->crossfade_frames -= fade_samples;
    }

    /* Save output for potential future crossfade */
    uint32_t save_frames = (frames < s->buffer_capacity) ? frames : s->buffer_capacity;
    memcpy(s->crossfade_buffer, output, save_frames * 2 * sizeof(float));

    s->prev_zone = current_zone;

    /* Update atomic position for external readers */
    int_position = (int64_t)s->fractional_position;
    atomic_store(&s->position, int_position);

    if (out_position) *out_position = (uint64_t)int_position;
    return frames;
}
