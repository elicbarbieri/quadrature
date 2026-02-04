#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/quadrature_audio.h"
#include "quadrature/quadrature_library.h"

#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static pthread_once_t ffmpeg_init_once = PTHREAD_ONCE_INIT;
static void ffmpeg_init_internal(void) {
    avformat_network_init();
    g_message("FFmpeg initialized (%s)", av_version_info());
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Decoder Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t ffmpeg_decoder_open(ffmpeg_decoder_t* dec, const char* path, uint32_t rate) {
    pthread_once(&ffmpeg_init_once, ffmpeg_init_internal);

    memset(dec, 0, sizeof(*dec));
    dec->output_sample_rate = (int)rate;

    int ret;
    const AVCodec* codec = NULL;

    ret = avformat_open_input(&dec->fmt_ctx, path, NULL, NULL);
    if (ret < 0) {
        g_critical("Cannot open %s", path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    ret = avformat_find_stream_info(dec->fmt_ctx, NULL);
    if (ret < 0) goto fail;

    dec->stream_index = av_find_best_stream(dec->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (dec->stream_index < 0) goto fail;

    AVStream* stream = dec->fmt_ctx->streams[dec->stream_index];

    dec->codec_ctx = avcodec_alloc_context3(codec);
    if (!dec->codec_ctx) goto fail;

    ret = avcodec_parameters_to_context(dec->codec_ctx, stream->codecpar);
    if (ret < 0) goto fail;

    ret = avcodec_open2(dec->codec_ctx, codec, NULL);
    if (ret < 0) goto fail;

    dec->source_sample_rate = dec->codec_ctx->sample_rate;
    AVChannelLayout* ch_layout = &dec->codec_ctx->ch_layout;
    dec->output_channels = ch_layout->nb_channels;
    if (dec->output_channels == 0) dec->output_channels = 2;

    /* Resample to target rate and stereo */
    AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
    ret = swr_alloc_set_opts2(&dec->swr_ctx,
                              &stereo_layout, AV_SAMPLE_FMT_FLT, dec->output_sample_rate,
                              ch_layout, dec->codec_ctx->sample_fmt, dec->source_sample_rate,
                              0, NULL);
    if (ret < 0 || !dec->swr_ctx) goto fail;

    ret = swr_init(dec->swr_ctx);
    if (ret < 0) goto fail;

    /* Output is always stereo after resampling */
    dec->output_channels = 2;

    dec->frame = av_frame_alloc();
    dec->packet = av_packet_alloc();
    if (!dec->frame || !dec->packet) goto fail;

    dec->stream_start_time = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
    dec->time_base = av_q2d(stream->time_base);
    dec->eof = false;

    return QUADRATURE_OK;

fail:
    ffmpeg_decoder_close(dec);
    return QUADRATURE_ERROR_UNSUPPORTED_FORMAT;
}

int ffmpeg_decoder_read(ffmpeg_decoder_t* dec, float* buffer, size_t max_frames) {
    if (!dec->fmt_ctx || !dec->codec_ctx) return -1;

    int ret;

    while (1) {
        ret = avcodec_receive_frame(dec->codec_ctx, dec->frame);
        if (ret == 0) {
            int out = swr_convert(dec->swr_ctx,
                                  (uint8_t**)&buffer,
                                  (int)max_frames,
                                  (const uint8_t**)dec->frame->extended_data,
                                  dec->frame->nb_samples);
            av_frame_unref(dec->frame);
            if (out < 0) return -1;
            return out;
        } else if (ret == AVERROR(EAGAIN)) {
            /* need more packets */
        } else if (ret == AVERROR_EOF) {
            int out = swr_convert(dec->swr_ctx,
                                  (uint8_t**)&buffer,
                                  (int)max_frames,
                                  NULL, 0);
            if (out > 0) return out;
            dec->eof = true;
            return 0;
        } else {
            return -1;
        }

        ret = av_read_frame(dec->fmt_ctx, dec->packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                avcodec_send_packet(dec->codec_ctx, NULL);
                continue;
            }
            return -1;
        }

        if (dec->packet->stream_index != dec->stream_index) {
            av_packet_unref(dec->packet);
            continue;
        }

        ret = avcodec_send_packet(dec->codec_ctx, dec->packet);
        av_packet_unref(dec->packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) return -1;
    }
}

quadrature_result_t ffmpeg_decoder_seek(ffmpeg_decoder_t* dec, uint64_t position) {
    if (!dec->fmt_ctx || !dec->codec_ctx) return QUADRATURE_ERROR_INTERNAL;

    double target_time = (double)position / dec->output_sample_rate;
    int64_t ts = (int64_t)(target_time / dec->time_base) + dec->stream_start_time;

    if (av_seek_frame(dec->fmt_ctx, dec->stream_index, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        return QUADRATURE_ERROR_INTERNAL;
    }

    ffmpeg_decoder_flush(dec);
    return QUADRATURE_OK;
}

void ffmpeg_decoder_flush(ffmpeg_decoder_t* dec) {
    if (dec->codec_ctx) {
        avcodec_flush_buffers(dec->codec_ctx);
    }
    /* Reset resampler to flush its internal buffer */
    if (dec->swr_ctx) {
        swr_init(dec->swr_ctx);
    }
    dec->eof = false;
}

void ffmpeg_decoder_close(ffmpeg_decoder_t* dec) {
    if (dec->frame) { av_frame_free(&dec->frame); dec->frame = NULL; }
    if (dec->packet) { av_packet_free(&dec->packet); dec->packet = NULL; }
    if (dec->swr_ctx) { swr_free(&dec->swr_ctx); dec->swr_ctx = NULL; }
    if (dec->codec_ctx) { avcodec_free_context(&dec->codec_ctx); dec->codec_ctx = NULL; }
    if (dec->fmt_ctx) { avformat_close_input(&dec->fmt_ctx); dec->fmt_ctx = NULL; }
    dec->eof = false;
}

uint64_t ffmpeg_decoder_duration(ffmpeg_decoder_t* dec) {
    if (!dec->fmt_ctx) return 0;

    AVStream* stream = dec->fmt_ctx->streams[dec->stream_index];
    double duration = 0;

    if (stream->duration != AV_NOPTS_VALUE) {
        duration = stream->duration * dec->time_base;
    } else if (dec->fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration = dec->fmt_ctx->duration / (double)AV_TIME_BASE;
    }

    return (uint64_t)(duration * dec->output_sample_rate);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Unified Buffer Flush
 *
 * Coordinates flushing all audio buffers in a single call.
 * Call this after seek to prevent stale audio from playing.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void audio_player_flush_all(audio_player_t* p) {
    g_assert(p != NULL);
    g_assert(p->scrubber != NULL);
    g_assert(p->spectrum_buffer != NULL);

    audio_scrubber_flush(p->scrubber);
    spa_ringbuffer_init(&p->spectrum_rb);
    memset(p->spectrum_buffer, 0, SPECTRUM_RINGBUF_SIZE * sizeof(float));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Metering Helper
 * ═══════════════════════════════════════════════════════════════════════════ */

void meter_accum_store(meter_accum_t* m, audio_player_t* player) {
    if (m->frame_count == 0) return;

    atomic_store(&player->peak_left, m->peak_left);
    atomic_store(&player->peak_right, m->peak_right);
    atomic_store(&player->rms_left, sqrtf(m->sum_sq_left / m->frame_count));
    atomic_store(&player->rms_right, sqrtf(m->sum_sq_right / m->frame_count));

    /* Update peak hold if new peak is higher (sample-based timing, no syscall) */
    float hold_l = atomic_load(&player->peak_hold_left);
    float hold_r = atomic_load(&player->peak_hold_right);

    bool new_peak = false;
    if (m->peak_left > hold_l) {
        atomic_store(&player->peak_hold_left, m->peak_left);
        new_peak = true;
    }
    if (m->peak_right > hold_r) {
        atomic_store(&player->peak_hold_right, m->peak_right);
        new_peak = true;
    }

    /* Reset age counter on new peak, otherwise increment by frame count */
    if (new_peak) {
        atomic_store(&player->peak_hold_age_frames, 0);
    } else {
        atomic_fetch_add(&player->peak_hold_age_frames, m->frame_count);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Position Snapshot Helper
 *
 * Updates the position snapshot for UI interpolation using seqlock pattern.
 * Called from audio callback (after processing) and state-change functions
 * (for immediate UI feedback without waiting for next callback).
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Update position snapshot for UI interpolation using seqlock pattern.
 *
 * When called from audio callback: pass current sample_count from callback_sample_count
 * When called from UI thread with PipeWire lock: pass 0 for sample_count (will read atomic)
 */
static inline void player_update_position_snap(audio_player_t* p, uint64_t position,
                                                float speed, bool playing,
                                                uint64_t sample_count) {
    uint32_t seq = atomic_load_explicit(&p->position_seq, memory_order_relaxed);
    atomic_store_explicit(&p->position_seq, seq + 1, memory_order_release);

    p->position_snap.position = position;
    /* RT-safe: use passed sample_count if provided, else read atomic (for UI thread calls) */
    p->position_snap.sample_count = sample_count ? sample_count :
        atomic_load_explicit(&p->callback_sample_count, memory_order_relaxed);
    p->position_snap.speed = speed;
    p->position_snap.playing = playing ? 1 : 0;

    atomic_store_explicit(&p->position_seq, seq + 2, memory_order_release);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Processing - Buffer-Based Playback
 *
 * Unified playback through rate processor. Speed=1.0 is normal playback.
 * All playback uses pre-decoded buffers - no streaming fallback.
 * ═══════════════════════════════════════════════════════════════════════════ */

static size_t process_buffer_audio(audio_player_t* p, float* out, uint32_t frame_count,
                                    meter_accum_t* meter) {
    audio_buffer_t* buffer = atomic_load_explicit(&p->buffer, memory_order_acquire);
    if (!buffer) return 0;

    const float* samples = audio_buffer_get_samples(buffer);
    uint64_t num_frames = audio_buffer_get_num_frames(buffer);
    if (!samples || num_frames == 0) return 0;

    /* Scrubber is required - if missing, channel is broken */
    if (!p->scrubber) {
        atomic_store(&p->state, CHANNEL_ERROR);
        memset(out, 0, frame_count * 2 * sizeof(float));
        return 0;
    }

    /* Sync scrubber position with player position */
    audio_scrubber_set_position(p->scrubber, (int64_t)p->current_frame);

    /* Process through scrubber (handles all speeds including 1.0x) */
    uint64_t new_pos;
    audio_scrubber_process(p->scrubber, samples, num_frames,
                           out, frame_count, &new_pos);
    p->current_frame = new_pos;

    /* Check for end of track */
    if (new_pos >= num_frames - 1) {
        if (atomic_load(&p->repeat)) {
            /* Repeat mode: restart current track */
            p->current_frame = 0;
            audio_scrubber_set_position(p->scrubber, 0);
        } else {
            /* Auto-advance: try to swap to preloaded next buffer (RT-safe: atomics only) */
            audio_buffer_t* next = atomic_load_explicit(&p->next_buffer, memory_order_acquire);
            if (next) {
                /* Store old track ID for deferred cleanup */
                int64_t old_track_id = atomic_load(&p->current_track_id);
                int64_t next_track_id = atomic_load(&p->next_track_id);

                /* Swap buffers (atomic) */
                atomic_store_explicit(&p->buffer, next, memory_order_release);
                atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);

                /* Update track IDs */
                atomic_store(&p->current_track_id, next_track_id);
                atomic_store(&p->next_track_id, 0);

                /* Reset playback position */
                p->current_frame = 0;
                atomic_store(&p->length_samples, audio_buffer_get_num_frames(next));
                audio_scrubber_set_position(p->scrubber, 0);

                /* Signal for deferred cleanup (unlock old track, preload new next) */
                atomic_store(&p->advance_old_track_id, old_track_id);
                atomic_store(&p->advance_pending, true);
                atomic_fetch_add_explicit(&p->stats_instant_advances, 1, memory_order_relaxed);
                
                /* Record instant advance in perf dashboard */
                if (p->pipeline && p->pipeline->perf) {
                    perf_record_track_advance(p->pipeline->perf, p->player_id, true);
                }

                /* Check autoplay: if disabled, stop after advancing */
                if (!atomic_load(&p->autoplay)) {
                    atomic_store(&p->state, CHANNEL_STOPPED);
                }
                /* Otherwise stay in PLAYING state - continue playback */
            } else {
                /* No preloaded buffer - check if next track exists */
                int64_t next_track_id = atomic_load(&p->next_track_id);
                if (next_track_id > 0) {
                    /* Next track exists.
                     * Clear buffer so callback outputs silence and stops hitting end-of-track.
                     * Signal main thread to call set_player_track(next_track_id). */
                    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
                    atomic_store(&p->advance_pending, true);
                    atomic_fetch_add_explicit(&p->stats_deferred_advances, 1, memory_order_relaxed);

                    /* Record deferred advance in perf dashboard */
                    if (p->pipeline && p->pipeline->perf) {
                        perf_record_track_advance(p->pipeline->perf, p->player_id, false);
                    }

                    /* If autoplay disabled, stop on the new track at 0:00 */
                    if (!atomic_load(&p->autoplay)) {
                        atomic_store(&p->state, CHANNEL_STOPPED);
                    }
                    /* Otherwise stay in PLAYING - main thread will load the track */
                } else {
                    /* No next track - stop */
                    atomic_store(&p->state, CHANNEL_STOPPED);
                }
            }
        }
    }

    /* Accumulate meters */
    for (uint32_t i = 0; i < frame_count; i++) {
        meter_accum_process(meter, out[i * 2], out[i * 2 + 1]);
    }

    return frame_count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Process Callback
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_process(void* userdata) {
    audio_player_t* p = (audio_player_t*)userdata;

    /* RT-safe timing: VDSO-mapped clock_gettime (~20ns) */
    uint64_t cb_start = time_ns();

    struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
    if (!b) {
        atomic_fetch_add_explicit(&p->stats_dequeue_failures, 1, memory_order_relaxed);
        if (p->pipeline && p->pipeline->perf) {
            perf_record_dequeue_failure(p->pipeline->perf, p->player_id);
        }
        return;
    }

    float* out = (float*)b->buffer->datas[0].data;
    if (!out) {
        pw_stream_queue_buffer(p->stream, b);
        return;
    }
    uint32_t max_frames = b->buffer->datas[0].maxsize / (sizeof(float) * 2);
    uint32_t frame_count = SPA_MIN(b->requested, max_frames);

    /* Read current state */
    channel_state_t state = atomic_load(&p->state);
    float scrub_speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
    bool scrubbing = (fabsf(scrub_speed - 1.0f) > 0.01f);

    /* Increment sample counter and get current value (RT-safe timestamp) */
    uint64_t sample_count = atomic_fetch_add(&p->callback_sample_count, frame_count);

    /* Process audio when:
     * - PLAYING state, OR
     * - Actively scrubbing (speed != 1.0) regardless of play state
     * STOPPED state always outputs silence - stop means stop */
    audio_buffer_t* buf = atomic_load_explicit(&p->buffer, memory_order_acquire);
    bool should_play = buf && (state == CHANNEL_PLAYING || scrubbing);

    /* Per-player stats (RT-safe: relaxed atomics) */
    atomic_fetch_add_explicit(&p->stats_cb_count, 1, memory_order_relaxed);

    /* Legacy pipeline-level + perf dashboard (backward compat) */
    if (p->pipeline) {
        atomic_fetch_add(&p->pipeline->stats_callback_count, 1);
        if (state == CHANNEL_PLAYING && !buf) {
            atomic_fetch_add(&p->pipeline->stats_underrun_count, 1);
        }
        if (p->pipeline->perf) {
            perf_record_callback(p->pipeline->perf, p->player_id, sample_count);
            if (state == CHANNEL_PLAYING && !buf) {
                perf_record_underrun(p->pipeline->perf, p->player_id);
            }
        }
    }

    if (should_play) {
        /* Initialize metering accumulator */
        meter_accum_t meter;
        meter_accum_init(&meter);

        /* Process audio from buffer */
        size_t metered_frames = process_buffer_audio(p, out, frame_count, &meter);

        /* Store metering values */
        if (metered_frames > 0) {
            meter_accum_store(&meter, p);

            /* Feed spectrum analyzer ring buffer (non-blocking, lock-free) */
            if (p->spectrum_buffer) {
                uint32_t index;
                int32_t avail = spa_ringbuffer_get_write_index(&p->spectrum_rb, &index);
                uint32_t to_write = (uint32_t)(metered_frames * 2);

                int32_t space = SPECTRUM_RINGBUF_SIZE - avail;
                if ((int32_t)to_write <= space) {
                    spa_ringbuffer_write_data(&p->spectrum_rb, p->spectrum_buffer,
                        SPECTRUM_RINGBUF_SIZE, index % SPECTRUM_RINGBUF_SIZE,
                        out, to_write * sizeof(float));
                    spa_ringbuffer_write_update(&p->spectrum_rb, index + to_write);
                }
            }
        }

        atomic_store(&p->position_samples, p->current_frame);
    } else {
        memset(out, 0, frame_count * 2 * sizeof(float));
    }

    /* ALWAYS update snapshot - callback is the single owner when running
     * State-change functions take the PipeWire lock to synchronize with this */
    player_update_position_snap(p,
        atomic_load(&p->position_samples),
        scrub_speed,
        state == CHANNEL_PLAYING,
        sample_count);

    /* Callback timing */
    uint64_t cb_elapsed = time_ns() - cb_start;
    atomic_fetch_add_explicit(&p->stats_cb_time_sum_ns, cb_elapsed, memory_order_relaxed);

    /* Update peak (CAS loop) */
    uint64_t cur_max = atomic_load_explicit(&p->stats_cb_time_max_ns, memory_order_relaxed);
    while (cb_elapsed > cur_max) {
        if (atomic_compare_exchange_weak_explicit(&p->stats_cb_time_max_ns,
                &cur_max, cb_elapsed, memory_order_relaxed, memory_order_relaxed))
            break;
    }

    /* Budget overrun check (50% of period) */
    if (p->pipeline) {
        uint64_t half_budget = (uint64_t)frame_count * 500000000ULL / p->pipeline->sample_rate;
        if (cb_elapsed > half_budget) {
            atomic_fetch_add_explicit(&p->stats_budget_overruns, 1, memory_order_relaxed);
        }
    }

    /* Feed perf dashboard: per-player histogram + fix legacy broken timing */
    if (p->pipeline && p->pipeline->perf) {
        perf_record_callback_time(p->pipeline->perf, p->player_id, cb_elapsed, frame_count);
    }
    if (p->pipeline) {
        uint64_t us = cb_elapsed / 1000;
        atomic_fetch_add(&p->pipeline->stats_callback_time_sum_us, us);
        uint64_t pmax = atomic_load(&p->pipeline->stats_callback_time_max_us);
        while (us > pmax) {
            if (atomic_compare_exchange_weak(&p->pipeline->stats_callback_time_max_us, &pmax, us))
                break;
        }
    }

    b->buffer->datas[0].chunk->offset = 0;
    b->buffer->datas[0].chunk->stride = sizeof(float) * 2;
    b->buffer->datas[0].chunk->size = frame_count * sizeof(float) * 2;
    pw_stream_queue_buffer(p->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Stream Creation Helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static quadrature_result_t create_player_stream(audio_player_t* p,
                                                 uint32_t sample_rate,
                                                 struct pw_thread_loop* loop) {
    char stream_name[64];
    snprintf(stream_name, sizeof(stream_name), "quadrature-player-%d", p->player_id);

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_LATENCY, "512/48000",
        NULL);

    if (p->target_device[0]) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, p->target_device);
    }

    p->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(loop),
        stream_name,
        props,
        &stream_events,
        p);

    if (!p->stream) {
        return QUADRATURE_ERROR_INTERNAL;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = 2,
            .rate = sample_rate
        ));

    int res = pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS,
        params, 1);

    if (res < 0) {
        pw_stream_destroy(p->stream);
        p->stream = NULL;
        return QUADRATURE_ERROR_INTERNAL;
    }

    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static quadrature_result_t player_init(audio_player_t* p, int id, uint32_t sample_rate,
                                        struct pw_thread_loop* loop, const char* target_device) {
    memset(p, 0, sizeof(*p));
    p->player_id = id;

    /* Allocate spectrum ring buffer */
    p->spectrum_buffer = malloc(SPECTRUM_RINGBUF_SIZE * sizeof(float));
    if (!p->spectrum_buffer) {
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }
    spa_ringbuffer_init(&p->spectrum_rb);

    atomic_store(&p->state, CHANNEL_STOPPED);

    /* Initialize playback options */
    atomic_store(&p->repeat, false);
    atomic_store(&p->autoplay, true);  /* Auto-continue on track advance */

    /* Initialize track_id state */
    atomic_store(&p->current_track_id, 0);
    atomic_store(&p->next_track_id, 0);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);
    atomic_store(&p->advance_pending, false);
    atomic_store(&p->advance_old_track_id, 0);
    atomic_store(&p->pending_buffer_track_id, 0);

    /* Initialize buffer state */
    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    p->current_frame = 0;

    /* Create rate processor for variable-speed playback */
    quadrature_result_t scrub_res = audio_scrubber_create(sample_rate, &p->scrubber);
    if (scrub_res != QUADRATURE_OK) {
        free(p->spectrum_buffer);
        return scrub_res;
    }

    /* Initialize position snapshot (seqlock pattern) */
    atomic_store(&p->position_seq, 0);
    memset(&p->position_snap, 0, sizeof(p->position_snap));

    /* Initialize sample counter for RT-safe timestamps */
    atomic_store(&p->callback_sample_count, 0);

    /* Initialize metering atomics */
    atomic_store(&p->peak_left, 0.0f);
    atomic_store(&p->peak_right, 0.0f);
    atomic_store(&p->rms_left, 0.0f);
    atomic_store(&p->rms_right, 0.0f);
    atomic_store(&p->peak_hold_left, 0.0f);
    atomic_store(&p->peak_hold_right, 0.0f);
    atomic_store(&p->peak_hold_age_frames, 0);

    /* Initialize per-player stats */
    atomic_store(&p->stats_cb_count, 0);
    atomic_store(&p->stats_cb_time_sum_ns, 0);
    atomic_store(&p->stats_cb_time_max_ns, 0);
    atomic_store(&p->stats_budget_overruns, 0);
    atomic_store(&p->stats_dequeue_failures, 0);
    atomic_store(&p->stats_deferred_advances, 0);
    atomic_store(&p->stats_instant_advances, 0);

    /* Initialize spectrum bars (stereo: left + right) */
    for (int i = 0; i < SPECTRUM_BARS * 2; i++) {
        atomic_store(&p->spectrum_bars[i], 0.0f);
    }

    /* Store target device */
    if (target_device && target_device[0]) {
        strncpy(p->target_device, target_device, sizeof(p->target_device) - 1);
        p->target_device[sizeof(p->target_device) - 1] = '\0';
    } else {
        p->target_device[0] = '\0';
    }

    /* Create PipeWire stream */
    quadrature_result_t stream_res = create_player_stream(p, sample_rate, loop);
    if (stream_res != QUADRATURE_OK) {
        if (p->scrubber) {
            audio_scrubber_destroy(p->scrubber);
            p->scrubber = NULL;
        }
        free(p->spectrum_buffer);
        return stream_res;
    }

    if (p->target_device[0]) {
        g_message("Player %d initialized (%uHz, device: %s)", id, sample_rate, p->target_device);
    } else {
        g_message("Player %d initialized (%uHz)", id, sample_rate);
    }
    return QUADRATURE_OK;
}

static void player_destroy(audio_player_t* p, audio_cache_t* cache) {
    if (p->stream) {
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }
    /* Clear buffer pointers */
    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);

    /* Unlock any locked tracks */
    int64_t current_id = atomic_load(&p->current_track_id);
    int64_t next_id = atomic_load(&p->next_track_id);
    if (cache && current_id > 0) {
        audio_cache_unlock(cache, current_id);
    }
    if (cache && next_id > 0) {
        audio_cache_unlock(cache, next_id);
    }
    free(p->spectrum_buffer);
    p->spectrum_buffer = NULL;
    if (p->scrubber) {
        audio_scrubber_destroy(p->scrubber);
        p->scrubber = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper to recreate a player's stream with a new target device
 * ═══════════════════════════════════════════════════════════════════════════ */

static quadrature_result_t player_recreate_stream(audio_player_t* p, uint32_t sample_rate,
                                                   struct pw_thread_loop* loop, const char* target_device) {
    channel_state_t prev_state = atomic_load(&p->state);
    bool was_playing = (prev_state == CHANNEL_PLAYING);

    if (was_playing) {
        atomic_store(&p->state, CHANNEL_PAUSED);
    }

    if (p->stream) {
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }

    /* Update target device */
    if (target_device && target_device[0]) {
        strncpy(p->target_device, target_device, sizeof(p->target_device) - 1);
        p->target_device[sizeof(p->target_device) - 1] = '\0';
        g_message("Player %d retargeting to device: %s", p->player_id, p->target_device);
    } else {
        p->target_device[0] = '\0';
        g_message("Player %d retargeting to default device", p->player_id);
    }

    /* Create new stream using shared helper */
    quadrature_result_t res = create_player_stream(p, sample_rate, loop);

    if (res == QUADRATURE_OK && was_playing) {
        atomic_store(&p->state, CHANNEL_PLAYING);
    }

    return res;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Pipeline Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declaration for auto-advance timeout */
static gboolean advance_timeout_callback(gpointer user_data);

quadrature_result_t audio_pipeline_create(library_cache_t* library,
                                           uint32_t sample_rate,
                                           audio_pipeline_t** pipeline) {
    if (!pipeline) return QUADRATURE_ERROR_INVALID_PARAM;

    pw_init(NULL, NULL);

    *pipeline = calloc(1, sizeof(audio_pipeline_t));
    if (!*pipeline) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    (*pipeline)->sample_rate = sample_rate;
    (*pipeline)->library = library;
    (*pipeline)->track_changed_callback = NULL;
    (*pipeline)->track_changed_user_data = NULL;
    (*pipeline)->advance_timeout_id = 0;

    /* Initialize stats */
    atomic_store(&(*pipeline)->stats_callback_count, 0);
    atomic_store(&(*pipeline)->stats_underrun_count, 0);
    atomic_store(&(*pipeline)->stats_callback_time_sum_us, 0);
    atomic_store(&(*pipeline)->stats_callback_time_max_us, 0);
    atomic_store(&(*pipeline)->stats_track_changes, 0);
    atomic_store(&(*pipeline)->stats_instant_advances, 0);

    (*pipeline)->loop = pw_thread_loop_new("quadrature", NULL);
    if (!(*pipeline)->loop) {
        free(*pipeline);
        *pipeline = NULL;
        return QUADRATURE_ERROR_INTERNAL;
    }

    (*pipeline)->pw_ctx = pw_context_new(pw_thread_loop_get_loop((*pipeline)->loop), NULL, 0);
    if (!(*pipeline)->pw_ctx) {
        pw_thread_loop_destroy((*pipeline)->loop);
        free(*pipeline);
        *pipeline = NULL;
        return QUADRATURE_ERROR_INTERNAL;
    }

    (*pipeline)->core = pw_context_connect((*pipeline)->pw_ctx, NULL, 0);
    if (!(*pipeline)->core) {
        pw_context_destroy((*pipeline)->pw_ctx);
        pw_thread_loop_destroy((*pipeline)->loop);
        free(*pipeline);
        *pipeline = NULL;
        return QUADRATURE_ERROR_INTERNAL;
    }

    atomic_store(&(*pipeline)->system_active, false);

    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++) {
        quadrature_result_t r = player_init(&(*pipeline)->players[i], i, sample_rate, (*pipeline)->loop, NULL);
        if (r != QUADRATURE_OK) {
            for (int j = 0; j < i; j++) player_destroy(&(*pipeline)->players[j], NULL);
            pw_core_disconnect((*pipeline)->core);
            pw_context_destroy((*pipeline)->pw_ctx);
            pw_thread_loop_destroy((*pipeline)->loop);
            free(*pipeline);
            *pipeline = NULL;
            return r;
        }
    }

    /* Create spectrum analyzer */
#ifndef QUADRATURE_DISABLE_SPECTRUM
    (*pipeline)->spectrum = spectrum_create(SPECTRUM_BARS, sample_rate, MAX_AUDIO_PLAYERS,
                                             (*pipeline)->players);
    if (!(*pipeline)->spectrum) {
        g_warning("Spectrum analyzer creation failed - continuing without spectrum");
    }
#else
    (*pipeline)->spectrum = NULL;
#endif

    /* Create audio cache (requires library for track_id resolution) */
    quadrature_result_t cache_result = audio_cache_create(library, sample_rate, &(*pipeline)->cache);
    if (cache_result != QUADRATURE_OK) {
        g_warning("Audio cache creation failed - continuing without cache");
        (*pipeline)->cache = NULL;
    }

    /* Create performance dashboard */
    quadrature_result_t perf_result = perf_dashboard_create(sample_rate, &(*pipeline)->perf);
    if (perf_result != QUADRATURE_OK) {
        g_warning("Performance dashboard creation failed - continuing without perf");
        (*pipeline)->perf = NULL;
    }

    /* Set player back-references */
    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++) {
        (*pipeline)->players[i].pipeline = *pipeline;
    }

    g_message("Pipeline created (%d players, %uHz)", MAX_AUDIO_PLAYERS, sample_rate);
    return QUADRATURE_OK;
}

void audio_pipeline_destroy(audio_pipeline_t* pipeline) {
    if (!pipeline) return;

    if (atomic_load(&pipeline->system_active)) {
        pw_thread_loop_stop(pipeline->loop);
        atomic_store(&pipeline->system_active, false);
    }

    if (pipeline->spectrum) {
        spectrum_destroy(pipeline->spectrum);
        pipeline->spectrum = NULL;
    }

    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++) {
        player_destroy(&pipeline->players[i], pipeline->cache);
    }

    if (pipeline->cache) {
        audio_cache_destroy(pipeline->cache);
        pipeline->cache = NULL;
    }

    if (pipeline->perf) {
        perf_dashboard_destroy(pipeline->perf);
        pipeline->perf = NULL;
    }

    if (pipeline->core) pw_core_disconnect(pipeline->core);
    if (pipeline->pw_ctx) pw_context_destroy(pipeline->pw_ctx);
    if (pipeline->loop) pw_thread_loop_destroy(pipeline->loop);

    pw_deinit();

    free(pipeline);
    g_message("Pipeline destroyed");
}

quadrature_result_t audio_pipeline_start(audio_pipeline_t* pipeline) {
    if (!pipeline) return QUADRATURE_ERROR_INVALID_PARAM;

    if (pw_thread_loop_start(pipeline->loop) < 0) {
        return QUADRATURE_ERROR_INTERNAL;
    }

    atomic_store(&pipeline->system_active, true);

    /* Start auto-advance timeout (50ms interval on main thread) */
    pipeline->advance_timeout_id = g_timeout_add(50, advance_timeout_callback, pipeline);

    g_message("Pipeline started");
    return QUADRATURE_OK;
}

quadrature_result_t audio_pipeline_stop(audio_pipeline_t* pipeline) {
    if (!pipeline) return QUADRATURE_ERROR_INVALID_PARAM;

    /* Stop auto-advance timeout */
    if (pipeline->advance_timeout_id > 0) {
        g_source_remove(pipeline->advance_timeout_id);
        pipeline->advance_timeout_id = 0;
    }

    pw_thread_loop_stop(pipeline->loop);

    atomic_store(&pipeline->system_active, false);
    g_message("Pipeline stopped");
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Control
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline bool valid_player(int id) { return id >= 0 && id < MAX_AUDIO_PLAYERS; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Track ID Based Player Control (New API)
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_set_player_track(audio_pipeline_t* pipeline,
                                                     int player_id,
                                                     int64_t track_id) {
    if (!pipeline || !valid_player(player_id) || track_id <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (!pipeline->cache) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    audio_player_t* p = &pipeline->players[player_id];

    /* PRECONDITION: Track must already be loaded into cache (caller's responsibility).
     * This allows set_player_track() to be non-blocking. */
    audio_cache_status_t status = audio_cache_get_status(pipeline->cache, track_id);
    if (status == AUDIO_CACHE_NOT_FOUND) {
        g_error("audio_pipeline_set_player_track: track %" G_GINT64_FORMAT " not in cache - "
                "call audio_cache_load() first", track_id);
    }

    /* Get old track IDs for cleanup */
    int64_t old_current_id = atomic_load(&p->current_track_id);
    int64_t old_next_id = atomic_load(&p->next_track_id);

    /* Clear buffer pointers - audio callback outputs silence when buffer is NULL */
    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);
    atomic_store(&p->pending_buffer_track_id, 0);
    atomic_thread_fence(memory_order_seq_cst);

    if (old_current_id > 0) audio_cache_unlock_delayed(pipeline->cache, old_current_id);
    if (old_next_id > 0) audio_cache_unlock_delayed(pipeline->cache, old_next_id);

    /* Reset state */
    p->current_frame = 0;
    atomic_store(&p->current_track_id, track_id);
    atomic_store(&p->next_track_id, 0);
    atomic_store(&p->position_samples, 0);
    audio_scrubber_flush(p->scrubber);

    /* Store path for display (get from library cache) */
    if (pipeline->library) {
        const library_track_info_t* info = library_cache_get_track(pipeline->library, track_id);
        if (info && info->path) {
            strncpy(p->filepath, info->path, MAX_FILENAME_LENGTH - 1);
            p->filepath[MAX_FILENAME_LENGTH - 1] = '\0';
        }
    }

    /* Lock the track (already in cache due to precondition) */
    audio_cache_lock_result_t lock_result = audio_cache_lock(pipeline->cache, track_id);

    if (lock_result == AUDIO_CACHE_LOCK_FAILED) {
        g_warning("Failed to lock track %" G_GINT64_FORMAT " - decode failed", track_id);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (lock_result == AUDIO_CACHE_LOCK_READY) {
        /* Already decoded - set buffer immediately */
        audio_buffer_t* buf = audio_cache_get_locked(pipeline->cache, track_id);
        if (!buf) {
            g_warning("Buffer unavailable for track %" G_GINT64_FORMAT " despite READY status", track_id);
            audio_cache_unlock(pipeline->cache, track_id);
            return QUADRATURE_ERROR_INTERNAL;
        }

        atomic_store_explicit(&p->buffer, buf, memory_order_release);
        atomic_store(&p->length_samples, audio_buffer_get_num_frames(buf));

        /* Update snapshot with PipeWire lock to synchronize with callback */
        pw_thread_loop_lock(pipeline->loop);
        float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
        player_update_position_snap(p, 0, speed, false, 0);
        pw_thread_loop_unlock(pipeline->loop);

        /* Fire callback immediately */
        if (pipeline->track_changed_callback) {
            pipeline->track_changed_callback(player_id, track_id, pipeline->track_changed_user_data);
        }

        /* Preload NEXT track (async - this is where set_player_track does call load) */
        if (!atomic_load(&p->repeat) && pipeline->library) {
            int64_t next_id = library_cache_get_next_track_id(pipeline->library, track_id);
            if (next_id > 0) {
                atomic_store(&p->next_track_id, next_id);
                audio_cache_load(pipeline->cache, next_id);
                audio_cache_lock_result_t next_result = audio_cache_lock(pipeline->cache, next_id);
                /* Only get buffer if already ready - don't block for preload */
                if (next_result == AUDIO_CACHE_LOCK_READY) {
                    audio_buffer_t* next_buf = audio_cache_get_locked(pipeline->cache, next_id);
                    if (next_buf) {
                        atomic_store_explicit(&p->next_buffer, next_buf, memory_order_release);
                    }
                }
            }
        }
    } else {
        /* LOADING - mark pending for 50ms timeout to check */
        atomic_store(&p->pending_buffer_track_id, track_id);
        g_debug("Player %d: track %" G_GINT64_FORMAT " still decoding, will poll", player_id, track_id);

        /* Still preload next track (async) */
        if (!atomic_load(&p->repeat) && pipeline->library) {
            int64_t next_id = library_cache_get_next_track_id(pipeline->library, track_id);
            if (next_id > 0) {
                atomic_store(&p->next_track_id, next_id);
                audio_cache_load(pipeline->cache, next_id);
                audio_cache_lock(pipeline->cache, next_id);
            }
        }
    }

    return QUADRATURE_OK;
}

int64_t audio_pipeline_get_player_track_id(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return 0;
    return atomic_load(&pipeline->players[player_id].current_track_id);
}

void audio_pipeline_set_track_changed_callback(audio_pipeline_t* pipeline,
                                                audio_track_changed_cb callback,
                                                void* user_data) {
    g_assert(pipeline != NULL);
    pipeline->track_changed_callback = callback;
    pipeline->track_changed_user_data = user_data;
}

/**
 * Internal: Process pending buffer loads and auto-advance actions.
 * Called from GLib timeout on the main thread every 50ms.
 *
 * Handles three cases:
 * 1. Pending buffer: Track was set but decode wasn't complete - poll for completion
 * 2. Instant advance: Buffer swap happened in RT callback, old_track_id > 0
 *    - Just cleanup old track and preload new next
 * 3. Deferred advance: Buffer wasn't ready, old_track_id = 0, buffer = NULL
 *    - Call set_player_track(next_track_id) which handles everything
 */
static void process_pending_advances_internal(audio_pipeline_t* pipeline) {
    g_assert(pipeline != NULL);

    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++) {
        audio_player_t* p = &pipeline->players[i];

        /* Check for pending buffer loads (non-blocking track set waiting for decode) */
        int64_t pending_id = atomic_load(&p->pending_buffer_track_id);
        if (pending_id > 0 && pipeline->cache) {
            audio_cache_status_t status = audio_cache_get_status(pipeline->cache, pending_id);

            if (status == AUDIO_CACHE_READY) {
                audio_buffer_t* buf = audio_cache_get_locked(pipeline->cache, pending_id);
                if (buf) {
                    atomic_store_explicit(&p->buffer, buf, memory_order_release);
                    atomic_store(&p->length_samples, audio_buffer_get_num_frames(buf));
                    atomic_store(&p->pending_buffer_track_id, 0);

                    g_debug("Player %d: track %" G_GINT64_FORMAT " decode complete, buffer attached",
                            i, pending_id);

                    /* Update snapshot with PipeWire lock */
                    pw_thread_loop_lock(pipeline->loop);
                    float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
                    player_update_position_snap(p, 0, speed, false, 0);
                    pw_thread_loop_unlock(pipeline->loop);

                    /* Fire callback */
                    if (pipeline->track_changed_callback) {
                        pipeline->track_changed_callback(i, pending_id, pipeline->track_changed_user_data);
                    }

                    /* Try to attach next buffer if it's ready now */
                    int64_t next_id = atomic_load(&p->next_track_id);
                    if (next_id > 0) {
                        audio_cache_status_t next_status = audio_cache_get_status(pipeline->cache, next_id);
                        if (next_status == AUDIO_CACHE_READY) {
                            audio_buffer_t* next_buf = audio_cache_get_locked(pipeline->cache, next_id);
                            if (next_buf) {
                                atomic_store_explicit(&p->next_buffer, next_buf, memory_order_release);
                            }
                        }
                    }
                }
            } else if (status == AUDIO_CACHE_FAILED) {
                g_warning("Player %d: track %" G_GINT64_FORMAT " decode failed", i, pending_id);
                atomic_store(&p->pending_buffer_track_id, 0);
                audio_cache_unlock(pipeline->cache, pending_id);
            }
            /* LOADING: continue polling next iteration */
        }

        /* Check for pending auto-advances */
        if (!atomic_load(&p->advance_pending)) continue;

        /* Clear pending flag */
        atomic_store(&p->advance_pending, false);

        /* Check if this is instant or deferred advance */
        int64_t old_track_id = atomic_exchange(&p->advance_old_track_id, 0);
        audio_buffer_t* current_buf = atomic_load_explicit(&p->buffer, memory_order_acquire);

        if (old_track_id == 0 && !current_buf) {
            /* Deferred advance: buffer wasn't preloaded, need to load and set */
            int64_t next_id = atomic_load(&p->next_track_id);
            if (next_id > 0) {
                g_debug("Player %d: deferred advance to track %" G_GINT64_FORMAT, i, next_id);
                /* Ensure track is loaded before calling set_player_track */
                audio_cache_load(pipeline->cache, next_id);
                audio_pipeline_set_player_track(pipeline, i, next_id);
            }
            continue;
        }

        /* Instant advance: buffer swap already happened in RT callback */
        int64_t current_id = atomic_load(&p->current_track_id);

        /* Unlock old track with delay for safe buffer transition */
        if (pipeline->cache && old_track_id > 0) {
            audio_cache_unlock_delayed(pipeline->cache, old_track_id);
        }

        /* Preload new next track */
        if (!atomic_load(&p->repeat) && pipeline->library && pipeline->cache) {
            int64_t new_next_id = library_cache_get_next_track_id(pipeline->library, current_id);
            if (new_next_id > 0) {
                atomic_store(&p->next_track_id, new_next_id);
                audio_cache_load(pipeline->cache, new_next_id);
                audio_cache_lock(pipeline->cache, new_next_id);
                /* Try to get buffer for next time */
                audio_buffer_t* next_buf = audio_cache_get_locked(pipeline->cache, new_next_id);
                if (next_buf) {
                    atomic_store_explicit(&p->next_buffer, next_buf, memory_order_release);
                }
            }
        }

        /* Track statistics */
        atomic_fetch_add(&pipeline->stats_track_changes, 1);
        atomic_fetch_add(&pipeline->stats_instant_advances, 1);

        /* Fire callback on main thread */
        if (pipeline->track_changed_callback) {
            pipeline->track_changed_callback(i, current_id, pipeline->track_changed_user_data);
        }
    }
}

/**
 * GLib timeout callback for auto-advance processing.
 * Runs every 50ms on the main thread while pipeline is active.
 */
static gboolean advance_timeout_callback(gpointer user_data) {
    audio_pipeline_t* pipeline = (audio_pipeline_t*)user_data;
    g_assert(pipeline != NULL);
    if (!atomic_load(&pipeline->system_active)) {
        return G_SOURCE_REMOVE;
    }
    process_pending_advances_internal(pipeline);

    /* Sample budget utilization every ~1 second (20 * 50ms) */
    static unsigned int budget_sample_counter = 0;
    if (++budget_sample_counter >= 20 && pipeline->perf) {
        perf_sample_budget_utilization(pipeline->perf);
        budget_sample_counter = 0;
    }

    return G_SOURCE_CONTINUE;
}

quadrature_result_t audio_pipeline_player_play(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];

    /* Atomic state transition: STOPPED or PAUSED → PLAYING */
    int expected = CHANNEL_STOPPED;
    if (atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PLAYING)) {
        pw_thread_loop_lock(pipeline->loop);
        float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
        player_update_position_snap(p, atomic_load(&p->position_samples), speed, true, 0);
        pw_thread_loop_unlock(pipeline->loop);
        g_message("Player %d playing (from stopped)", player_id);
        return QUADRATURE_OK;
    }

    expected = CHANNEL_PAUSED;
    if (atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PLAYING)) {
        pw_thread_loop_lock(pipeline->loop);
        float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
        player_update_position_snap(p, atomic_load(&p->position_samples), speed, true, 0);
        pw_thread_loop_unlock(pipeline->loop);
        g_message("Player %d playing (from paused)", player_id);
        return QUADRATURE_OK;
    }

    /* Already playing or in invalid state */
    int current = atomic_load(&p->state);
    if (current == CHANNEL_PLAYING) {
        return QUADRATURE_OK;  /* Already playing - success */
    }

    g_debug("Player %d play failed: state=%d", player_id, current);
    return QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t audio_pipeline_player_stop(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];

    /* Take PipeWire lock for synchronized state change + flush + snapshot */
    pw_thread_loop_lock(pipeline->loop);

    int prev = atomic_exchange(&p->state, CHANNEL_STOPPED);

    /* Reset position and flush buffers */
    p->current_frame = 0;
    audio_player_flush_all(p);
    atomic_store(&p->position_samples, 0);

    /* Update snapshot while holding lock to synchronize with callback */
    float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
    player_update_position_snap(p, 0, speed, false, 0);

    pw_thread_loop_unlock(pipeline->loop);

    if (prev != CHANNEL_STOPPED) {
        g_message("Player %d stopped", player_id);
    }
    return QUADRATURE_OK;
}

quadrature_result_t audio_pipeline_player_toggle_play(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];

    /* Check current state and toggle atomically */
    int current = atomic_load(&p->state);
    int64_t track_id = atomic_load(&p->current_track_id);
    g_debug("Player %d toggle_play: state=%d, track_id=%" G_GINT64_FORMAT, player_id, current, track_id);

    if (current == CHANNEL_PLAYING) {
        /* Pause: PLAYING → PAUSED */
        int expected = CHANNEL_PLAYING;
        if (atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PAUSED)) {
            pw_thread_loop_lock(pipeline->loop);
            float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
            player_update_position_snap(p, atomic_load(&p->position_samples), speed, false, 0);
            pw_thread_loop_unlock(pipeline->loop);
            g_message("Player %d paused", player_id);
            return QUADRATURE_OK;
        }
        return QUADRATURE_OK;  /* Already changed state */
    } else {
        return audio_pipeline_player_play(pipeline, player_id);
    }
}

quadrature_result_t audio_pipeline_player_seek(audio_pipeline_t* pipeline, int player_id, uint64_t position) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];

    audio_buffer_t* buf = atomic_load_explicit(&p->buffer, memory_order_acquire);
    if (!buf) return QUADRATURE_ERROR_INTERNAL;

    uint64_t num_frames = audio_buffer_get_num_frames(buf);
    if (position > num_frames) {
        position = num_frames;
    }

    /* Take PipeWire lock to synchronize with audio callback */
    pw_thread_loop_lock(pipeline->loop);

    p->current_frame = position;
    audio_player_flush_all(p);
    atomic_store(&p->position_samples, position);

    /* Update snapshot while holding lock to synchronize with callback */
    channel_state_t snap_state = atomic_load(&p->state);
    float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
    player_update_position_snap(p, position, speed, snap_state == CHANNEL_PLAYING, 0);

    pw_thread_loop_unlock(pipeline->loop);

    g_debug("Player %d seek to %" G_GUINT64_FORMAT, player_id, position);
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Playback Speed Control
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_player_set_speed(audio_pipeline_t* pipeline,
                                                     int player_id, float speed) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t* p = &pipeline->players[player_id];
    if (!p->scrubber) return QUADRATURE_ERROR_INTERNAL;

    /* Speed control requires buffer - buffer-first architecture means this always works when ready */
    if (!atomic_load_explicit(&p->buffer, memory_order_acquire)) {
        g_debug("Player %d cannot change speed - buffer not ready", player_id);
        return QUADRATURE_ERROR_INTERNAL;
    }

    audio_scrubber_set_speed(p->scrubber, speed);

    /* Update snapshot with PipeWire lock to synchronize with callback */
    pw_thread_loop_lock(pipeline->loop);
    channel_state_t snap_state = atomic_load(&p->state);
    player_update_position_snap(p, atomic_load(&p->position_samples), speed, snap_state == CHANNEL_PLAYING, 0);
    pw_thread_loop_unlock(pipeline->loop);

    return QUADRATURE_OK;
}

