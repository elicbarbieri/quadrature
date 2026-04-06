#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/audio.h"
#include "quadrature/library.h"
#include "quadrature/settings.h"
#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Generation-Checked Idle Callback Data
 *
 * player_reconnect_idle and player_deactivate_idle run on the GTK main thread
 * and take the PW lock. If player_recreate_stream() ran between scheduling and
 * execution, the idle would operate on replaced streams → stale/dangerous.
 * Capture the stream_generation at schedule time; skip if it changed.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_player_t *player;
    unsigned int    generation;   /* stream_generation at schedule time */
} player_idle_data_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Performance Event Recording (RT-Safe)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Record event to pipeline ring buffer (lock-free, RT-safe).
 * Used by audio callback thread to log performance issues.
 */
static inline void pipeline_record_event(audio_pipeline_t* p, 
                                         const audio_pipeline_event_t* event) {
    unsigned int idx = atomic_fetch_add_explicit(&p->event_write, 1, 
                                                  memory_order_relaxed) % AUDIO_EVENT_RING_SIZE;
    p->events[idx] = *event;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static pthread_once_t ffmpeg_init_once = PTHREAD_ONCE_INIT;
static void ffmpeg_init_internal(void) {
    av_log_set_level(AV_LOG_ERROR);
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

    audio_scrubber_flush(p->scrubber);
    p->spectrum.input_buffer_fill = 0;
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
                                   uint64_t cb_start) {
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

                /* Record instant advance event */
                if (p->pipeline) {
                    pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                        .timestamp_ns = cb_start,
                        .type = AUDIO_EVENT_INSTANT_ADVANCE,
                        .player_id = p->player_id,
                        .track_id = old_track_id,
                        .data.transition = {
                            .speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f,
                            .old_queue_size = 0,
                            .new_queue_size = 0,
                        }
                    });
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

                    /* Record deferred advance event */
                    if (p->pipeline) {
                        pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                            .timestamp_ns = cb_start,
                            .type = AUDIO_EVENT_DEFERRED_ADVANCE,
                            .player_id = p->player_id,
                            .track_id = atomic_load(&p->current_track_id),
                            .data.transition = {
                                .speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f,
                                .old_queue_size = 0,
                                .new_queue_size = 0,
                            }
                        });
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

    return frame_count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Process Callback
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_process(void* userdata) {
    audio_player_t* p = (audio_player_t*)userdata;

    /* RT-safe timing: VDSO-mapped clock_gettime (~20ns) */
    uint64_t cb_start = time_ns();

    /* Gate all metrics on stream state — skip if not streaming */
    enum pw_stream_state pw_state = atomic_load_explicit(&p->pw_stream_state, memory_order_relaxed);
    if (pw_state != PW_STREAM_STATE_STREAMING) {
        struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
        if (b) {
            /* Output silence and re-queue without recording any metrics */
            float* out = (float*)b->buffer->datas[0].data;
            if (out) {
                uint32_t max_frames = b->buffer->datas[0].maxsize / (sizeof(float) * 2);
                uint32_t frame_count = SPA_MIN(b->requested, max_frames);
                memset(out, 0, frame_count * 2 * sizeof(float));
                b->buffer->datas[0].chunk->offset = 0;
                b->buffer->datas[0].chunk->stride = sizeof(float) * 2;
                b->buffer->datas[0].chunk->size = frame_count * sizeof(float) * 2;
            }
            pw_stream_queue_buffer(p->stream, b);
        }
        /* Reset interval tracking so first streaming callback doesn't see
         * a stale timestamp from before a non-streaming gap */
        p->last_callback_ns = 0;
        return;
    }

    /* Callback interval tracking: measure time since last callback.
     * Only computed here; ring buffer write + event emission happen in the
     * ~10ms sampling block below to avoid per-callback overhead. */
    int64_t interval_dev_ns = 0;
    bool has_interval = (p->last_callback_ns > 0 && p->pipeline);
    if (has_interval) {
        uint64_t interval = cb_start - p->last_callback_ns;
        uint64_t expected = (uint64_t)p->quantum_frames * p->pipeline->ns_per_frame;
        interval_dev_ns = (int64_t)interval - (int64_t)expected;
        /* Track peak absolute deviation for ring buffer sampling */
        int64_t abs_dev = interval_dev_ns > 0 ? interval_dev_ns : -interval_dev_ns;
        if (abs_dev > p->interval_peak_dev_ns)
            p->interval_peak_dev_ns = abs_dev;
    }
    p->last_callback_ns = cb_start;

    struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
    if (!b) {
        atomic_fetch_add_explicit(&p->stats_dequeue_failures, 1, memory_order_relaxed);
        if (p->pipeline) {
            pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                .timestamp_ns = cb_start,
                .type = AUDIO_EVENT_DEQUEUE_FAILURE,
                .player_id = p->player_id,
                .track_id = atomic_load(&p->current_track_id),
                .data.dequeue = {
                    .queue_size = 0,
                }
            });
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

    /* Read PipeWire native metrics (RT-safe via pw_stream_get_time_n) */
#if PW_CHECK_VERSION(0, 3, 50)
    if (p->pipeline && p->pipeline->perf) {
        struct pw_time pw_t;
        if (pw_stream_get_time_n(p->stream, &pw_t, sizeof(pw_t)) == 0) {
            atomic_store_explicit(&p->pipeline->perf->pw_avail_buffers[p->player_id],
                                   (uint64_t)pw_t.avail_buffers, memory_order_relaxed);
            atomic_store_explicit(&p->pipeline->perf->pw_queued_buffers[p->player_id],
                                   (uint64_t)pw_t.queued_buffers, memory_order_relaxed);
            atomic_store_explicit(&p->pipeline->perf->pw_delay_samples[p->player_id],
                                   pw_t.delay, memory_order_relaxed);
        }
    }
#endif

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

    /* Legacy pipeline-level stats + buffer underrun events */
    if (p->pipeline) {
        atomic_fetch_add(&p->pipeline->stats_callback_count, 1);
        if (state == CHANNEL_PLAYING && !buf) {
            atomic_fetch_add(&p->pipeline->stats_underrun_count, 1);
            /* Record buffer underrun event */
            pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                .timestamp_ns = cb_start,
                .type = AUDIO_EVENT_BUFFER_UNDERRUN,
                .player_id = p->player_id,
                .track_id = atomic_load(&p->current_track_id),
                .data.underrun = {
                    .requested_frames = frame_count,
                    .available_frames = 0,
                    .speed = scrub_speed,
                }
            });
        }
    }

    if (should_play) {
        /* Process audio from buffer */
        process_buffer_audio(p, out, frame_count, cb_start);

        /* Detect scrubber underflows by comparing count before/after */
        if (p->scrubber && p->pipeline) {
            uint64_t cur_underflows = audio_scrubber_get_underflows(p->scrubber);
            if (cur_underflows > p->prev_scrubber_underflows) {
                uint32_t delta = (uint32_t)(cur_underflows - p->prev_scrubber_underflows);
                pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                    .timestamp_ns = cb_start,
                    .type = AUDIO_EVENT_SCRUBBER_UNDERFLOW,
                    .player_id = p->player_id,
                    .track_id = atomic_load(&p->current_track_id),
                    .data.underrun = {
                        .requested_frames = frame_count,
                        .available_frames = frame_count - delta,
                        .scrub_fill = 0,
                        .speed = scrub_speed,
                    }
                });
                p->prev_scrubber_underflows = cur_underflows;
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

    /* Pre-compute budget once (multiply, no division in RT path) */
    uint64_t budget_ns = p->pipeline ? (uint64_t)frame_count * p->pipeline->ns_per_frame : 0;

    /* Budget + latency ring buffers — sample every ~10ms */
    if (p->pipeline) {
        uint64_t now_ns = cb_start;
        if (now_ns - p->budget_rb->last_write_ns >= 10000000ULL) {
            /* Budget centipercent (0-10000 for 0.01% resolution) */
            uint32_t cpct = budget_ns > 0 ? (uint32_t)((cb_elapsed * 10000ULL) / budget_ns) : 0;
            if (cpct > 10000) cpct = 10000;
            uint32_t pos = atomic_load_explicit(&p->budget_rb->write_pos, memory_order_relaxed);
            p->budget_rb->samples[pos & (BUDGET_RB_CAPACITY - 1)] = (uint16_t)cpct;
            atomic_store_explicit(&p->budget_rb->write_pos, pos + 1, memory_order_release);
            p->budget_rb->last_write_ns = now_ns;

            /* Latency µs */
            uint16_t lat_us = (cb_elapsed / 1000 > 65535) ? 65535 : (uint16_t)(cb_elapsed / 1000);
            uint32_t lpos = atomic_load_explicit(&p->latency_rb->write_pos, memory_order_relaxed);
            p->latency_rb->samples[lpos & (LATENCY_RB_CAPACITY - 1)] = lat_us;
            atomic_store_explicit(&p->latency_rb->write_pos, lpos + 1, memory_order_release);
            p->latency_rb->last_write_ns = now_ns;

            /* Interval deviation: write raw peak ns, then reset */
            uint32_t ipos = atomic_load_explicit(&p->interval_rb->write_pos, memory_order_relaxed);
            p->interval_rb->samples[ipos & (INTERVAL_RB_CAPACITY - 1)] = p->interval_peak_dev_ns;
            atomic_store_explicit(&p->interval_rb->write_pos, ipos + 1, memory_order_release);
            p->interval_rb->last_write_ns = now_ns;
            p->interval_peak_dev_ns = 0;

            /* Fire SCHEDULING_DELAY event if callback arrived >2x period late */
            if (has_interval && interval_dev_ns > (int64_t)(budget_ns * 2)) {
                pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                    .timestamp_ns = cb_start,
                    .type = AUDIO_EVENT_SCHEDULING_DELAY,
                    .player_id = p->player_id,
                    .track_id = atomic_load(&p->current_track_id),
                    .data.scheduling = {
                        .deviation_ns = interval_dev_ns,
                        .expected_ns = (int64_t)budget_ns,
                    }
                });
            }
        }
    }

    /* Update peak (CAS loop) */
    uint64_t cur_max = atomic_load_explicit(&p->stats_cb_time_max_ns, memory_order_relaxed);
    while (cb_elapsed > cur_max) {
        if (atomic_compare_exchange_weak_explicit(&p->stats_cb_time_max_ns,
                &cur_max, cb_elapsed, memory_order_relaxed, memory_order_relaxed))
            break;
    }

    /* Budget overrun check (50% of period) — emit event */
    if (p->pipeline) {
        uint64_t half_budget = budget_ns / 2;
        if (cb_elapsed > half_budget) {
            atomic_fetch_add_explicit(&p->stats_budget_overruns, 1, memory_order_relaxed);
            pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                .timestamp_ns = cb_start,
                .type = AUDIO_EVENT_BUDGET_OVERRUN,
                .player_id = p->player_id,
                .track_id = atomic_load(&p->current_track_id),
                .data.budget = {
                    .elapsed_ns = cb_elapsed,
                    .budget_ns = budget_ns,
                }
            });
        }
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

