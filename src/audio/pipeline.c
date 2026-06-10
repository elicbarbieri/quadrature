#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/audio.h"
#include "quadrature/library.h"
#include <glib.h>
#include <pipewire/pipewire.h>

#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline — the container.
 *
 * Owns the array of players, the shared PipeWire context, the audio cache and
 * perf dashboard, and the 50ms main-thread loop that drives auto-advance and
 * next-track preloading. Exposes the public audio_pipeline_* control API.
 * Per-channel mechanics (RT callback, streams, lifecycle) live in player.c.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Pipeline Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declaration for auto-advance timeout */
static gboolean advance_timeout_callback(gpointer user_data);

quadrature_result_t
audio_pipeline_create(library_cache_t *library,
                      uint32_t sample_rate,
                      uint32_t channels,
                      audio_pipeline_t **pipeline)
{
    if (!pipeline || channels < 1 || channels > 16)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *pipeline = NULL;

    pw_init(NULL, NULL);

    audio_pipeline_t *p = calloc(1, sizeof(*p));
    if (!p)
        return QUADRATURE_ERROR_OUT_OF_MEMORY;

    /* calloc zero-inits the rest (callbacks, event ring, flags); only the
     * meaningful non-zero fields are set explicitly. This is the canonical
     * format: cache + buffers + players all inherit it. */
    p->format = (audio_format_t){ .sample_rate = sample_rate, .channels = channels };
    p->ns_per_frame = 1000000000ULL / sample_rate;
    p->library = library;

    /* Resources are acquired in order below; each failure jumps to the label
     * that unwinds exactly what was acquired so far, in reverse. */
    quadrature_result_t ret = QUADRATURE_ERROR_INTERNAL;
    int initialized = 0; /* players successfully player_init()'d */

    p->loop = pw_thread_loop_new("quadrature", NULL);
    if (!p->loop)
        goto err_free;

    p->pw_ctx = pw_context_new(pw_thread_loop_get_loop(p->loop), NULL, 0);
    if (!p->pw_ctx)
        goto err_destroy_loop;

    p->core = pw_context_connect(p->pw_ctx, NULL, 0);
    if (!p->core)
        goto err_destroy_ctx;

    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++) {
        quadrature_result_t r = player_init(&p->players[i], i, p->format);
        if (r != QUADRATURE_OK) {
            ret = r;
            goto err_destroy_players;
        }
        initialized = i + 1;
    }

    /* Audio cache and perf dashboard are non-fatal: the pipeline runs without
     * them, only with reduced functionality. */
    if (audio_cache_create(library, p->format, &p->cache) != QUADRATURE_OK) {
        g_warning("Audio cache creation failed - continuing without cache");
        p->cache = NULL;
    }
    if (perf_dashboard_create(sample_rate, &p->perf) != QUADRATURE_OK) {
        g_warning("Performance dashboard creation failed - continuing without perf");
        p->perf = NULL;
    }
    if (p->perf) {
        perf_dashboard_set_audio_pipeline(p->perf, p);
        perf_dashboard_set_audio_cache(p->perf, p->cache);
    }

    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++)
        p->players[i].pipeline = p;

    if (pw_thread_loop_start(p->loop) < 0)
        goto err_teardown_full;

    atomic_store(&p->system_active, true);
    *pipeline = p;
    p->advance_timeout_id = g_timeout_add(50, advance_timeout_callback, p);

    g_message("Pipeline created (%d players, %uHz)", MAX_AUDIO_PLAYERS, sample_rate);
    return QUADRATURE_OK;

err_teardown_full: /* loop_start failed: cache/perf may exist, players fully init'd */
    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++)
        player_destroy(&p->players[i], p->cache);
    if (p->perf)
        perf_dashboard_destroy(p->perf);
    if (p->cache)
        audio_cache_destroy(p->cache);
    pw_core_disconnect(p->core);
    goto err_destroy_ctx;

err_destroy_players: /* player_init failed at index `initialized`; no cache/perf yet */
    for (int j = 0; j < initialized; j++)
        player_destroy(&p->players[j], NULL);
    pw_core_disconnect(p->core);
