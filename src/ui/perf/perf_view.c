/**
 * @file perf_view.c
 * @brief Performance dashboard — RT audio diagnostic instrument
 *
 * Layout (top to bottom):
 *   Section 1: Channel Status Bar — state, p99, budget%, health dot, reconnect btn
 *   Section 2: Fault Timeline — 5-minute event timeline with severity markers
 *   Section 3: Audio Health — callback latency grouped histogram
 *   Section 3b: Scheduling Jitter — per-channel log-Y line chart
 *   Section 4: Memory Usage — stacked area chart (audio, lib cache, artwork)
 *   Section 5: Cache Latency — log-Y p90 line chart (texture hit, atlas decode)
 *   Section 6: Decode Time Distribution — adaptive histogram
 */

#include "internal.h"
#include "../internal.h"
#include "../../audio/internal.h"
#include "../library/internal.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Private State
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    perf_dashboard_t* dashboard;
    audio_cache_t*    audio_cache;
    audio_pipeline_t* pipeline;
    library_cache_t*  library_cache;
    ArtworkManager*   artwork_mgr;

    /* Section 1: Channel Status Bar */
    GtkWidget* channel_cells[PERF_MAX_PLAYERS];
    GtkWidget* channel_state_labels[PERF_MAX_PLAYERS];
    GtkWidget* channel_health_dots[PERF_MAX_PLAYERS];
    GtkWidget* channel_p99_labels[PERF_MAX_PLAYERS];
    GtkWidget* channel_budget_labels[PERF_MAX_PLAYERS];
    GtkWidget* channel_reconnect_btns[PERF_MAX_PLAYERS];

    /* Section 2: Fault Timeline */
    PerfTimelineChart* timeline;

    /* Section 3: Audio Health */
    int             tick_count;  /* 100ms ticks; emit latency point every 10 */
    PerfGroupedHist* latency_hist;  /* Adaptive callback latency grouped histogram */

    /* Section 3b: Scheduling Jitter */
    PerfLineChart*      jitter_chart;         /* 4 series: per-channel scheduling deviation */
    int64_t*            interval_buf;         /* pre-allocated ring buffer read (INTERVAL_RB_CAPACITY) */
    double*             jitter_buf;           /* pre-allocated decimated display buffer */

    /* Section 4: Memory Usage (stacked area) */
    PerfStackedAreaChart* mem_stacked_chart;

    /* Section 5: Cache Latency (log-Y line chart) */
    PerfLineChart* cache_lat_chart;
    /* Snapshot-delta windowing: previous bucket counts for p90 over last 2s window */
    uint64_t prev_tex_buckets[PERF_HIST_BUCKETS];
    uint64_t prev_atlas_buckets[PERF_HIST_BUCKETS];
    double   tex_p90_ring[PERF_TIMESERIES_SIZE];
    double   atlas_p90_ring[PERF_TIMESERIES_SIZE];
    size_t   lat_write_idx;

    /* Section 6: Decode Time */
    PerfHistogramChart* decode_time_hist;

    /* Jitter scroll epoch tracking */
    uint32_t jitter_last_epoch;  /* floor(max_write_pos / JITTER_DECIMATE) */

    /* Cached CSS state — avoid unconditional class add/remove every tick */
    const char* prev_health_class[PERF_MAX_PLAYERS];  /* "perf-channel-ok" etc. */
    gboolean    prev_dimmed[PERF_MAX_PLAYERS];
    gboolean    prev_error_border[PERF_MAX_PLAYERS];

    /* Cached label text — skip gtk_label_set_text when unchanged */
    char prev_state_text[PERF_MAX_PLAYERS][16];
    char prev_p99_text[PERF_MAX_PLAYERS][32];
    char prev_budget_text[PERF_MAX_PLAYERS][24];

    /* Update timer */
    guint timer_id;
} PerfViewPrivate;

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Compute p50/p90/p99 from raw µs samples via counting sort.
 * O(n) time — uses prefix-sum over value range instead of comparison sort.
 */
