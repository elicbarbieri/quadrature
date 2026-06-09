/**
 * Quadrature Audio Subsystem - Internal Header
 *
 * Private types, shared helpers, and constants for the audio subsystem.
 * This header should NOT be included by code outside src/audio/.
 */

#ifndef QUADRATURE_AUDIO_INTERNAL_H
#define QUADRATURE_AUDIO_INTERNAL_H

#include "quadrature/quadrature.h"
#include "quadrature/audio.h"
#include "../core/internal.h"
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* FFmpeg headers */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

/* PipeWire headers */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* Note: FFmpeg is still used for decoding, but scrubbing now uses rubberband */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Consolidated Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Budget ring buffer capacity: 10-min @ ~10ms intervals = 60,000 entries */
#define BUDGET_RB_CAPACITY     65536 /* must be power-of-2 */

#define LOUDNESS_BINS          1024
#define DECODE_BUFFER_FRAMES   4096
#define SPECTRUM_BARS          24
#define FFT_SAMPLES            2048
#define SCRUB_SPEED_MULTIPLIER 3
#define MAX_AUDIO_PLAYERS      4

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct audio_player audio_player_t;
typedef struct audio_pipeline audio_pipeline_t;
typedef struct audio_cache audio_cache_t;
typedef struct audio_buffer audio_buffer_t;
typedef struct audio_scrubber audio_scrubber_t;
typedef struct library_cache library_cache_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Budget Ring Buffer
 *
 * Lock-free ring buffer recording per-callback budget utilization in centipercent
 * (0-10000 = 0.00%-100.00%). Written by the audio thread at ~10ms intervals,
 * read by the UI thread.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t samples[BUDGET_RB_CAPACITY]; /* 0-10000 = centipercent; written by audio thread */
    uint64_t last_write_ns;               /* audio-thread-only; no sync needed */
    atomic_uint write_pos;                /* release-store after each write */
} budget_rb_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Latency Ring Buffer
 *
 * Lock-free ring buffer recording per-callback latency in µs (capped at 65535).
 * Written by the audio thread at ~10ms intervals, read by the UI thread.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LATENCY_RB_CAPACITY 65536 /* must be power-of-2 */

typedef struct {
    uint16_t samples[LATENCY_RB_CAPACITY]; /* µs, capped at 65535 */
    uint64_t last_write_ns;                /* audio-thread-only; no sync needed */
    atomic_uint write_pos;                 /* release-store after each write */
} latency_rb_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback Interval Ring Buffer
 *
 * Lock-free ring buffer recording peak absolute scheduling deviation in raw
 * nanoseconds. Each sample is the peak |deviation| within a ~10ms sampling
 * window. Always >= 0. Written by the audio thread with zero conversion;
 * unit scaling (ns → µs) is performed by the reader (perf UI).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define INTERVAL_RB_CAPACITY 8192 /* must be power-of-2; ~82s at 10ms */

typedef struct {
    int64_t samples[INTERVAL_RB_CAPACITY]; /* raw ns, always >= 0 */
    uint64_t last_write_ns;                /* audio-thread-only; no sync needed */
    atomic_uint write_pos;                 /* release-store after each write */
} interval_rb_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-Player Spectrum State (cavacore FFT)
 *
 * Processed inline in the PipeWire monitor callback — no separate thread.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct cava_plan; /* Forward declaration from cavacore */

typedef struct {
    struct cava_plan *plan;
    double *input_buffer; /* Accumulation buffer for cavacore */
    double *output_bars;  /* cavacore output (num_bars * 2) */
    size_t input_buffer_size;
    size_t input_buffer_fill;
    int sample_rate;           /* Cached for threshold recomputation */
    _Atomic int fft_threshold; /* Stereo-interleaved samples per FFT; tuned to display Hz */
} spectrum_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Decoder Helper
 *
 * Low-level FFmpeg decode logic used by audio_cache.
 * All audio is fully decoded to PCM buffers before playback - no streaming.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    AVFormatContext *fmt_ctx;
    AVCodecContext *codec_ctx;
    SwrContext *swr_ctx;
    AVFrame *frame;
    AVPacket *packet;
    int stream_index;
    int64_t stream_start_time;
    double time_base;
    int source_sample_rate;
    int output_sample_rate;
    int output_channels;
    bool eof;
} ffmpeg_decoder_t;

