#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Telemetry
 *
 * Everything the RT callback and the UI exchange for diagnostics lives here:
 *   - the lock-free SPSC event ring (publish from RT, drain from UI)
 *   - the per-callback recording block (budget/latency/interval rings + the
 *     fault events derived from one callback's timing)
 *   - the rate/percentage rollup the perf dashboard reads
 *
 * The audio-thread entry points (publish_event, record_callback) are RT-safe:
 * lock-free atomics only, no allocation. They are not `static inline` because
 * they run on exceptional/sampled paths (not every callback's hot loop), so a
 * cross-TU call is free relative to the work they do.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Event Ring — single lock-free SPSC push.
 *
 * Every audio-thread / stream-callback event flows through here. Callers
 * compose the event as a value; the modulo-indexed atomic store happens once,
 * in one place. Pass-by-value so callsites read as `publish(pl, (E){...})`
 * with no `&` noise — inline copy elision keeps it free.
 * ═══════════════════════════════════════════════════════════════════════════ */

void
audio_pipeline_publish_event(audio_pipeline_t *pl, audio_pipeline_event_t event)
{
    /* Single-producer (the PipeWire thread). Write the event payload BEFORE
     * publishing the advanced index with a release store, so a reader that
     * acquire-observes the new index is guaranteed to see the populated slot.
     * The previous fetch_add bumped the index before the store, letting a
     * reader read an as-yet-unwritten slot. */
    unsigned int w = atomic_load_explicit(&pl->event_write, memory_order_relaxed);
    pl->events[w % AUDIO_EVENT_RING_SIZE] = event;
    atomic_store_explicit(&pl->event_write, w + 1, memory_order_release);
}

int
audio_pipeline_get_events(audio_pipeline_t *pipeline, audio_pipeline_event_t *out, int max)
{
    if (!pipeline || !out || max <= 0)
        return 0;

    /* Acquire-load pairs with the release store in publish_event: observing the
     * advanced write index guarantees the event payloads below are visible. */
    unsigned int write_idx = atomic_load_explicit(&pipeline->event_write, memory_order_acquire);
    unsigned int read_idx = atomic_load_explicit(&pipeline->event_read, memory_order_relaxed);

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-Callback Recording
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Push one value into a telemetry ring: relaxed-load the write position, store
 * the sample at the masked slot, then release-store the advanced position so a
 * UI reader that observes it also sees the sample. CAP must be a power of two. */
#define TELEMETRY_RING_PUSH(ring, cap, value, now)                                      \
    do {                                                                                \
        uint32_t _pos = atomic_load_explicit(&(ring)->write_pos, memory_order_relaxed); \
        (ring)->samples[_pos & ((cap) - 1)] = (value);                                  \
        atomic_store_explicit(&(ring)->write_pos, _pos + 1, memory_order_release);      \
        (ring)->last_write_ns = (now);                                                  \
    } while (0)

void
audio_telemetry_record_callback(audio_player_t *p,
                                uint32_t frame_count,
                                uint64_t cb_start,
                                bool has_interval,
                                int64_t interval_dev_ns)
{
    audio_pipeline_t *pl = p->pipeline;

    /* Callback timing */
    uint64_t cb_elapsed = time_ns() - cb_start;
    atomic_fetch_add_explicit(&p->stats_cb_time_sum_ns, cb_elapsed, memory_order_relaxed);

    /* Pre-compute budget once (multiply, no division in RT path) */
    uint64_t budget_ns = (uint64_t)frame_count * pl->ns_per_frame;

    /* Budget + latency + interval ring buffers — sample every ~10ms */
    uint64_t now_ns = cb_start;
    if (now_ns - p->budget_rb->last_write_ns >= RINGBUF_SAMPLE_INTERVAL_NS) {
        /* Budget centipercent (0-10000 for 0.01% resolution) */
        uint32_t cpct = budget_ns > 0 ? (uint32_t)((cb_elapsed * 10000ULL) / budget_ns) : 0;
        if (cpct > 10000)
            cpct = 10000;
        TELEMETRY_RING_PUSH(p->budget_rb, BUDGET_RB_CAPACITY, (uint16_t)cpct, now_ns);

        /* Latency µs */
        uint16_t lat_us = (cb_elapsed / 1000 > 65535) ? 65535 : (uint16_t)(cb_elapsed / 1000);
        TELEMETRY_RING_PUSH(p->latency_rb, LATENCY_RB_CAPACITY, lat_us, now_ns);

        /* Interval deviation: write raw peak ns, then reset */
        TELEMETRY_RING_PUSH(p->interval_rb, INTERVAL_RB_CAPACITY, p->interval_peak_dev_ns, now_ns);
        p->interval_peak_dev_ns = 0;

        /* Fire SCHEDULING_DELAY event if callback arrived >2x period late */
        if (has_interval && interval_dev_ns > (int64_t)(budget_ns * 2)) {
            audio_pipeline_publish_event(
                pl,
                (audio_pipeline_event_t){ .timestamp_ns = cb_start,
                                          .type = AUDIO_EVENT_SCHEDULING_DELAY,
                                          .player_id = p->player_id,
                                          .track_id = atomic_load(&p->current_track_id),
                                          .data.scheduling = {
                                              .deviation_ns = interval_dev_ns,
                                              .expected_ns = (int64_t)budget_ns,
                                          } });
        }
    }

    /* Update peak (CAS loop) */
    uint64_t cur_max = atomic_load_explicit(&p->stats_cb_time_max_ns, memory_order_relaxed);
    while (cb_elapsed > cur_max) {
        if (atomic_compare_exchange_weak_explicit(&p->stats_cb_time_max_ns,
                                                  &cur_max,
                                                  cb_elapsed,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }

    /* Budget overrun check (50% of period) — emit event */
    if (cb_elapsed > budget_ns / 2) {
        atomic_fetch_add_explicit(&p->stats_budget_overruns, 1, memory_order_relaxed);
        audio_pipeline_publish_event(
            pl,
            (audio_pipeline_event_t){ .timestamp_ns = cb_start,
                                      .type = AUDIO_EVENT_BUDGET_OVERRUN,
                                      .player_id = p->player_id,
                                      .track_id = atomic_load(&p->current_track_id),
                                      .data.budget = {
                                          .elapsed_ns = cb_elapsed,
                                          .budget_ns = budget_ns,
                                      } });
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Stats Rollup (UI thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

void
audio_pipeline_get_player_stats(audio_pipeline_t *pipeline,
                                int player_id,
                                audio_player_stats_t *stats)
{
    g_assert(pipeline != NULL);
    g_assert(player_id >= 0 && player_id < MAX_AUDIO_PLAYERS);
    g_assert(stats != NULL);

    audio_player_t *p = &pipeline->players[player_id];

    /* Callback performance */
    uint64_t count = atomic_load_explicit(&p->stats_cb_count, memory_order_relaxed);
    uint64_t sum_ns = atomic_load_explicit(&p->stats_cb_time_sum_ns, memory_order_relaxed);
    uint64_t max_ns = atomic_load_explicit(&p->stats_cb_time_max_ns, memory_order_relaxed);

    stats->callback_time_avg_us
        = count > 0 ? (float)((double)sum_ns / (double)count / 1000.0) : 0.0f;
    stats->callback_time_max_us = (float)((double)max_ns / 1000.0);

    /* Budget % = avg_time / budget * 100 */
    uint64_t period_ns = (uint64_t)p->quantum_frames * 1000000000ULL / pipeline->format.sample_rate;
    stats->budget_pct
        = count > 0 ? (float)((double)sum_ns / (double)count / (double)period_ns * 100.0) : 0.0f;
    stats->budget_overruns = atomic_load_explicit(&p->stats_budget_overruns, memory_order_relaxed);

    /* Underrun rate is per-player: this player's buffer-underrun callbacks
     * as a fraction of its total callbacks. */
    uint64_t underruns = atomic_load_explicit(&p->stats_buffer_underruns, memory_order_relaxed);
    stats->underrun_rate_pct
        = count > 0 ? (float)((double)underruns / (double)count * 100.0) : 0.0f;

    /* Fault events */
    stats->dequeue_failures
        = atomic_load_explicit(&p->stats_dequeue_failures, memory_order_relaxed);
    stats->shuttle_speed_underflows = audio_shuttle_speed_get_underflows(p->shuttle_speed);
    stats->deferred_advances
        = atomic_load_explicit(&p->stats_deferred_advances, memory_order_relaxed);

    /* Advance quality */
    uint64_t instant = atomic_load_explicit(&p->stats_instant_advances, memory_order_relaxed);
    uint64_t deferred = stats->deferred_advances;
    uint64_t total_advances = instant + deferred;
    stats->advance_hit_rate_pct
        = total_advances > 0 ? (float)((double)instant / (double)total_advances * 100.0) : 100.0f;
}