quadrature_result_t audio_pipeline_player_set_shuttle_mode(audio_pipeline_t* pipeline,
                                                           int player_id, shuttle_mode_t mode) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];
    if (!p->scrubber) return QUADRATURE_ERROR_INTERNAL;
    audio_scrubber_set_shuttle_mode(p->scrubber, mode);
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Repeat Control
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_player_set_repeat(audio_pipeline_t* pipeline, int player_id, bool repeat) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    atomic_store(&pipeline->players[player_id].repeat, repeat);
    return QUADRATURE_OK;
}

bool audio_pipeline_player_get_repeat(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return false;
    return atomic_load(&pipeline->players[player_id].repeat);
}

quadrature_result_t audio_pipeline_player_set_autoplay(audio_pipeline_t* pipeline, int player_id, bool autoplay) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    atomic_store(&pipeline->players[player_id].autoplay, autoplay);
    return QUADRATURE_OK;
}

bool audio_pipeline_player_get_autoplay(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return true;  /* Default: autoplay enabled */
    return atomic_load(&pipeline->players[player_id].autoplay);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Routing
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_set_player_device(audio_pipeline_t* pipeline, int player_id, const char* device_name) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t* p = &pipeline->players[player_id];

    const char* current = p->target_device[0] ? p->target_device : NULL;
    const char* new_dev = (device_name && device_name[0]) ? device_name : NULL;

    if ((current == NULL && new_dev == NULL) ||
        (current && new_dev && strcmp(current, new_dev) == 0)) {
        return QUADRATURE_OK;
    }

    pw_thread_loop_lock(pipeline->loop);
    quadrature_result_t result = player_recreate_stream(p, pipeline->sample_rate,
                                                         pipeline->loop, device_name);
    pw_thread_loop_unlock(pipeline->loop);

    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Monitoring
 * ═══════════════════════════════════════════════════════════════════════════ */

channel_state_t audio_pipeline_get_player_state(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return CHANNEL_ERROR;
    return atomic_load(&pipeline->players[player_id].state);
}

uint64_t audio_pipeline_get_player_position(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return 0;
    return atomic_load(&pipeline->players[player_id].position_samples);
}

uint64_t audio_pipeline_get_player_length(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return 0;
    return atomic_load(&pipeline->players[player_id].length_samples);
}

uint32_t audio_pipeline_get_sample_rate(audio_pipeline_t* pipeline) {
    if (!pipeline) return 0;
    return pipeline->sample_rate;
}

double audio_pipeline_get_player_position_smooth(audio_pipeline_t* pipeline,
                                                  int player_id,
                                                  float* out_speed) {
    if (!pipeline || !valid_player(player_id)) {
        if (out_speed) *out_speed = 1.0f;
        return 0.0;
    }

    audio_player_t* p = &pipeline->players[player_id];
    position_snapshot_t snap = {0};
    uint32_t seq1, seq2 = 0;

    /* Seqlock read with retry */
    do {
        seq1 = atomic_load_explicit(&p->position_seq, memory_order_acquire);
        if (seq1 & 1) continue;  /* Writer is active, spin */
        snap = p->position_snap;
        atomic_thread_fence(memory_order_acquire);
        seq2 = atomic_load_explicit(&p->position_seq, memory_order_relaxed);
    } while (seq1 != seq2);

    if (out_speed) *out_speed = snap.speed;

    /* No interpolation if not playing */
    if (!snap.playing || fabsf(snap.speed) < 0.01f) {
        return (double)snap.position;
    }

    /* Sample-count based interpolation (RT-safe - no syscalls in audio callback) */
    uint64_t current_count = atomic_load(&p->callback_sample_count);
    uint64_t elapsed_samples = current_count - snap.sample_count;

    /* Clamp to prevent stale data (max ~50ms at any sample rate) */
    uint64_t max_elapsed = pipeline->sample_rate / 20;
    if (elapsed_samples > max_elapsed) {
        g_debug("Stale snapshot: elapsed=%" G_GUINT64_FORMAT " max=%" G_GUINT64_FORMAT,
                elapsed_samples, max_elapsed);
        elapsed_samples = max_elapsed;
    }

    double delta = (double)elapsed_samples * snap.speed;
    double result = (double)snap.position + delta;

    /* Clamp to track bounds */
    uint64_t length = atomic_load(&p->length_samples);
    if (result < 0.0) result = 0.0;
    if (result > (double)length) result = (double)length;

    return result;
}

void audio_pipeline_get_player_spectrum(audio_pipeline_t* pipeline, int player_id,
                                        float* left, float* right, int num_bars) {
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));
    g_assert(left != NULL);
    g_assert(right != NULL);
    g_assert(num_bars > 0);

    audio_player_t* p = &pipeline->players[player_id];
    int count = (num_bars > SPECTRUM_BARS) ? SPECTRUM_BARS : num_bars;

    for (int i = 0; i < count; i++) {
        left[i] = atomic_load(&p->spectrum_bars[i]);
        right[i] = atomic_load(&p->spectrum_bars[SPECTRUM_BARS + i]);
    }
    for (int i = count; i < num_bars; i++) {
        left[i] = 0.0f;
        right[i] = 0.0f;
    }
}