static gboolean player_reconnect_idle(gpointer data);
static gboolean player_deactivate_idle(gpointer data);
static void player_deactivate_streams(audio_player_t* p);

static void on_state_changed(void* userdata, enum pw_stream_state old,
                              enum pw_stream_state state, const char* error) {
    audio_player_t* p = (audio_player_t*)userdata;
    (void)old;
    (void)error;
    atomic_store_explicit(&p->pw_stream_state, (int)state, memory_order_relaxed);

    if (state == PW_STREAM_STATE_ERROR) {
        atomic_store_explicit(&p->device_error, true, memory_order_release);
        if (p->stream)
            pw_stream_set_active(p->stream, false);
        if (p->pipeline) {
            pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                .type = AUDIO_EVENT_PW_ERROR,
                .timestamp_ns = time_ns(),
                .player_id = p->player_id,
                .track_id = atomic_load_explicit(&p->current_track_id, memory_order_relaxed),
            });
        }

        /* One reconnect attempt for transient errors (e.g., device config change).
         * If we already tried once, this is a persistent failure — deactivate. */
        if (!atomic_exchange(&p->reconnect_attempted, true)) {
            g_warning("Player %d: PW error (first) — attempting reconnect", p->player_id);
            player_idle_data_t *d = g_new(player_idle_data_t, 1);
            d->player = p;
            d->generation = atomic_load(&p->stream_generation);
            g_idle_add(player_reconnect_idle, d);
        } else {
            g_warning("Player %d: PW error (second) — deactivating streams", p->player_id);
            player_idle_data_t *d = g_new(player_idle_data_t, 1);
            d->player = p;
            d->generation = atomic_load(&p->stream_generation);
            g_idle_add(player_deactivate_idle, d);
        }
    } else if (state == PW_STREAM_STATE_STREAMING) {
        atomic_store_explicit(&p->device_error, false, memory_order_release);
        /* Successful stream — reset reconnect counter for next error cycle */
        atomic_store(&p->reconnect_attempted, false);
    }
}

