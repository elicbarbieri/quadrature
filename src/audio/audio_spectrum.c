/**
 * Per-player spectrum state: cavacore init/destroy and inline FFT processing.
 * Called from PipeWire monitor callback — no separate thread.
 */

#include "internal.h"
#include "cavacore.h"

#include <stdlib.h>
#include <string.h>

quadrature_result_t spectrum_init(spectrum_state_t* s, int num_bars, int sample_rate) {
    g_assert(s != NULL);
    g_assert(num_bars > 0 && num_bars <= 64);
    g_assert(sample_rate > 0);

    memset(s, 0, sizeof(*s));

    s->plan = cava_init(num_bars, (unsigned int)sample_rate, 2, 1, 0.77, 50, 10000);
    if (!s->plan || s->plan->status != 0) {
        g_critical("spectrum_init: cava_init failed: %s",
                   s->plan ? s->plan->error_message : "allocation failed");
        if (s->plan) { cava_destroy(s->plan); s->plan = NULL; }
        return QUADRATURE_ERROR_INTERNAL;
    }

    s->input_buffer_size = FFT_SAMPLES * 2 * 2;
    s->input_buffer = calloc(s->input_buffer_size, sizeof(double));
    s->output_bars = calloc((size_t)num_bars * 2, sizeof(double));
    if (!s->input_buffer || !s->output_bars) {
        spectrum_cleanup(s);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    return QUADRATURE_OK;
}

void spectrum_cleanup(spectrum_state_t* s) {
    if (!s) return;
    if (s->plan) { cava_destroy(s->plan); s->plan = NULL; }
    free(s->input_buffer);  s->input_buffer = NULL;
    free(s->output_bars);   s->output_bars = NULL;
    s->input_buffer_fill = 0;
}

void spectrum_process(spectrum_state_t* s, const float* in, uint32_t frames,
                      _Atomic float* bars) {
    if (!s->plan || !in || frames == 0) return;

    size_t to_read = (size_t)frames * 2;  /* stereo interleaved */

    /* Prevent overflow: keep most recent half if buffer would overfill */
    if (s->input_buffer_fill + to_read > s->input_buffer_size) {
        size_t keep = s->input_buffer_size / 2;
        memmove(s->input_buffer,
                s->input_buffer + s->input_buffer_fill - keep,
                keep * sizeof(double));
        s->input_buffer_fill = keep;
    }

    /* Convert float → double and accumulate */
    for (size_t i = 0; i < to_read; i++) {
        s->input_buffer[s->input_buffer_fill++] = (double)in[i];
    }

    /* Run FFT when enough samples accumulated */
    if (s->input_buffer_fill >= 512) {
        cava_execute(s->input_buffer, (int)s->input_buffer_fill,
                     s->output_bars, s->plan);
        s->input_buffer_fill = 0;

        /* Write clamped stereo results atomically */
        for (int b = 0; b < SPECTRUM_BARS; b++) {
            float left = (float)s->output_bars[b];
            float right = (float)s->output_bars[SPECTRUM_BARS + b];
            if (left < 0.0f) left = 0.0f;
            if (left > 1.0f) left = 1.0f;
            if (right < 0.0f) right = 0.0f;
            if (right > 1.0f) right = 1.0f;
            atomic_store(&bars[b], left);
            atomic_store(&bars[SPECTRUM_BARS + b], right);
        }
    }
}
