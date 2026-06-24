#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/audio.h"
#include "quadrature/settings.h"
#include "quadrature/thread_util.h"
#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Player — one channel.
 *
 * Owns a player's PipeWire output + monitor streams, the RT process callback
 * and its DSP fill loop, and the create/activate/deactivate/destroy lifecycle.
 * The threading model and per-field atomic ordering are documented on the
 * audio_player struct in internal.h; pipeline-facing entry points are declared
 * there as well.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* Unified Buffer Flush
 *
 * Coordinates flushing all audio buffers in a single call.
 * Call this after seek to prevent stale audio from playing.
 */
void
audio_player_flush_all(audio_player_t *p)
{
    g_assert(p != NULL);
    g_assert(p->shuttle_speed != NULL);

    audio_shuttle_speed_flush(p->shuttle_speed);
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
void
player_update_position_snap(
    audio_player_t *p, uint64_t position, float speed, bool playing, uint64_t sample_count)
{
    /* Seqlock writer. Single writer (audio thread; UI calls take the PW lock).
     *
     * The leading store only signals "writing in progress" — readers retry on
     * any odd value, so relaxed suffices. The trailing release-store is what
     * orders the data writes before publication: a reader's acquire-load that
     * observes the trailing value sees all prior data writes. */
    uint32_t seq = atomic_load_explicit(&p->position_seq, memory_order_relaxed);
    atomic_store_explicit(&p->position_seq, seq + 1, memory_order_relaxed);

    p->position_snap.position = position;
    p->position_snap.sample_count
        = sample_count ? sample_count
                       : atomic_load_explicit(&p->callback_sample_count, memory_order_relaxed);
    p->position_snap.speed = speed;
    p->position_snap.playing = playing ? 1 : 0;

    atomic_store_explicit(&p->position_seq, seq + 2, memory_order_release);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Processing - Buffer-Based Playback
 *
 * Unified playback through rate processor. Speed=1.0 is normal playback.
 *
 * Decomposition:
 *   render_from_buffer  — pulls samples from p->buffer through the shuttle_speed.
 *   on_track_end        — applies end-of-track policy (repeat / advance /
 *                         defer / stop). Touches state, not `out`.
 *   process_buffer_audio — fill loop that composes the two; postcondition:
 *                          `out` is fully written on return.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    EOT_REPEATED, /* same buffer rewound; caller should keep rendering */
    EOT_ADVANCED, /* swapped to next_buffer; caller should keep rendering */
    EOT_DEFERRED, /* waiting on main thread; rest of quantum is silence */
    EOT_STOPPED,  /* terminal: nothing left to play */
} eot_outcome_t;

/*
 * Atomically swap the player onto a preloaded next buffer.
 *
 * Ordering: writes that the main thread needs to *see together* as a unit
 * (length_samples, current_track_id, next_track_id=0, advance_old_track_id,
 * advance_pending=true) must all be visible by the time advance_pending is
 * observed. advance_pending is the release fence the main thread acquires on.
 */
static void
swap_to_next_buffer(audio_player_t *p, audio_buffer_t *next, int64_t next_track_id)
{
    audio_format_t nfmt = audio_buffer_get_format(next);
    g_assert(audio_format_equal(&nfmt, &p->format));

    int64_t old_track_id = atomic_load(&p->current_track_id);

    atomic_store_explicit(&p->buffer, next, memory_order_release);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);

    atomic_store(&p->current_track_id, next_track_id);
    atomic_store(&p->next_track_id, 0);

    atomic_store(&p->length_samples, audio_buffer_get_num_frames(next));
    audio_seek_position_set(&p->seek_position, 0);

    atomic_store(&p->advance_old_track_id, old_track_id);
    atomic_store(&p->advance_pending, true);
}

