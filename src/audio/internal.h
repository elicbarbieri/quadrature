/**
 * Quadrature Audio Subsystem - Internal Header
 *
 * Private types, shared helpers, and constants for the audio subsystem.
 * This header should NOT be included by code outside src/audio/.
 */

#ifndef QUADRATURE_AUDIO_INTERNAL_H
#define QUADRATURE_AUDIO_INTERNAL_H

#include "quadrature/quadrature.h"
#include "../core/internal.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>

/* FFmpeg headers */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

/* PipeWire headers */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>

/* Note: FFmpeg is still used for decoding, but scrubbing now uses rubberband */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Consolidated Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DECODE_BUFFER_FRAMES      4096
#define SPECTRUM_RINGBUF_SIZE     16384
#define SPECTRUM_BARS             24
#define FFT_SAMPLES               2048
#define SPECTRUM_UPDATE_INTERVAL_US 16000
#define PEAK_HOLD_DECAY_MS        2000
#define SCRUB_SPEED_MULTIPLIER    3
#define DECAY_FACTOR              0.85f
#define MAX_AUDIO_PLAYERS         4
#define MAX_FILENAME_LENGTH       512

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct audio_player audio_player_t;
typedef struct audio_pipeline audio_pipeline_t;
typedef struct audio_cache audio_cache_t;
typedef struct audio_buffer audio_buffer_t;
typedef struct spectrum_channel spectrum_channel_t;
typedef struct spectrum_analyzer spectrum_analyzer_t;
typedef struct audio_scrubber audio_scrubber_t;
typedef struct library_cache library_cache_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Spectrum Analyzer (Full Definition)
 *
 * Per-channel spectrum analyzer state using cavacore FFT.
 * Types are opaque in public header; full definition here for implementation.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <pthread.h>

struct cava_plan;  /* Forward declaration from cavacore */

struct spectrum_channel {
    struct cava_plan* plan;
    double* input_buffer;
    double* output_bars;
    float* read_buffer;      /* Per-channel buffer for reading from ring buffer */
    size_t input_buffer_size;
    size_t input_buffer_fill;
};

struct spectrum_analyzer {
    spectrum_channel_t channels[4];
    int num_channels;
    int num_bars;
    int sample_rate;

    pthread_t thread;
    atomic_bool running;