err_destroy_ctx:
    pw_context_destroy(p->pw_ctx);
err_destroy_loop:
    pw_thread_loop_destroy(p->loop);
err_free:
    free(p);
    return ret;
}

void
audio_pipeline_destroy(audio_pipeline_t *pipeline)
{
    if (!pipeline)
        return;

    /* Remove GLib advance timer before anything else — prevents callbacks
     * firing on partially-destroyed pipeline state */
    if (pipeline->advance_timeout_id > 0) {
        g_source_remove(pipeline->advance_timeout_id);
        pipeline->advance_timeout_id = 0;
    }

    /* Clear track callbacks so no stale pointer can be invoked */
    pipeline->track_changed_callback = NULL;
    pipeline->track_changed_user_data = NULL;
    pipeline->track_failed_callback = NULL;
    pipeline->track_failed_user_data = NULL;

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

    if (pipeline->core)
        pw_core_disconnect(pipeline->core);
    if (pipeline->pw_ctx)
        pw_context_destroy(pipeline->pw_ctx);
    if (pipeline->loop)
        pw_thread_loop_destroy(pipeline->loop);

    pw_deinit();

    free(pipeline);
    g_message("Pipeline destroyed");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Control
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool
valid_player(int id)
{
    return id >= 0 && id < MAX_AUDIO_PLAYERS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Track ID Based Player Control (New API)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Find the track that follows current_id in its album and preload it as this
 * player's next_buffer, so end-of-track advance is gapless.
 *
 * No-op under TRACK_END_REPEAT or when there is no library/cache. The decode
 * is kicked off asynchronously; if it has already finished the buffer is
 * attached immediately, otherwise next_track_id is recorded and the 50ms poll
 * (try_attach_next_buffer) attaches the buffer once the decode completes.
 */
static void
preload_next_track(audio_pipeline_t *pipeline, audio_player_t *p, int64_t current_id)
{
    if ((track_end_mode_t)atomic_load(&p->end_mode) == TRACK_END_REPEAT)
        return;
    if (!pipeline->library || !pipeline->cache)
        return;

    int64_t next_id = library_cache_get_next_track_id(pipeline->library, current_id);
    if (next_id <= 0)
        return;

    atomic_store(&p->next_track_id, next_id);
    audio_cache_load(pipeline->cache, next_id);

    /* Only fetch the buffer if the decode is already done — never block on a
     * preload. get_locked requires the track to be locked, which we just did. */
    if (audio_cache_lock(pipeline->cache, next_id) != AUDIO_CACHE_READY)
        return;

    audio_buffer_t *next_buf = audio_cache_get_locked(pipeline->cache, next_id);
    if (next_buf)
        atomic_store_explicit(&p->next_buffer, next_buf, memory_order_release);
}

/*
 * Attach the preloaded next track's buffer once its decode completes. Safe to
 * call every poll tick: no-op if there is no next track, the buffer is already
 * attached, or the decode is still in flight. The track was locked by
 * preload_next_track(), satisfying get_locked()'s precondition.
 */
static void
try_attach_next_buffer(audio_pipeline_t *pipeline, audio_player_t *p)
{
    if (!pipeline->cache)
        return;

    int64_t next_id = atomic_load(&p->next_track_id);
    if (next_id <= 0)
        return;
    if (atomic_load_explicit(&p->next_buffer, memory_order_acquire))
        return; /* already attached */
    if (audio_cache_get_status(pipeline->cache, next_id) != AUDIO_CACHE_READY)
        return;

    audio_buffer_t *next_buf = audio_cache_get_locked(pipeline->cache, next_id);
    if (next_buf)
        atomic_store_explicit(&p->next_buffer, next_buf, memory_order_release);
}

quadrature_result_t
audio_pipeline_set_player_track(audio_pipeline_t *pipeline, int player_id, int64_t track_id)
{
    if (!pipeline || !valid_player(player_id) || track_id <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (!pipeline->cache) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    audio_player_t *p = &pipeline->players[player_id];

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
                "call audio_cache_load() first",
                track_id);
    }

    /* Get old track IDs for cleanup */
    int64_t old_current_id = atomic_load(&p->current_track_id);
    int64_t old_next_id = atomic_load(&p->next_track_id);

    /* Clear buffer pointers - audio callback outputs silence when buffer is NULL.
     * Release on the buffer stores publishes them to the RT thread; the
     * pending_buffer_track_id store is data, not synchronization. */
    atomic_store_explicit(&p->buffer, NULL, memory_order_release);
    atomic_store_explicit(&p->next_buffer, NULL, memory_order_release);
    atomic_store_explicit(&p->pending_buffer_track_id, 0, memory_order_relaxed);

    if (old_current_id > 0)
        audio_cache_unlock(pipeline->cache, old_current_id, AUDIO_CACHE_UNLOCK_DEFERRED);
    if (old_next_id > 0)
        audio_cache_unlock(pipeline->cache, old_next_id, AUDIO_CACHE_UNLOCK_DEFERRED);

    /* Reset state */
    atomic_store(&p->current_track_id, track_id);
    atomic_store(&p->next_track_id, 0);
    audio_seek_position_set(&p->seek_position, 0);
    audio_shuttle_speed_flush(p->shuttle_speed);

    /* Lock the track (already in cache due to precondition) */
    audio_cache_status_t lock_result = audio_cache_lock(pipeline->cache, track_id);

    if (lock_result == AUDIO_CACHE_FAILED) {
        g_warning("Failed to lock track %" G_GINT64_FORMAT " - decode failed", track_id);
        audio_cache_unlock(pipeline->cache, track_id, AUDIO_CACHE_UNLOCK_IMMEDIATE);
        atomic_store(&p->current_track_id, 0);
        if (pipeline->track_failed_callback)
            pipeline->track_failed_callback(player_id, track_id, pipeline->track_failed_user_data);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (lock_result == AUDIO_CACHE_READY) {
        /* Already decoded - set buffer immediately */
        audio_buffer_t *buf = audio_cache_get_locked(pipeline->cache, track_id);
        if (!buf) {
            g_warning("Buffer unavailable for track %" G_GINT64_FORMAT " despite READY status",
                      track_id);
            audio_cache_unlock(pipeline->cache, track_id, AUDIO_CACHE_UNLOCK_IMMEDIATE);
            return QUADRATURE_ERROR_INTERNAL;
        }

        /* Format invariant: a future conversion stage replaces this with a
         * downmix/upmix. Today, mismatch is a configuration bug. */
        audio_format_t bfmt = audio_buffer_get_format(buf);
        g_assert(audio_format_equal(&bfmt, &p->format));

        atomic_store_explicit(&p->buffer, buf, memory_order_release);
        atomic_store(&p->length_samples, audio_buffer_get_num_frames(buf));

        /* Position snapshot: just store atomic position; the audio callback
         * will write a full position_snap within ~10ms. Avoids taking the
         * PW lock on the GTK main thread which can deadlock during device changes. */
        audio_seek_position_set(&p->seek_position, 0);

        /* Fire callback immediately */
        if (pipeline->track_changed_callback) {
            pipeline->track_changed_callback(
                player_id, track_id, pipeline->track_changed_user_data);
        }

        preload_next_track(pipeline, p, track_id);
    } else {
        /* LOADING - mark pending for 50ms timeout to check */
        atomic_store(&p->pending_buffer_track_id, track_id);
        g_debug(
            "Player %d: track %" G_GINT64_FORMAT " still decoding, will poll", player_id, track_id);

        preload_next_track(pipeline, p, track_id);
    }

    return QUADRATURE_OK;
}

int64_t
audio_pipeline_get_player_track_id(audio_pipeline_t *pipeline, int player_id)
{
    if (!pipeline || !valid_player(player_id))
        return 0;
    return atomic_load(&pipeline->players[player_id].current_track_id);
}

void
audio_pipeline_set_track_changed_callback(audio_pipeline_t *pipeline,
                                          audio_track_changed_cb callback,
                                          void *user_data)
{
    g_assert(pipeline != NULL);
    pipeline->track_changed_callback = callback;
    pipeline->track_changed_user_data = user_data;
}

void
audio_pipeline_set_track_failed_callback(audio_pipeline_t *pipeline,
                                         audio_track_failed_cb callback,
                                         void *user_data)
{
    g_assert(pipeline != NULL);
    pipeline->track_failed_callback = callback;
    pipeline->track_failed_user_data = user_data;
}

/*
 * Poll a non-blocking track set: if the decode the RT thread is waiting on has
 * finished, attach the buffer and fire the track-changed callback; if it
 * failed, clear the pending state. No-op while the decode is still in flight.
 * Early-returns keep this flat — there is no work unless a buffer is pending.
 */
static void
poll_pending_buffer(audio_pipeline_t *pipeline, audio_player_t *p, int player_id)
{
    int64_t pending_id = atomic_load(&p->pending_buffer_track_id);
    if (pending_id <= 0 || !pipeline->cache)
        return;

    audio_cache_status_t status = audio_cache_get_status(pipeline->cache, pending_id);

    if (status == AUDIO_CACHE_FAILED) {
        g_warning("Player %d: track %" G_GINT64_FORMAT " decode failed", player_id, pending_id);
        atomic_store(&p->pending_buffer_track_id, 0);
        atomic_store(&p->current_track_id, 0);
        audio_cache_unlock(pipeline->cache, pending_id, AUDIO_CACHE_UNLOCK_IMMEDIATE);
        if (pipeline->track_failed_callback)
            pipeline->track_failed_callback(
                player_id, pending_id, pipeline->track_failed_user_data);
        return;
    }
    if (status != AUDIO_CACHE_READY)
        return; /* LOADING: keep polling next tick */

    audio_buffer_t *buf = audio_cache_get_locked(pipeline->cache, pending_id);
    if (!buf)
        return;

    audio_format_t bfmt = audio_buffer_get_format(buf);
    g_assert(audio_format_equal(&bfmt, &p->format));
    atomic_store_explicit(&p->buffer, buf, memory_order_release);
    atomic_store(&p->length_samples, audio_buffer_get_num_frames(buf));
    atomic_store(&p->pending_buffer_track_id, 0);

    g_debug("Player %d: track %" G_GINT64_FORMAT " decode complete, buffer attached",
            player_id,
            pending_id);

    /* Position snapshot: just store atomic position; the audio callback will
     * write a full position_snap within ~10ms. Avoids taking the PW lock on the
     * GTK main thread which can deadlock during device changes. */
    audio_seek_position_set(&p->seek_position, 0);

    if (pipeline->track_changed_callback)
        pipeline->track_changed_callback(player_id, pending_id, pipeline->track_changed_user_data);

    /* The preloaded next buffer (if any) is attached by try_attach_next_buffer()
     * in the caller. */
}

/*
 * Complete a pending auto-advance flagged by the RT callback. Two shapes:
 *   - Deferred: no buffer was preloaded, so load + set the next track here.
 *   - Instant:  the RT thread already swapped buffers; just unlock the old
 *               track, preload the new next, and fire the callback.
 */
static void
handle_pending_advance(audio_pipeline_t *pipeline, audio_player_t *p, int player_id)
{
    if (!atomic_load(&p->advance_pending))
        return;
    atomic_store(&p->advance_pending, false);

    int64_t old_track_id = atomic_exchange(&p->advance_old_track_id, 0);
    audio_buffer_t *current_buf = atomic_load_explicit(&p->buffer, memory_order_acquire);

    if (old_track_id == 0 && !current_buf) {
        /* Deferred advance: buffer wasn't preloaded, need to load and set. */
        int64_t next_id = atomic_load(&p->next_track_id);
        if (next_id > 0) {
            g_debug("Player %d: deferred advance to track %" G_GINT64_FORMAT, player_id, next_id);
            audio_cache_load(pipeline->cache, next_id);
            audio_pipeline_set_player_track(pipeline, player_id, next_id);
        }
        return;
    }

    /* Instant advance: buffer swap already happened in the RT callback. */
    int64_t current_id = atomic_load(&p->current_track_id);

    if (pipeline->cache && old_track_id > 0)
        audio_cache_unlock(pipeline->cache, old_track_id, AUDIO_CACHE_UNLOCK_DEFERRED);

    preload_next_track(pipeline, p, current_id);

    if (pipeline->track_changed_callback)
        pipeline->track_changed_callback(player_id, current_id, pipeline->track_changed_user_data);
}

/*
 * Internal: drive per-player buffer loads and auto-advance. Called from the
 * GLib timeout on the main thread every 50ms. Each player runs three flat
 * steps: poll a pending decode, attach a ready next buffer, finish any advance.
 */
static void
process_pending_advances_internal(audio_pipeline_t *pipeline)
{
    g_assert(pipeline != NULL);

    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++) {
        audio_player_t *p = &pipeline->players[i];
        poll_pending_buffer(pipeline, p, i);
        try_attach_next_buffer(pipeline, p);
        handle_pending_advance(pipeline, p, i);
    }
}

/**
 * GLib timeout callback for auto-advance processing.
 * Runs every 50ms on the main thread while pipeline is active.
 */
static gboolean
advance_timeout_callback(gpointer user_data)
{
    audio_pipeline_t *pipeline = (audio_pipeline_t *)user_data;
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

quadrature_result_t
audio_pipeline_player_play(audio_pipeline_t *pipeline, int player_id)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t *p = &pipeline->players[player_id];

    if (!atomic_load(&p->streams_active))
        return QUADRATURE_ERROR_INVALID_PARAM;

    /* Atomic state transition: STOPPED or PAUSED → PLAYING */
    int expected = CHANNEL_STOPPED;
    if (!atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PLAYING)) {
        expected = CHANNEL_PAUSED;
        if (!atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PLAYING)) {
            if (expected == CHANNEL_PLAYING)
                return QUADRATURE_OK;
            g_debug("Player %d play failed: state=%d", player_id, expected);
            return QUADRATURE_ERROR_INTERNAL;
        }
    }

    const char *from = (expected == CHANNEL_STOPPED) ? "stopped" : "paused";
    pw_thread_loop_lock(pipeline->loop);
    if (p->stream)
        pw_stream_set_active(p->stream, true);
    float speed = audio_shuttle_speed_get_speed(p->shuttle_speed);
    player_update_position_snap(p, audio_seek_position_get(&p->seek_position), speed, true, 0);
    pw_thread_loop_unlock(pipeline->loop);
    g_info("Player %d playing (from %s)", player_id, from);
    return QUADRATURE_OK;
}

