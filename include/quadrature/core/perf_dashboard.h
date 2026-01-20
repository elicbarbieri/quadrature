/**
 * @file perf_dashboard.h
 * @brief Performance dashboard for real-time metrics collection
 *
 * Provides lock-free atomic counters and histograms for audio, artwork,
 * and cache performance monitoring. All recording functions are thread-safe.
 */

#ifndef QUADRATURE_PERF_DASHBOARD_H
#define QUADRATURE_PERF_DASHBOARD_H

#include "types.h"
#include <glib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Log Entry
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    PERF_LOG_DEBUG,
    PERF_LOG_INFO,
    PERF_LOG_WARN,
    PERF_LOG_ERROR
} perf_log_level_t;

typedef struct {
    uint64_t timestamp_us;
    perf_log_level_t level;
    char source[16];      /* "audio", "artwork", "cache" */
    char message[200];
} perf_log_entry_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Histogram (Log-Scale Buckets)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_HIST_BUCKETS 20  /* 0-1ms, 1-2ms, 2-4ms, ... 512ms+ */

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
    uint64_t bucket_counts[PERF_HIST_BUCKETS];  /* For chart rendering */
} perf_hist_stats_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Time Series (Ring Buffer for Line Charts)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TIMESERIES_SIZE 300  /* 5 minutes at 1/sec */

typedef struct {
    double values[PERF_TIMESERIES_SIZE];
    uint64_t timestamps[PERF_TIMESERIES_SIZE];
    atomic_uint write_index;
    GMutex lock;  /* For bulk reads */
} perf_timeseries_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Health (Per-Player)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    atomic_uint_fast64_t underruns;
    atomic_uint_fast64_t callbacks;
    atomic_uint_fast64_t late_callbacks;
    atomic_uint_fast64_t jitter_sum_samples;
    atomic_uint_fast64_t jitter_max_samples;
    atomic_uint_fast64_t last_callback_sample;
} perf_audio_health_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Dashboard State
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_LOG_SIZE 1000
#define PERF_MAX_PLAYERS 4

typedef struct perf_dashboard perf_dashboard_t;

struct perf_dashboard {
    /* Audio metrics */
    perf_histogram_t audio_decode;
    perf_histogram_t audio_latency;
    perf_audio_health_t audio_health[PERF_MAX_PLAYERS];
    uint32_t sample_rate;

    /* Artwork metrics */
    atomic_uint_fast64_t artwork_hits;
    atomic_uint_fast64_t artwork_misses;
    atomic_uint_fast64_t artwork_evictions;
    atomic_uint_fast64_t artwork_failures;
    atomic_uint_fast64_t artwork_timeouts;
    perf_histogram_t artwork_load_time;

    /* Library cache metrics */
    atomic_uint_fast64_t cache_hits;
    atomic_uint_fast64_t cache_misses;
    atomic_uint_fast64_t cache_evictions;

    /* Time series for line charts (1 sample/sec) */
    perf_timeseries_t artwork_hit_rate;
    perf_timeseries_t cache_hit_rate;
    perf_timeseries_t memory_usage;

    /* Log ring buffer */
    perf_log_entry_t logs[PERF_LOG_SIZE];
    atomic_uint log_write;
    atomic_uint log_read;

    /* Control */
    atomic_bool enabled;
    atomic_bool paused;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * API - Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create a new performance dashboard
 * @param sample_rate Audio sample rate for jitter calculations
 * @param out Output pointer for created dashboard
 * @return QUADRATURE_OK on success
 */
quadrature_result_t perf_dashboard_create(uint32_t sample_rate, perf_dashboard_t** out);

/**
 * Destroy a performance dashboard
 * @param d Dashboard to destroy (NULL safe)
 */
void perf_dashboard_destroy(perf_dashboard_t* d);

/**
 * Reset all metrics to zero
 * @param d Dashboard to reset
 */
void perf_dashboard_reset(perf_dashboard_t* d);

/**
 * Pause/resume metric collection
 * @param d Dashboard
 * @param pause True to pause, false to resume
 */
void perf_dashboard_pause(perf_dashboard_t* d, bool pause);

/* ═══════════════════════════════════════════════════════════════════════════
 * API - Recording (Thread-Safe)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Audio (RT-safe - atomics only) */
void perf_record_decode(perf_dashboard_t* d, uint64_t us);
void perf_record_latency(perf_dashboard_t* d, uint64_t us);
void perf_record_underrun(perf_dashboard_t* d, int player_id);
void perf_record_callback(perf_dashboard_t* d, int player_id, uint64_t sample);

/* Artwork */
void perf_record_artwork_stats(perf_dashboard_t* d,
    uint64_t hits, uint64_t misses, uint64_t evictions,
    uint64_t failures, uint64_t timeouts);
void perf_record_artwork_load(perf_dashboard_t* d, uint64_t us);

/* Cache */
void perf_record_cache_stats(perf_dashboard_t* d,
    uint64_t hits, uint64_t misses, uint64_t evictions);

/* Logging (any thread) */
void perf_log(perf_dashboard_t* d, perf_log_level_t level,
              const char* source, const char* fmt, ...) G_GNUC_PRINTF(4, 5);

/* ═══════════════════════════════════════════════════════════════════════════
 * API - Querying (UI Thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get computed statistics from a histogram
 * @param h Histogram to query
 * @param out Output stats structure
 */
void perf_get_histogram_stats(const perf_histogram_t* h, perf_hist_stats_t* out);

/**
 * Get audio health metrics for a player
 * @param d Dashboard
 * @param player Player index (0-3)
 * @param underruns Output: underrun count
 * @param callbacks Output: callback count
 * @param jitter_ms Output: average jitter in milliseconds
 */
void perf_get_audio_health(const perf_dashboard_t* d, int player,
    uint64_t* underruns, uint64_t* callbacks, double* jitter_ms);

/**
 * Get time series data for charting
 * @param ts Time series to read
 * @param out Output array (must be PERF_TIMESERIES_SIZE)
 * @param count Output: number of valid entries
 */
void perf_get_timeseries(perf_timeseries_t* ts, double* out, size_t* count);

/**
 * Read log entries from the ring buffer
 * @param d Dashboard
 * @param out Output array for log entries
 * @param max Maximum entries to read
 * @return Number of entries read
 */
int perf_read_logs(perf_dashboard_t* d, perf_log_entry_t* out, int max);

/**
 * Add a time series sample
 * @param ts Time series
 * @param value Value to add
 */
void perf_timeseries_add(perf_timeseries_t* ts, double value);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_PERF_DASHBOARD_H */
