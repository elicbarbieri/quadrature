/**
 * @file perf_dashboard.c
 * @brief Performance dashboard implementation
 *
 * Lock-free metrics collection with log-scale histograms and ring buffers.
 */

#include "internal.h"
#include "quadrature/audio.h"
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

    /* Initialize time series */
    perf_memory_multi_init(&d->memory_multi);

    /* Initialize PipeWire metrics */
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        atomic_store(&d->pw_avail_buffers[i], 0);
        atomic_store(&d->pw_queued_buffers[i], 0);
        atomic_store(&d->pw_delay_samples[i], 0);
        timeseries_init(&d->pw_queue_depth[i]);
    }

    /* Initialize component pointers (set later via registration functions) */
    d->audio_pipeline = NULL;
    d->audio_cache = NULL;
    /* Enable by default */
    atomic_store(&d->enabled, true);
    atomic_store(&d->paused, false);

    *out = d;
    return QUADRATURE_OK;
}

void perf_dashboard_destroy(perf_dashboard_t* d) {
    if (!d) return;

    g_mutex_clear(&d->memory_multi.lock);
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        timeseries_clear(&d->pw_queue_depth[i]);
    }

    g_free(d);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Component Registration (for event polling)
 * ═══════════════════════════════════════════════════════════════════════════ */

void perf_dashboard_set_audio_pipeline(perf_dashboard_t* d, void* pipeline) {
    if (d) d->audio_pipeline = pipeline;
}

void perf_dashboard_set_audio_cache(perf_dashboard_t* d, void* cache) {
    if (d) d->audio_cache = cache;
}

void perf_dashboard_set_library_cache(perf_dashboard_t* d, void* library_cache) {
    if (d) d->library_cache = library_cache;
}

void perf_dashboard_set_artwork_mgr(perf_dashboard_t* d, void* artwork_mgr) {
    if (d) d->artwork_mgr = artwork_mgr;
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

/* PipeWire queue depth sampling (called from ~1s timer) */
void perf_sample_pw_queue_depth(perf_dashboard_t* d) {
    if (!d || atomic_load(&d->paused)) return;

    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        uint64_t avail = atomic_load(&d->pw_avail_buffers[i]);
        perf_timeseries_add(&d->pw_queue_depth[i], (double)avail);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Multi-Series Memory Time Series
 * ═══════════════════════════════════════════════════════════════════════════ */

void perf_memory_multi_init(perf_memory_multi_t* mm) {
    memset(mm, 0, sizeof(*mm));
    g_mutex_init(&mm->lock);
}

void perf_memory_multi_add(perf_memory_multi_t* mm,
                           double audio_cache_mb,
                           const double* lib_cache_mb,
                           double artwork_texture_mb,
                           const double* artwork_atlas_mb,
                           int lib_count) {
    g_mutex_lock(&mm->lock);
    unsigned int idx = atomic_load(&mm->write_index) % PERF_TIMESERIES_SIZE;

    mm->audio_cache_mb[idx]    = audio_cache_mb;
    mm->artwork_texture_mb[idx] = artwork_texture_mb;

    int n = lib_count < PERF_MAX_LIBRARIES ? lib_count : PERF_MAX_LIBRARIES;
    mm->lib_count = n;
    for (int i = 0; i < n; i++) {
        mm->lib_cache_mb[i][idx]    = lib_cache_mb ? lib_cache_mb[i] : 0.0;
        mm->artwork_atlas_mb[i][idx] = artwork_atlas_mb ? artwork_atlas_mb[i] : 0.0;
    }
    /* Zero unused libraries at this index */
    for (int i = n; i < PERF_MAX_LIBRARIES; i++) {
        mm->lib_cache_mb[i][idx]    = 0.0;
        mm->artwork_atlas_mb[i][idx] = 0.0;
    }

    atomic_fetch_add(&mm->write_index, 1);
    g_mutex_unlock(&mm->lock);
}

/**
 * Get a single series from the multi-series memory ring buffer.
 * Series index encoding:
 *   0                  = audio cache MB
 *   1 .. lib_count     = library cache MB [0..lib_count-1]
 *   lib_count + 1      = artwork texture cache MB
 *   lib_count + 2 .. + lib_count + 1 + lib_count = artwork atlas MB [0..lib_count-1]
 */
void perf_memory_multi_get(perf_memory_multi_t* mm, int series_idx,
                           double* out, size_t* count) {
    g_mutex_lock(&mm->lock);
    unsigned int write_idx = atomic_load(&mm->write_index);
    size_t n = write_idx < PERF_TIMESERIES_SIZE ? write_idx : PERF_TIMESERIES_SIZE;

    /* Select source array */
    const double *src = NULL;
    int lc = mm->lib_count;
    if (series_idx == 0) {
        src = mm->audio_cache_mb;
    } else if (series_idx >= 1 && series_idx <= lc) {
        src = mm->lib_cache_mb[series_idx - 1];
    } else if (series_idx == lc + 1) {
        src = mm->artwork_texture_mb;
    } else if (series_idx >= lc + 2 && series_idx < lc + 2 + lc) {
        src = mm->artwork_atlas_mb[series_idx - lc - 2];
    }

    if (!src) { *count = 0; g_mutex_unlock(&mm->lock); return; }

    /* Copy in chronological order */
    if (write_idx < PERF_TIMESERIES_SIZE) {
        memcpy(out, src, n * sizeof(double));
    } else {
        unsigned int start = write_idx % PERF_TIMESERIES_SIZE;
        size_t tail = PERF_TIMESERIES_SIZE - start;
        memcpy(out, &src[start], tail * sizeof(double));
        memcpy(&out[tail], src, start * sizeof(double));
    }
    *count = n;
    g_mutex_unlock(&mm->lock);
}