/**
 * Open an audio file for decoding.
 *
 * @param dec    Decoder state (zero-initialized)
 * @param path   Path to audio file
 * @param rate   Target output sample rate
 * @return QUADRATURE_OK on success
 */
quadrature_result_t ffmpeg_decoder_open(ffmpeg_decoder_t *dec, const char *path, uint32_t rate);

/**
 * Read decoded frames into buffer.
 *
 * @param dec         Decoder state
 * @param buffer      Output buffer (stereo interleaved float)
 * @param max_frames  Maximum frames to read
 * @return Number of frames read, 0 on EOF, -1 on error
 */
int ffmpeg_decoder_read(ffmpeg_decoder_t *dec, float *buffer, size_t max_frames);

/**
 * Seek to a position in samples.
 *
 * @param dec       Decoder state
 * @param position  Target position in samples (at output sample rate)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t ffmpeg_decoder_seek(ffmpeg_decoder_t *dec, uint64_t position);

/**
 * Flush decoder buffers (call after seek).
 */
void ffmpeg_decoder_flush(ffmpeg_decoder_t *dec);

/**
 * Close decoder and free resources.
 */
void ffmpeg_decoder_close(ffmpeg_decoder_t *dec);

/**
 * Get total duration in frames (at output sample rate).
 */
uint64_t ffmpeg_decoder_duration(ffmpeg_decoder_t *dec);

/* ═══════════════════════════════════════════════════════════════════════════
 * Time Utilities
 *
 * clock_gettime(CLOCK_MONOTONIC) is VDSO-mapped on Linux — executes entirely
 * in userspace with no kernel transition (~20ns). Same mechanism used by
 * PipeWire, JACK, and Ardour in their RT paths.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t
time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Nanosecond-precision monotonic timer for RT callback profiling.
 */
static inline uint64_t
time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Soft Limiter (Safety)
 *
 * Two-stage limiter for rubberband output which can exceed unity gain:
 * 1. Soft knee compression starting at threshold (default 0.8)
 * 2. Hard tanh saturation as final safety net
 *
 * This provides transparent limiting for moderate overshoots while
 * preventing harsh clipping on extreme peaks.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LIMITER_THRESHOLD 0.8f /* Start soft knee at 80% */
#define LIMITER_KNEE      0.2f /* Knee width */