static void on_stream_io_changed(void* data, uint32_t id, void* area, uint32_t size) {
    audio_player_t* p = data;
    (void)size;
    /* area == NULL for SPA_IO_Position signals a PipeWire xrun */
    if (id == SPA_IO_Position && area == NULL) {
        if (p->pipeline) {
            pipeline_record_event(p->pipeline, &(audio_pipeline_event_t){
                .type = AUDIO_EVENT_PW_XRUN,
                .timestamp_ns = time_ns(),
                .player_id = p->player_id,
                .track_id = atomic_load_explicit(&p->current_track_id, memory_order_relaxed),
            });
        }
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process       = on_process,
    .state_changed = on_state_changed,
    .io_changed    = on_stream_io_changed,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Stream Creation Helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static quadrature_result_t create_player_stream(audio_player_t* p,
                                                 uint32_t sample_rate) {
    char stream_name[64];
    snprintf(stream_name, sizeof(stream_name), "quadrature-player-%d", p->player_id);

    char latency_str[32];
    snprintf(latency_str, sizeof(latency_str), "%u/%u", p->quantum_frames, sample_rate);

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_LATENCY, latency_str,
        NULL);

    if (p->target_device[0]) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, p->target_device);
    }

    if (p->exclusive) {
        pw_properties_set(props, PW_KEY_NODE_EXCLUSIVE, "true");
    }

    p->stream = pw_stream_new(p->pipeline->core, stream_name, props);
    if (p->stream) {
        pw_stream_add_listener(p->stream, &p->stream_listener, &stream_events, p);
    }

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

    /* Cache params for auto-reconnect: copy pod bytes + fix up pointer */
    ptrdiff_t pod_offset = (const uint8_t*)params[0] - buffer;
    memcpy(p->cached_params_buf, buffer, sizeof(buffer));
    p->cached_params[0] = (const struct spa_pod*)(p->cached_params_buf + pod_offset);
    p->num_cached_params = 1;

    int res = pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS,
        params, 1);

    if (res < 0) {
        spa_hook_remove(&p->stream_listener);
        pw_stream_destroy(p->stream);
        p->stream = NULL;
        return QUADRATURE_ERROR_INTERNAL;
    }

    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Monitor Stream (Spectrum Capture from Device)
 *
 * Captures audio from the sink's monitor port so the spectrum reflects
 * actual device output rather than the internal decode pipeline.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_monitor_process(void* userdata) {
    audio_player_t* p = (audio_player_t*)userdata;
    struct pw_buffer* b = pw_stream_dequeue_buffer(p->monitor_stream);
    if (!b) return;

    struct spa_data* d = &b->buffer->datas[0];
    float* in = d->data;
    if (!in || d->chunk->size == 0) {
        pw_stream_queue_buffer(p->monitor_stream, b);
        return;
    }

    uint32_t n_frames = d->chunk->size / (sizeof(float) * 2);

    /* Process spectrum FFT inline — no ring buffer, no separate thread */
    spectrum_process(&p->spectrum, in, n_frames, p->spectrum_bars, &p->spectrum_generation);

    pw_stream_queue_buffer(p->monitor_stream, b);
}