quadrature_result_t
audio_pipeline_player_stop(audio_pipeline_t *pipeline, int player_id)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t *p = &pipeline->players[player_id];

    /* Take PipeWire lock for synchronized state change + flush + snapshot */
    pw_thread_loop_lock(pipeline->loop);

    int prev = atomic_exchange(&p->state, CHANNEL_STOPPED);

    /* Reset position and flush buffers */
    audio_player_flush_all(p);
    audio_seek_position_set(&p->seek_position, 0);

    /* Update snapshot while holding lock to synchronize with callback */
    float speed = audio_shuttle_speed_get_speed(p->shuttle_speed);
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

quadrature_result_t
audio_pipeline_player_toggle_play(audio_pipeline_t *pipeline, int player_id)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t *p = &pipeline->players[player_id];

    /* Check current state and toggle atomically */
    int current = atomic_load(&p->state);
    int64_t track_id = atomic_load(&p->current_track_id);
    g_debug("Player %d toggle_play: state=%d, track_id=%" G_GINT64_FORMAT,
            player_id,
            current,
            track_id);

    if (current == CHANNEL_PLAYING) {
        /* Pause: PLAYING → PAUSED */
        int expected = CHANNEL_PLAYING;
        if (atomic_compare_exchange_strong(&p->state, &expected, CHANNEL_PAUSED)) {
            pw_thread_loop_lock(pipeline->loop);
            float speed = audio_shuttle_speed_get_speed(p->shuttle_speed);
            player_update_position_snap(
                p, audio_seek_position_get(&p->seek_position), speed, false, 0);
            pw_thread_loop_unlock(pipeline->loop);
            g_info("Player %d paused", player_id);
            return QUADRATURE_OK;
        }
        return QUADRATURE_OK; /* Already changed state */
    } else {
        return audio_pipeline_player_play(pipeline, player_id);
    }
}