void audio_pipeline_get_stats(audio_pipeline_t* pipeline, audio_pipeline_stats_t* stats) {
    g_assert(pipeline != NULL);
    g_assert(stats != NULL);

    stats->callback_count = atomic_load(&pipeline->stats_callback_count);
    stats->underrun_count = atomic_load(&pipeline->stats_underrun_count);

    /* Calculate average callback time */
    uint64_t sum = atomic_load(&pipeline->stats_callback_time_sum_us);
    stats->callback_time_avg_us = (stats->callback_count > 0)
        ? (float)sum / (float)stats->callback_count
        : 0.0f;
    stats->callback_time_max_us = (float)atomic_load(&pipeline->stats_callback_time_max_us);

    stats->track_changes = atomic_load(&pipeline->stats_track_changes);
    stats->instant_advances = atomic_load(&pipeline->stats_instant_advances);
}

void audio_pipeline_get_player_stats(audio_pipeline_t* pipeline,
                                      int player_id,
                                      audio_player_stats_t* stats) {
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));
    g_assert(stats != NULL);

    audio_player_t* p = &pipeline->players[player_id];

    /* Callback performance */
    uint64_t count = atomic_load_explicit(&p->stats_cb_count, memory_order_relaxed);
    uint64_t sum_ns = atomic_load_explicit(&p->stats_cb_time_sum_ns, memory_order_relaxed);
    uint64_t max_ns = atomic_load_explicit(&p->stats_cb_time_max_ns, memory_order_relaxed);

    stats->callback_time_avg_us = count > 0
        ? (float)((double)sum_ns / (double)count / 1000.0) : 0.0f;
    stats->callback_time_max_us = (float)((double)max_ns / 1000.0);

    /* Budget % = avg_time / budget * 100 */
    uint64_t period_ns = (uint64_t)512 * 1000000000ULL / pipeline->sample_rate;
    stats->budget_pct = count > 0
        ? (float)((double)sum_ns / (double)count / (double)period_ns * 100.0)
        : 0.0f;
    stats->budget_overruns = atomic_load_explicit(&p->stats_budget_overruns, memory_order_relaxed);

    /* Audio health — pull from perf dashboard for underruns and jitter */
    if (pipeline->perf) {
        uint64_t underruns, callbacks;
        double jitter;
        perf_get_audio_health(pipeline->perf, player_id, &underruns, &callbacks, &jitter);
        stats->underrun_rate_pct = callbacks > 0
            ? (float)((double)underruns / (double)callbacks * 100.0) : 0.0f;
        stats->jitter_ms = (float)jitter;
    } else {
        stats->underrun_rate_pct = 0.0f;
        stats->jitter_ms = 0.0f;
    }

    /* Fault events */
    stats->dequeue_failures = atomic_load_explicit(&p->stats_dequeue_failures, memory_order_relaxed);
    stats->scrubber_underflows = p->scrubber
        ? audio_scrubber_get_underflows(p->scrubber) : 0;
    stats->deferred_advances = atomic_load_explicit(&p->stats_deferred_advances, memory_order_relaxed);

    /* Advance quality */
    uint64_t instant = atomic_load_explicit(&p->stats_instant_advances, memory_order_relaxed);
    uint64_t deferred = stats->deferred_advances;
    uint64_t total_advances = instant + deferred;
    stats->advance_hit_rate_pct = total_advances > 0
        ? (float)((double)instant / (double)total_advances * 100.0) : 100.0f;
}

perf_dashboard_t* audio_pipeline_get_perf_dashboard(audio_pipeline_t* pipeline) {
    g_assert(pipeline != NULL);
    return pipeline->perf;
}

audio_cache_t* audio_pipeline_get_audio_cache(audio_pipeline_t* pipeline) {
    g_assert(pipeline != NULL);
    return pipeline->cache;
}