/*
 * Renders up to `capacity` frames from p->buffer through the shuttle_speed.
 *
 * Preconditions:
 *   - capacity > 0
 *   - p->buffer is NULL or a well-formed buffer (samples != NULL, num_frames > 0).
 *     Malformed buffers are an invariant violation, not a runtime case.
 *
 * Postconditions:
 *   - N frames produced; out[0 .. N*channels) written (interleaved). The
 *     tail out[N*channels .. capacity*channels) is untouched.
 *   - p->seek_position reflects the engine's new playhead.
 *   - *hit_eot is true iff the engine reached the end of the source buffer.
 */
static uint32_t
render_from_buffer(audio_player_t *p, float *out, uint32_t capacity, bool *hit_eot)
{
    g_assert(capacity > 0);
    *hit_eot = false;

    audio_buffer_t *buffer = atomic_load_explicit(&p->buffer, memory_order_acquire);
    if (!buffer)
        return 0;

    const float *samples = audio_buffer_get_samples(buffer);
    uint64_t num_frames = audio_buffer_get_num_frames(buffer);
    g_assert(samples != NULL && num_frames > 0);

    uint32_t produced = audio_shuttle_speed_process(
        p->shuttle_speed, &p->seek_position, samples, num_frames, out, capacity);

    *hit_eot = (audio_seek_position_get(&p->seek_position) >= num_frames);
    return produced;
}

/*
 * Applies the end-of-track policy. Mutates player state, emits one telemetry
 * event, and never touches `out`. Called only when render_from_buffer reported
 * hit_eot.
 *
 * On EOT_REPEATED / EOT_ADVANCED the caller MUST loop and render again to fill
 * the rest of the quantum from the new position. On EOT_DEFERRED / EOT_STOPPED
 * the caller fills the remaining frames with silence.
 */
static eot_outcome_t
on_track_end(audio_player_t *p, uint64_t cb_start)
{
    track_end_mode_t mode = (track_end_mode_t)atomic_load(&p->end_mode);

    if (mode == TRACK_END_REPEAT) {
        audio_seek_position_set(&p->seek_position, 0);
        return EOT_REPEATED;
    }

    audio_buffer_t *next = atomic_load_explicit(&p->next_buffer, memory_order_acquire);
    if (next) {
        int64_t old_track_id = atomic_load(&p->current_track_id);
        int64_t next_track_id = atomic_load(&p->next_track_id);
        swap_to_next_buffer(p, next, next_track_id);
        atomic_fetch_add_explicit(&p->stats_instant_advances, 1, memory_order_relaxed);
        audio_pipeline_publish_event(p->pipeline,
                                     (audio_pipeline_event_t){
                                         .timestamp_ns = cb_start,
                                         .type = AUDIO_EVENT_INSTANT_ADVANCE,
                                         .player_id = p->player_id,
                                         .track_id = old_track_id,
                                     });
        if (mode == TRACK_END_STOP) {
            atomic_store(&p->state, CHANNEL_STOPPED);
            return EOT_STOPPED;
        }
        return EOT_ADVANCED;
    }

    if (atomic_load(&p->next_track_id) > 0) {
        /* Next track exists but no preloaded buffer — defer to main thread.
         * Clear buffer so subsequent callbacks output silence cleanly. */
        atomic_store_explicit(&p->buffer, NULL, memory_order_release);
        atomic_store(&p->advance_pending, true);
        atomic_fetch_add_explicit(&p->stats_deferred_advances, 1, memory_order_relaxed);
        audio_pipeline_publish_event(p->pipeline,
                                     (audio_pipeline_event_t){
                                         .timestamp_ns = cb_start,
                                         .type = AUDIO_EVENT_DEFERRED_ADVANCE,
                                         .player_id = p->player_id,
                                         .track_id = atomic_load(&p->current_track_id),
                                     });
        if (mode == TRACK_END_STOP) {
            atomic_store(&p->state, CHANNEL_STOPPED);
        }
        return EOT_DEFERRED;
    }

    atomic_store(&p->state, CHANNEL_STOPPED);
    return EOT_STOPPED;
}