static void compute_percentiles_from_samples(uint16_t* samples, uint32_t count,
                                              double* p50, double* p90, double* p99) {
    if (count == 0) { *p50 = *p90 = *p99 = 0; return; }

    /* Find max to bound the counting array */
    uint16_t max_val = 0;
    for (uint32_t i = 0; i < count; i++)
        if (samples[i] > max_val) max_val = samples[i];

    /* Stack-allocate counts (typically <1KB for real latency values) */
    uint32_t* counts = g_alloca((max_val + 1) * sizeof(uint32_t));
    memset(counts, 0, (max_val + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; i++)
        counts[samples[i]]++;

    /* Walk prefix sums to find percentile values */
    uint32_t target50 = count / 2;
    uint32_t target90 = count * 9 / 10;
    uint32_t target99 = count * 99 / 100;
    uint32_t cumulative = 0;
    *p50 = *p90 = *p99 = 0;
    for (uint32_t v = 0; v <= max_val; v++) {
        cumulative += counts[v];
        if (*p50 == 0 && cumulative > target50) *p50 = (double)v;
        if (*p90 == 0 && cumulative > target90) *p90 = (double)v;
        if (cumulative > target99) { *p99 = (double)v; break; }
    }
}

/**
 * Fixed logarithmic bins for callback latency histogram.
 *
 * Convention: every serious latency monitoring tool (Prometheus, HdrHistogram,
 * Grafana) uses fixed log-spaced bins. Adaptive bins cause visual instability
 * because edges shift every frame. Fixed bins are stable and the power-of-2
 * spacing gives implicit log-scale on the X axis — each equal-width bar
 * covers one octave.
 *
 * Bin layout (12 bins, edges in µs):
 *   [0] 0     [1] 1      [2] 2      [3] 3-4     [4] 4-8     [5] 8-16
 *   [6] 16-32  [7] 32-64  [8] 64-128  [9] 128-256  [10] 256-512  [11] ≥512
 *
 * Sub-4µs bins are individual microseconds (where most samples cluster).
 * Power-of-2 doubling from 4µs onward. Anything above 512µs is trouble.
 */
enum { LOG_NUM_BINS = 12 };
static const uint16_t LOG_EDGES[LOG_NUM_BINS + 1] = {
    0, 1, 2, 3, 4, 8, 16, 32, 64, 128, 256, 512, UINT16_MAX
};
static const char* LOG_LABELS[LOG_NUM_BINS] = {
    "0", "1", "2", "3", "4", "8", "16", "32", "64", "128", "256", "≥512"
};

static void compute_log_bins(
    uint16_t* sorted_samples[PERF_MAX_PLAYERS],
    uint32_t  per_player_count[PERF_MAX_PLAYERS],
    PerfGroupedHist* hist) {

    for (int p = 0; p < PERF_MAX_PLAYERS; p++) {
        uint32_t buckets[16] = {0};
        uint32_t n = per_player_count[p];

        /* Linear scan through sorted data against fixed edges */
        int bin = 0;
        for (uint32_t i = 0; i < n; i++) {
            while (bin < LOG_NUM_BINS - 1 &&
                   sorted_samples[p][i] >= LOG_EDGES[bin + 1])
                bin++;
            buckets[bin]++;
        }
        perf_grouped_hist_set_data(hist, p, buckets, LOG_NUM_BINS);
    }
}

/**
 * Compute p90 from a µs-scale histogram bucket delta (current - previous).
 *
 * Bucket boundaries for perf_histogram_record_us():
 *   bucket 0 → [0, 1) µs
 *   bucket i → [2^(i-1), 2^i) µs   (i > 0)
 *
 * Returns the upper-bound µs of the bucket containing the 90th percentile,
 * or 0 if no samples in the delta window.
 */
static double p90_from_us_delta(const uint64_t *cur_buckets,
                                const uint64_t *prev_buckets) {
    uint64_t delta[PERF_HIST_BUCKETS];
    uint64_t total = 0;
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        /* Saturating subtract — counters only grow */
        delta[i] = cur_buckets[i] > prev_buckets[i]
                   ? cur_buckets[i] - prev_buckets[i] : 0;
        total += delta[i];
    }
    if (total == 0) return 0.0;

    uint64_t target = total * 90 / 100;
    uint64_t cum = 0;
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        cum += delta[i];
        if (cum >= target) {
            /* Upper bound of bucket i in µs */
            return (i == 0) ? 1.0 : (double)(1ULL << i);
        }
    }
    return (double)(1ULL << (PERF_HIST_BUCKETS - 1));
}

/* 20:1 max-pool decimation: 20 raw samples (10ms each) → 1 display point (200ms) */
#define JITTER_DECIMATE 20
#define JITTER_DISPLAY_MAX ((INTERVAL_RB_CAPACITY / JITTER_DECIMATE) + 1)