static const struct pw_stream_events monitor_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_monitor_process,
};

static quadrature_result_t create_monitor_stream(audio_player_t* p,
                                                  uint32_t sample_rate) {
    char name[64];
    snprintf(name, sizeof(name), "quadrature-spectrum-%d", p->player_id);

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true",   /* capture from sink monitor */
        PW_KEY_NODE_PASSIVE, "true",           /* don't keep sink alive */
        NULL);

    if (p->target_device[0]) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, p->target_device);
    }

    p->monitor_stream = pw_stream_new(p->pipeline->core, name, props);
    if (p->monitor_stream) {
        pw_stream_add_listener(p->monitor_stream, &p->monitor_stream_listener, &monitor_stream_events, p);
    }

    if (!p->monitor_stream) {
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

    int res = pw_stream_connect(p->monitor_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS,
        params, 1);

    if (res < 0) {
        spa_hook_remove(&p->monitor_stream_listener);
        pw_stream_destroy(p->monitor_stream);
        p->monitor_stream = NULL;
        return QUADRATURE_ERROR_INTERNAL;
    }

    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Reconnect (Main Thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Reconnect a player's PipeWire stream after a device error.
 * Runs on the main GLib thread via g_idle_add().
 */
static gboolean player_reconnect_idle(gpointer data) {
    player_idle_data_t* d = data;
    audio_player_t* p = d->player;
    unsigned int sched_gen = d->generation;
    g_free(d);

    audio_pipeline_t* pipeline = p->pipeline;

    /* Stale: stream was recreated since this idle was scheduled */
    if (atomic_load(&p->stream_generation) != sched_gen) {
        g_debug("Player %d: reconnect_idle skipped (generation %u → %u)",
                p->player_id, sched_gen, atomic_load(&p->stream_generation));
        return G_SOURCE_REMOVE;
    }

    if (!atomic_load_explicit(&p->device_error, memory_order_acquire))
        return G_SOURCE_REMOVE;  /* Already recovered */

    pw_thread_loop_lock(pipeline->loop);
    if (!p->stream) {
        pw_thread_loop_unlock(pipeline->loop);
        return G_SOURCE_REMOVE;
    }
    pw_stream_disconnect(p->stream);
    pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
        p->cached_params, p->num_cached_params);

    /* Reconnect monitor stream alongside output stream */
    if (p->monitor_stream) {
        pw_stream_disconnect(p->monitor_stream);
        uint8_t mbuf[1024];
        struct spa_pod_builder mb = SPA_POD_BUILDER_INIT(mbuf, sizeof(mbuf));
        const struct spa_pod* mparams[1];
        mparams[0] = spa_format_audio_raw_build(&mb, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(
                .format = SPA_AUDIO_FORMAT_F32,
                .channels = 2,
                .rate = pipeline->sample_rate
            ));
        pw_stream_connect(p->monitor_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
            mparams, 1);
    }
    pw_thread_loop_unlock(pipeline->loop);

    /* state_changed callback will clear device_error when STREAMING */
    return G_SOURCE_REMOVE;
}

/**
 * Deactivate a player's streams after persistent device failure.
 * Runs on the main GLib thread via g_idle_add().
 */
static gboolean player_deactivate_idle(gpointer data) {
    player_idle_data_t* d = data;
    audio_player_t* p = d->player;
    unsigned int sched_gen = d->generation;
    g_free(d);

    audio_pipeline_t* pipeline = p->pipeline;
    if (!pipeline) return G_SOURCE_REMOVE;

    /* Stale: stream was recreated since this idle was scheduled */
    if (atomic_load(&p->stream_generation) != sched_gen) {
        g_debug("Player %d: deactivate_idle skipped (generation %u → %u)",
                p->player_id, sched_gen, atomic_load(&p->stream_generation));
        return G_SOURCE_REMOVE;
    }

    pw_thread_loop_lock(pipeline->loop);
    player_deactivate_streams(p);
    pw_thread_loop_unlock(pipeline->loop);

    g_warning("Player %d: device lost — streams deactivated", p->player_id);
    return G_SOURCE_REMOVE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static quadrature_result_t player_init(audio_player_t* p, int id, uint32_t sample_rate) {
    memset(p, 0, sizeof(*p));
    p->player_id = id;

    /* Initialize per-player spectrum state (cava FFT plan + buffers) */
    quadrature_result_t spec_res = spectrum_init(&p->spectrum, SPECTRUM_BARS, sample_rate);
    if (spec_res != QUADRATURE_OK) {
        return spec_res;
    }

    atomic_store(&p->state, CHANNEL_STOPPED);
    atomic_store(&p->pw_stream_state, (int)PW_STREAM_STATE_UNCONNECTED);
    atomic_store_explicit(&p->device_error, false, memory_order_relaxed);

    /* Heap-allocate ring buffers (~128KB each, keeps audio_player_t small) */
    p->budget_rb = calloc(1, sizeof(budget_rb_t));
    p->latency_rb = calloc(1, sizeof(latency_rb_t));
    p->interval_rb = calloc(1, sizeof(interval_rb_t));
    if (!p->budget_rb || !p->latency_rb || !p->interval_rb) {
        free(p->budget_rb);
        free(p->latency_rb);
        free(p->interval_rb);
        spectrum_cleanup(&p->spectrum);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }
    p->last_callback_ns = 0;
    p->interval_peak_dev_ns = 0;

    p->num_cached_params = 0;
    p->prev_scrubber_underflows = 0;

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
        spectrum_cleanup(&p->spectrum);
        return scrub_res;
    }

    /* Initialize position snapshot (seqlock pattern) */
    atomic_store(&p->position_seq, 0);
    memset(&p->position_snap, 0, sizeof(p->position_snap));

    /* Initialize sample counter for RT-safe timestamps */
    atomic_store(&p->callback_sample_count, 0);

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

    /* No target device at init — player starts dormant */
    p->target_device[0] = '\0';

    /* Default quantum (overridden by settings restore in device_enum_done) */
    p->quantum_frames = APP_SETTINGS_DEFAULT_QUANTUM;

    /* Initialize stream activation state — streams are created on-demand
     * when a valid output device is assigned via set_player_device() */
    p->stream = NULL;
    p->monitor_stream = NULL;
    atomic_store(&p->streams_active, false);
    atomic_store(&p->reconnect_attempted, false);

    g_message("Player %d initialized (%uHz, dormant — no device)", id, sample_rate);
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stream Activation / Deactivation
 *
 * Players start dormant (no PW streams). Streams are created when a valid
 * output device is assigned, and destroyed when the device is removed or
 * becomes invalid. This keeps the PipeWire graph clean — only players with
 * valid output devices have nodes on the graph.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create PipeWire output + monitor streams for a player.
 * Precondition: p->target_device is set to a valid device name.
 * Must be called with PipeWire thread loop lock held.
 */
static quadrature_result_t player_activate_streams(audio_player_t* p,
                                                    uint32_t sample_rate) {
    g_assert(p->stream == NULL);
    g_assert(p->monitor_stream == NULL);

    quadrature_result_t res = create_player_stream(p, sample_rate);
    if (res != QUADRATURE_OK) {
        g_warning("Player %d: output stream creation failed", p->player_id);
        return res;
    }

    /* Monitor stream for spectrum (non-fatal if it fails) */
    quadrature_result_t mon_res = create_monitor_stream(p, sample_rate);
    if (mon_res != QUADRATURE_OK) {
        g_warning("Player %d: monitor stream failed, spectrum will be inactive", p->player_id);
    }

    atomic_store(&p->streams_active, true);
    atomic_store(&p->reconnect_attempted, false);
    atomic_store_explicit(&p->device_error, false, memory_order_release);

    g_message("Player %d streams activated (device: %s)", p->player_id, p->target_device);
    return QUADRATURE_OK;
}

/**
 * Destroy PipeWire streams and reset player to dormant state.
 * Stops playback, clears spectrum ring buffer, zeroes metering.
 * Must be called with PipeWire thread loop lock held.
 */
static void player_deactivate_streams(audio_player_t* p) {
    if (!atomic_load(&p->streams_active) && !p->stream && !p->monitor_stream)
        return;

    /* Stop playback first */
    int prev_state = atomic_exchange(&p->state, CHANNEL_STOPPED);
    (void)prev_state;

    if (p->monitor_stream) {
        spa_hook_remove(&p->monitor_stream_listener);
        pw_stream_disconnect(p->monitor_stream);
        pw_stream_destroy(p->monitor_stream);
        p->monitor_stream = NULL;
    }
    if (p->stream) {
        spa_hook_remove(&p->stream_listener);
        pw_stream_disconnect(p->stream);
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }

    /* Reset spectrum state so stale data doesn't linger */
    p->spectrum.input_buffer_fill = 0;
    for (int i = 0; i < SPECTRUM_BARS * 2; i++) {
        atomic_store(&p->spectrum_bars[i], 0.0f);
    }

    /* Reset stream-related state */
    atomic_store_explicit(&p->pw_stream_state, (int)PW_STREAM_STATE_UNCONNECTED, memory_order_relaxed);
    atomic_store_explicit(&p->device_error, false, memory_order_release);
    atomic_store(&p->streams_active, false);
    atomic_store(&p->reconnect_attempted, false);
    p->last_callback_ns = 0;
    p->interval_peak_dev_ns = 0;

    g_message("Player %d streams deactivated", p->player_id);
}

static void player_destroy(audio_player_t* p, audio_cache_t* cache) {
    if (p->monitor_stream) {
        spa_hook_remove(&p->monitor_stream_listener);
        pw_stream_disconnect(p->monitor_stream);
        pw_stream_destroy(p->monitor_stream);
        p->monitor_stream = NULL;
    }
    if (p->stream) {
        spa_hook_remove(&p->stream_listener);
        pw_stream_disconnect(p->stream);
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
    spectrum_cleanup(&p->spectrum);
    free(p->budget_rb);
    p->budget_rb = NULL;
    free(p->latency_rb);
    p->latency_rb = NULL;
    free(p->interval_rb);
    p->interval_rb = NULL;
    if (p->scrubber) {
        audio_scrubber_destroy(p->scrubber);
        p->scrubber = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper to recreate a player's stream with a new target device
 * ═══════════════════════════════════════════════════════════════════════════ */

static quadrature_result_t player_recreate_stream(audio_player_t* p, uint32_t sample_rate,
                                                   const char* target_device) {
    channel_state_t prev_state = atomic_load(&p->state);

    /* Bump generation so stale reconnect/deactivate idles are no-ops */
    atomic_fetch_add(&p->stream_generation, 1);

    /* Tear down existing streams */
    player_deactivate_streams(p);

    /* Update target device */
    if (target_device && target_device[0]) {
        strncpy(p->target_device, target_device, sizeof(p->target_device) - 1);
        p->target_device[sizeof(p->target_device) - 1] = '\0';
        g_message("Player %d retargeting to device: %s", p->player_id, p->target_device);
    } else {
        p->target_device[0] = '\0';
        g_message("Player %d retargeting to default device", p->player_id);
    }

    /* Only activate if we have a valid device target */
    if (!p->target_device[0]) {
        g_message("Player %d: no device — remaining dormant", p->player_id);
        return QUADRATURE_OK;
    }

    quadrature_result_t res = player_activate_streams(p, sample_rate);

    /* Restore previous state: PLAYING resumes, PAUSED stays paused */
    if (res == QUADRATURE_OK && prev_state != CHANNEL_STOPPED) {
        atomic_store(&p->state, prev_state);
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
    (*pipeline)->ns_per_frame = 1000000000ULL / sample_rate;
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

    /* Initialize event ring buffer */
    atomic_store(&(*pipeline)->event_write, 0);
    atomic_store(&(*pipeline)->event_read, 0);

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
        quadrature_result_t r = player_init(&(*pipeline)->players[i], i, sample_rate);
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

    /* Link perf dashboard to components for event polling */
    if ((*pipeline)->perf) {
        perf_dashboard_set_audio_pipeline((*pipeline)->perf, *pipeline);
        perf_dashboard_set_audio_cache((*pipeline)->perf, (*pipeline)->cache);

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

    /* Remove GLib advance timer before anything else — prevents callbacks
     * firing on partially-destroyed pipeline state */
    if (pipeline->advance_timeout_id > 0) {
        g_source_remove(pipeline->advance_timeout_id);
        pipeline->advance_timeout_id = 0;
    }

    /* Clear track-changed callback so no stale pointer can be invoked */
    pipeline->track_changed_callback = NULL;
    pipeline->track_changed_user_data = NULL;

    /* Stop device monitor before halting the PW thread.  Null the callback
     * first so any in-flight PW event cannot call into a stale UI pointer. */
    pipeline->device_changed_cb = NULL;
    audio_devices_monitor_stop(pipeline);

    if (atomic_load(&pipeline->system_active)) {
        pw_thread_loop_stop(pipeline->loop);
        atomic_store(&pipeline->system_active, false);
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

    /* Reject if player has no active output device */
    if (!atomic_load(&p->streams_active)) {
        g_warning("Player %d: cannot set track — no active output device", player_id);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

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

        /* Position snapshot: just store atomic position; the audio callback
         * will write a full position_snap within ~10ms. Avoids taking the
         * PW lock on the GTK main thread which can deadlock during device changes. */
        atomic_store(&p->position_samples, 0);

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

                    /* Position snapshot: just store atomic position; the audio callback
                     * will write a full position_snap within ~10ms. Avoids taking the
                     * PW lock on the GTK main thread which can deadlock during device changes. */
                    atomic_store(&p->position_samples, 0);

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

        /* Poll for next_buffer readiness: if next_track_id is set but
         * next_buffer is NULL, check if decode has completed */
        if (pipeline->cache) {
            int64_t next_id = atomic_load(&p->next_track_id);
            audio_buffer_t* nb = atomic_load_explicit(&p->next_buffer, memory_order_acquire);
            if (next_id > 0 && !nb) {
                audio_cache_status_t ns = audio_cache_get_status(pipeline->cache, next_id);
                if (ns == AUDIO_CACHE_READY) {
                    audio_buffer_t* next_buf = audio_cache_get_locked(pipeline->cache, next_id);
                    if (next_buf) {
                        atomic_store_explicit(&p->next_buffer, next_buf, memory_order_release);
                    }
                }
            }
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

    /* Sample PW queue depth every ~1 second (20 * 50ms) */
    static unsigned int sample_counter = 0;
    if (++sample_counter >= 20 && pipeline->perf) {
        perf_sample_pw_queue_depth(pipeline->perf);
        sample_counter = 0;
    }

    return G_SOURCE_CONTINUE;
}

quadrature_result_t audio_pipeline_player_play(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t* p = &pipeline->players[player_id];

    if (!atomic_load(&p->streams_active)) return QUADRATURE_ERROR_INVALID_PARAM;

    /* Atomic state transition: STOPPED or PAUSED → PLAYING */
    int expected = CHANNEL_STOPPED;
    if (!atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PLAYING)) {
        expected = CHANNEL_PAUSED;
        if (!atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PLAYING)) {
            if (expected == CHANNEL_PLAYING) return QUADRATURE_OK;
            g_debug("Player %d play failed: state=%d", player_id, expected);
            return QUADRATURE_ERROR_INTERNAL;
        }
    }

    const char *from = (expected == CHANNEL_STOPPED) ? "stopped" : "paused";
    pw_thread_loop_lock(pipeline->loop);
    if (p->stream) pw_stream_set_active(p->stream, true);
    float speed = p->scrubber ? audio_scrubber_get_speed(p->scrubber) : 1.0f;
    player_update_position_snap(p, atomic_load(&p->position_samples), speed, true, 0);
    pw_thread_loop_unlock(pipeline->loop);
    g_info("Player %d playing (from %s)", player_id, from);
    return QUADRATURE_OK;
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

    /* Deactivate stream so PipeWire stops scheduling RT callbacks */
    if (prev != CHANNEL_STOPPED && p->stream)
        pw_stream_set_active(p->stream, false);

    pw_thread_loop_unlock(pipeline->loop);

    if (prev != CHANNEL_STOPPED) {
        g_info("Player %d stopped", player_id);
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
            g_info("Player %d paused", player_id);
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
    g_info("Ch%d: speed set to %.2fx", player_id + 1, speed);

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
    
    /* Log mode change */
    const char* mode_names[] = { "off", "keylock", "pitched" };
    const char* mode_name = (mode >= 0 && mode <= 2) ? mode_names[mode] : "unknown";
    g_info("Ch%d: shuttle mode set to %s", player_id + 1, mode_name);
    
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

    /* Lockless early-out: skip PW lock entirely for no-op device changes.
     * target_device is only written under the PW lock, so worst case of a
     * torn read is a single extra lock acquisition — not a safety issue.
     * Exception: if streams were deactivated (PW error recovery), always
     * re-create even for the same device name. */
    const char* current = p->target_device[0] ? p->target_device : NULL;
    const char* new_dev = (device_name && device_name[0]) ? device_name : NULL;
    bool streams_up = atomic_load(&p->streams_active);
    if (streams_up &&
        ((current == NULL && new_dev == NULL) ||
         (current && new_dev && strcmp(current, new_dev) == 0)))
        return QUADRATURE_OK;

    pw_thread_loop_lock(pipeline->loop);
    quadrature_result_t result = player_recreate_stream(p, pipeline->sample_rate, device_name);
    pw_thread_loop_unlock(pipeline->loop);

    return result;
}

void audio_pipeline_set_player_exclusive(audio_pipeline_t* pipeline, int player_id, bool exclusive) {
    if (!pipeline || !valid_player(player_id)) return;
    audio_player_t* p = &pipeline->players[player_id];
    p->exclusive = exclusive;
    /* Takes effect on next stream recreate (device change, reconnect, etc.) */
}

quadrature_result_t audio_pipeline_set_player_quantum(audio_pipeline_t* pipeline, int player_id, uint32_t quantum_frames) {
    if (!pipeline || !valid_player(player_id)) return QUADRATURE_ERROR_INVALID_PARAM;
    if (quantum_frames < 32 || quantum_frames > 2048 || (quantum_frames & (quantum_frames - 1)) != 0)
        return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t* p = &pipeline->players[player_id];

    pw_thread_loop_lock(pipeline->loop);

    if (p->quantum_frames == quantum_frames) {
        pw_thread_loop_unlock(pipeline->loop);
        return QUADRATURE_OK;
    }

    p->quantum_frames = quantum_frames;

    /* Update audio cache unlock delay to match new quantum */
    audio_cache_set_quantum(pipeline->cache, quantum_frames);

    quadrature_result_t result = player_recreate_stream(p, pipeline->sample_rate,
                                                         p->target_device[0] ? p->target_device : NULL);
    pw_thread_loop_unlock(pipeline->loop);

    g_message("Player %d quantum set to %u frames", player_id, quantum_frames);
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

    /* Seqlock read with retry limit.
     * If the writer is pathologically active (shouldn't happen in practice),
     * return the last snapshot — stale data for one UI frame is acceptable. */
    #define SEQLOCK_MAX_RETRIES 4
    for (int attempt = 0; attempt < SEQLOCK_MAX_RETRIES; attempt++) {
        seq1 = atomic_load_explicit(&p->position_seq, memory_order_acquire);
        if (seq1 & 1)
            continue;  /* Writer is active, spin */
        snap = p->position_snap;
        atomic_thread_fence(memory_order_acquire);
        seq2 = atomic_load_explicit(&p->position_seq, memory_order_relaxed);
        if (seq1 == seq2) break;
    }

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

    /* When not playing, return zeros — UI smoothing handles fadeout */
    channel_state_t state = atomic_load(&p->state);
    if (state != CHANNEL_PLAYING) {
        memset(left, 0, (size_t)num_bars * sizeof(float));
        memset(right, 0, (size_t)num_bars * sizeof(float));
        return;
    }

    for (int i = 0; i < count; i++) {
        left[i] = atomic_load(&p->spectrum_bars[i]);
        right[i] = atomic_load(&p->spectrum_bars[SPECTRUM_BARS + i]);
    }
    for (int i = count; i < num_bars; i++) {
        left[i] = 0.0f;
        right[i] = 0.0f;
    }
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
    uint64_t period_ns = (uint64_t)p->quantum_frames * 1000000000ULL / pipeline->sample_rate;
    stats->budget_pct = count > 0
        ? (float)((double)sum_ns / (double)count / (double)period_ns * 100.0)
        : 0.0f;
    stats->budget_overruns = atomic_load_explicit(&p->stats_budget_overruns, memory_order_relaxed);

    /* Compute underrun rate from pipeline stats */
    uint64_t total_callbacks = atomic_load(&pipeline->stats_callback_count);
    uint64_t total_underruns = atomic_load(&pipeline->stats_underrun_count);
    stats->underrun_rate_pct = total_callbacks > 0
        ? (float)((double)total_underruns / (double)total_callbacks * 100.0) : 0.0f;
    
    /* Jitter tracking removed - was never written to since perf_record_callback() not called */
    stats->jitter_ms = 0.0f;

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

void audio_pipeline_get_budget_histogram(audio_pipeline_t* pipeline, int player_id,
                                          uint32_t out_buckets[10]) {
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));
    g_assert(out_buckets != NULL);

    memset(out_buckets, 0, 10 * sizeof(uint32_t));

    const budget_rb_t* rb = pipeline->players[player_id].budget_rb;
    uint32_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);

    /* Read up to BUDGET_RB_CAPACITY samples (or all written so far) */
    uint32_t total = write_pos < BUDGET_RB_CAPACITY ? write_pos : BUDGET_RB_CAPACITY;
    uint32_t start = write_pos - total;  /* wraps naturally with unsigned arithmetic */

    for (uint32_t i = 0; i < total; i++) {
        uint16_t cpct = rb->samples[(start + i) & (BUDGET_RB_CAPACITY - 1)];
        int bucket = cpct / 1000;  /* centipercent → 10% buckets */
        if (bucket >= 10) bucket = 9;
        out_buckets[bucket]++;
    }
}

double audio_pipeline_get_budget_max(audio_pipeline_t* pipeline, int player_id) {
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));

    const budget_rb_t* rb = pipeline->players[player_id].budget_rb;
    uint32_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);

    /* ~1 second of samples at ~10ms intervals */
    uint32_t window = 100;
    uint32_t available = write_pos < BUDGET_RB_CAPACITY ? write_pos : BUDGET_RB_CAPACITY;
    if (available == 0) return 0.0;
    if (window > available) window = available;

    uint32_t start = write_pos - window;
    uint16_t max_cpct = 0;
    for (uint32_t i = 0; i < window; i++) {
        uint16_t v = rb->samples[(start + i) & (BUDGET_RB_CAPACITY - 1)];
        if (v > max_cpct) max_cpct = v;
    }

    return (double)max_cpct / 100.0;
}

uint32_t audio_pipeline_get_latency_samples(audio_pipeline_t* pipeline, int player_id,
                                              uint16_t* out, uint32_t max_samples) {
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));
    g_assert(out != NULL);

    const latency_rb_t* rb = pipeline->players[player_id].latency_rb;
    uint32_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);

    uint32_t total = write_pos < LATENCY_RB_CAPACITY ? write_pos : LATENCY_RB_CAPACITY;
    if (total > max_samples) total = max_samples;

    uint32_t start = write_pos - total;
    for (uint32_t i = 0; i < total; i++) {
        out[i] = rb->samples[(start + i) & (LATENCY_RB_CAPACITY - 1)];
    }
    return total;
}

uint32_t audio_pipeline_get_interval_samples(audio_pipeline_t* pipeline, int player_id,
                                               int64_t* out, uint32_t max_samples,
                                               uint32_t* out_write_pos) {
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));
    g_assert(out != NULL);

    const interval_rb_t* rb = pipeline->players[player_id].interval_rb;
    uint32_t write_pos = atomic_load_explicit(&rb->write_pos, memory_order_acquire);

    uint32_t total = write_pos < INTERVAL_RB_CAPACITY ? write_pos : INTERVAL_RB_CAPACITY;
    if (total > max_samples) total = max_samples;

    uint32_t start = write_pos - total;
    for (uint32_t i = 0; i < total; i++) {
        out[i] = rb->samples[(start + i) & (INTERVAL_RB_CAPACITY - 1)];
    }
    if (out_write_pos) *out_write_pos = write_pos;
    return total;
}

