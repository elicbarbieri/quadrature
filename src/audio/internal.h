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

/* Note: FFmpeg is still used for decoding, but shuttling now uses rubberband */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Consolidated Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Budget ring buffer capacity: 10-min @ ~10ms intervals = 60,000 entries */
#define BUDGET_RB_CAPACITY 65536 /* must be power-of-2 */

#define WAVEFORM_RMS_BINS  1024
#define SPECTRUM_BARS      24
#define FFT_SAMPLES        2048
#define MAX_AUDIO_PLAYERS  4

/* Telemetry ring buffers (budget/latency/interval) sample on this cadence —
 * one entry every ~10ms while audio is streaming. */
#define RINGBUF_SAMPLE_INTERVAL_NS 10000000ULL /* 10 ms */

/* Playback speed below this magnitude is treated as stopped: the shuttle
 * engine emits silence and the player is considered not actively playing for
 * UI purposes (no smooth-position interpolation). */
#define SPEED_STOPPED_EPSILON 0.01f

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct audio_player audio_player_t;
typedef struct audio_pipeline audio_pipeline_t;
typedef struct audio_cache audio_cache_t;
typedef struct audio_buffer audio_buffer_t;
typedef struct audio_shuttle_speed audio_shuttle_speed_t;
typedef struct library_cache library_cache_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Format
 *
 * Single source of truth for the wire format of PCM audio flowing through
 * the pipeline. All sample buffers are interleaved float32; layout is
 * (sample_rate, channels). Replaces hardcoded "2" channel constants —
 * makes 5.1 / 7.1 / 7.1.4-bed (Atmos) a configuration change rather than a
 * codebase grep.
 *
 * Invariant: player.format == shuttle_speed.format == buffer.format whenever a
 * buffer is loaded into a player. Mismatch is an assertion failure today;
 * a future conversion stage will replace the assert with a downmix/upmix.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t sample_rate; /* Hz */
    uint32_t channels;    /* 1 (mono), 2 (stereo), 6 (5.1), 8 (7.1), 12 (7.1.4) ... */
} audio_format_t;

static inline size_t
audio_format_bytes_per_frame(const audio_format_t *f)
{
    return (size_t)f->channels * sizeof(float);
}

static inline size_t
audio_format_samples_per_frame(const audio_format_t *f)
{
    return (size_t)f->channels;
}

static inline bool
audio_format_equal(const audio_format_t *a, const audio_format_t *b)
{
    return a->sample_rate == b->sample_rate && a->channels == b->channels;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Telemetry Ring Buffers
 *
 * Lock-free SPSC rings: the audio thread pushes one sample per ~10ms window via
 * TELEMETRY_RING_PUSH (telemetry.c); the UI thread snapshots via
 * telemetry_ring_window (below). All three share the same shape — a typed
 * sample array, an audio-thread-only last_write_ns, and a release-published
 * write_pos — so one macro stamps out the type. Capacity must be power-of-2.
 *
 *   budget_rb_t   — per-callback budget utilization, centipercent (0-10000).
 *   latency_rb_t  — per-callback processing latency, µs (capped at 65535).
 *   interval_rb_t — peak |scheduling deviation| within the window, raw ns (>=0).
 *                   Unit scaling (ns → µs) is left to the reader (perf UI).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LATENCY_RB_CAPACITY  65536 /* must be power-of-2 */
#define INTERVAL_RB_CAPACITY 8192  /* must be power-of-2; ~82s at 10ms */

#define TELEMETRY_RING_TYPE(Name, ElemType, Capacity)                   \
    typedef struct {                                                    \
        ElemType samples[Capacity];                                     \
        uint64_t last_write_ns; /* audio-thread-only; no sync needed */ \
        atomic_uint write_pos;  /* release-store after each write */    \
    } Name

TELEMETRY_RING_TYPE(budget_rb_t, uint16_t, BUDGET_RB_CAPACITY);
TELEMETRY_RING_TYPE(latency_rb_t, uint16_t, LATENCY_RB_CAPACITY);
TELEMETRY_RING_TYPE(interval_rb_t, int64_t, INTERVAL_RB_CAPACITY);

/* Snapshot the readable window of a telemetry ring. Given the acquire-loaded
 * write_pos, returns the most recent min(write_pos, capacity, max_count) samples
 * and writes their masked start index to *out_start. Capacity must be power-of-2;
 * callers index `samples[(start + i) & (capacity - 1)]`. */
static inline uint32_t
telemetry_ring_window(uint32_t write_pos,
                      uint32_t capacity,
                      uint32_t max_count,
                      uint32_t *out_start)
{
    uint32_t avail = write_pos < capacity ? write_pos : capacity;
    uint32_t count = avail < max_count ? avail : max_count;
    *out_start = write_pos - count;
    return count;
}

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
    uint32_t sample_rate;      /* Cached for threshold recomputation; matches audio_format_t */
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
    double time_base;       /* stream time_base, for duration() */
    int output_sample_rate; /* target rate; swr resamples to this */
} ffmpeg_decoder_t;