quadrature_result_t
audio_pipeline_player_seek(audio_pipeline_t *pipeline, int player_id, double seconds)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t *p = &pipeline->players[player_id];

    audio_buffer_t *buf = atomic_load_explicit(&p->buffer, memory_order_acquire);
    if (!buf)
        return QUADRATURE_ERROR_INTERNAL;

    if (seconds < 0.0)
        seconds = 0.0;
    uint64_t num_frames = audio_buffer_get_num_frames(buf);
    uint64_t position = (uint64_t)(seconds * (double)pipeline->format.sample_rate);
    if (position > num_frames)
        position = num_frames;

    /* Take PipeWire lock to synchronize with audio callback */
    pw_thread_loop_lock(pipeline->loop);

    audio_seek_position_set(&p->seek_position, position);
    audio_player_flush_all(p);

    /* Update snapshot while holding lock to synchronize with callback */
    channel_state_t snap_state = atomic_load(&p->state);
    float speed = audio_shuttle_speed_get_speed(p->shuttle_speed);
    player_update_position_snap(p, position, speed, snap_state == CHANNEL_PLAYING, 0);

    pw_thread_loop_unlock(pipeline->loop);

    g_debug("Player %d seek to %.3fs (%" G_GUINT64_FORMAT " frames)", player_id, seconds, position);
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Playback Speed Control
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
audio_pipeline_set_player_speed(audio_pipeline_t *pipeline, int player_id, float speed)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t *p = &pipeline->players[player_id];
    g_assert(p->shuttle_speed != NULL);

    /* Speed control requires buffer - buffer-first architecture means this always works when ready */
    if (!atomic_load_explicit(&p->buffer, memory_order_acquire)) {
        g_debug("Player %d cannot change speed - buffer not ready", player_id);
        return QUADRATURE_ERROR_INTERNAL;
    }

    audio_shuttle_speed_set_speed(p->shuttle_speed, speed);
    g_info("Ch%d: speed set to %.2fx", player_id + 1, speed);

    /* Update snapshot with PipeWire lock to synchronize with callback */
    pw_thread_loop_lock(pipeline->loop);
    channel_state_t snap_state = atomic_load(&p->state);
    player_update_position_snap(
        p, audio_seek_position_get(&p->seek_position), speed, snap_state == CHANNEL_PLAYING, 0);
    pw_thread_loop_unlock(pipeline->loop);

    return QUADRATURE_OK;
}

