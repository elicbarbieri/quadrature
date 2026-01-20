/**
 * Quadrature Audio Subsystem - Internal Header
 *
 * Private types, shared helpers, and constants for the audio subsystem.
 * This header should NOT be included by code outside src/audio/.
 */

#ifndef QUADRATURE_AUDIO_INTERNAL_H
#define QUADRATURE_AUDIO_INTERNAL_H

#include "quadrature/core/types.h"
#include "quadrature/core/perf_dashboard.h"
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct audio_player audio_player_t;
typedef struct audio_pipeline audio_pipeline_t;
typedef struct audio_buffer_store audio_buffer_store_t;
typedef struct audio_buffer audio_buffer_t;
typedef struct spectrum_channel spectrum_channel_t;
typedef struct spectrum_analyzer spectrum_analyzer_t;
typedef struct audio_scrubber audio_scrubber_t;

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
 * Low-level FFmpeg decode logic used exclusively by audio_buffer_store.
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
 * Time Utility (UI thread only - NOT for RT callback)
 *
 * Note: time_ms() uses clock_gettime() which is a system call. Do NOT call
 * from the real-time audio callback. Use sample-based timing instead.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
    atomic_int state;  /* CHANNEL_STOPPED, CHANNEL_LOADING, CHANNEL_PLAYING, CHANNEL_PAUSED, CHANNEL_ERROR */
    atomic_uint_fast64_t position_samples;
    atomic_uint_fast64_t length_samples;
    atomic_bool repeat;

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

    /* Track info */
    char filepath[MAX_FILENAME_LENGTH];

    /* Metering (atomic, updated per callback) */
    _Atomic float peak_left;
    _Atomic float peak_right;
    _Atomic float rms_left;
    _Atomic float rms_right;
    _Atomic float peak_hold_left;
    _Atomic float peak_hold_right;
    atomic_uint_fast64_t peak_hold_age_frames;  /* Frames since last peak (sample-based timing) */

    /* Spectrum analyzer data */
    _Atomic float spectrum_bars[SPECTRUM_BARS];
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

    /* Buffer store (shared by all players) */
    audio_buffer_store_t* store;

    /* Performance dashboard (optional) */
    perf_dashboard_t* perf;

    atomic_bool system_active;
};

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_INTERNAL_H */