/**
 * Decoded-source metadata, all from ffmpeg (ground truth — never the file
 * extension). Populated from the open decoder; copy out before close.
 */
typedef struct {
    char codec_name[16];      /* e.g. "flac", "aac", "alac", "mp3", "pcm_s24le" */
    uint64_t duration_frames; /* total length in frames at the OUTPUT sample rate */
    uint32_t sample_rate;     /* source sample rate, Hz (pre-resample) */
    uint32_t channels;        /* source channel count (pre-downmix) */
    int64_t bit_rate;         /* source bitrate, bits/sec; 0 if unknown */
    uint32_t bit_depth;       /* source bits per sample; 0 if compressed/unknown */
} ffmpeg_decoder_metadata_t;

/**
 * Open an audio file for decoding.
 *
 * @param dec       Decoder state (zero-initialized)
 * @param path      Path to audio file
 * @param rate      Target output sample rate
 * @param channels  Target output channel count (output is interleaved float32
 *                  in the default layout for this count; source is down/upmixed)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t
ffmpeg_decoder_open(ffmpeg_decoder_t *dec, const char *path, uint32_t rate, uint32_t channels);

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
 * Close decoder and free resources.
 */
void ffmpeg_decoder_close(ffmpeg_decoder_t *dec);

/**
 * Read all source metadata (codec, duration, rate, channels, bitrate, depth)
 * from the open decoder. Returns by value; valid for an open decoder only.
 */
ffmpeg_decoder_metadata_t ffmpeg_decoder_metadata(const ffmpeg_decoder_t *dec);

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
 * Audio Shuttle Speed (Variable-Speed Playback Engine)
 *
 * shuttle_mode selects the DSP path:
 *   - OFF: direct memcpy, zero CPU
 *   - PITCHED: cubic-hermite interp; pitch shifts with speed (turntable)
 *   - KEYLOCK: librubberband time-stretch; pitch preserved
 *
 * A short crossfade hides clicks when the mode changes. The playhead lives in
 * audio_seek_position_t (below), passed in per process() call.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * audio_seek_position_t — the playhead
 *
 * Atomic int64 wrapped in a struct so the API is unambiguous. UI thread can
 * seek and read it directly. audio_shuttle_speed_process() reads it at start
 * and writes it back as it consumes samples — the engine is one consumer of
 * the playhead, not its owner.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    atomic_uint_fast64_t value; /* Source-frame units; matches num_frames / length_samples. */
} audio_seek_position_t;

static inline void
audio_seek_position_init(audio_seek_position_t *p, uint64_t initial)
{
    atomic_store(&p->value, initial);
}

static inline uint64_t
audio_seek_position_get(const audio_seek_position_t *p)
{
    return atomic_load(&p->value);
}

static inline void
audio_seek_position_set(audio_seek_position_t *p, uint64_t v)
{
    atomic_store(&p->value, v);
}

/* Lifecycle */
quadrature_result_t audio_shuttle_speed_create(audio_format_t format, audio_shuttle_speed_t **out);
void audio_shuttle_speed_destroy(audio_shuttle_speed_t *s);

/* Control API (atomic stores; no allocation, no DSP setup) */
void audio_shuttle_speed_set_speed(audio_shuttle_speed_t *s, float speed); /* -4.0 to +4.0 */
void audio_shuttle_speed_set_mode(audio_shuttle_speed_t *s, shuttle_mode_t mode);

/* Allocate/prime DSP resources for a mode. UI/main thread only — may malloc.
 * Idempotent. Currently only SHUTTLE_MODE_KEYLOCK needs preparation
 * (rubberband state); other modes are a no-op. Caller MUST invoke this
 * before set_mode(mode) to avoid silence on the RT path. */
void audio_shuttle_speed_prepare_mode(audio_shuttle_speed_t *s, shuttle_mode_t mode);

/* Query API (thread-safe) */
float audio_shuttle_speed_get_speed(const audio_shuttle_speed_t *s);
shuttle_mode_t audio_shuttle_speed_get_mode(const audio_shuttle_speed_t *s);