static inline float
soft_limit(float x)
{
    float abs_x = fabsf(x);
    float sign = copysignf(1.0f, x);
    float overshoot = abs_x - LIMITER_THRESHOLD;

    /* Compute all paths unconditionally — select via ternary (cmov, no branches).
     * Enables auto-vectorization of caller loops with -O3 + AVX2 baseline. */
    float knee
        = LIMITER_THRESHOLD + (1.0f - LIMITER_THRESHOLD) * (overshoot / (overshoot + LIMITER_KNEE));
    float x2 = abs_x * abs_x;
    float tanh_approx = abs_x * (27.0f + x2) / (27.0f + 9.0f * x2);

    return (abs_x <= LIMITER_THRESHOLD) ? x
           : (overshoot < 1.0f)         ? sign * knee
           : (abs_x > 2.0f)             ? sign
                                        : sign * tanh_approx;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Scrubber (Variable-Speed Playback)
 *
 * User-controlled mode selection for variable-speed playback:
 *   - Passthrough (speed ≈ 1.0): Direct copy, zero CPU - always active
 *   - Rubberband (default): Pitch-preserved time stretching, 0.5x-4.0x
 *   - Turntable (pitched mode): Pitch shifts with speed, 0.5x-2.0x
 *
 * Mode is controlled via pitched_mode flag. Crossfade handles smooth
 * transitions when switching modes while playing.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Lifecycle */
quadrature_result_t audio_scrubber_create(uint32_t sample_rate, audio_scrubber_t **out);
void audio_scrubber_destroy(audio_scrubber_t *s);

/* Control API (UI thread, atomic-safe) */
void audio_scrubber_set_speed(audio_scrubber_t *s, float speed); /* -4.0 to +4.0 */
void audio_scrubber_set_position(audio_scrubber_t *s, int64_t position);
void audio_scrubber_set_shuttle_mode(audio_scrubber_t *s, shuttle_mode_t mode);

/* Query API (thread-safe) */
float audio_scrubber_get_speed(const audio_scrubber_t *s);
int64_t audio_scrubber_get_position(const audio_scrubber_t *s);
shuttle_mode_t audio_scrubber_get_shuttle_mode(const audio_scrubber_t *s);

/* Stats query (thread-safe) */
uint64_t audio_scrubber_get_underflows(const audio_scrubber_t *s);
int audio_scrubber_get_zone(const audio_scrubber_t *s);

/* Audio thread - unified playback at variable speed */
uint32_t audio_scrubber_process(audio_scrubber_t *s,
                                const float *samples,
                                uint64_t num_frames,
                                float *output,
                                uint32_t frames,
                                uint64_t *out_position);

/**
 * Flush all internal scrubber buffers.
 * Call after seek to prevent stale audio from playing.
 */
void audio_scrubber_flush(audio_scrubber_t *s);

/* ═══════════════════════════════════════════════════════════════════════════
 * Position Snapshot (Seqlock Pattern)
 *
 * Captures position + timestamp + speed + state atomically in audio thread.
 * UI thread reads with retry for smooth interpolation between updates.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t position;     /* Position in samples */
    uint64_t sample_count; /* Callback sample counter (RT-safe, replaces timestamp_us) */
    float speed;           /* Playback speed (-4.0 to +4.0) */
    uint8_t playing;       /* 1 if playing, 0 otherwise */
    uint8_t _pad[3];       /* Alignment padding */
} position_snapshot_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Player (Internal Definition)
 *
 * Individual audio player with buffer-based playback and PipeWire output.
 * All playback happens from decoded PCM buffers - no streaming fallback.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_player {
    int player_id;
    audio_pipeline_t *pipeline; /* Back-reference for perf access */

    /* Playback state (atomic for real-time safety) */
    atomic_int state; /* CHANNEL_STOPPED, CHANNEL_PLAYING, CHANNEL_PAUSED, CHANNEL_ERROR */
    atomic_uint_fast64_t position_samples;
    atomic_uint_fast64_t length_samples;
    atomic_int end_mode; /* track_end_mode_t — default TRACK_END_AUTOPLAY */

    /* Track ID-based playback (replaces filepath) */
    atomic_int_fast64_t current_track_id;  /* Currently playing track */
    atomic_int_fast64_t next_track_id;     /* Preloaded for instant advance */
    _Atomic(audio_buffer_t *) next_buffer; /* Preloaded buffer */
    atomic_bool advance_pending;           /* Signal for deferred cleanup */
    atomic_int_fast64_t advance_old_track_id;
    atomic_int_fast64_t pending_buffer_track_id; /* Track awaiting decode completion */

    /* Buffer playback (ALWAYS present when ready to play) */
    /* Atomic for thread-safe access from audio callback */
    _Atomic(audio_buffer_t *) buffer;
    uint64_t current_frame;

    /* Rate processor for variable-speed playback (ALWAYS attached) */
    audio_scrubber_t *scrubber;

    /* Position snapshot for smooth UI interpolation (seqlock pattern) */
    atomic_uint position_seq;          /* Seqlock sequence (odd = writing) */
    position_snapshot_t position_snap; /* Snapshot data */

    /* Sample counter for RT-safe timestamps (incremented by callback) */
    atomic_uint_fast64_t callback_sample_count;

    /* RT timing for fault diagnostics */
    uint64_t prev_scrubber_underflows; /* Snapshot for delta detection */

    /* PipeWire stream state (updated by state_changed callback) */
    atomic_int pw_stream_state; /* enum pw_stream_state */

    /* Per-player RT stats (atomic — written in callback, read from UI thread) */
    atomic_uint_fast64_t stats_cb_count;          /* Internal: for computing averages */
    atomic_uint_fast64_t stats_cb_time_sum_ns;    /* Cumulative processing time */
    atomic_uint_fast64_t stats_cb_time_max_ns;    /* Peak processing time */
    atomic_uint_fast64_t stats_budget_overruns;   /* Callbacks exceeding 50% of period budget */
    atomic_uint_fast64_t stats_dequeue_failures;  /* pw_stream_dequeue_buffer returned NULL */
    atomic_uint_fast64_t stats_deferred_advances; /* Silence gap — no preloaded buffer */
    atomic_uint_fast64_t stats_instant_advances;  /* Gapless buffer swap in RT callback */

    /* Spectrum analyzer data (stereo: 0..SPECTRUM_BARS-1 = left, SPECTRUM_BARS..2*SPECTRUM_BARS-1 = right) */
    _Atomic float spectrum_bars[SPECTRUM_BARS * 2];
    _Atomic uint32_t spectrum_generation; /* Incremented after cava_execute produces new output */
    spectrum_state_t spectrum;            /* Inline cava state, processed in monitor callback */

    /* PipeWire streams */
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct pw_stream *monitor_stream; /* INPUT stream capturing from device monitor */
    struct spa_hook monitor_stream_listener;
    char target_device[256];
    bool exclusive;          /* PipeWire exclusive mode on device */
    uint32_t quantum_frames; /* PipeWire quantum / buffer size (default: 512) */

    /* Device error state + auto-reconnect */
    atomic_bool device_error;        /* true when PW stream is in ERROR state */
    atomic_bool reconnect_attempted; /* true after first reconnect try; second ERROR deactivates */
    atomic_bool streams_active;      /* true when PW streams are connected */
    atomic_uint
        stream_generation; /* bumped on every player_recreate_stream; stale idles check this */

    /* Budget ring buffer (~10-min history, sampled every ~10ms) */
    budget_rb_t *budget_rb; /* heap-allocated, ~128KB */

    /* Callback latency ring buffer (µs, ~10-min history at ~10ms intervals) */
    latency_rb_t *latency_rb; /* heap-allocated, ~128KB */

    /* Callback interval deviation ring buffer (signed µs, peak per ~10ms window) */
    interval_rb_t *interval_rb;   /* heap-allocated, ~128KB */
    uint64_t last_callback_ns;    /* audio-thread-only: previous callback timestamp */
    int64_t interval_peak_dev_ns; /* audio-thread-only: max |deviation| since last rb write */

    /* Cached SPA pod params for stream reconnect */
    uint8_t cached_params_buf[1024];
    const struct spa_pod *cached_params[1];
    int num_cached_params;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline Event Types
 * 
 * Note: Types defined in public header (quadrature/audio.h).
 * Ring buffer size defined here since it's internal implementation detail.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AUDIO_EVENT_RING_SIZE 512 /* Power of 2, large enough for RT events */

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline (Internal Definition)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_pipeline {
    audio_player_t players[MAX_AUDIO_PLAYERS];
    uint32_t sample_rate;
    uint64_t ns_per_frame; /* 1000000000 / sample_rate, pre-computed at init */

    /* PipeWire shared context */
    struct pw_thread_loop *loop;
    struct pw_context *pw_ctx;
    struct pw_core *core;

    /* Device hot-plug monitor (persistent PW registry listener) */
    struct pw_registry *device_monitor_registry;
    struct spa_hook device_monitor_registry_listener;
    struct spa_hook device_monitor_core_listener;
    int device_monitor_sync_seq;         /* expected seq for settled handshake */
    bool device_monitor_settled;         /* true once initial burst is past */
    GHashTable *device_monitor_sink_ids; /* set of known Audio/Sink node IDs */
    void (*device_changed_cb)(void *user_data);
    void *device_changed_user_data;

    /* Audio cache (shared by all players) - track_id based */
    audio_cache_t *cache;

    /* Library cache (for track_id -> path resolution, next/prev navigation) */
    library_cache_t *library;

    /* Track changed callback */
    void (*track_changed_callback)(int player_id, int64_t track_id, void *user_data);
    void *track_changed_user_data;

    /* Auto-advance timeout (runs on main thread) */
    guint advance_timeout_id;

    /* Event ring buffer for performance monitoring (lock-free SPSC) */
    audio_pipeline_event_t events[AUDIO_EVENT_RING_SIZE];
    atomic_uint event_write;
    atomic_uint event_read;

    /* Performance dashboard (for aggregated view, not written to directly) */
    perf_dashboard_t *perf;

    /* Pipeline statistics (atomic for thread-safe reads) */
    atomic_uint_fast64_t stats_callback_count;
    atomic_uint_fast64_t stats_underrun_count;
    atomic_uint_fast64_t stats_callback_time_sum_us;
    atomic_uint_fast64_t stats_callback_time_max_us;
    atomic_uint_fast64_t stats_track_changes;
    atomic_uint_fast64_t stats_instant_advances;

    atomic_bool system_active;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-Player Spectrum Init/Destroy
 *
 * Initialize/destroy the cava plan + buffers embedded in audio_player_t.
 * FFT processing runs inline in the PipeWire monitor callback.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Initialize a player's spectrum state (cava plan + accumulation buffers).
 * Called from player_init().
 */
quadrature_result_t spectrum_init(spectrum_state_t *s, int num_bars, int sample_rate);

/**
 * Destroy a player's spectrum state.
 * Called from player_destroy().
 */
void spectrum_cleanup(spectrum_state_t *s);

/**
 * Process audio samples through cavacore FFT.
 * Called from the PipeWire monitor callback.
 * Writes results to player->spectrum_bars[] atomically.
 *
 * @param s       Per-player spectrum state
 * @param in      Interleaved stereo float samples
 * @param frames  Number of frames (samples / 2)
 * @param bars    Atomic spectrum_bars array to write results
 */
void spectrum_set_refresh_hz(spectrum_state_t *s, double hz);

void spectrum_process(spectrum_state_t *s,
                      const float *in,
                      uint32_t frames,
                      _Atomic float *bars,
                      _Atomic uint32_t *generation);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Device Enumeration API
 *
 * Enumerate available PipeWire audio sinks for device routing.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Audio output device information */
typedef struct {
    char node_name[256];   /* For PW_KEY_TARGET_OBJECT */
    char description[256]; /* Human-readable name for UI */
    char serial[64];       /* object.serial for stable identification */
    uint32_t id;           /* PipeWire node ID */
} audio_device_t;

/* List of available audio devices */
typedef struct {
    audio_device_t *devices;
    int count;
    int capacity;
} audio_device_list_t;

/**
 * Enumerate available PipeWire audio sinks.
 * The pipeline must be started before calling this function.
 * Caller must free the list with audio_devices_free().
 */
quadrature_result_t audio_devices_enumerate(audio_pipeline_t *pipeline, audio_device_list_t *list);

/**
 * Free device list resources.
 */
void audio_devices_free(audio_device_list_t *list);

/**
 * Start a persistent PipeWire registry monitor that calls cb(user_data) whenever
 * an Audio/Sink node is added or removed.  cb is invoked on the PipeWire thread —
 * callers must marshal to the GTK main thread themselves (e.g. g_main_context_invoke).
 *
 * Uses pw_core_sync to suppress the initial population burst so that only genuine
 * topology changes after startup trigger the callback.
 *
 * Safe to call multiple times — subsequent calls are no-ops if already started.
 * The pipeline must be started before calling.
 */
quadrature_result_t audio_devices_monitor_start(audio_pipeline_t *pipeline,
                                                void (*cb)(void *user_data),
                                                void *user_data);

/**
 * Stop the device monitor and release its PipeWire resources.
 * Safe to call if the monitor was never started.
 * Must be called before audio_pipeline_destroy.
 */
void audio_devices_monitor_stop(audio_pipeline_t *pipeline);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Cache API (Internal)
 *
 * Thread-safe LRU cache for fully decoded audio buffers.
 * Uses track_id as key with LibraryCache for path resolution.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Cache memory limit: 1GB for DEBUG/BROADCAST modes (configurable via env) */
#define AUDIO_CACHE_DEFAULT_MEMORY_LIMIT (1024 * 1024 * 1024)

/* Maximum concurrent decode tasks */
#define AUDIO_CACHE_MAX_DECODE_WORKERS 4

/* Delayed unlock: derived from audio quantum, not hardcoded.
 * Formula: 3 × (quantum_frames / sample_rate × 1000).
 * At 48kHz/256 frames = ~16ms; at 48kHz/512 = ~32ms; at 48kHz/4096 = ~256ms.
 * The 3× multiplier provides margin for scheduling jitter.
 * Minimum clamped to 16ms to avoid sub-timer-tick unlocks. */
#define AUDIO_CACHE_UNLOCK_DELAY_MIN_MS 16
static inline uint32_t
audio_cache_compute_unlock_delay(uint32_t quantum_frames, uint32_t sample_rate)
{
    uint32_t period_ms = (quantum_frames * 1000 + sample_rate - 1) / sample_rate;
    uint32_t delay = period_ms * 3;
    return delay < AUDIO_CACHE_UNLOCK_DELAY_MIN_MS ? AUDIO_CACHE_UNLOCK_DELAY_MIN_MS : delay;
}

/* Cache Status */
typedef enum {
    AUDIO_CACHE_NOT_FOUND, /* Track ID not in cache */
    AUDIO_CACHE_LOADING,   /* Decode in progress */
    AUDIO_CACHE_READY,     /* Decode complete, buffer available */
    AUDIO_CACHE_FAILED     /* Decode failed */
} audio_cache_status_t;

/* Decode event ring buffer settings */
#define AUDIO_CACHE_MAX_DECODE_EVENTS 100

/* Raw decode event stored in ring buffer */
typedef struct {
    int64_t track_id;
    uint64_t file_size;          /* File size in bytes */
    uint32_t audio_duration_ms;  /* Track length in milliseconds */
    char filetype[8];            /* File extension (e.g., "mp3", "flac") */
    uint32_t decode_duration_ms; /* How long decode took */
    uint64_t timestamp_ms;       /* When load() was called (monotonic) */
} audio_cache_decode_event_t;

/* Cache Lifecycle */
quadrature_result_t
audio_cache_create(library_cache_t *library, uint32_t sample_rate, audio_cache_t **out);
void audio_cache_destroy(audio_cache_t *cache);

/* Loading API (Background Decoding) */
quadrature_result_t audio_cache_load(audio_cache_t *cache, int64_t track_id);
void audio_cache_cancel_load(audio_cache_t *cache, int64_t track_id);
void audio_cache_cancel_all_loads(audio_cache_t *cache);

/* Lock result for audio_cache_lock() */
typedef enum {
    AUDIO_CACHE_LOCK_READY,   /* Buffer available now */
    AUDIO_CACHE_LOCK_LOADING, /* Decode in progress, call wait_ready() */
    AUDIO_CACHE_LOCK_FAILED   /* Decode failed or track not found */
} audio_cache_lock_result_t;

/* Lock/Unlock API (Eviction Protection) */
audio_cache_lock_result_t audio_cache_lock(audio_cache_t *cache, int64_t track_id);
void audio_cache_unlock(audio_cache_t *cache, int64_t track_id);
void audio_cache_unlock_delayed(audio_cache_t *cache, int64_t track_id);
void audio_cache_set_quantum(audio_cache_t *cache, uint32_t quantum_frames);

/* Buffer Access (For Locked Tracks) */
audio_buffer_t *audio_cache_get_locked(audio_cache_t *cache, int64_t track_id);

/* Status Query */
audio_cache_status_t audio_cache_get_status(audio_cache_t *cache, int64_t track_id);

/* Buffer Accessors */
int64_t audio_buffer_get_track_id(const audio_buffer_t *buf);
const float *audio_buffer_get_samples(const audio_buffer_t *buf);
uint64_t audio_buffer_get_num_frames(const audio_buffer_t *buf);
const float *audio_buffer_get_loudness(const audio_buffer_t *buf);
bool audio_buffer_is_loudness_ready(const audio_buffer_t *buf);

/* Cache Management */
/** Evict unlocked buffers not accessed within max_age_us microseconds. */
void audio_cache_sweep_stale(audio_cache_t *cache, int64_t max_age_us);
size_t audio_cache_get_memory_used(audio_cache_t *cache);

/* Statistics */
uint32_t audio_cache_get_decode_events(audio_cache_t *cache,
                                       audio_cache_decode_event_t *out_events,
                                       uint32_t max_events);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline Internal Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Manually trigger stream reconnect for a player in error state.
 * Safe to call from main thread; schedules reconnect on next idle.
 */
void audio_pipeline_player_reconnect(audio_pipeline_t *pipeline, int player_id);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_INTERNAL_H */
