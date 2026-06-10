/**
 * Quadrature Core - Internal Header
 *
 * Consolidates all internal core utilities:
 * - Build configuration and feature flags
 * - Performance dashboard for real-time metrics collection
 *
 * This header should NOT be included by code outside the src/ tree.
 */

#ifndef QUADRATURE_CORE_INTERNAL_H
#define QUADRATURE_CORE_INTERNAL_H

#include "quadrature/quadrature.h"
#include <glib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Build Configuration
 * ============================================================================= */

/* Default to DEBUG if neither is defined */
#if !defined(QUADRATURE_BUILD_DEBUG) && !defined(QUADRATURE_BUILD_BROADCAST)
#define QUADRATURE_BUILD_DEBUG
#endif

/* =============================================================================
 * Performance Dashboard
 * ============================================================================= */

/* Histogram (Log-Scale Buckets) */
#define PERF_HIST_BUCKETS 20 /* 0-1ms, 1-2ms, 2-4ms, ... 512ms+ */

typedef struct {
    atomic_uint_fast64_t buckets[PERF_HIST_BUCKETS];
    atomic_uint_fast64_t count;
    atomic_uint_fast64_t sum;
    atomic_uint_fast64_t min;
    atomic_uint_fast64_t max;
} perf_histogram_t;

typedef struct {
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
    uint64_t max;
    uint64_t min;
    uint64_t mean;
    uint64_t count;
    uint64_t bucket_counts[PERF_HIST_BUCKETS]; /* For chart rendering */
} perf_hist_stats_t;

/* Microsecond-scale histogram — identical layout to perf_histogram_t but
 * with µs-scale bucket boundaries: [0,1µs), [1,2µs), [2,4µs), ... [512µs+).
 * Used for cache hit latencies where ms-scale buckets are too coarse.
 *
 * Recording is a single atomic_fetch_add — safe for hot-path use. */
typedef perf_histogram_t perf_histogram_us_t;

/** Record a µs value into a µs-scale histogram.  O(1), lock-free. */
static inline void
perf_histogram_record_us(perf_histogram_us_t *h, uint64_t us)
{
    /* Map to log-spaced bucket: bucket k = floor(log2(us)) + 1, clamped. */
    int bucket;
    if (us == 0) {
        bucket = 0;
    } else {
        /* __builtin_clzll: count leading zeros → floor(log2) */
        bucket = (int)(63 - __builtin_clzll(us)) + 1;
        if (bucket >= PERF_HIST_BUCKETS)
            bucket = PERF_HIST_BUCKETS - 1;
    }
    atomic_fetch_add_explicit(&h->buckets[bucket], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->sum, us, memory_order_relaxed);

    /* Update min/max with CAS loops (relaxed ordering — dashboard-only data) */
    uint64_t cur_min = atomic_load_explicit(&h->min, memory_order_relaxed);
    while (us < cur_min
           && !atomic_compare_exchange_weak_explicit(
               &h->min, &cur_min, us, memory_order_relaxed, memory_order_relaxed))
        ;

    uint64_t cur_max = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (us > cur_max
           && !atomic_compare_exchange_weak_explicit(
               &h->max, &cur_max, us, memory_order_relaxed, memory_order_relaxed))
        ;
}

/* Time Series (Ring Buffer for Line Charts) */
#define PERF_TIMESERIES_SIZE 300 /* 5 minutes at 1/sec */

typedef struct {
    double values[PERF_TIMESERIES_SIZE];
    uint64_t timestamps[PERF_TIMESERIES_SIZE];
    atomic_uint write_index;
    GMutex lock; /* For bulk reads */
} perf_timeseries_t;

/* Multi-Series Memory Time Series (for stacked area chart) */
#define PERF_MAX_LIBRARIES 8

typedef struct {
    double audio_cache_mb[PERF_TIMESERIES_SIZE];
    double lib_cache_mb[PERF_MAX_LIBRARIES][PERF_TIMESERIES_SIZE];
    double artwork_texture_mb[PERF_TIMESERIES_SIZE];
    double artwork_atlas_mb[PERF_MAX_LIBRARIES][PERF_TIMESERIES_SIZE];
    atomic_uint write_index;
    int lib_count;
    GMutex lock;
} perf_memory_multi_t;