/* Stats query (thread-safe) */
uint64_t audio_shuttle_speed_get_underflows(const audio_shuttle_speed_t *s);

/*
 * Audio thread — unified playback at variable speed.
 *
 * The playhead is read from `*pos` at entry and written back as the engine
 * consumes samples (one atomic store at end). External readers (UI seek bar)
 * can `audio_seek_position_get(pos)` at any time.
 */
uint32_t audio_shuttle_speed_process(audio_shuttle_speed_t *s,
                                     audio_seek_position_t *pos,
                                     const float *samples,
                                     uint64_t num_frames,
                                     float *output,
                                     uint32_t frames);

/**
 * Flush all internal shuttle_speed buffers.
 * Call after seek to prevent stale audio from playing. Fractional position is
 * resynced on the next process() call via its mismatch detection — flush only
 * needs to clear DSP scratch state (ring buffers, crossfade, zone).
 */
void audio_shuttle_speed_flush(audio_shuttle_speed_t *s);

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
 *
 * ─── Threading model ──────────────────────────────────────────────────────
 *
 *   RT thread (PipeWire callback)         — on_process(), on_monitor_process()
 *   UI thread (GTK main loop)             — set_player_track, play/pause, seek
 *   Worker pool                           — audio cache decode threads
 *
 * Cross-thread coordination uses C11 atomics with deliberate memory ordering.
 * Categories of atomic fields below:
 *
 *   • Counters / stats (stats_*, callback_sample_count, length_samples,
 *     stream_generation) — relaxed everywhere. No ordering relation to other
 *     data; UI reads them as "best effort current value."
 *
 *   • Identifiers (current_track_id, next_track_id, end_mode,
 *     advance_old_track_id, pending_buffer_track_id) — relaxed in principle.
 *     Some sites still use default seq_cst out of legacy; on x86 this is the
 *     same `mov`, on ARM a fractionally heavier `ldar`. Not worth churn.
 *
 *   • Publication pointers (buffer, next_buffer) — release on store, acquire
 *     on load. The buffer's contents (samples, num_frames, format) must be
 *     visible the moment the RT thread observes the pointer.
 *
 *   • State machine (state) — managed via CAS for transitions; default seq_cst
 *     is retained because the state machine is small and the CAS cost is
 *     already absorbed by the RMW.
 *
 *   • Seqlock (position_seq + position_snap) — single writer (RT thread; UI
 *     callers take the PW thread-loop lock and write via the same helper).
 *     Trailing store is release; readers acquire-load and retry on odd seq.
 *
 *   • Spectrum (spectrum_bars + spectrum_generation) — bars are relaxed;
 *     `generation` is release on the writer (publishes the batch) and
 *     acquire on the reader (gate before reading bars).
 *
 *   • RT flags (pw_stream_state, device_error, streams_active,
 *     reconnect_attempted) — release/acquire where the value gates entry into
 *     other state; relaxed when used as a pure boolean signal.
 *
 * On x86-TSO, atomic loads of any order ≤ acquire compile to plain `mov`.
 * Atomic stores compile to `mov` for relaxed/release; only seq_cst stores
 * and RMWs carry a fence. On ARMv8, relaxed is `ldr`/`str`, acquire/release
 * are `ldar`/`stlr`, and seq_cst stores carry a `dmb ish`. The orderings
 * chosen here are the minimum that the synchronization actually requires.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_player {
    int player_id;
    audio_pipeline_t *pipeline; /* Back-reference for perf access */

    /* Wire format negotiated with PipeWire. Fixed for player lifetime;
     * reconfiguration tears down and recreates the streams. All sample math
     * (frame strides, memsets, shuttle_speed I/O) reads from this. */
    audio_format_t format;

    /* Playback state (atomic for real-time safety) */
    atomic_int state; /* CHANNEL_STOPPED, CHANNEL_PLAYING, CHANNEL_PAUSED, CHANNEL_ERROR */
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

    /* Playhead — atomic, shared between UI (seek bar) and audio thread.
     * The shuttle engine reads/writes during process(). The sole source of
     * truth for "where is playback right now". */
    audio_seek_position_t seek_position;

    /* Variable-speed playback engine (ALWAYS attached) */
    audio_shuttle_speed_t *shuttle_speed;

    /* Position snapshot for smooth UI interpolation (seqlock pattern) */
    atomic_uint position_seq;          /* Seqlock sequence (odd = writing) */
    position_snapshot_t position_snap; /* Snapshot data */

    /* Sample counter for RT-safe timestamps (incremented by callback) */
    atomic_uint_fast64_t callback_sample_count;

    /* RT timing for fault diagnostics */
    uint64_t prev_shuttle_speed_underflows; /* Snapshot for delta detection */

    /* PipeWire stream state (updated by state_changed callback) */
    atomic_int pw_stream_state; /* enum pw_stream_state */

    /* Per-player RT stats (atomic — written in callback, read from UI thread) */
    atomic_uint_fast64_t stats_cb_count;          /* Internal: for computing averages */
    atomic_uint_fast64_t stats_cb_time_sum_ns;    /* Cumulative processing time */
    atomic_uint_fast64_t stats_cb_time_max_ns;    /* Peak processing time */
    atomic_uint_fast64_t stats_budget_overruns;   /* Callbacks exceeding 50% of period budget */
    atomic_uint_fast64_t stats_dequeue_failures;  /* pw_stream_dequeue_buffer returned NULL */
    atomic_uint_fast64_t stats_buffer_underruns;  /* PLAYING state with no buffer loaded */
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
 * Player (Internal API — src/audio/player.c)
 *
 * One channel: its PipeWire streams, RT callback, DSP engine, and lifecycle.
 * The pipeline (src/audio/pipeline.c) owns the array of players and drives them
 * through this narrow surface.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Generation-checked idle callback payload.
 *
 * player_reconnect_idle / player_deactivate_idle run on the GTK main thread and
 * take the PW lock. If player_recreate_stream() ran between scheduling and
 * execution, the idle would operate on replaced streams → stale/dangerous.
 * Capture stream_generation at schedule time; the idle skips if it changed.
 */
typedef struct {
    audio_player_t *player;
    unsigned int generation; /* stream_generation at schedule time */
} player_idle_data_t;

/** Initialize a player in-place (dormant — no streams until a device is set). */
quadrature_result_t player_init(audio_player_t *p, int id, audio_format_t format);

/** Tear down a player's streams/DSP and unlock its tracks via `cache` (may be NULL). */
void player_destroy(audio_player_t *p, audio_cache_t *cache);

/** Recreate a player's streams on a new target device. PW thread-loop lock held by caller. */
quadrature_result_t
player_recreate_stream(audio_player_t *p, uint32_t sample_rate, const char *target_device);

/** GLib idle: reconnect a player's streams after a transient device error. */
gboolean player_reconnect_idle(gpointer data);

/** Publish a position snapshot (seqlock writer). Caller holds the PW lock off the RT thread. */
void player_update_position_snap(
    audio_player_t *p, uint64_t position, float speed, bool playing, uint64_t sample_count);

/** Flush a player's DSP/buffer scratch state (call after seek/stop). */
void audio_player_flush_all(audio_player_t *p);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline Event Types
 * 
 * Note: Types defined in public header (quadrature/audio.h).
 * Ring buffer size defined here since it's internal implementation detail.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AUDIO_EVENT_RING_SIZE 512 /* Power of 2, large enough for RT events */

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Telemetry (src/audio/telemetry.c)
 *
 * RT-safe diagnostics plumbing shared between the audio callback and the perf
 * UI. publish_event / record_callback are lock-free (atomics only, no alloc)
 * and callable from the PipeWire RT thread.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Publish one event into the lock-free SPSC ring. RT-safe. Pass-by-value so
 * callsites read as publish(pl, (E){...}).
 */
void audio_pipeline_publish_event(audio_pipeline_t *pl, audio_pipeline_event_t event);

/**
 * Record one callback's timing into the budget/latency/interval rings and emit
 * any derived fault events (scheduling delay, budget overrun). RT-safe; called
 * once per on_process() after the audio has been rendered.
 *
 * @param frame_count     Frames produced this callback (for budget math)
 * @param cb_start        Callback entry timestamp (time_ns())
 * @param has_interval    Whether a previous-callback timestamp was available
 * @param interval_dev_ns Signed deviation of the callback interval from nominal
 */
void audio_telemetry_record_callback(audio_player_t *p,
                                     uint32_t frame_count,
                                     uint64_t cb_start,
                                     bool has_interval,
                                     int64_t interval_dev_ns);

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Pipeline (Internal Definition)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct audio_pipeline {
    audio_player_t players[MAX_AUDIO_PLAYERS];

    /* Canonical wire format. Single source of truth for the entire subsystem:
     *   cache decodes to this format; every player starts in this format;
     *   every buffer's format equals this (enforced at buffer-load asserts).
     * Established once in audio_pipeline_create and never mutated. */
    audio_format_t format;
    uint64_t ns_per_frame; /* 1e9 / format.sample_rate, pre-computed at init */

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

    /* Track decode-failure callback */
    void (*track_failed_callback)(int player_id, int64_t track_id, void *user_data);
    void *track_failed_user_data;

    /* Auto-advance timeout (runs on main thread) */
    guint advance_timeout_id;

    /* Event ring buffer for performance monitoring (lock-free SPSC) */
    audio_pipeline_event_t events[AUDIO_EVENT_RING_SIZE];
    atomic_uint event_write;
    atomic_uint event_read;

    /* Performance dashboard (for aggregated view, not written to directly) */
    perf_dashboard_t *perf;

    atomic_bool system_active;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-Player Spectrum Init/Destroy
 *
 * Initialize/destroy the cava plan + buffers embedded in audio_player_t.
 * FFT processing runs inline in the PipeWire monitor callback.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Initialize a player's spectrum state (cava plan + accumulation buffers). */
quadrature_result_t spectrum_init(spectrum_state_t *s, uint32_t sample_rate);

/* Destroy a player's spectrum state. */
void spectrum_cleanup(spectrum_state_t *s);

/* Set the FFT display refresh rate (clamped to [30, 165] Hz). */
void spectrum_set_refresh_hz(spectrum_state_t *s, double hz);

/*
 * Run the cavacore FFT over interleaved stereo `in` (`frames` frames) from the
 * PipeWire monitor callback. On each completed display frame, writes SPECTRUM_BARS
 * stereo magnitudes into `bars` (relaxed) then bumps `generation` (release) to
 * publish the batch to UI readers.
 */
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

/* Cache status — the single enum describing a track's decode state, returned by
 * both audio_cache_get_status() (pure query) and audio_cache_lock() (acquire +
 * report). NOT_FOUND is only ever returned by get_status(); lock() crashes on a
 * missing track since that is a caller bug (load() must precede lock()). */
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
    char codec[16];              /* ffmpeg codec name (e.g. "flac", "aac", "alac") */
    uint32_t decode_duration_ms; /* How long decode took */
    uint64_t timestamp_ms;       /* When load() was called (monotonic) */
} audio_cache_decode_event_t;

/* Cache Lifecycle */
quadrature_result_t
audio_cache_create(library_cache_t *library, audio_format_t format, audio_cache_t **out);
void audio_cache_destroy(audio_cache_t *cache);

/* Loading API (Background Decoding) */
quadrature_result_t audio_cache_load(audio_cache_t *cache, int64_t track_id);
void audio_cache_cancel_load(audio_cache_t *cache, int64_t track_id);
void audio_cache_cancel_all_loads(audio_cache_t *cache);

/* Lock/Unlock API (Eviction Protection) */

/* Lock a track against LRU eviction and report its decode status. Returns the
 * shared audio_cache_status_t (never NOT_FOUND — see that enum's note). */
audio_cache_status_t audio_cache_lock(audio_cache_t *cache, int64_t track_id);

/* delay_ms argument to audio_cache_unlock():
 *   AUDIO_CACHE_UNLOCK_IMMEDIATE (0)  — drop the lock synchronously now.
 *   AUDIO_CACHE_UNLOCK_DEFERRED (-1)  — defer by the cache's quantum-derived
 *                                       safe delay; use when transitioning away
 *                                       from a playing track so the RT callback
 *                                       finishes reading before eviction.
 *   > 0                               — defer by exactly this many milliseconds. */
#define AUDIO_CACHE_UNLOCK_IMMEDIATE 0
#define AUDIO_CACHE_UNLOCK_DEFERRED  (-1)
void audio_cache_unlock(audio_cache_t *cache, int64_t track_id, int delay_ms);
void audio_cache_set_quantum(audio_cache_t *cache, uint32_t quantum_frames);

/* Buffer Access (For Locked Tracks) */
audio_buffer_t *audio_cache_get_locked(audio_cache_t *cache, int64_t track_id);

/* Status Query */
audio_cache_status_t audio_cache_get_status(audio_cache_t *cache, int64_t track_id);

/* Buffer Accessors */
int64_t audio_buffer_get_track_id(const audio_buffer_t *buf);
const float *audio_buffer_get_samples(const audio_buffer_t *buf);
uint64_t audio_buffer_get_num_frames(const audio_buffer_t *buf);
audio_format_t audio_buffer_get_format(const audio_buffer_t *buf);
const float *audio_buffer_get_waveform_rms(const audio_buffer_t *buf);
bool audio_buffer_is_waveform_rms_ready(const audio_buffer_t *buf);

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