bool audio_pipeline_player_has_device_error(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return false;
    return atomic_load_explicit(&pipeline->players[player_id].device_error, memory_order_acquire);
}

bool audio_pipeline_player_streams_active(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return false;
    return atomic_load(&pipeline->players[player_id].streams_active);
}

void audio_pipeline_player_reconnect(audio_pipeline_t* pipeline, int player_id) {
    if (!pipeline || !valid_player(player_id)) return;
    audio_player_t *p = &pipeline->players[player_id];
    player_idle_data_t *d = g_new(player_idle_data_t, 1);
    d->player = p;
    d->generation = atomic_load(&p->stream_generation);
    g_idle_add(player_reconnect_idle, d);
}

int audio_pipeline_get_events(audio_pipeline_t* pipeline,
                               audio_pipeline_event_t* out,
                               int max) {
    if (!pipeline || !out || max <= 0) return 0;
    
    unsigned int write_idx = atomic_load(&pipeline->event_write);
    unsigned int read_idx = atomic_load(&pipeline->event_read);
    
    /* Handle wraparound: limit to ring buffer size */
    if (write_idx - read_idx > AUDIO_EVENT_RING_SIZE) {
        read_idx = write_idx - AUDIO_EVENT_RING_SIZE;
    }
    
    int count = 0;
    while (read_idx < write_idx && count < max) {
        unsigned int idx = read_idx % AUDIO_EVENT_RING_SIZE;
        out[count++] = pipeline->events[idx];
        read_idx++;
    }
    
    atomic_store(&pipeline->event_read, read_idx);
    return count;
}