/* ═══════════════════════════════════════════════════════════════════════════
 * Update Timer
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean update_dashboard(gpointer data) {
    PerfViewPrivate* priv = data;
    if (!priv->dashboard) return G_SOURCE_CONTINUE;

    /* ─── Section 1: Channel Status Bar ─── */
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        channel_state_t state = CHANNEL_STOPPED;
        if (priv->pipeline)
            state = audio_pipeline_get_player_state(priv->pipeline, i);

        const char* state_str = "Stopped";
        if (state == CHANNEL_PLAYING)      state_str = "Playing";
        else if (state == CHANNEL_PAUSED)  state_str = "Paused";
        else if (state == CHANNEL_ERROR)   state_str = "Error";
        if (strcmp(priv->prev_state_text[i], state_str) != 0) {
            gtk_label_set_text(GTK_LABEL(priv->channel_state_labels[i]), state_str);
            g_strlcpy(priv->prev_state_text[i], state_str, sizeof(priv->prev_state_text[i]));
        }

        /* Health dot */
        const char* dot_class = "perf-channel-ok";
        if (priv->pipeline) {
            audio_player_stats_t stats;
            audio_pipeline_get_player_stats(priv->pipeline, i, &stats);
            if (stats.budget_overruns > 0)       dot_class = "perf-channel-warn";
            if (stats.underrun_rate_pct > 0.0f)  dot_class = "perf-channel-error";
        }
        if (priv->channel_health_dots[i] && dot_class != priv->prev_health_class[i]) {
            if (priv->prev_health_class[i])
                gtk_widget_remove_css_class(priv->channel_health_dots[i], priv->prev_health_class[i]);
            gtk_widget_add_css_class(priv->channel_health_dots[i], dot_class);
            priv->prev_health_class[i] = dot_class;
        }

        /* Dim stopped channels and hide their latency series */
        gboolean active = (state == CHANNEL_PLAYING);
        gboolean dimmed = (state == CHANNEL_STOPPED);
        if (dimmed != priv->prev_dimmed[i]) {
            if (dimmed)
                gtk_widget_add_css_class(priv->channel_cells[i], "perf-channel-dimmed");
            else
                gtk_widget_remove_css_class(priv->channel_cells[i], "perf-channel-dimmed");
            priv->prev_dimmed[i] = dimmed;
        }
        /* p99 label — computed from ring buffer in 1/sec block below */
        if (!active && strcmp(priv->prev_p99_text[i], "p99: --") != 0) {
            gtk_label_set_text(GTK_LABEL(priv->channel_p99_labels[i]), "p99: --");
            g_strlcpy(priv->prev_p99_text[i], "p99: --", sizeof(priv->prev_p99_text[i]));
        }

        /* Budget % label — only for active channels */
        if (priv->pipeline && active) {
            char buf[24];
            double bpct = audio_pipeline_get_budget_max(priv->pipeline, i);
            snprintf(buf, sizeof(buf), "Bgt: %.2f%%", bpct);
            if (strcmp(priv->prev_budget_text[i], buf) != 0) {
                gtk_label_set_text(GTK_LABEL(priv->channel_budget_labels[i]), buf);
                g_strlcpy(priv->prev_budget_text[i], buf, sizeof(priv->prev_budget_text[i]));
            }
        } else if (strcmp(priv->prev_budget_text[i], "Bgt: --") != 0) {
            gtk_label_set_text(GTK_LABEL(priv->channel_budget_labels[i]), "Bgt: --");
            g_strlcpy(priv->prev_budget_text[i], "Bgt: --", sizeof(priv->prev_budget_text[i]));
        }

        /* Reconnect button — show only on device error */
        if (priv->pipeline) {
            bool has_err = audio_pipeline_player_has_device_error(priv->pipeline, i);
            gtk_widget_set_visible(priv->channel_reconnect_btns[i], has_err);
            if (has_err != priv->prev_error_border[i]) {
                if (has_err)
                    gtk_widget_add_css_class(priv->channel_cells[i], "perf-channel-error-border");
                else
                    gtk_widget_remove_css_class(priv->channel_cells[i], "perf-channel-error-border");
                priv->prev_error_border[i] = has_err;
            }
        }
    }

    /* ─── Section 2: Fault Timeline ─── */
    if (priv->pipeline) {
        audio_pipeline_event_t events[64];
        int nevents = audio_pipeline_get_events(priv->pipeline, events, 64);
        for (int i = 0; i < nevents; i++) {
            const audio_pipeline_event_t* ev = &events[i];
            int lane = (ev->player_id >= 0 && ev->player_id < PERF_MAX_PLAYERS)
                ? ev->player_id : 4;
            int severity = 1;
            const char* type_name = "Unknown";
            switch (ev->type) {
                case AUDIO_EVENT_BUFFER_UNDERRUN:
                    type_name = "Buffer Underrun"; severity = 2; break;
                case AUDIO_EVENT_DEQUEUE_FAILURE:
                    type_name = "Dequeue Failure"; severity = 2; break;
                case AUDIO_EVENT_SCRUBBER_UNDERFLOW:
                    type_name = "Scrubber Underflow"; severity = 1; break;
                case AUDIO_EVENT_BUDGET_OVERRUN:
                    type_name = "Budget Overrun"; severity = 1; break;
                case AUDIO_EVENT_INSTANT_ADVANCE:
                    type_name = "Instant Advance"; severity = 0; break;
                case AUDIO_EVENT_DEFERRED_ADVANCE:
                    type_name = "Deferred Advance"; severity = 2; break;
                case AUDIO_EVENT_PW_XRUN:
                    type_name = "PW Xrun"; severity = 2; break;
                case AUDIO_EVENT_PW_ERROR:
                    type_name = "PW Error"; severity = 2; break;
                case AUDIO_EVENT_SCHEDULING_DELAY:
                    type_name = "Sched Delay"; severity = 1; break;
            }
            char tooltip[256];
            int ch = ev->player_id >= 0 ? ev->player_id + 1 : 0;
            switch (ev->type) {
                case AUDIO_EVENT_BUFFER_UNDERRUN:
                case AUDIO_EVENT_SCRUBBER_UNDERFLOW:
                    snprintf(tooltip, sizeof(tooltip),
                        "%s\nCh%d | Track %" G_GINT64_FORMAT
                        "\nReq: %u  Avail: %u  Speed: %.2f",
                        type_name, ch, ev->track_id,
                        ev->data.underrun.requested_frames,
                        ev->data.underrun.available_frames,
                        (double)ev->data.underrun.speed);
                    break;
                case AUDIO_EVENT_BUDGET_OVERRUN:
                    snprintf(tooltip, sizeof(tooltip),
                        "%s\nCh%d | Track %" G_GINT64_FORMAT
                        "\nElapsed: %.1f ms  Budget: %.1f ms (%.0f%%)",
                        type_name, ch, ev->track_id,
                        (double)ev->data.budget.elapsed_ns / 1e6,
                        (double)ev->data.budget.budget_ns / 1e6,
                        ev->data.budget.budget_ns > 0
                            ? (double)ev->data.budget.elapsed_ns / (double)ev->data.budget.budget_ns * 100.0
                            : 0.0);
                    break;
                case AUDIO_EVENT_SCHEDULING_DELAY:
                    snprintf(tooltip, sizeof(tooltip),
                        "%s\nCh%d | Track %" G_GINT64_FORMAT
                        "\nLate by: %.2f ms  Expected: %.2f ms",
                        type_name, ch, ev->track_id,
                        (double)ev->data.scheduling.deviation_ns / 1e6,
                        (double)ev->data.scheduling.expected_ns / 1e6);
                    severity = 2; /* Scheduling delays are errors */
                    break;
                default:
                    snprintf(tooltip, sizeof(tooltip), "%s\nCh%d | Track %" G_GINT64_FORMAT,
                             type_name, ch, ev->track_id);
                    break;
            }
            perf_timeline_chart_add_event(priv->timeline, ev->timestamp_ns / 1000,
                                           lane, severity, type_name, tooltip);
        }
    }

    /* ─── Section 3: Audio Health — latency (1pt/sec from ring buffer) ─── */
    priv->tick_count++;
    if (priv->tick_count % 10 == 0 && priv->pipeline) {
        /* Read ring buffer samples once — serves p99 labels, line chart, and grouped hist */
        static uint16_t lat_buf[PERF_MAX_PLAYERS][8192];
        uint16_t* ptrs[PERF_MAX_PLAYERS];
        uint32_t  counts[PERF_MAX_PLAYERS];

        for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
            ptrs[i] = lat_buf[i];
            counts[i] = audio_pipeline_get_latency_samples(priv->pipeline, i, lat_buf[i], 8192);

            channel_state_t st = audio_pipeline_get_player_state(priv->pipeline, i);
            if (priv->latency_hist)
                perf_grouped_hist_set_group_visible(priv->latency_hist, i, st != CHANNEL_STOPPED);

            /* Compute percentiles from ring buffer samples */
            double p50 = 0, p90 = 0, p99 = 0;
            if (counts[i] > 0 && st != CHANNEL_STOPPED) {
                compute_percentiles_from_samples(lat_buf[i], counts[i], &p50, &p90, &p99);

                /* Update p99 label */
                char buf[32];
                snprintf(buf, sizeof(buf), "p99: %.0f µs", p99);
                if (strcmp(priv->prev_p99_text[i], buf) != 0) {
                    gtk_label_set_text(GTK_LABEL(priv->channel_p99_labels[i]), buf);
                    g_strlcpy(priv->prev_p99_text[i], buf, sizeof(priv->prev_p99_text[i]));
                }
            }

        }

        /* Feed grouped histogram from already-sorted samples (no re-read needed) */
        if (priv->latency_hist) {
            compute_log_bins(ptrs, counts, priv->latency_hist);
        }
    }

    /* ─── Section 4: System Health ─── */

    /* Scheduling Jitter chart — 20:1 max-pool decimation (200ms per display point) */
    if (priv->pipeline && priv->jitter_chart) {
        double global_max = 0.0;
        uint32_t channel_write_pos[PERF_MAX_PLAYERS] = {0};
        for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
            channel_state_t st = audio_pipeline_get_player_state(priv->pipeline, i);
            perf_line_chart_set_series_visible(priv->jitter_chart, i, st != CHANNEL_STOPPED);

            uint32_t n = audio_pipeline_get_interval_samples(
                priv->pipeline, i, priv->interval_buf, INTERVAL_RB_CAPACITY, &channel_write_pos[i]);

            /* Max-pool decimation aligned to absolute time boundaries.
             *
             * Groups are aligned to multiples of JITTER_DECIMATE from the
             * cumulative write_pos, so the same absolute raw samples always
             * fall in the same group.  Only COMPLETE groups are emitted —
             * the partial group at the newest end is excluded because its
             * max-pool value changes as new raw samples fill it.
             *
             * Result: old display points are perfectly stable across reads.
             * New points appear only when a full group completes (~200ms). */
            uint32_t start_pos = channel_write_pos[i] - n;
            uint32_t skip = (JITTER_DECIMATE - (start_pos % JITTER_DECIMATE)) % JITTER_DECIMATE;

            uint32_t out = 0;
            for (uint32_t j = skip; j + JITTER_DECIMATE <= n; j += JITTER_DECIMATE) {
                double mx = 0;
                for (uint32_t k = j; k < j + JITTER_DECIMATE; k++) {
                    double us = (double)priv->interval_buf[k] / 1000.0;
                    if (us > mx) mx = us;
                }
                if (mx > global_max) global_max = mx;
                priv->jitter_buf[out++] = mx;
            }
            perf_line_chart_set_data(priv->jitter_chart, i, priv->jitter_buf, out);
        }

        /* Gate scroll reset on epoch: floor(write_pos / JITTER_DECIMATE) bumps
         * exactly when a new complete display group forms (~every 200ms).
         * Unlike tracking decimated output count (which oscillates due to
         * alignment), write_pos is monotonically increasing and reliable. */
        uint32_t max_epoch = 0;
        for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
            channel_state_t st = audio_pipeline_get_player_state(priv->pipeline, i);
            if (st == CHANNEL_STOPPED) continue;
            uint32_t epoch = channel_write_pos[i] / JITTER_DECIMATE;
            if (epoch > max_epoch) max_epoch = epoch;
        }
        if (max_epoch > priv->jitter_last_epoch) {
            priv->jitter_last_epoch = max_epoch;
            perf_line_chart_scroll_reset(priv->jitter_chart);
        }

        double ceiling = 10.0;
        if (global_max > 1.0)
            ceiling = pow(10.0, ceil(log10(global_max)));
        if (ceiling < 10.0) ceiling = 10.0;
        perf_line_chart_set_range(priv->jitter_chart, 0, ceiling);
    }

    /* ─── Sections 4-5: Memory + Cache Latency (2-second sampling → 10min window) ─── */
    gboolean slow_tick = (priv->tick_count % 20 == 0);

    if (slow_tick && priv->dashboard && priv->mem_stacked_chart) {
        /* Collect memory from all subsystems */
        double audio_mb = 0;
        if (priv->audio_cache) {
            size_t used = audio_cache_get_memory_used(priv->audio_cache);
            audio_mb = (double)used / (1024.0 * 1024.0);
        }

        double lib_cache_mb[PERF_MAX_LIBRARIES] = {0};
        int lib_count = 0;
        if (priv->library_cache) {
            lib_count = library_cache_get_library_count(priv->library_cache);
            if (lib_count > PERF_MAX_LIBRARIES) lib_count = PERF_MAX_LIBRARIES;
            for (int i = 0; i < lib_count; i++) {
                int bi = library_cache_get_bitmap_index(priv->library_cache, i);
                size_t bytes = library_cache_get_slot_memory_bytes(priv->library_cache, bi);
                lib_cache_mb[i] = (double)bytes / (1024.0 * 1024.0);
            }
        }

        double art_texture_mb = 0;
        double art_atlas_mb[PERF_MAX_LIBRARIES] = {0};
        if (priv->artwork_mgr) {
            artwork_manager_stats_t art_stats;
            artwork_manager_get_stats(priv->artwork_mgr, &art_stats);
            art_texture_mb = (double)art_stats.texture_cache_bytes / (1024.0 * 1024.0);
            for (int i = 0; i < art_stats.lib_count && i < PERF_MAX_LIBRARIES; i++)
                art_atlas_mb[i] = (double)art_stats.atlas_mmap_bytes[i] / (1024.0 * 1024.0);
            if (art_stats.lib_count > lib_count) lib_count = art_stats.lib_count;
        }

        /* Feed multi-series ring buffer */
        perf_memory_multi_add(&priv->dashboard->memory_multi,
                              audio_mb, lib_cache_mb, art_texture_mb, art_atlas_mb, lib_count);

        /* Read all series and feed stacked chart */
        double values[PERF_TIMESERIES_SIZE];
        size_t count;
        int total_series = 1 + lib_count + 1 + lib_count;  /* audio + libs + texture + atlases */
        for (int s = 0; s < total_series && s < 16; s++) {
            perf_memory_multi_get(&priv->dashboard->memory_multi, s, values, &count);
            perf_stacked_area_chart_set_data(priv->mem_stacked_chart, s, values, count);
        }

    }

    /* ─── Section 5: Cache Latency (windowed p90 via snapshot-delta, 2s) ─── */
    if (slow_tick && priv->artwork_mgr && priv->cache_lat_chart) {
        const perf_histogram_us_t *tex_hist =
            (const perf_histogram_us_t *)artwork_manager_get_texture_hit_hist(priv->artwork_mgr);
        const perf_histogram_us_t *atlas_hist =
            (const perf_histogram_us_t *)artwork_manager_get_atlas_decode_hist(priv->artwork_mgr);

        if (tex_hist && atlas_hist) {
            /* Snapshot current bucket counts (relaxed atomics — dashboard data) */
            uint64_t cur_tex[PERF_HIST_BUCKETS], cur_atlas[PERF_HIST_BUCKETS];
            for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
                cur_tex[i]   = atomic_load_explicit(&tex_hist->buckets[i], memory_order_relaxed);
                cur_atlas[i] = atomic_load_explicit(&atlas_hist->buckets[i], memory_order_relaxed);
            }

            /* Compute p90 from the delta (only events in the last 2s window) */
            double tex_p90   = p90_from_us_delta(cur_tex,   priv->prev_tex_buckets);
            double atlas_p90 = p90_from_us_delta(cur_atlas, priv->prev_atlas_buckets);

            /* Store current as previous for next window */
            memcpy(priv->prev_tex_buckets,   cur_tex,   sizeof(cur_tex));
            memcpy(priv->prev_atlas_buckets, cur_atlas, sizeof(cur_atlas));

            /* Write into ring buffer */
            size_t idx = priv->lat_write_idx % PERF_TIMESERIES_SIZE;
            priv->tex_p90_ring[idx]   = tex_p90;
            priv->atlas_p90_ring[idx] = atlas_p90;
            priv->lat_write_idx++;

            /* Feed chart in chronological order */
            size_t n = priv->lat_write_idx < PERF_TIMESERIES_SIZE
                       ? priv->lat_write_idx : PERF_TIMESERIES_SIZE;
            if (priv->lat_write_idx <= PERF_TIMESERIES_SIZE) {
                perf_line_chart_set_data(priv->cache_lat_chart, 0, priv->tex_p90_ring, n);
                perf_line_chart_set_data(priv->cache_lat_chart, 1, priv->atlas_p90_ring, n);
            } else {
                double ordered[PERF_TIMESERIES_SIZE];
                size_t start = priv->lat_write_idx % PERF_TIMESERIES_SIZE;
                size_t tail = PERF_TIMESERIES_SIZE - start;
                memcpy(ordered, &priv->tex_p90_ring[start], tail * sizeof(double));
                memcpy(&ordered[tail], priv->tex_p90_ring, start * sizeof(double));
                perf_line_chart_set_data(priv->cache_lat_chart, 0, ordered, n);

                memcpy(ordered, &priv->atlas_p90_ring[start], tail * sizeof(double));
                memcpy(&ordered[tail], priv->atlas_p90_ring, start * sizeof(double));
                perf_line_chart_set_data(priv->cache_lat_chart, 1, ordered, n);
            }

            /* Auto-scale Y ceiling from most recent window p90 */
            double max_p90 = tex_p90 > atlas_p90 ? tex_p90 : atlas_p90;
            double ceiling = 10.0;
            if (max_p90 > 1.0) ceiling = pow(10.0, ceil(log10(max_p90)));
            if (ceiling < 10.0) ceiling = 10.0;
            perf_line_chart_set_range(priv->cache_lat_chart, 0, ceiling);
        }
    }

    if (priv->audio_cache && priv->decode_time_hist) {
        audio_cache_decode_event_t events[100];
        uint32_t event_count = audio_cache_get_decode_events(priv->audio_cache, events, 100);

        if (event_count > 0) {
            /* Build sorted array of decode times in ms */
            double times_ms[100];
            double d_min = 1e9, d_max = 0.0, d_sum = 0.0;
            for (uint32_t i = 0; i < event_count; i++) {
                times_ms[i] = (double)events[i].decode_duration_ms;
                if (times_ms[i] < d_min) d_min = times_ms[i];
                if (times_ms[i] > d_max) d_max = times_ms[i];
                d_sum += times_ms[i];
            }

            /* Simple insertion sort for percentiles (small N) */
            for (int i = 1; i < (int)event_count; i++) {
                double key = times_ms[i];
                int j = i - 1;
                while (j >= 0 && times_ms[j] > key) {
                    times_ms[j + 1] = times_ms[j];
                    j--;
                }
                times_ms[j + 1] = key;
            }

            /* Compute adaptive bins */
            perf_adaptive_hist_data_t adaptive = {0};
            adaptive.count = event_count;
            adaptive.min = d_min;
            adaptive.max = d_max;
            adaptive.p50 = times_ms[event_count / 2];
            adaptive.p90 = times_ms[event_count * 9 / 10];
            adaptive.p99 = times_ms[event_count > 1 ? event_count * 99 / 100 : 0];

            int target = 12;
            if ((int)event_count < target) target = (int)event_count;
            if (target < 2) target = 2;
            if (target > PERF_ADAPTIVE_MAX_BINS) target = PERF_ADAPTIVE_MAX_BINS;
            adaptive.num_bins = (uint32_t)target;

            double range = d_max - d_min;
            if (range < 0.001) range = 1.0;
            double bin_w = range / target;
            double inv_bin_w = (double)target / range;

            for (int b = 0; b < target; b++) {
                double edge = d_min + b * bin_w;
                snprintf(adaptive.bin_labels[b], sizeof(adaptive.bin_labels[b]),
                         "%.1f", edge);
            }

            for (uint32_t i = 0; i < event_count; i++) {
                int b = (int)((times_ms[i] - d_min) * inv_bin_w);
                if (b >= target) b = target - 1;
                if (b < 0) b = 0;
                adaptive.bin_counts[b]++;
            }

            perf_histogram_chart_set_adaptive_data(priv->decode_time_hist, &adaptive);
        }
    }

    return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Reconnect button callback
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { audio_pipeline_t* pipeline; int player_id; } ReconnectData;

