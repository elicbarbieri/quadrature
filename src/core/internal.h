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

/* Feature flags */
#ifdef QUADRATURE_BUILD_BROADCAST
    #define QUADRATURE_FEATURE_MULTIPLE_SOURCES  1
    #define QUADRATURE_FEATURE_NETWORK_MOUNTS    1
    #define QUADRATURE_FEATURE_FANOTIFY          1
    #define QUADRATURE_FEATURE_DAEMON            1
    #define QUADRATURE_FEATURE_REPLICATION       1
#else
    #define QUADRATURE_FEATURE_MULTIPLE_SOURCES  0
    #define QUADRATURE_FEATURE_NETWORK_MOUNTS    0
    #define QUADRATURE_FEATURE_FANOTIFY          0
    #define QUADRATURE_FEATURE_DAEMON            0
    #define QUADRATURE_FEATURE_REPLICATION       0
#endif

/* Version info */
#define QUADRATURE_VERSION_MAJOR 0
#define QUADRATURE_VERSION_MINOR 1
#define QUADRATURE_VERSION_PATCH 0
#define QUADRATURE_VERSION_STRING "0.1.0"

/* Default paths */
#ifndef QUADRATURE_DATA_DIR
    #define QUADRATURE_DATA_DIR "~/.local/share/quadrature"
#endif

#ifndef QUADRATURE_CONFIG_DIR
    #define QUADRATURE_CONFIG_DIR "~/.config/quadrature"
#endif

/* Build mode name */
#ifdef QUADRATURE_BUILD_BROADCAST
    #define QUADRATURE_BUILD_MODE_NAME "Broadcast"
#else
    #define QUADRATURE_BUILD_MODE_NAME "Debug"
#endif

/* =============================================================================
 * Performance Dashboard
 * ============================================================================= */

/* Log Entry */
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

/* Histogram (Log-Scale Buckets) */
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

/* Time Series (Ring Buffer for Line Charts) */
#define PERF_TIMESERIES_SIZE 300  /* 5 minutes at 1/sec */

typedef struct {
    double values[PERF_TIMESERIES_SIZE];
    uint64_t timestamps[PERF_TIMESERIES_SIZE];
    atomic_uint write_index;
    GMutex lock;  /* For bulk reads */
} perf_timeseries_t;

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
    atomic_uint_fast64_t budget_overruns;      /* Callbacks exceeding 50% budget */
    atomic_uint_fast64_t dequeue_failures;     /* PipeWire couldn't provide output buffer */
    atomic_uint_fast64_t scrubber_underflows;  /* Rubberband couldn't fill requested frames */
    atomic_uint_fast64_t deferred_advances;    /* Track advance with audible gap (preload miss) */
    
    /* Advance quality tracking */
    atomic_uint_fast64_t instant_advances;     /* Preloaded track advances (no gap) */
    atomic_uint_fast64_t total_advances;       /* Total track changes */
} perf_audio_health_t;

/* Dashboard State */
#define PERF_LOG_SIZE 1000
#define PERF_MAX_PLAYERS 4

typedef struct perf_dashboard perf_dashboard_t;

struct perf_dashboard {
    /* Audio metrics */
    perf_histogram_t audio_decode;
    perf_histogram_t audio_latency;
    perf_audio_health_t audio_health[PERF_MAX_PLAYERS];
    uint32_t sample_rate;

    /* Per-player callback latency distribution */
    perf_histogram_t callback_time[PERF_MAX_PLAYERS];

    /* Per-player budget utilization % over time (line chart) */
    perf_timeseries_t budget_pct[PERF_MAX_PLAYERS];

    /* Snapshot state for windowed budget computation (main thread only) */
    uint64_t budget_last_sum_ns[PERF_MAX_PLAYERS];
    uint64_t budget_last_count[PERF_MAX_PLAYERS];

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

/* Lifecycle */
quadrature_result_t perf_dashboard_create(uint32_t sample_rate, perf_dashboard_t** out);
void perf_dashboard_destroy(perf_dashboard_t* d);
void perf_dashboard_reset(perf_dashboard_t* d);
void perf_dashboard_pause(perf_dashboard_t* d, bool pause);

/* Recording (Thread-Safe) */
void perf_record_decode(perf_dashboard_t* d, uint64_t us);
void perf_record_latency(perf_dashboard_t* d, uint64_t us);
void perf_record_underrun(perf_dashboard_t* d, int player_id);
void perf_record_callback(perf_dashboard_t* d, int player_id, uint64_t sample);
void perf_record_callback_time(perf_dashboard_t* d, int player_id,
    uint64_t elapsed_ns, uint32_t frame_count);
void perf_sample_budget_utilization(perf_dashboard_t* d);

/* Fault event recording */
void perf_record_dequeue_failure(perf_dashboard_t* d, int player_id);
void perf_record_scrubber_underflow(perf_dashboard_t* d, int player_id);
void perf_record_track_advance(perf_dashboard_t* d, int player_id, bool instant);

void perf_record_artwork_stats(perf_dashboard_t* d,
    uint64_t hits, uint64_t misses, uint64_t evictions,
    uint64_t failures, uint64_t timeouts);
void perf_record_artwork_load(perf_dashboard_t* d, uint64_t us);
void perf_record_cache_stats(perf_dashboard_t* d,
    uint64_t hits, uint64_t misses, uint64_t evictions);
void perf_log(perf_dashboard_t* d, perf_log_level_t level,
              const char* source, const char* fmt, ...) G_GNUC_PRINTF(4, 5);

/* Audio Cache Decode Metrics (computed from decode events) */
typedef struct {
    uint32_t total_decodes;
    float avg_decode_time_ms;
    float p50_decode_time_ms;
    float p90_decode_time_ms;
    float p99_decode_time_ms;
    float max_decode_time_ms;
    
    /* File type breakdown */
    uint32_t mp3_count;
    uint32_t flac_count;
    uint32_t m4a_count;
    uint32_t other_count;
} audio_cache_decode_metrics_t;

/* Querying (UI Thread) */
void perf_get_histogram_stats(const perf_histogram_t* h, perf_hist_stats_t* out);
void perf_get_audio_health(const perf_dashboard_t* d, int player,
    uint64_t* underruns, uint64_t* callbacks, double* jitter_ms);
/* Note: perf_get_player_stats defined in quadrature_audio.h (audio_pipeline_get_player_stats).
 * Use audio_pipeline API directly - no internal implementation needed. */
void perf_get_timeseries(perf_timeseries_t* ts, double* out, size_t* count);
int perf_read_logs(perf_dashboard_t* d, perf_log_entry_t* out, int max);
void perf_timeseries_add(perf_timeseries_t* ts, double value);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_CORE_INTERNAL_H */
