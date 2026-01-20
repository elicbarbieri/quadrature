#include "internal.h"
#include "quadrature/audio/audio_spectrum.h"

#include <glib.h>
#include "cavacore.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void* spectrum_thread(void* arg) {
    spectrum_analyzer_t* s = (spectrum_analyzer_t*)arg;
    audio_player_t* players = (audio_player_t*)s->players;

    while (atomic_load(&s->running)) {
        bool any_processed = false;

        for (int ch = 0; ch < s->num_channels; ch++) {
            spectrum_channel_t* chan = &s->channels[ch];
            audio_player_t* player = &players[ch];

            if (!chan->plan || !player->spectrum_buffer || !chan->read_buffer)
                continue;

            /* Decay bars when not playing */
            channel_state_t state = atomic_load(&player->state);
            if (state != CHANNEL_PLAYING) {
                for (int b = 0; b < SPECTRUM_BARS; b++) {
                    float cur = atomic_load(&player->spectrum_bars[b]);
                    if (cur > 0.001f) {
                        atomic_store(&player->spectrum_bars[b], cur * DECAY_FACTOR);
                    } else {
                        atomic_store(&player->spectrum_bars[b], 0.0f);
                    }
                }
                continue;
            }

            /* Read available samples from ring buffer */
            uint32_t index;
            int32_t avail = spa_ringbuffer_get_read_index(&player->spectrum_rb, &index);
            if (avail < 512) continue;

            size_t to_read = (avail > FFT_SAMPLES * 2) ? FFT_SAMPLES * 2 : (size_t)avail;

            spa_ringbuffer_read_data(&player->spectrum_rb, player->spectrum_buffer,
                SPECTRUM_RINGBUF_SIZE, index % SPECTRUM_RINGBUF_SIZE,
                chan->read_buffer, to_read * sizeof(float));
            spa_ringbuffer_read_update(&player->spectrum_rb, index + (uint32_t)to_read);

            /* Ensure we don't overflow input buffer */
            if (chan->input_buffer_fill + to_read > chan->input_buffer_size) {
                size_t keep = chan->input_buffer_size / 2;
                memmove(chan->input_buffer,
                        chan->input_buffer + chan->input_buffer_fill - keep,
                        keep * sizeof(double));
                chan->input_buffer_fill = keep;
            }

            /* Convert float to double for cavacore */
            for (size_t i = 0; i < to_read; i++) {
                chan->input_buffer[chan->input_buffer_fill++] = (double)chan->read_buffer[i];
            }

            /* Process when we have enough samples */
            if (chan->input_buffer_fill >= 512) {
                cava_execute(chan->input_buffer, (int)chan->input_buffer_fill,
                             chan->output_bars, chan->plan);

                chan->input_buffer_fill = 0;

                /* Write results to player's spectrum_bars (atomic) */
                int bars_per_channel = s->num_bars;
                for (int b = 0; b < bars_per_channel && b < SPECTRUM_BARS; b++) {
                    float val = (float)(chan->output_bars[b] + chan->output_bars[bars_per_channel + b]) / 2.0f;
                    if (val < 0.0f) val = 0.0f;
                    if (val > 1.0f) val = 1.0f;
                    atomic_store(&player->spectrum_bars[b], val);
                }

                any_processed = true;
            }
        }

        usleep(any_processed ? SPECTRUM_UPDATE_INTERVAL_US / 2 : SPECTRUM_UPDATE_INTERVAL_US);
    }

    return NULL;
}

spectrum_analyzer_t* spectrum_create(int num_bars, int sample_rate, int num_channels,
                                     void* players_ptr) {
    audio_player_t* players = (audio_player_t*)players_ptr;
    if (num_bars <= 0 || num_bars > 64 || sample_rate <= 0 ||
        num_channels <= 0 || num_channels > 4 || !players) {
        g_critical("spectrum_create: invalid parameters");
        return NULL;
    }

    spectrum_analyzer_t* s = calloc(1, sizeof(spectrum_analyzer_t));
    if (!s) {
        g_critical("spectrum_create: allocation failed");
        return NULL;
    }

    s->num_bars = num_bars;
    s->sample_rate = sample_rate;
    s->num_channels = num_channels;
    s->players = players_ptr;
    atomic_store(&s->running, false);

    for (int ch = 0; ch < num_channels; ch++) {
        spectrum_channel_t* chan = &s->channels[ch];

        chan->plan = cava_init(num_bars, (unsigned int)sample_rate, 2,
                               1, 0.77, 50, 10000);

        if (!chan->plan || chan->plan->status != 0) {
            g_critical("spectrum_create: cava_init failed for channel %d: %s",
                          ch, chan->plan ? chan->plan->error_message : "allocation failed");
            spectrum_destroy(s);
            return NULL;
        }

        chan->input_buffer_size = FFT_SAMPLES * 2 * 2;
        chan->input_buffer = calloc(chan->input_buffer_size, sizeof(double));
        chan->output_bars = calloc((size_t)num_bars * 2, sizeof(double));
        chan->read_buffer = calloc(FFT_SAMPLES * 2, sizeof(float));

        if (!chan->input_buffer || !chan->output_bars || !chan->read_buffer) {
            g_critical("spectrum_create: buffer allocation failed");
            spectrum_destroy(s);
            return NULL;
        }

        chan->input_buffer_fill = 0;
    }

    atomic_store(&s->running, true);
    if (pthread_create(&s->thread, NULL, spectrum_thread, s) != 0) {
        g_critical("spectrum_create: pthread_create failed");
        atomic_store(&s->running, false);
        spectrum_destroy(s);
        return NULL;
    }

    g_message("Spectrum analyzer created: %d bars, %d Hz, %d channels",
                 num_bars, sample_rate, num_channels);

    return s;
}

void spectrum_destroy(spectrum_analyzer_t* s) {
    if (!s) return;

    if (atomic_load(&s->running)) {
        atomic_store(&s->running, false);
        pthread_join(s->thread, NULL);
    }

    for (int ch = 0; ch < s->num_channels; ch++) {
        spectrum_channel_t* chan = &s->channels[ch];
        if (chan->plan) cava_destroy(chan->plan);
        free(chan->input_buffer);
        free(chan->output_bars);
        free(chan->read_buffer);
    }

    free(s);
    g_message("Spectrum analyzer destroyed");
}

bool spectrum_is_running(spectrum_analyzer_t* s) {
    if (!s) return false;
    return atomic_load(&s->running);
}