quadrature_result_t
audio_pipeline_set_player_shuttle_mode(audio_pipeline_t *pipeline,
                                       int player_id,
                                       shuttle_mode_t mode)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    audio_player_t *p = &pipeline->players[player_id];
    g_assert(p->shuttle_speed != NULL);

    /* Allocate DSP resources on the UI thread BEFORE flipping the atomic the
     * RT path reads — keeps malloc out of the audio callback. */
    audio_shuttle_speed_prepare_mode(p->shuttle_speed, mode);
    audio_shuttle_speed_set_mode(p->shuttle_speed, mode);

    /* Log mode change */
    const char *mode_names[] = { "off", "keylock", "pitched" };
    const char *mode_name = (mode >= 0 && mode <= 2) ? mode_names[mode] : "unknown";
    g_info("Ch%d: shuttle mode set to %s", player_id + 1, mode_name);

    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Repeat Control
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
audio_pipeline_set_player_end_mode(audio_pipeline_t *pipeline, int player_id, track_end_mode_t mode)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    atomic_store(&pipeline->players[player_id].end_mode, (int)mode);
    return QUADRATURE_OK;
}

track_end_mode_t
audio_pipeline_get_player_end_mode(audio_pipeline_t *pipeline, int player_id)
{
    if (!pipeline || !valid_player(player_id))
        return TRACK_END_AUTOPLAY;
    return (track_end_mode_t)atomic_load(&pipeline->players[player_id].end_mode);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Routing
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
audio_pipeline_set_player_device(audio_pipeline_t *pipeline, int player_id, const char *device_name)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t *p = &pipeline->players[player_id];

    /* Lockless early-out: skip PW lock entirely for no-op device changes.
     * target_device is only written under the PW lock, so worst case of a
     * torn read is a single extra lock acquisition — not a safety issue.
     * Exception: if streams were deactivated (PW error recovery), always
     * re-create even for the same device name. */
    const char *current = p->target_device[0] ? p->target_device : NULL;
    const char *new_dev = (device_name && device_name[0]) ? device_name : NULL;
    bool streams_up = atomic_load(&p->streams_active);
    if (streams_up
        && ((current == NULL && new_dev == NULL)
            || (current && new_dev && g_strcmp0(current, new_dev) == 0)))
        return QUADRATURE_OK;

    pw_thread_loop_lock(pipeline->loop);
    quadrature_result_t result
        = player_recreate_stream(p, pipeline->format.sample_rate, device_name);
    pw_thread_loop_unlock(pipeline->loop);

    return result;
}