static void on_reconnect_clicked(GtkButton* btn, gpointer data) {
    (void)btn;
    ReconnectData* d = data;
    audio_pipeline_player_reconnect(d->pipeline, d->player_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle — map/unmap timer gating
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_perf_map(GtkWidget* w, gpointer data) {
    (void)w;
    PerfViewPrivate* priv = data;
    if (priv->timer_id == 0) {
        priv->timer_id = g_timeout_add(100, update_dashboard, priv);
        g_debug("perf: timer started (map)");
    }
}

static void on_perf_unmap(GtkWidget* w, gpointer data) {
    (void)w;
    PerfViewPrivate* priv = data;
    if (priv->timer_id) {
        g_source_remove(priv->timer_id);
        priv->timer_id = 0;
        g_debug("perf: timer stopped (unmap)");
    }
}

static void on_view_destroy(GtkWidget* widget, gpointer data) {
    (void)widget;
    PerfViewPrivate* priv = data;
    if (priv->timer_id) {
        g_source_remove(priv->timer_id);
        priv->timer_id = 0;
    }
    g_free(priv->interval_buf);
    g_free(priv->jitter_buf);
    g_free(priv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Widget Construction
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget* perf_view_new(perf_dashboard_t* dashboard, audio_cache_t* cache,
                           audio_pipeline_t* pipeline,
                           library_cache_t* library_cache,
                           ArtworkManager* artwork_mgr) {
    PerfViewPrivate* priv = g_new0(PerfViewPrivate, 1);
    priv->dashboard     = dashboard;
    priv->audio_cache   = cache;
    priv->pipeline      = pipeline;
    priv->library_cache = library_cache;
    priv->artwork_mgr   = artwork_mgr;
    priv->interval_buf = g_malloc(INTERVAL_RB_CAPACITY * sizeof(int64_t));
    priv->jitter_buf   = g_malloc(JITTER_DISPLAY_MAX * sizeof(double));

    GtkBuilder* builder = gtk_builder_new_from_resource("/org/quadrature/ui/perf_view.ui");
    GtkWidget* root = GTK_WIDGET(gtk_builder_get_object(builder, "perf_view"));
    g_object_ref(root);

    /* ── Section 1: load channel cells from template into grid ── */
    GtkGrid* channel_grid = GTK_GRID(gtk_builder_get_object(builder, "channel_grid"));
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        GtkBuilder* cell_builder = gtk_builder_new_from_resource("/org/quadrature/ui/perf_channel_cell.ui");

        GtkWidget* cell = GTK_WIDGET(gtk_builder_get_object(cell_builder, "cell"));
        priv->channel_cells[i]         = cell;
        priv->channel_state_labels[i]  = GTK_WIDGET(gtk_builder_get_object(cell_builder, "state"));
        priv->channel_p99_labels[i]    = GTK_WIDGET(gtk_builder_get_object(cell_builder, "p99"));
        priv->channel_budget_labels[i] = GTK_WIDGET(gtk_builder_get_object(cell_builder, "bgt"));
        priv->channel_health_dots[i]   = GTK_WIDGET(gtk_builder_get_object(cell_builder, "dot"));
        priv->channel_reconnect_btns[i] = GTK_WIDGET(gtk_builder_get_object(cell_builder, "reconnect"));

        /* Set channel name */
        GtkWidget* name_label = GTK_WIDGET(gtk_builder_get_object(cell_builder, "channel_name"));
        char name[8];
        snprintf(name, sizeof(name), "Ch%d", i + 1);
        gtk_label_set_text(GTK_LABEL(name_label), name);

        /* Place in 2×2 grid */
        gtk_grid_attach(channel_grid, cell, i % 2, i / 2, 1, 1);

        if (pipeline) {
            ReconnectData* rd = g_new(ReconnectData, 1);
            rd->pipeline  = pipeline;
            rd->player_id = i;
            g_signal_connect_data(priv->channel_reconnect_btns[i], "clicked",
                                  G_CALLBACK(on_reconnect_clicked), rd,
                                  (GClosureNotify)(void(*)(void))g_free, 0);
        }

        g_object_unref(cell_builder);
    }

    /* ── Section 2: attach timeline chart ── */
    GtkWidget* timeline_container = GTK_WIDGET(gtk_builder_get_object(builder, "timeline_container"));
    priv->timeline = PERF_TIMELINE_CHART(perf_timeline_chart_new("", PERF_TIMELINE_MAX_LANES));
    gtk_widget_set_vexpand(GTK_WIDGET(priv->timeline), TRUE);
    gtk_box_append(GTK_BOX(timeline_container), GTK_WIDGET(priv->timeline));

    /* ── Section 3: attach callback latency grouped histogram ── */
    GtkWidget* latency_hist_container = GTK_WIDGET(gtk_builder_get_object(builder, "latency_hist_container"));
    priv->latency_hist = PERF_GROUPED_HIST(
        perf_grouped_hist_new("", "µs", PERF_MAX_PLAYERS, LOG_NUM_BINS));
    perf_grouped_hist_set_log_scale(priv->latency_hist, TRUE);
    /* Fixed log-spaced bin labels — set once, never change */
    for (int b = 0; b < LOG_NUM_BINS; b++)
        perf_grouped_hist_set_bucket_label(priv->latency_hist, b, LOG_LABELS[b]);
    gtk_widget_set_vexpand(GTK_WIDGET(priv->latency_hist), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(priv->latency_hist), TRUE);
    gtk_box_append(GTK_BOX(latency_hist_container), GTK_WIDGET(priv->latency_hist));

    /* ── Section 4: attach system health charts ── */

    /* Scheduling Jitter line chart (4 series: one per channel) */
    GtkWidget* jitter_container = GTK_WIDGET(gtk_builder_get_object(builder, "jitter_container"));
    static const GdkRGBA ch_colors[PERF_MAX_PLAYERS] = {
        {0.38, 0.63, 0.91, 1.0},  /* Ch1: blue */
        {0.29, 0.87, 0.50, 1.0},  /* Ch2: green */
        {0.96, 0.62, 0.04, 1.0},  /* Ch3: amber */
        {0.69, 0.47, 0.87, 1.0},  /* Ch4: purple */
    };
    priv->jitter_chart = PERF_LINE_CHART(perf_line_chart_new("", PERF_MAX_PLAYERS));
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        char label[8];
        snprintf(label, sizeof(label), "Ch%d", i + 1);
        perf_line_chart_set_series(priv->jitter_chart, i, label, &ch_colors[i]);
    }
    perf_line_chart_set_range(priv->jitter_chart, 0, 10000); /* log: 1-10000 µs */
    perf_line_chart_set_log_scale(priv->jitter_chart, TRUE);
    perf_line_chart_set_hover(priv->jitter_chart, 200.0, "µs"); /* 200ms decimated interval */
    perf_line_chart_set_capacity(priv->jitter_chart, JITTER_DISPLAY_MAX);
    gtk_widget_set_vexpand(GTK_WIDGET(priv->jitter_chart), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(priv->jitter_chart), TRUE);
    gtk_box_append(GTK_BOX(jitter_container), GTK_WIDGET(priv->jitter_chart));

    /* ── Section 4: Memory Usage stacked area chart ── */
    GtkWidget* mem_container = GTK_WIDGET(gtk_builder_get_object(builder, "memory_container"));
    {
        /* Determine series count: audio(1) + lib_cache(N) + artwork_tex(1) + atlas(N) */
        int lib_count = library_cache ? library_cache_get_library_count(library_cache) : 0;
        if (lib_count > PERF_MAX_LIBRARIES) lib_count = PERF_MAX_LIBRARIES;
        int total_series = 1 + lib_count + 1 + lib_count;
        if (total_series > 16) total_series = 16;

        priv->mem_stacked_chart = PERF_STACKED_AREA_CHART(
            perf_stacked_area_chart_new("", total_series));
        perf_stacked_area_chart_set_hover(priv->mem_stacked_chart, 2000.0, "MB");

        /* Series 0: Audio cache */
        perf_stacked_area_chart_set_series(priv->mem_stacked_chart, 0, "Audio Cache", NULL);

        /* Series 1..N: Library cache per-lib */
        for (int i = 0; i < lib_count; i++) {
            int bi = library_cache_get_bitmap_index(library_cache, i);
            char label[64];
            const char *name = library_cache_get_library_name(library_cache, bi);
            snprintf(label, sizeof(label), "Lib: %s", name ? name : "?");
            perf_stacked_area_chart_set_series(priv->mem_stacked_chart, 1 + i, label, NULL);
        }

        /* Series N+1: Artwork texture cache */
        perf_stacked_area_chart_set_series(priv->mem_stacked_chart, 1 + lib_count,
                                            "Art Textures", NULL);

        /* Series N+2..2N+1: Artwork atlas mmap per-lib */
        for (int i = 0; i < lib_count; i++) {
            int bi = library_cache_get_bitmap_index(library_cache, i);
            char label[64];
            const char *name = library_cache_get_library_name(library_cache, bi);
            snprintf(label, sizeof(label), "Atlas: %s", name ? name : "?");
            perf_stacked_area_chart_set_series(priv->mem_stacked_chart,
                                                2 + lib_count + i, label, NULL);
        }

        gtk_widget_set_vexpand(GTK_WIDGET(priv->mem_stacked_chart), TRUE);
        gtk_widget_set_hexpand(GTK_WIDGET(priv->mem_stacked_chart), TRUE);
        gtk_box_append(GTK_BOX(mem_container), GTK_WIDGET(priv->mem_stacked_chart));
    }

    /* ── Section 5: Cache Latency log-Y line chart ── */
    GtkWidget* cache_lat_container = GTK_WIDGET(gtk_builder_get_object(builder, "cache_lat_container"));
    {
        static const GdkRGBA tex_color   = {0.00, 0.83, 1.00, 1.0};  /* Cyan */
        static const GdkRGBA atlas_color = {0.97, 0.62, 0.04, 1.0};  /* Orange */
        priv->cache_lat_chart = PERF_LINE_CHART(perf_line_chart_new("", 2));
        perf_line_chart_set_series(priv->cache_lat_chart, 0, "Texture Hit p90", &tex_color);
        perf_line_chart_set_series(priv->cache_lat_chart, 1, "Atlas Decode p90", &atlas_color);
        perf_line_chart_set_range(priv->cache_lat_chart, 0, 10000);  /* 1-10000 µs */
        perf_line_chart_set_log_scale(priv->cache_lat_chart, TRUE);
        perf_line_chart_set_hover(priv->cache_lat_chart, 2000.0, "µs");
        perf_line_chart_set_time_axis(priv->cache_lat_chart, TRUE);
        gtk_widget_set_vexpand(GTK_WIDGET(priv->cache_lat_chart), TRUE);
        gtk_widget_set_hexpand(GTK_WIDGET(priv->cache_lat_chart), TRUE);
        gtk_box_append(GTK_BOX(cache_lat_container), GTK_WIDGET(priv->cache_lat_chart));
    }

    /* ── Section 6: Decode Time Distribution ── */
    GtkWidget* decode_container = GTK_WIDGET(gtk_builder_get_object(builder, "decode_container"));
    priv->decode_time_hist = PERF_HISTOGRAM_CHART(perf_histogram_chart_new("", "ms"));
    gtk_widget_set_vexpand(GTK_WIDGET(priv->decode_time_hist), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(priv->decode_time_hist), TRUE);
    gtk_box_append(GTK_BOX(decode_container), GTK_WIDGET(priv->decode_time_hist));

    g_signal_connect(root, "destroy", G_CALLBACK(on_view_destroy), priv);
    g_signal_connect(root, "map", G_CALLBACK(on_perf_map), priv);
    g_signal_connect(root, "unmap", G_CALLBACK(on_perf_unmap), priv);

    g_object_unref(builder);
    return root;
}
