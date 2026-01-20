#include "internal.h"
#include "quadrature/audio/audio_pipeline.h"
#include "quadrature/audio/audio_buffer_store.h"
#include "quadrature/audio/audio_spectrum.h"

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
    if (!p) return;

    /* 1: Scrubber */
    if (p->scrubber) {
        audio_scrubber_flush(p->scrubber);
    }

    /* 2: Spectrum ring buffer */
    spa_ringbuffer_init(&p->spectrum_rb);
    if (p->spectrum_buffer) {
        memset(p->spectrum_buffer, 0, SPECTRUM_RINGBUF_SIZE * sizeof(float));
    }
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
            p->current_frame = 0;
            audio_scrubber_set_position(p->scrubber, 0);
        } else {
            atomic_store(&p->state, CHANNEL_STOPPED);
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
    struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
    if (!b) return;

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

    /* Record callback timing for perf dashboard (RT-safe: atomics only) */
    if (p->pipeline && p->pipeline->perf) {
        perf_record_callback(p->pipeline->perf, p->player_id, sample_count);

        /* Detect underrun: playing state but no buffer available */
        if (state == CHANNEL_PLAYING && !buf) {
            perf_record_underrun(p->pipeline->perf, p->player_id);
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

    /* Initialize spectrum bars */
    for (int i = 0; i < SPECTRUM_BARS; i++) {
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

static void player_destroy(audio_player_t* p, audio_buffer_store_t* store) {
    if (p->stream) {
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }
    /* Release buffer if held */
    audio_buffer_t* buf = atomic_load_explicit(&p->buffer, memory_order_acquire);
    if (buf && store) {
        audio_buffer_store_release(store, buf);
        atomic_store_explicit(&p->buffer, NULL, memory_order_release);
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

quadrature_result_t audio_pipeline_create(uint32_t sample_rate, audio_pipeline_t** pipeline) {
    if (!pipeline) return QUADRATURE_ERROR_INVALID_PARAM;

    pw_init(NULL, NULL);

    *pipeline = calloc(1, sizeof(audio_pipeline_t));
    if (!*pipeline) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    (*pipeline)->sample_rate = sample_rate;

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

    /* Create buffer store */
    quadrature_result_t store_result = audio_buffer_store_create(sample_rate, &(*pipeline)->store);
    if (store_result != QUADRATURE_OK) {
        g_warning("Buffer store creation failed - continuing without cache");
        (*pipeline)->store = NULL;
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
        player_destroy(&pipeline->players[i], pipeline->store);
    }

    if (pipeline->store) {
        audio_buffer_store_destroy(pipeline->store);
        pipeline->store = NULL;
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
    g_message("Pipeline started");
    return QUADRATURE_OK;
}

quadrature_result_t audio_pipeline_stop(audio_pipeline_t* pipeline) {
    if (!pipeline) return QUADRATURE_ERROR_INVALID_PARAM;

    pw_thread_loop_stop(pipeline->loop);

    atomic_store(&pipeline->system_active, false);
    g_message("Pipeline stopped");
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Control
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline bool valid_player(int id) { return id >= 0 && id < MAX_AUDIO_PLAYERS; }

quadrature_result_t audio_pipeline_player_load(audio_pipeline_t* pipeline,
                                                int player_id,
                                                const char* path) {
    if (!pipeline || !path || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t* p = &pipeline->players[player_id];

    /* Stop any current playback and set loading state */
    atomic_store(&p->state, CHANNEL_LOADING);

    /* Synchronize with audio callback */
    atomic_thread_fence(memory_order_seq_cst);
    sched_yield();

    /* Release previous buffer if any */
    audio_buffer_t* old_buf = atomic_load_explicit(&p->buffer, memory_order_acquire);
    if (old_buf && pipeline->store) {
        audio_buffer_store_release(pipeline->store, old_buf);
        atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    }

    /* Reset state */
    p->current_frame = 0;
    strncpy(p->filepath, path, MAX_FILENAME_LENGTH - 1);
    p->filepath[MAX_FILENAME_LENGTH - 1] = '\0';
    audio_scrubber_flush(p->scrubber);

    /* Start decode if not already cached */
    if (pipeline->store) {
        audio_buffer_store_load(pipeline->store, path, NULL, NULL);

        /* Try to acquire immediately (may already be cached) */
        audio_buffer_t* buf = audio_buffer_store_try_acquire(pipeline->store, path);
        if (buf) {
            atomic_store_explicit(&p->buffer, buf, memory_order_release);
            atomic_store(&p->length_samples, audio_buffer_get_num_frames(buf));
            atomic_store(&p->position_samples, 0);
            atomic_store(&p->state, CHANNEL_STOPPED);

            /* Update snapshot with PipeWire lock to synchronize with callback */
            pw_thread_loop_lock(pipeline->loop);
            float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
            player_update_position_snap(p, 0, speed, false, 0);
            pw_thread_loop_unlock(pipeline->loop);

            g_message("Player %d loaded (cached): %s", player_id, path);
            return QUADRATURE_OK;
        }
    }

    /* Buffer not ready yet - stays in LOADING state */
    g_message("Player %d loading: %s", player_id, path);
    return QUADRATURE_OK;
}

/**
 * Try to acquire the decoded buffer for a player.
 * Called internally when transitioning from LOADING to ready state.
 * Returns true if buffer is now available.
 */
static bool try_acquire_buffer(audio_pipeline_t* pipeline, audio_player_t* p) {
    /* Already have buffer */
    if (atomic_load_explicit(&p->buffer, memory_order_acquire)) return true;

    /* Try to acquire from store */
    if (pipeline->store && p->filepath[0]) {
        buffer_status_t status = audio_buffer_store_get_status(pipeline->store, p->filepath);
        if (status == BUFFER_STATUS_READY) {
            audio_buffer_t* buf = audio_buffer_store_try_acquire(pipeline->store, p->filepath);
            if (buf) {
                atomic_store_explicit(&p->buffer, buf, memory_order_release);
                atomic_store(&p->length_samples, audio_buffer_get_num_frames(buf));

                /* Transition LOADING → STOPPED (only valid transition) */
                int expected = CHANNEL_LOADING;
                atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_STOPPED);
                return true;
            }
        } else if (status == BUFFER_STATUS_FAILED) {
            /* Transition LOADING → ERROR */
            int expected = CHANNEL_LOADING;
            atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_ERROR);
        }
    }

    return false;
}

bool audio_pipeline_player_is_ready(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return false;
    audio_player_t* p = &pipeline->players[player_id];

    /* Try to acquire buffer if needed */
    return try_acquire_buffer(pipeline, p);
}

quadrature_result_t audio_pipeline_player_play(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];

    /* Check if buffer is available */
    if (!audio_pipeline_player_is_ready(pipeline, player_id)) {
        return QUADRATURE_ERROR_INTERNAL;
    }

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

void audio_pipeline_get_player_spectrum(audio_pipeline_t* pipeline, int player_id, float* bars, int num_bars) {
    if (!pipeline || !valid_player(player_id) || !bars || num_bars <= 0) return;

    audio_player_t* p = &pipeline->players[player_id];
    int count = (num_bars > SPECTRUM_BARS) ? SPECTRUM_BARS : num_bars;

    for (int i = 0; i < count; i++) {
        bars[i] = atomic_load(&p->spectrum_bars[i]);
    }
    for (int i = count; i < num_bars; i++) {
        bars[i] = 0.0f;
    }
}

perf_dashboard_t* audio_pipeline_get_perf(audio_pipeline_t* pipeline) {
    return pipeline ? pipeline->perf : NULL;
}