void
audio_pipeline_set_player_exclusive(audio_pipeline_t *pipeline, int player_id, bool exclusive)
{
    if (!pipeline || !valid_player(player_id))
        return;
    audio_player_t *p = &pipeline->players[player_id];
    p->exclusive = exclusive;
    /* Takes effect on next stream recreate (device change, reconnect, etc.) */
}

quadrature_result_t
audio_pipeline_set_player_quantum(audio_pipeline_t *pipeline,
                                  int player_id,
                                  uint32_t quantum_frames)
{
    if (!pipeline || !valid_player(player_id))
        return QUADRATURE_ERROR_INVALID_PARAM;
    if (quantum_frames < 32 || quantum_frames > 2048
        || (quantum_frames & (quantum_frames - 1)) != 0)
        return QUADRATURE_ERROR_INVALID_PARAM;

    audio_player_t *p = &pipeline->players[player_id];

    pw_thread_loop_lock(pipeline->loop);

    if (p->quantum_frames == quantum_frames) {
        pw_thread_loop_unlock(pipeline->loop);
        return QUADRATURE_OK;
    }

    p->quantum_frames = quantum_frames;

    /* Update audio cache unlock delay to match new quantum */
    audio_cache_set_quantum(pipeline->cache, quantum_frames);

    quadrature_result_t result = player_recreate_stream(
        p, pipeline->format.sample_rate, p->target_device[0] ? p->target_device : NULL);
    pw_thread_loop_unlock(pipeline->loop);

    g_message("Player %d quantum set to %u frames", player_id, quantum_frames);
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Monitoring
 * ═══════════════════════════════════════════════════════════════════════════ */

void
audio_pipeline_get_player_display(audio_pipeline_t *pipeline,
                                  int player_id,
                                  audio_player_display_t *out)
{
    if (!out)
        return;
    *out = (audio_player_display_t){ 0 };

    if (!pipeline || !valid_player(player_id)) {
        out->state = CHANNEL_ERROR;
        out->speed = 1.0f;
        return;
    }

    audio_player_t *p = &pipeline->players[player_id];
    const double inv_rate = 1.0 / (double)pipeline->format.sample_rate;

    out->state = atomic_load(&p->state);

    position_snapshot_t snap = { 0 };
    uint32_t seq1, seq2 = 0;

/* Seqlock read with retry limit. If the writer is pathologically active
     * (shouldn't happen in practice), fall through with the last snapshot —
     * stale data for one UI frame is acceptable. */
#define SEQLOCK_MAX_RETRIES 4
    for (int attempt = 0; attempt < SEQLOCK_MAX_RETRIES; attempt++) {
        seq1 = atomic_load_explicit(&p->position_seq, memory_order_acquire);
        if (seq1 & 1)
            continue; /* Writer is active, spin */
        snap = p->position_snap;
        atomic_thread_fence(memory_order_acquire);
        seq2 = atomic_load_explicit(&p->position_seq, memory_order_relaxed);
        if (seq1 == seq2)
            break;
    }

    out->speed = snap.speed;

    double pos_samples;
    if (!snap.playing || fabsf(snap.speed) < SPEED_STOPPED_EPSILON) {
        pos_samples = (double)snap.position;
    } else {
        /* Sample-count based interpolation (RT-safe — no syscalls) */
        uint64_t current_count = atomic_load(&p->callback_sample_count);
        uint64_t elapsed_samples = current_count - snap.sample_count;

        /* Clamp to prevent stale data (max ~50ms at any sample rate) */
        uint64_t max_elapsed = pipeline->format.sample_rate / 20;
        if (elapsed_samples > max_elapsed) {
            g_debug("Stale snapshot: elapsed=%" G_GUINT64_FORMAT " max=%" G_GUINT64_FORMAT,
                    elapsed_samples,
                    max_elapsed);
            elapsed_samples = max_elapsed;
        }

        pos_samples = (double)snap.position + (double)elapsed_samples * snap.speed;
    }

    uint64_t length_samples = atomic_load(&p->length_samples);
    if (pos_samples < 0.0)
        pos_samples = 0.0;
    if (pos_samples > (double)length_samples)
        pos_samples = (double)length_samples;

    out->position_seconds = pos_samples * inv_rate;
    out->length_seconds = (double)length_samples * inv_rate;
}

void
audio_pipeline_set_spectrum_refresh_hz(audio_pipeline_t *pipeline, double hz)
{
    g_assert(pipeline != NULL);
    for (int i = 0; i < MAX_AUDIO_PLAYERS; i++)
        spectrum_set_refresh_hz(&pipeline->players[i].spectrum, hz);
}

void
audio_pipeline_get_player_spectrum(
    audio_pipeline_t *pipeline, int player_id, float *left, float *right, int num_bars)
{
    g_assert(pipeline != NULL);
    g_assert(valid_player(player_id));
    g_assert(left != NULL);
    g_assert(right != NULL);
    g_assert(num_bars > 0);

    audio_player_t *p = &pipeline->players[player_id];
    int count = (num_bars > SPECTRUM_BARS) ? SPECTRUM_BARS : num_bars;

    /* When not playing, return zeros — UI smoothing handles fadeout */
    channel_state_t state = atomic_load(&p->state);
    if (state != CHANNEL_PLAYING) {
        memset(left, 0, (size_t)num_bars * sizeof(float));
        memset(right, 0, (size_t)num_bars * sizeof(float));
        return;
    }

    /* Bars are relaxed: tearing across the loop is acceptable for visualization
     * (UI smooths) and channel_strip.c uses spectrum_generation as the
     * cache-invalidation signal for "new FFT batch ready". */
    for (int i = 0; i < count; i++) {
        left[i] = atomic_load_explicit(&p->spectrum_bars[i], memory_order_relaxed);
        right[i] = atomic_load_explicit(&p->spectrum_bars[SPECTRUM_BARS + i], memory_order_relaxed);
    }
    for (int i = count; i < num_bars; i++) {
        left[i] = 0.0f;
        right[i] = 0.0f;
    }
}

void
audio_pipeline_player_reconnect(audio_pipeline_t *pipeline, int player_id)
{
    if (!pipeline || !valid_player(player_id))
        return;
    audio_player_t *p = &pipeline->players[player_id];
    player_idle_data_t *d = g_new(player_idle_data_t, 1);
    d->player = p;
    d->generation = atomic_load(&p->stream_generation);
    g_idle_add(player_reconnect_idle, d);
}