/* Audio Health (Per-Player) */
typedef struct {
    atomic_uint_fast64_t underruns;
    atomic_uint_fast64_t callbacks;
    atomic_uint_fast64_t late_callbacks;
    atomic_uint_fast64_t jitter_sum_samples;
    atomic_uint_fast64_t jitter_max_samples;
    atomic_uint_fast64_t last_callback_sample;

    /* Callback timing (for windowed budget utilization computation) */
    atomic_uint_fast64_t callback_time_sum_ns;
    atomic_uint_fast64_t callback_count_for_budget;

    /* Fault event counters (should be 0 in normal operation) */
    atomic_uint_fast64_t budget_overruns;          /* Callbacks exceeding 50% budget */
    atomic_uint_fast64_t dequeue_failures;         /* PipeWire couldn't provide output buffer */
    atomic_uint_fast64_t shuttle_speed_underflows; /* Rubberband couldn't fill requested frames */
    atomic_uint_fast64_t deferred_advances; /* Track advance with audible gap (preload miss) */

    /* Advance quality tracking */
    atomic_uint_fast64_t instant_advances; /* Preloaded track advances (no gap) */
    atomic_uint_fast64_t total_advances;   /* Total track changes */
} perf_audio_health_t;

/* Dashboard State */
#define PERF_MAX_PLAYERS 4

typedef struct perf_dashboard perf_dashboard_t;

struct perf_dashboard {
    uint32_t sample_rate;

    /* Multi-series memory tracking (for stacked area chart) */
    perf_memory_multi_t memory_multi;

    /* PipeWire native metrics (updated from on_process callback) */
    atomic_uint_fast64_t pw_avail_buffers[PERF_MAX_PLAYERS];
    atomic_uint_fast64_t pw_queued_buffers[PERF_MAX_PLAYERS];
    atomic_int_fast64_t pw_delay_samples[PERF_MAX_PLAYERS];
    perf_timeseries_t pw_queue_depth[PERF_MAX_PLAYERS];

    /* Component references for polling (weak pointers, not owned) */
    void *audio_pipeline; /* audio_pipeline_t* - use void* to avoid circular dependency */
    void *audio_cache;    /* audio_cache_t* - use void* to avoid circular dependency */
    void *library_cache;  /* library_cache_t* */
    void *artwork_mgr;    /* ArtworkManager* */

    /* Control */
    atomic_bool enabled;
    atomic_bool paused;
};

/* Lifecycle */
quadrature_result_t perf_dashboard_create(uint32_t sample_rate, perf_dashboard_t **out);
void perf_dashboard_destroy(perf_dashboard_t *d);

/* Component Registration (for event polling) */
void perf_dashboard_set_audio_pipeline(perf_dashboard_t *d, void *pipeline);
void perf_dashboard_set_audio_cache(perf_dashboard_t *d, void *cache);

/* Querying (UI Thread) */
void perf_get_histogram_stats(const perf_histogram_t *h, perf_hist_stats_t *out);
void perf_get_timeseries(perf_timeseries_t *ts, double *out, size_t *count);
void perf_timeseries_add(perf_timeseries_t *ts, double value);
void perf_sample_pw_queue_depth(perf_dashboard_t *d);

/* Multi-series memory time series helpers */
void perf_memory_multi_init(perf_memory_multi_t *mm);
void perf_memory_multi_add(perf_memory_multi_t *mm,
                           double audio_cache_mb,
                           const double *lib_cache_mb,
                           double artwork_texture_mb,
                           const double *artwork_atlas_mb,
                           int lib_count);
void perf_memory_multi_get(perf_memory_multi_t *mm, int series_idx, double *out, size_t *count);

/* Component registration */
void perf_dashboard_set_library_cache(perf_dashboard_t *d, void *library_cache);
void perf_dashboard_set_artwork_mgr(perf_dashboard_t *d, void *artwork_mgr);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_CORE_INTERNAL_H */