    /* Pointer to players array (uses void* to avoid circular include) */
    void* players;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Decoder Helper
 *
 * Low-level FFmpeg decode logic used by audio_cache.
 * All audio is fully decoded to PCM buffers before playback - no streaming.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    SwrContext* swr_ctx;
    AVFrame* frame;
    AVPacket* packet;
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
quadrature_result_t ffmpeg_decoder_open(ffmpeg_decoder_t* dec, const char* path, uint32_t rate);

/**
 * Read decoded frames into buffer.
 *
 * @param dec         Decoder state
 * @param buffer      Output buffer (stereo interleaved float)
 * @param max_frames  Maximum frames to read
 * @return Number of frames read, 0 on EOF, -1 on error
 */
int ffmpeg_decoder_read(ffmpeg_decoder_t* dec, float* buffer, size_t max_frames);

/**
 * Seek to a position in samples.
 *
 * @param dec       Decoder state
 * @param position  Target position in samples (at output sample rate)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t ffmpeg_decoder_seek(ffmpeg_decoder_t* dec, uint64_t position);

/**
 * Flush decoder buffers (call after seek).
 */
void ffmpeg_decoder_flush(ffmpeg_decoder_t* dec);

/**
 * Close decoder and free resources.
 */
void ffmpeg_decoder_close(ffmpeg_decoder_t* dec);

/**
 * Get total duration in frames (at output sample rate).
 */
uint64_t ffmpeg_decoder_duration(ffmpeg_decoder_t* dec);

/* ═══════════════════════════════════════════════════════════════════════════
 * Metering Accumulator
 *
 * Accumulates peak and RMS values during audio processing.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    float peak_left;
    float peak_right;
    float sum_sq_left;
    float sum_sq_right;
    size_t frame_count;
} meter_accum_t;

static inline void meter_accum_init(meter_accum_t* m) {
    m->peak_left = 0.0f;
    m->peak_right = 0.0f;
    m->sum_sq_left = 0.0f;
    m->sum_sq_right = 0.0f;
    m->frame_count = 0;
}

static inline void meter_accum_process(meter_accum_t* m, float left, float right) {
    float abs_l = fabsf(left);
    float abs_r = fabsf(right);
    if (abs_l > m->peak_left) m->peak_left = abs_l;
    if (abs_r > m->peak_right) m->peak_right = abs_r;
    m->sum_sq_left += left * left;
    m->sum_sq_right += right * right;
    m->frame_count++;
}

/* Store metering values to player atomics - implemented in audio_pipeline.c */
void meter_accum_store(meter_accum_t* m, audio_player_t* player);

/* ═══════════════════════════════════════════════════════════════════════════
 * Time Utilities
 *
 * clock_gettime(CLOCK_MONOTONIC) is VDSO-mapped on Linux — executes entirely
 * in userspace with no kernel transition (~20ns). Same mechanism used by
 * PipeWire, JACK, and Ardour in their RT paths.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Nanosecond-precision monotonic timer for RT callback profiling.
 */
static inline uint64_t time_ns(void) {
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

#define LIMITER_THRESHOLD 0.8f   /* Start soft knee at 80% */
#define LIMITER_KNEE      0.2f   /* Knee width */

static inline float soft_limit(float x) {
    float abs_x = fabsf(x);

    /* Below threshold: pass through */
    if (abs_x <= LIMITER_THRESHOLD) {
        return x;
    }

    /* Soft knee region: gradual compression */
    float sign = (x >= 0.0f) ? 1.0f : -1.0f;
    float overshoot = abs_x - LIMITER_THRESHOLD;

    /* Attempt soft compression first */
    if (overshoot < 1.0f) {
        /* Soft knee: asymptotic approach to 1.0 */
        float compressed = LIMITER_THRESHOLD + (1.0f - LIMITER_THRESHOLD) * (overshoot / (overshoot + LIMITER_KNEE));
        return sign * compressed;
    }

    /* Hard saturation for extreme values (fast tanh approximation) */
    if (abs_x > 2.0f) return sign * 1.0f;
    float x2 = abs_x * abs_x;
    return sign * (abs_x * (27.0f + x2) / (27.0f + 9.0f * x2));
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
quadrature_result_t audio_scrubber_create(uint32_t sample_rate, audio_scrubber_t** out);
void audio_scrubber_destroy(audio_scrubber_t* s);

/* Control API (UI thread, atomic-safe) */
void audio_scrubber_set_speed(audio_scrubber_t* s, float speed);  /* -4.0 to +4.0 */
void audio_scrubber_set_position(audio_scrubber_t* s, int64_t position);
void audio_scrubber_set_shuttle_mode(audio_scrubber_t* s, shuttle_mode_t mode);

/* Query API (thread-safe) */
float audio_scrubber_get_speed(const audio_scrubber_t* s);
int64_t audio_scrubber_get_position(const audio_scrubber_t* s);
shuttle_mode_t audio_scrubber_get_shuttle_mode(const audio_scrubber_t* s);

/* Stats query (thread-safe) */
uint64_t audio_scrubber_get_underflows(const audio_scrubber_t* s);

/* Audio thread - unified playback at variable speed */
uint32_t audio_scrubber_process(audio_scrubber_t* s,
                                 const float* samples, uint64_t num_frames,
                                 float* output, uint32_t frames,
                                 uint64_t* out_position);

/**
 * Flush all internal scrubber buffers.
 * Call after seek to prevent stale audio from playing.
 */
void audio_scrubber_flush(audio_scrubber_t* s);

/* ═══════════════════════════════════════════════════════════════════════════
 * Position Snapshot (Seqlock Pattern)
 *
 * Captures position + timestamp + speed + state atomically in audio thread.
 * UI thread reads with retry for smooth interpolation between updates.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t position;       /* Position in samples */
    uint64_t sample_count;   /* Callback sample counter (RT-safe, replaces timestamp_us) */
    float    speed;          /* Playback speed (-4.0 to +4.0) */
    uint8_t  playing;        /* 1 if playing, 0 otherwise */
    uint8_t  _pad[3];        /* Alignment padding */
} position_snapshot_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Player (Internal Definition)
 *
 * Individual audio player with buffer-based playback and PipeWire output.
 * All playback happens from decoded PCM buffers - no streaming fallback.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_player {
    int player_id;
    audio_pipeline_t* pipeline;  /* Back-reference for perf access */

    /* Playback state (atomic for real-time safety) */
    atomic_int state;  /* CHANNEL_STOPPED, CHANNEL_PLAYING, CHANNEL_PAUSED, CHANNEL_ERROR */
    atomic_uint_fast64_t position_samples;
    atomic_uint_fast64_t length_samples;
    atomic_bool repeat;
    atomic_bool autoplay;  /* Continue playing on track advance (default: true) */

    /* Track ID-based playback (replaces filepath) */
    atomic_int_fast64_t current_track_id;   /* Currently playing track */
    atomic_int_fast64_t next_track_id;      /* Preloaded for instant advance */
    _Atomic(audio_buffer_t*) next_buffer;   /* Preloaded buffer */
    atomic_bool advance_pending;             /* Signal for deferred cleanup */
    atomic_int_fast64_t advance_old_track_id;
    atomic_int_fast64_t pending_buffer_track_id;  /* Track awaiting decode completion */

    /* Buffer playback (ALWAYS present when ready to play) */
    /* Atomic for thread-safe access from audio callback */
    _Atomic(audio_buffer_t*) buffer;
    uint64_t current_frame;

    /* Rate processor for variable-speed playback (ALWAYS attached) */
    audio_scrubber_t* scrubber;

    /* Position snapshot for smooth UI interpolation (seqlock pattern) */
    atomic_uint position_seq;           /* Seqlock sequence (odd = writing) */
    position_snapshot_t position_snap;  /* Snapshot data */

    /* Sample counter for RT-safe timestamps (incremented by callback) */
    atomic_uint_fast64_t callback_sample_count;

    /* Track info (kept for backward compatibility during transition) */
    char filepath[MAX_FILENAME_LENGTH];

    /* Metering (atomic, updated per callback) */
    _Atomic float peak_left;
    _Atomic float peak_right;
    _Atomic float rms_left;
    _Atomic float rms_right;
    _Atomic float peak_hold_left;
    _Atomic float peak_hold_right;
    atomic_uint_fast64_t peak_hold_age_frames;  /* Frames since last peak (sample-based timing) */

    /* Per-player RT stats (atomic — written in callback, read from UI thread) */
    atomic_uint_fast64_t stats_cb_count;            /* Internal: for computing averages */
    atomic_uint_fast64_t stats_cb_time_sum_ns;      /* Cumulative processing time */
    atomic_uint_fast64_t stats_cb_time_max_ns;      /* Peak processing time */
    atomic_uint_fast64_t stats_budget_overruns;      /* Callbacks exceeding 50% of period budget */
    atomic_uint_fast64_t stats_dequeue_failures;     /* pw_stream_dequeue_buffer returned NULL */
    atomic_uint_fast64_t stats_deferred_advances;    /* Silence gap — no preloaded buffer */
    atomic_uint_fast64_t stats_instant_advances;     /* Gapless buffer swap in RT callback */

    /* Spectrum analyzer data (stereo: 0..SPECTRUM_BARS-1 = left, SPECTRUM_BARS..2*SPECTRUM_BARS-1 = right) */
    _Atomic float spectrum_bars[SPECTRUM_BARS * 2];
    struct spa_ringbuffer spectrum_rb;
    float* spectrum_buffer;

    /* PipeWire stream */
    struct pw_stream* stream;
    char target_device[256];
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline (Internal Definition)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_pipeline {
    audio_player_t players[MAX_AUDIO_PLAYERS];
    uint32_t sample_rate;

    /* PipeWire shared context */
    struct pw_thread_loop* loop;
    struct pw_context* pw_ctx;
    struct pw_core* core;

    /* Spectrum analyzer (shared, processes all players) */
    spectrum_analyzer_t* spectrum;

    /* Audio cache (shared by all players) - track_id based */
    audio_cache_t* cache;

    /* Library cache (for track_id -> path resolution, next/prev navigation) */
    library_cache_t* library;

    /* Track changed callback */
    void (*track_changed_callback)(int player_id, int64_t track_id, void* user_data);
    void* track_changed_user_data;

    /* Auto-advance timeout (runs on main thread) */
    guint advance_timeout_id;

    /* Performance dashboard (optional, for detailed timing) */
    perf_dashboard_t* perf;

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
 * Spectrum Analyzer API
 *
 * Runs a background thread that reads samples from each player's ring buffer,
 * processes through cavacore FFT, and writes spectrum bars to player structs.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create and start a spectrum analyzer.
 *
 * @param num_bars Number of frequency bars (1-64, typically 24)
 * @param sample_rate Audio sample rate (e.g., 48000)
 * @param num_channels Number of players to analyze (1-4)
 * @param players Pointer to audio_player_t array (must outlive analyzer)
 * @return New spectrum analyzer, or NULL on failure
 */
spectrum_analyzer_t* spectrum_create(int num_bars, int sample_rate, int num_channels,
                                     void* players);

/**
 * Stop and destroy a spectrum analyzer.
 */
void spectrum_destroy(spectrum_analyzer_t* s);

/**
 * Check if the spectrum analyzer is running.
 */
bool spectrum_is_running(spectrum_analyzer_t* s);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Device Enumeration API
 *
 * Enumerate available PipeWire audio sinks for device routing.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Audio output device information */
typedef struct {
    char node_name[256];      /* For PW_KEY_TARGET_OBJECT */
    char description[256];    /* Human-readable name for UI */
    char serial[64];          /* object.serial for stable identification */
    uint32_t id;              /* PipeWire node ID */
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

/* Delayed unlock timeout in milliseconds */
#define AUDIO_CACHE_UNLOCK_DELAY_MS 200

/* Cache Status */
typedef enum {
    AUDIO_CACHE_NOT_FOUND,  /* Track ID not in cache */
    AUDIO_CACHE_LOADING,    /* Decode in progress */
    AUDIO_CACHE_READY,      /* Decode complete, buffer available */
    AUDIO_CACHE_FAILED      /* Decode failed */
} audio_cache_status_t;

/* Decode event ring buffer settings */
#define AUDIO_CACHE_MAX_DECODE_EVENTS 100

/* Raw decode event stored in ring buffer */
typedef struct {
    int64_t track_id;
    uint64_t file_size;           /* File size in bytes */
    uint32_t audio_duration_ms;   /* Track length in milliseconds */
    char filetype[8];             /* File extension (e.g., "mp3", "flac") */
    uint32_t decode_duration_ms;  /* How long decode took */
    uint64_t timestamp_ms;        /* When load() was called (monotonic) */
} audio_cache_decode_event_t;

/* Cache Statistics (simple metrics, histogram computed externally from events) */
typedef struct {
    float memory_usage_pct;           /* (used / limit) * 100 */
    uint32_t cached_buffer_seconds;   /* Total seconds of audio in decoded buffers */
    uint32_t prefetch_tracks;         /* Total tracks passed to audio_cache_prefetch() */
    uint32_t event_count;             /* Number of events in ring buffer */
} audio_cache_stats_t;

/* Cache Lifecycle */
quadrature_result_t audio_cache_create(library_cache_t* library,
                                        uint32_t sample_rate,
                                        audio_cache_t** out);
void audio_cache_destroy(audio_cache_t* cache);

/* Loading API (Background Decoding) */
quadrature_result_t audio_cache_load(audio_cache_t* cache, int64_t track_id);
void audio_cache_cancel_load(audio_cache_t* cache, int64_t track_id);
void audio_cache_cancel_all_loads(audio_cache_t* cache);

/* Lock result for audio_cache_lock() */
typedef enum {
    AUDIO_CACHE_LOCK_READY,    /* Buffer available now */
    AUDIO_CACHE_LOCK_LOADING,  /* Decode in progress, call wait_ready() */
    AUDIO_CACHE_LOCK_FAILED    /* Decode failed or track not found */
} audio_cache_lock_result_t;

/* Lock/Unlock API (Eviction Protection) */
audio_cache_lock_result_t audio_cache_lock(audio_cache_t* cache, int64_t track_id);
void audio_cache_unlock(audio_cache_t* cache, int64_t track_id);
void audio_cache_unlock_delayed(audio_cache_t* cache, int64_t track_id);

/* Wait for decode completion (blocks until ready or timeout) */
bool audio_cache_wait_ready(audio_cache_t* cache, int64_t track_id, int64_t timeout_ms);

/* Buffer Access (For Locked Tracks) */
audio_buffer_t* audio_cache_get_locked(audio_cache_t* cache, int64_t track_id);

/* Prefetch API */
void audio_cache_prefetch(audio_cache_t* cache,
                          const int64_t* track_ids,
                          size_t count);

/* Status Query */
audio_cache_status_t audio_cache_get_status(audio_cache_t* cache, int64_t track_id);

/* Buffer Accessors */
int64_t audio_buffer_get_track_id(const audio_buffer_t* buf);
const float* audio_buffer_get_samples(const audio_buffer_t* buf);
uint64_t audio_buffer_get_num_frames(const audio_buffer_t* buf);
uint32_t audio_buffer_get_sample_rate(const audio_buffer_t* buf);

/* Cache Management */
void audio_cache_evict(audio_cache_t* cache, int64_t track_id);
void audio_cache_clear(audio_cache_t* cache);
void audio_cache_set_memory_limit(audio_cache_t* cache, size_t memory_limit);
size_t audio_cache_get_memory_used(audio_cache_t* cache);
size_t audio_cache_get_count(audio_cache_t* cache);

/* Statistics */
void audio_cache_get_stats(audio_cache_t* cache, audio_cache_stats_t* stats);
uint32_t audio_cache_get_decode_events(audio_cache_t* cache,
                                        audio_cache_decode_event_t* out_events,
                                        uint32_t max_events);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline Internal Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get the performance dashboard for the pipeline (internal API).
 * Used by perf view for detailed timing histograms.
 *
 * @param pipeline  Pipeline instance
 * @return Performance dashboard (may be NULL if not enabled)
 */
perf_dashboard_t* audio_pipeline_get_perf_dashboard(audio_pipeline_t* pipeline);

/**
 * Get the audio cache for the pipeline (internal API).
 * Used by perf view for decode metrics.
 *
 * @param pipeline  Pipeline instance
 * @return Audio cache (may be NULL)
 */
audio_cache_t* audio_pipeline_get_audio_cache(audio_pipeline_t* pipeline);

/**
 * Load an audio file for playback by path (legacy internal API).
 *
 * @deprecated Use audio_pipeline_set_player_track() with track ID instead.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param path       Path to audio file
 * @return QUADRATURE_OK on success (load started or cache hit)
 */
quadrature_result_t audio_pipeline_player_load(audio_pipeline_t* pipeline,
                                                int player_id,
                                                const char* path);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_INTERNAL_H */
