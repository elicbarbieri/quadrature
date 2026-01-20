/**
 * @file perf_dashboard.c
 * @brief Performance dashboard implementation
 *
 * Lock-free metrics collection with log-scale histograms and ring buffers.
 */

#include "quadrature/core/perf_dashboard.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper: Get current time in microseconds
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint64_t time_us(void) {
    return (uint64_t)g_get_monotonic_time();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Histogram: Log-scale bucket index
 * Bucket 0: 0-1ms, Bucket 1: 1-2ms, Bucket 2: 2-4ms, ... Bucket 19: 512ms+
 * ═══════════════════════════════════════════════════════════════════════════ */

static int histogram_bucket(uint64_t us) {
    if (us < 1000) return 0;  /* < 1ms */
    uint64_t ms = us / 1000;
    int bucket = (int)(log2((double)ms)) + 1;
    return bucket >= PERF_HIST_BUCKETS ? PERF_HIST_BUCKETS - 1 : bucket;
}

static void histogram_record(perf_histogram_t* h, uint64_t value) {
    int bucket = histogram_bucket(value);
    atomic_fetch_add_explicit(&h->buckets[bucket], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->sum, value, memory_order_relaxed);

    /* Update min/max with CAS loops */
    uint64_t cur_min = atomic_load_explicit(&h->min, memory_order_relaxed);
    while (value < cur_min) {
        if (atomic_compare_exchange_weak_explicit(&h->min, &cur_min, value,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }

    uint64_t cur_max = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (value > cur_max) {
        if (atomic_compare_exchange_weak_explicit(&h->max, &cur_max, value,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
}

static void histogram_reset(perf_histogram_t* h) {
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        atomic_store_explicit(&h->buckets[i], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&h->count, 0, memory_order_relaxed);
    atomic_store_explicit(&h->sum, 0, memory_order_relaxed);
    atomic_store_explicit(&h->min, UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&h->max, 0, memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Time Series
 * ═══════════════════════════════════════════════════════════════════════════ */

static void timeseries_init(perf_timeseries_t* ts) {
    memset(ts->values, 0, sizeof(ts->values));
    memset(ts->timestamps, 0, sizeof(ts->timestamps));
    atomic_store(&ts->write_index, 0);
    g_mutex_init(&ts->lock);
}

static void timeseries_clear(perf_timeseries_t* ts) {
    g_mutex_clear(&ts->lock);
}

void perf_timeseries_add(perf_timeseries_t* ts, double value) {
    g_mutex_lock(&ts->lock);
    unsigned int idx = atomic_load(&ts->write_index) % PERF_TIMESERIES_SIZE;
    ts->values[idx] = value;
    ts->timestamps[idx] = time_us();
    atomic_fetch_add(&ts->write_index, 1);
    g_mutex_unlock(&ts->lock);
}

void perf_get_timeseries(perf_timeseries_t* ts, double* out, size_t* count) {
    g_mutex_lock(&ts->lock);
    unsigned int write_idx = atomic_load(&ts->write_index);
    size_t n = write_idx < PERF_TIMESERIES_SIZE ? write_idx : PERF_TIMESERIES_SIZE;

    /* Copy in chronological order */
    if (write_idx < PERF_TIMESERIES_SIZE) {
        memcpy(out, ts->values, n * sizeof(double));
    } else {
        unsigned int start = write_idx % PERF_TIMESERIES_SIZE;
        size_t tail_size = PERF_TIMESERIES_SIZE - start;
        memcpy(out, &ts->values[start], tail_size * sizeof(double));
        memcpy(&out[tail_size], ts->values, start * sizeof(double));
    }
    *count = n;
    g_mutex_unlock(&ts->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t perf_dashboard_create(uint32_t sample_rate, perf_dashboard_t** out) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    perf_dashboard_t* d = g_malloc0(sizeof(perf_dashboard_t));
    if (!d) return QUADRATURE_ERROR_OUT_OF_MEMORY;

    d->sample_rate = sample_rate;

    /* Initialize histograms with UINT64_MAX min */
    histogram_reset(&d->audio_decode);
    histogram_reset(&d->audio_latency);
    histogram_reset(&d->artwork_load_time);

    /* Initialize time series */
    timeseries_init(&d->artwork_hit_rate);
    timeseries_init(&d->cache_hit_rate);
    timeseries_init(&d->memory_usage);

    /* Initialize log buffer */
    atomic_store(&d->log_write, 0);
    atomic_store(&d->log_read, 0);

    /* Enable by default */
    atomic_store(&d->enabled, true);
    atomic_store(&d->paused, false);

    *out = d;
    return QUADRATURE_OK;
}

void perf_dashboard_destroy(perf_dashboard_t* d) {
    if (!d) return;

    timeseries_clear(&d->artwork_hit_rate);
    timeseries_clear(&d->cache_hit_rate);
    timeseries_clear(&d->memory_usage);

    g_free(d);
}

void perf_dashboard_reset(perf_dashboard_t* d) {
    if (!d) return;

    histogram_reset(&d->audio_decode);
    histogram_reset(&d->audio_latency);
    histogram_reset(&d->artwork_load_time);

    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        atomic_store(&d->audio_health[i].underruns, 0);
        atomic_store(&d->audio_health[i].callbacks, 0);
        atomic_store(&d->audio_health[i].late_callbacks, 0);
        atomic_store(&d->audio_health[i].jitter_sum_samples, 0);
        atomic_store(&d->audio_health[i].jitter_max_samples, 0);
        atomic_store(&d->audio_health[i].last_callback_sample, 0);
    }

    atomic_store(&d->artwork_hits, 0);
    atomic_store(&d->artwork_misses, 0);
    atomic_store(&d->artwork_evictions, 0);
    atomic_store(&d->artwork_failures, 0);
    atomic_store(&d->artwork_timeouts, 0);

    atomic_store(&d->cache_hits, 0);
    atomic_store(&d->cache_misses, 0);
    atomic_store(&d->cache_evictions, 0);

    atomic_store(&d->log_write, 0);
    atomic_store(&d->log_read, 0);
}

void perf_dashboard_pause(perf_dashboard_t* d, bool pause) {
    if (d) atomic_store(&d->paused, pause);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Recording - Audio (RT-Safe)
 * ═══════════════════════════════════════════════════════════════════════════ */

void perf_record_decode(perf_dashboard_t* d, uint64_t us) {
    if (!d || atomic_load(&d->paused)) return;
    histogram_record(&d->audio_decode, us);
}

void perf_record_latency(perf_dashboard_t* d, uint64_t us) {
    if (!d || atomic_load(&d->paused)) return;
    histogram_record(&d->audio_latency, us);
}

void perf_record_underrun(perf_dashboard_t* d, int player_id) {
    if (!d || atomic_load(&d->paused)) return;
    if (player_id < 0 || player_id >= PERF_MAX_PLAYERS) return;
    atomic_fetch_add(&d->audio_health[player_id].underruns, 1);
}

void perf_record_callback(perf_dashboard_t* d, int player_id, uint64_t sample) {
    if (!d || atomic_load(&d->paused)) return;
    if (player_id < 0 || player_id >= PERF_MAX_PLAYERS) return;

    perf_audio_health_t* h = &d->audio_health[player_id];
    atomic_fetch_add(&h->callbacks, 1);

    /* Calculate jitter from expected vs actual sample position */
    uint64_t last = atomic_load(&h->last_callback_sample);
    if (last > 0 && sample > last) {
        /* Expected ~256 samples per callback at 48kHz (~5.3ms) */
        uint64_t expected = 256;
        uint64_t actual = sample - last;
        uint64_t jitter = actual > expected ? actual - expected : expected - actual;

        atomic_fetch_add(&h->jitter_sum_samples, jitter);

        uint64_t cur_max = atomic_load(&h->jitter_max_samples);
        while (jitter > cur_max) {
            if (atomic_compare_exchange_weak(&h->jitter_max_samples, &cur_max, jitter))
                break;
        }
    }
    atomic_store(&h->last_callback_sample, sample);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Recording - Artwork & Cache
 * ═══════════════════════════════════════════════════════════════════════════ */

void perf_record_artwork_stats(perf_dashboard_t* d,
        uint64_t hits, uint64_t misses, uint64_t evictions,
        uint64_t failures, uint64_t timeouts) {
    if (!d || atomic_load(&d->paused)) return;

    atomic_store(&d->artwork_hits, hits);
    atomic_store(&d->artwork_misses, misses);
    atomic_store(&d->artwork_evictions, evictions);
    atomic_store(&d->artwork_failures, failures);
    atomic_store(&d->artwork_timeouts, timeouts);

    /* Update hit rate time series */
    uint64_t total = hits + misses;
    double rate = total > 0 ? (double)hits / (double)total * 100.0 : 0.0;
    perf_timeseries_add(&d->artwork_hit_rate, rate);
}

void perf_record_artwork_load(perf_dashboard_t* d, uint64_t us) {
    if (!d || atomic_load(&d->paused)) return;
    histogram_record(&d->artwork_load_time, us);
}

void perf_record_cache_stats(perf_dashboard_t* d,
        uint64_t hits, uint64_t misses, uint64_t evictions) {
    if (!d || atomic_load(&d->paused)) return;

    atomic_store(&d->cache_hits, hits);
    atomic_store(&d->cache_misses, misses);
    atomic_store(&d->cache_evictions, evictions);

    /* Update hit rate time series */
    uint64_t total = hits + misses;
    double rate = total > 0 ? (double)hits / (double)total * 100.0 : 0.0;
    perf_timeseries_add(&d->cache_hit_rate, rate);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Logging
 * ═══════════════════════════════════════════════════════════════════════════ */

void perf_log(perf_dashboard_t* d, perf_log_level_t level,
              const char* source, const char* fmt, ...) {
    if (!d || atomic_load(&d->paused)) return;

    unsigned int write_idx = atomic_fetch_add(&d->log_write, 1) % PERF_LOG_SIZE;
    perf_log_entry_t* entry = &d->logs[write_idx];

    entry->timestamp_us = time_us();
    entry->level = level;
    g_strlcpy(entry->source, source, sizeof(entry->source));

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, sizeof(entry->message), fmt, args);
    va_end(args);
}

int perf_read_logs(perf_dashboard_t* d, perf_log_entry_t* out, int max) {
    if (!d || !out || max <= 0) return 0;

    unsigned int write_idx = atomic_load(&d->log_write);
    unsigned int read_idx = atomic_load(&d->log_read);

    /* Handle wraparound: limit to last PERF_LOG_SIZE entries */
    if (write_idx - read_idx > PERF_LOG_SIZE) {
        read_idx = write_idx - PERF_LOG_SIZE;
    }

    int count = 0;
    while (read_idx < write_idx && count < max) {
        unsigned int idx = read_idx % PERF_LOG_SIZE;
        out[count++] = d->logs[idx];
        read_idx++;
    }

    atomic_store(&d->log_read, read_idx);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Querying
 * ═══════════════════════════════════════════════════════════════════════════ */

void perf_get_histogram_stats(const perf_histogram_t* h, perf_hist_stats_t* out) {
    if (!h || !out) return;
    memset(out, 0, sizeof(*out));

    out->count = atomic_load_explicit(&h->count, memory_order_relaxed);
    if (out->count == 0) return;

    out->min = atomic_load_explicit(&h->min, memory_order_relaxed);
    out->max = atomic_load_explicit(&h->max, memory_order_relaxed);

    uint64_t sum = atomic_load_explicit(&h->sum, memory_order_relaxed);
    out->mean = sum / out->count;

    /* Copy bucket counts */
    uint64_t total = 0;
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        out->bucket_counts[i] = atomic_load_explicit(&h->buckets[i], memory_order_relaxed);
        total += out->bucket_counts[i];
    }

    /* Calculate percentiles by scanning buckets */
    uint64_t p50_target = total * 50 / 100;
    uint64_t p90_target = total * 90 / 100;
    uint64_t p99_target = total * 99 / 100;

    uint64_t cumulative = 0;
    bool found_p50 = false, found_p90 = false, found_p99 = false;

    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        cumulative += out->bucket_counts[i];

        /* Bucket upper bound in microseconds */
        uint64_t bucket_max_us = (i == 0) ? 1000 : ((1ULL << i) * 1000);

        if (!found_p50 && cumulative >= p50_target) {
            out->p50 = bucket_max_us;
            found_p50 = true;
        }
        if (!found_p90 && cumulative >= p90_target) {
            out->p90 = bucket_max_us;
            found_p90 = true;
        }
        if (!found_p99 && cumulative >= p99_target) {
            out->p99 = bucket_max_us;
            found_p99 = true;
        }
    }
}

void perf_get_audio_health(const perf_dashboard_t* d, int player,
        uint64_t* underruns, uint64_t* callbacks, double* jitter_ms) {
    if (!d || player < 0 || player >= PERF_MAX_PLAYERS) return;

    const perf_audio_health_t* h = &d->audio_health[player];

    if (underruns) *underruns = atomic_load(&h->underruns);
    if (callbacks) *callbacks = atomic_load(&h->callbacks);

    if (jitter_ms) {
        uint64_t cb = atomic_load(&h->callbacks);
        if (cb > 1) {
            uint64_t jitter_samples = atomic_load(&h->jitter_sum_samples);
            double avg_jitter_samples = (double)jitter_samples / (double)(cb - 1);
            *jitter_ms = (avg_jitter_samples / (double)d->sample_rate) * 1000.0;
        } else {
            *jitter_ms = 0.0;
        }
    }
}
