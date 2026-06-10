/**
 * Per-player spectrum state: cavacore init/destroy and inline FFT processing.
 * Called from PipeWire monitor callback — no separate thread.
 */

#include "internal.h"
#include "cavacore.h"

#include <stdlib.h>
#include <string.h>

quadrature_result_t
spectrum_init(spectrum_state_t *s, uint32_t sample_rate)
{
    g_assert(s != NULL);
    g_assert(sample_rate > 0);

    memset(s, 0, sizeof(*s));
    s->sample_rate = sample_rate;

    s->plan = cava_init(SPECTRUM_BARS, sample_rate, 2, 1, 0.30, 50, 10000);
    if (!s->plan || s->plan->status != 0) {
        g_critical("spectrum_init: cava_init failed: %s",
                   s->plan ? s->plan->error_message : "allocation failed");
        if (s->plan) {
            cava_destroy(s->plan);
            s->plan = NULL;
        }
        return QUADRATURE_ERROR_INTERNAL;
    }

    s->input_buffer_size = FFT_SAMPLES * 2 * 2;
    s->input_buffer = calloc(s->input_buffer_size, sizeof(double));
    s->output_bars = calloc((size_t)SPECTRUM_BARS * 2, sizeof(double));
    if (!s->input_buffer || !s->output_bars) {
        spectrum_cleanup(s);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    spectrum_set_refresh_hz(s, 60.0);
    return QUADRATURE_OK;
}

void
spectrum_set_refresh_hz(spectrum_state_t *s, double hz)
{
    g_assert(s != NULL);
    if (hz < 30.0)
        hz = 30.0;
    if (hz > 165.0)
        hz = 165.0;
    int samples_per_frame = (int)((double)s->sample_rate / hz);
    atomic_store_explicit(&s->fft_threshold, samples_per_frame * 2, memory_order_relaxed);
}

void
spectrum_cleanup(spectrum_state_t *s)
{
    if (!s)
        return;
    if (s->plan) {
        cava_destroy(s->plan);
        s->plan = NULL;
    }
    free(s->input_buffer);
    s->input_buffer = NULL;
    free(s->output_bars);
    s->output_bars = NULL;
    s->input_buffer_fill = 0;
}

void
spectrum_process(spectrum_state_t *s,
                 const float *in,
                 uint32_t frames,
                 _Atomic float *bars,
                 _Atomic uint32_t *generation)
{
    if (!s->plan || !in || frames == 0)
        return;

    size_t to_read = (size_t)frames * 2; /* stereo interleaved */

    /* Prevent overflow: discard stale samples rather than memmove on the RT thread.
     * This path fires only if we somehow accumulate >4096 samples without hitting
     * the 512-sample FFT threshold — practically never in normal operation. */
    if (s->input_buffer_fill + to_read > s->input_buffer_size)
        s->input_buffer_fill = 0;

    /* Convert float → double.  Start offset is decoupled from the loop
     * so the compiler can auto-vectorize (no loop-carried dependency). */
    double *dst = s->input_buffer + s->input_buffer_fill;
    for (size_t i = 0; i < to_read; i++)
        dst[i] = (double)in[i];
    s->input_buffer_fill += to_read;

    /* Run FFT when enough samples accumulated for one display frame. */
    size_t threshold = (size_t)atomic_load_explicit(&s->fft_threshold, memory_order_relaxed);
    if (s->input_buffer_fill >= threshold) {
        cava_execute(s->input_buffer, (int)s->input_buffer_fill, s->output_bars, s->plan);
        s->input_buffer_fill = 0;

        /* Bar writes are relaxed; the trailing release-bump of `generation`
         * publishes the whole batch. UI readers that need a coherent batch
         * snapshot acquire-load `generation` first. */
        for (int b = 0; b < SPECTRUM_BARS; b++) {
            float left = (float)s->output_bars[b];
            float right = (float)s->output_bars[SPECTRUM_BARS + b];
            if (left < 0.0f)
                left = 0.0f;
            if (left > 1.0f)
                left = 1.0f;
            if (right < 0.0f)
                right = 0.0f;
            if (right > 1.0f)
                right = 1.0f;
            atomic_store_explicit(&bars[b], left, memory_order_relaxed);
            atomic_store_explicit(&bars[SPECTRUM_BARS + b], right, memory_order_relaxed);
        }
        if (generation)
            atomic_fetch_add_explicit(generation, 1, memory_order_release);
    }
}