/*
 * Entry point. Guarantees out[0..frame_count*2) is fully written (audio
 * produced from one or more buffers, with any unfilled tail zeroed).
 *
 * Termination: each iteration either advances `written` toward frame_count
 * or exits via a terminal outcome. MAX_ADVANCES caps the buffer-swap chain
 * against pathological zero/near-zero-length buffers — a real "play next
 * track to fill out the quantum" needs at most one swap at normal speeds.
 */
static void
process_buffer_audio(audio_player_t *p, float *out, uint32_t frame_count, uint64_t cb_start)
{
    g_assert(p != NULL);
    g_assert(p->shuttle_speed != NULL);
    g_assert(out != NULL);
    g_assert(frame_count > 0);

    enum { MAX_ADVANCES = 2 };
    uint32_t written = 0;
    uint32_t advances = 0;

    const size_t spf = audio_format_samples_per_frame(&p->format);
    const size_t bpf = audio_format_bytes_per_frame(&p->format);

    while (written < frame_count) {
        bool hit_eot = false;
        written
            += render_from_buffer(p, out + (size_t)written * spf, frame_count - written, &hit_eot);
        if (!hit_eot)
            break;

        eot_outcome_t outcome = on_track_end(p, cb_start);
        if (outcome == EOT_DEFERRED || outcome == EOT_STOPPED)
            break;
        if (++advances > MAX_ADVANCES)
            break;
    }

    if (written < frame_count) {
        memset(out + (size_t)written * spf, 0, (frame_count - written) * bpf);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Process Callback
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
on_process(void *userdata)
{
    audio_player_t *p = (audio_player_t *)userdata;

    /* All players' streams run on the single PipeWire thread-loop thread; name
     * it (once — the guard means no syscall on the hot path) so the profiler
     * labels this thread's samples "audio-rt". */
    quad_set_thread_name("audio-rt");

    /* Invariants: PipeWire only invokes us once the stream is connected, which
     * means the player is fully constructed (back-ref + DSP engine wired). */
    g_assert(p->pipeline != NULL);
    g_assert(p->shuttle_speed != NULL);

    /* RT-safe timing: VDSO-mapped clock_gettime (~20ns) */
    uint64_t cb_start = time_ns();

    /* Gate all metrics on stream state — skip if not streaming */
    enum pw_stream_state pw_state = atomic_load_explicit(&p->pw_stream_state, memory_order_relaxed);
    if (pw_state != PW_STREAM_STATE_STREAMING) {
        struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
        if (b) {
            /* Output silence and re-queue without recording any metrics */
            float *out = (float *)b->buffer->datas[0].data;
            if (out) {
                const size_t bpf = audio_format_bytes_per_frame(&p->format);
                uint32_t max_frames = b->buffer->datas[0].maxsize / bpf;
                uint32_t frame_count = SPA_MIN(b->requested, max_frames);
                memset(out, 0, frame_count * bpf);
                b->buffer->datas[0].chunk->offset = 0;
                b->buffer->datas[0].chunk->stride = bpf;
                b->buffer->datas[0].chunk->size = frame_count * bpf;
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
    audio_pipeline_t *pl = p->pipeline;

    int64_t interval_dev_ns = 0;
    bool has_interval = (p->last_callback_ns > 0);
    if (has_interval) {
        uint64_t interval = cb_start - p->last_callback_ns;
        uint64_t expected = (uint64_t)p->quantum_frames * pl->ns_per_frame;
        interval_dev_ns = (int64_t)interval - (int64_t)expected;
        /* Track peak absolute deviation for ring buffer sampling */
        int64_t abs_dev = interval_dev_ns > 0 ? interval_dev_ns : -interval_dev_ns;
        if (abs_dev > p->interval_peak_dev_ns)
            p->interval_peak_dev_ns = abs_dev;
    }
    p->last_callback_ns = cb_start;

    struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
    if (!b) {
        atomic_fetch_add_explicit(&p->stats_dequeue_failures, 1, memory_order_relaxed);
        audio_pipeline_publish_event(pl,
                                     (audio_pipeline_event_t){
                                         .timestamp_ns = cb_start,
                                         .type = AUDIO_EVENT_DEQUEUE_FAILURE,
                                         .player_id = p->player_id,
                                         .track_id = atomic_load(&p->current_track_id),
                                     });
        return;
    }

    float *out = (float *)b->buffer->datas[0].data;
    if (!out) {
        pw_stream_queue_buffer(p->stream, b);
        return;
    }
    const size_t bpf = audio_format_bytes_per_frame(&p->format);
    uint32_t max_frames = b->buffer->datas[0].maxsize / bpf;
    uint32_t frame_count = SPA_MIN(b->requested, max_frames);

    /* Read PipeWire native metrics (RT-safe via pw_stream_get_time_n) */
#if PW_CHECK_VERSION(0, 3, 50)
    if (pl->perf) {
        struct pw_time pw_t;
        if (pw_stream_get_time_n(p->stream, &pw_t, sizeof(pw_t)) == 0) {
            atomic_store_explicit(&pl->perf->pw_avail_buffers[p->player_id],
                                  (uint64_t)pw_t.avail_buffers,
                                  memory_order_relaxed);
            atomic_store_explicit(&pl->perf->pw_queued_buffers[p->player_id],
                                  (uint64_t)pw_t.queued_buffers,
                                  memory_order_relaxed);
            atomic_store_explicit(
                &pl->perf->pw_delay_samples[p->player_id], pw_t.delay, memory_order_relaxed);
        }
    }
#endif

    /* Read current state */
    channel_state_t state = atomic_load(&p->state);
    float shuttle_speed = audio_shuttle_speed_get_speed(p->shuttle_speed);
    bool shuttling = (fabsf(shuttle_speed - 1.0f) > SPEED_STOPPED_EPSILON);

    /* Increment sample counter and get current value (RT-safe timestamp).
     * Relaxed: the counter has no ordering relation to other data. UI reads it
     * via the position seqlock, which provides its own publication ordering. */
    uint64_t sample_count
        = atomic_fetch_add_explicit(&p->callback_sample_count, frame_count, memory_order_relaxed);

    /* Process audio when:
     * - PLAYING state, OR
     * - Actively shuttling (speed != 1.0) regardless of play state
     * STOPPED state always outputs silence - stop means stop */
    audio_buffer_t *buf = atomic_load_explicit(&p->buffer, memory_order_acquire);
    bool should_play = buf && (state == CHANNEL_PLAYING || shuttling);

    /* Per-player stats (RT-safe: relaxed atomics) */
    atomic_fetch_add_explicit(&p->stats_cb_count, 1, memory_order_relaxed);

    /* Buffer underrun: PLAYING but no buffer loaded. Per-player stat (the rate
     * is reported per-player to UI) plus one event for the telemetry ring. */
    if (state == CHANNEL_PLAYING && !buf) {
        atomic_fetch_add_explicit(&p->stats_buffer_underruns, 1, memory_order_relaxed);
        audio_pipeline_publish_event(
            pl,
            (audio_pipeline_event_t){ .timestamp_ns = cb_start,
                                      .type = AUDIO_EVENT_BUFFER_UNDERRUN,
                                      .player_id = p->player_id,
                                      .track_id = atomic_load(&p->current_track_id),
                                      .data.underrun = {
                                          .requested_frames = frame_count,
                                          .available_frames = 0,
                                          .speed = shuttle_speed,
                                      } });
    }

    if (should_play) {
        /* Process audio from buffer */
        process_buffer_audio(p, out, frame_count, cb_start);

        /* Detect shuttle_speed underflows by comparing count before/after */
        uint64_t cur_underflows = audio_shuttle_speed_get_underflows(p->shuttle_speed);
        if (cur_underflows > p->prev_shuttle_speed_underflows) {
            uint32_t delta = (uint32_t)(cur_underflows - p->prev_shuttle_speed_underflows);
            audio_pipeline_publish_event(
                pl,
                (audio_pipeline_event_t){ .timestamp_ns = cb_start,
                                          .type = AUDIO_EVENT_SHUTTLE_SPEED_UNDERFLOW,
                                          .player_id = p->player_id,
                                          .track_id = atomic_load(&p->current_track_id),
                                          .data.underrun = {
                                              .requested_frames = frame_count,
                                              .available_frames = frame_count - delta,
                                              .speed = shuttle_speed,
                                          } });
            p->prev_shuttle_speed_underflows = cur_underflows;
        }
        /* seek_position was written by the shuttle engine during process(); no
         * second store needed here. */
    } else {
        memset(out, 0, frame_count * bpf);
    }

    /* ALWAYS update snapshot - callback is the single owner when running
     * State-change functions take the PipeWire lock to synchronize with this */
    player_update_position_snap(p,
                                audio_seek_position_get(&p->seek_position),
                                shuttle_speed,
                                state == CHANNEL_PLAYING,
                                sample_count);

    /* Record this callback's timing into the telemetry rings and emit any
     * derived fault events (scheduling delay, budget overrun). */
    audio_telemetry_record_callback(p, frame_count, cb_start, has_interval, interval_dev_ns);

    b->buffer->datas[0].chunk->offset = 0;
    b->buffer->datas[0].chunk->stride = bpf;
    b->buffer->datas[0].chunk->size = frame_count * bpf;
    pw_stream_queue_buffer(p->stream, b);
}

static gboolean player_deactivate_idle(gpointer data);
static void player_deactivate_streams(audio_player_t *p);

static void
on_state_changed(void *userdata,
                 enum pw_stream_state old,
                 enum pw_stream_state state,
                 const char *error)
{
    audio_player_t *p = (audio_player_t *)userdata;
    (void)old;
    (void)error;
    atomic_store_explicit(&p->pw_stream_state, (int)state, memory_order_relaxed);

    if (state == PW_STREAM_STATE_ERROR) {
        atomic_store_explicit(&p->device_error, true, memory_order_release);
        if (p->stream)
            pw_stream_set_active(p->stream, false);
        audio_pipeline_publish_event(
            p->pipeline,
            (audio_pipeline_event_t){
                .type = AUDIO_EVENT_PW_ERROR,
                .timestamp_ns = time_ns(),
                .player_id = p->player_id,
                .track_id = atomic_load_explicit(&p->current_track_id, memory_order_relaxed),
            });

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

static void
on_stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
    audio_player_t *p = data;
    (void)size;
    /* area == NULL for SPA_IO_Position signals a PipeWire xrun */
    if (id == SPA_IO_Position && area == NULL) {
        audio_pipeline_publish_event(
            p->pipeline,
            (audio_pipeline_event_t){
                .type = AUDIO_EVENT_PW_XRUN,
                .timestamp_ns = time_ns(),
                .player_id = p->player_id,
                .track_id = atomic_load_explicit(&p->current_track_id, memory_order_relaxed),
            });
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_state_changed,
    .io_changed = on_stream_io_changed,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PipeWire Stream Creation Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Flags shared by every quadrature stream connect (output + monitor, initial +
 * reconnect). Kept in one place so a change can't be applied to one path and
 * silently missed on another. */
#define QUADRATURE_PW_STREAM_FLAGS \
    (PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS)

/* Build the EnumFormat param describing our canonical F32 wire format into the
 * caller's pod builder. spa_format_audio_raw_build serializes into the builder
 * during the call, so the compound literal's lifetime ends safely here. */
static const struct spa_pod *
build_audio_format_param(struct spa_pod_builder *b, const audio_format_t *fmt)
{
    return spa_format_audio_raw_build(b,
                                      SPA_PARAM_EnumFormat,
                                      &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
                                                               .channels = fmt->channels,
                                                               .rate = fmt->sample_rate));
}

/* Tear down one PipeWire stream and its listener hook, leaving *stream NULL.
 * No-op if already NULL. Used by both deactivate (transient) and destroy. */
static void
destroy_stream(struct pw_stream **stream, struct spa_hook *listener)
{
    if (!*stream)
        return;
    spa_hook_remove(listener);
    pw_stream_disconnect(*stream);
    pw_stream_destroy(*stream);
    *stream = NULL;
}

static quadrature_result_t
create_player_stream(audio_player_t *p, uint32_t sample_rate)
{
    char stream_name[64];
    snprintf(stream_name, sizeof(stream_name), "quadrature-player-%d", p->player_id);

    char latency_str[32];
    snprintf(latency_str, sizeof(latency_str), "%u/%u", p->quantum_frames, sample_rate);

    struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                                    "Audio",
                                                    PW_KEY_MEDIA_CATEGORY,
                                                    "Playback",
                                                    PW_KEY_MEDIA_ROLE,
                                                    "Music",
                                                    PW_KEY_NODE_LATENCY,
                                                    latency_str,
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
    const struct spa_pod *params[1];
    params[0] = build_audio_format_param(&b, &p->format);
    (void)sample_rate; /* p->format.sample_rate is the source of truth */

    /* Cache params for auto-reconnect: copy pod bytes + fix up pointer */
    ptrdiff_t pod_offset = (const uint8_t *)params[0] - buffer;
    memcpy(p->cached_params_buf, buffer, sizeof(buffer));
    p->cached_params[0] = (const struct spa_pod *)(p->cached_params_buf + pod_offset);
    p->num_cached_params = 1;

    int res = pw_stream_connect(
        p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, QUADRATURE_PW_STREAM_FLAGS, params, 1);

    if (res < 0) {
        destroy_stream(&p->stream, &p->stream_listener);
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

static void
on_monitor_process(void *userdata)
{
    audio_player_t *p = (audio_player_t *)userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(p->monitor_stream);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    float *in = d->data;
    if (!in || d->chunk->size == 0) {
        pw_stream_queue_buffer(p->monitor_stream, b);
        return;
    }

    /* Only run the (expensive) cava/FFTW spectrum pass while this channel is
     * actually playing. The sink monitor keeps delivering buffers when the
     * channel is stopped/paused (device silence, or other apps' output), and
     * running the full FFT on that is wasted CPU on the real-time thread —
     * profiling showed ~16% of total CPU burned here on an idle player. We
     * still drain + requeue the buffer so the capture stream doesn't back up. */
    if (atomic_load(&p->state) != CHANNEL_PLAYING) {
        pw_stream_queue_buffer(p->monitor_stream, b);
        return;
    }

    uint32_t n_frames = d->chunk->size / audio_format_bytes_per_frame(&p->format);

    /* Process spectrum FFT inline — no ring buffer, no separate thread */
    spectrum_process(&p->spectrum, in, n_frames, p->spectrum_bars, &p->spectrum_generation);

    pw_stream_queue_buffer(p->monitor_stream, b);
}

static const struct pw_stream_events monitor_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_monitor_process,
};

static quadrature_result_t
create_monitor_stream(audio_player_t *p, uint32_t sample_rate)
{
    char name[64];
    snprintf(name, sizeof(name), "quadrature-spectrum-%d", p->player_id);

    struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE,
                                                    "Audio",
                                                    PW_KEY_MEDIA_CATEGORY,
                                                    "Capture",
                                                    PW_KEY_MEDIA_ROLE,
                                                    "Music",
                                                    PW_KEY_STREAM_CAPTURE_SINK,
                                                    "true", /* capture from sink monitor */
                                                    PW_KEY_NODE_PASSIVE,
                                                    "true", /* don't keep sink alive */
                                                    NULL);

    if (p->target_device[0]) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, p->target_device);
    }

    p->monitor_stream = pw_stream_new(p->pipeline->core, name, props);
    if (p->monitor_stream) {
        pw_stream_add_listener(
            p->monitor_stream, &p->monitor_stream_listener, &monitor_stream_events, p);
    }

    if (!p->monitor_stream) {
        return QUADRATURE_ERROR_INTERNAL;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = build_audio_format_param(&b, &p->format);
    (void)sample_rate; /* p->format.sample_rate is the source of truth */

    int res = pw_stream_connect(
        p->monitor_stream, PW_DIRECTION_INPUT, PW_ID_ANY, QUADRATURE_PW_STREAM_FLAGS, params, 1);

    if (res < 0) {
        destroy_stream(&p->monitor_stream, &p->monitor_stream_listener);
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
gboolean
player_reconnect_idle(gpointer data)
{
    player_idle_data_t *d = data;
    audio_player_t *p = d->player;
    unsigned int sched_gen = d->generation;
    g_free(d);

    audio_pipeline_t *pipeline = p->pipeline;

    /* Stale: stream was recreated since this idle was scheduled */
    if (atomic_load(&p->stream_generation) != sched_gen) {
        g_debug("Player %d: reconnect_idle skipped (generation %u → %u)",
                p->player_id,
                sched_gen,
                atomic_load(&p->stream_generation));
        return G_SOURCE_REMOVE;
    }

    if (!atomic_load_explicit(&p->device_error, memory_order_acquire))
        return G_SOURCE_REMOVE; /* Already recovered */

    pw_thread_loop_lock(pipeline->loop);
    if (!p->stream) {
        pw_thread_loop_unlock(pipeline->loop);
        return G_SOURCE_REMOVE;
    }
    pw_stream_disconnect(p->stream);
    pw_stream_connect(p->stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      QUADRATURE_PW_STREAM_FLAGS,
                      p->cached_params,
                      p->num_cached_params);

    /* Reconnect monitor stream alongside output stream */
    if (p->monitor_stream) {
        pw_stream_disconnect(p->monitor_stream);
        uint8_t mbuf[1024];
        struct spa_pod_builder mb = SPA_POD_BUILDER_INIT(mbuf, sizeof(mbuf));
        const struct spa_pod *mparams[1];
        mparams[0] = build_audio_format_param(&mb, &p->format);
        pw_stream_connect(p->monitor_stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          QUADRATURE_PW_STREAM_FLAGS,
                          mparams,
                          1);
    }
    pw_thread_loop_unlock(pipeline->loop);

    /* state_changed callback will clear device_error when STREAMING */
    return G_SOURCE_REMOVE;
}

/**
 * Deactivate a player's streams after persistent device failure.
 * Runs on the main GLib thread via g_idle_add().
 */
static gboolean
player_deactivate_idle(gpointer data)
{
    player_idle_data_t *d = data;
    audio_player_t *p = d->player;
    unsigned int sched_gen = d->generation;
    g_free(d);

    audio_pipeline_t *pipeline = p->pipeline;
    if (!pipeline)
        return G_SOURCE_REMOVE;

    /* Stale: stream was recreated since this idle was scheduled */
    if (atomic_load(&p->stream_generation) != sched_gen) {
        g_debug("Player %d: deactivate_idle skipped (generation %u → %u)",
                p->player_id,
                sched_gen,
                atomic_load(&p->stream_generation));
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

quadrature_result_t
player_init(audio_player_t *p, int id, audio_format_t format)
{
    memset(p, 0, sizeof(*p));
    p->player_id = id;

    /* Each player starts in the pipeline's canonical format. Per-player
     * divergence (different output sinks/channel counts) is a future feature;
     * the format-match assert in set_player_track is where the conversion
     * stage will live. */
    p->format = format;

    /* Initialize per-player spectrum state (cava FFT plan + buffers) */
    quadrature_result_t spec_res = spectrum_init(&p->spectrum, format.sample_rate);
    if (spec_res != QUADRATURE_OK) {
        return spec_res;
    }

    atomic_store(&p->state, CHANNEL_STOPPED);
    atomic_store(&p->pw_stream_state, (int)PW_STREAM_STATE_UNCONNECTED);
    atomic_store_explicit(&p->device_error, false, memory_order_relaxed);

    /* Heap-allocate ring buffers (~128KB each, keeps audio_player_t small) */
    p->budget_rb = calloc(1, sizeof(*p->budget_rb));
    p->latency_rb = calloc(1, sizeof(*p->latency_rb));
    p->interval_rb = calloc(1, sizeof(*p->interval_rb));
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
    p->prev_shuttle_speed_underflows = 0;

    /* Initialize playback options */
    atomic_store(&p->end_mode, TRACK_END_AUTOPLAY); /* Default: advance to next */

    /* Initialize track_id state */
    atomic_store(&p->current_track_id, 0);
    atomic_store(&p->next_track_id, 0);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);
    atomic_store(&p->advance_pending, false);
    atomic_store(&p->advance_old_track_id, 0);
    atomic_store(&p->pending_buffer_track_id, 0);

    /* Initialize buffer state */
    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    audio_seek_position_init(&p->seek_position, 0);

    /* Create rate processor for variable-speed playback */
    quadrature_result_t shuttle_res = audio_shuttle_speed_create(p->format, &p->shuttle_speed);
    if (shuttle_res != QUADRATURE_OK) {
        spectrum_cleanup(&p->spectrum);
        return shuttle_res;
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
        atomic_store_explicit(&p->spectrum_bars[i], 0.0f, memory_order_relaxed);
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

    g_message("Player %d initialized (%uHz, dormant — no device)", id, format.sample_rate);
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
static quadrature_result_t
player_activate_streams(audio_player_t *p, uint32_t sample_rate)
{
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
static void
player_deactivate_streams(audio_player_t *p)
{
    if (!atomic_load(&p->streams_active) && !p->stream && !p->monitor_stream)
        return;

    /* Stop playback first */
    int prev_state = atomic_exchange(&p->state, CHANNEL_STOPPED);
    (void)prev_state;

    destroy_stream(&p->monitor_stream, &p->monitor_stream_listener);
    destroy_stream(&p->stream, &p->stream_listener);

    /* Reset spectrum state so stale data doesn't linger */
    p->spectrum.input_buffer_fill = 0;
    for (int i = 0; i < SPECTRUM_BARS * 2; i++) {
        atomic_store_explicit(&p->spectrum_bars[i], 0.0f, memory_order_relaxed);
    }

    /* Reset stream-related state */
    atomic_store_explicit(
        &p->pw_stream_state, (int)PW_STREAM_STATE_UNCONNECTED, memory_order_relaxed);
    atomic_store_explicit(&p->device_error, false, memory_order_release);
    atomic_store(&p->streams_active, false);
    atomic_store(&p->reconnect_attempted, false);
    p->last_callback_ns = 0;
    p->interval_peak_dev_ns = 0;

    g_message("Player %d streams deactivated", p->player_id);
}

void
player_destroy(audio_player_t *p, audio_cache_t *cache)
{
    destroy_stream(&p->monitor_stream, &p->monitor_stream_listener);
    destroy_stream(&p->stream, &p->stream_listener);

    /* Clear buffer pointers */
    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);

    /* Unlock any locked tracks */
    int64_t current_id = atomic_load(&p->current_track_id);
    int64_t next_id = atomic_load(&p->next_track_id);
    if (cache && current_id > 0) {
        audio_cache_unlock(cache, current_id, AUDIO_CACHE_UNLOCK_IMMEDIATE);
    }
    if (cache && next_id > 0) {
        audio_cache_unlock(cache, next_id, AUDIO_CACHE_UNLOCK_IMMEDIATE);
    }
    spectrum_cleanup(&p->spectrum);
    free(p->budget_rb);
    p->budget_rb = NULL;
    free(p->latency_rb);
    p->latency_rb = NULL;
    free(p->interval_rb);
    p->interval_rb = NULL;
    if (p->shuttle_speed) {
        audio_shuttle_speed_destroy(p->shuttle_speed);
        p->shuttle_speed = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper to recreate a player's stream with a new target device
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
player_recreate_stream(audio_player_t *p, uint32_t sample_rate, const char *target_device)
{
    channel_state_t prev_state = atomic_load(&p->state);

    /* Bump generation so stale reconnect/deactivate idles are no-ops */
    atomic_fetch_add(&p->stream_generation, 1);

    /* Tear down existing streams */
    player_deactivate_streams(p);

    /* Update target device */
    if (target_device && target_device[0]) {
        g_strlcpy(p->target_device, target_device, sizeof(p->target_device));
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
