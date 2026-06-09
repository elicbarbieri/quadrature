/**
 * Quadrature Audio Pipeline API
 *
 * 4-channel audio playback engine with PipeWire output.
 * Features:
 * - Track-ID based playback (integrates with LibraryCache)
 * - Variable-speed playback (-4x to +4x) with keylock/pitched modes
 * - Auto-advance with next-track preloading
 * - Per-channel spectrum analyzer
 * - Device routing to different PipeWire sinks
 */

#ifndef QUADRATURE_AUDIO_H
#define QUADRATURE_AUDIO_H

#include "quadrature.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Opaque Types
 * ============================================================================= */

typedef struct audio_pipeline audio_pipeline_t;
typedef struct audio_player audio_player_t;
typedef struct library_cache library_cache_t;

/* =============================================================================
 * Performance Event Types
 * ============================================================================= */

/**
 * Event types for performance monitoring.
 * These events are recorded in the audio pipeline's lock-free ring buffer
 * and can be polled by performance dashboards or logging systems.
 */
typedef enum {
    AUDIO_EVENT_BUFFER_UNDERRUN,    /**< Audio buffer couldn't provide requested frames */
    AUDIO_EVENT_DEQUEUE_FAILURE,    /**< PipeWire couldn't provide output buffer */
    AUDIO_EVENT_SCRUBBER_UNDERFLOW, /**< Rubberband couldn't fill requested frames */
    AUDIO_EVENT_BUDGET_OVERRUN,     /**< Callback exceeded 50% of period budget */
    AUDIO_EVENT_INSTANT_ADVANCE,    /**< Track advanced with preloaded next track */
    AUDIO_EVENT_DEFERRED_ADVANCE,   /**< Track advanced without preload (audible gap) */
    AUDIO_EVENT_PW_XRUN,            /**< PipeWire buffer underrun/overrun detected */
    AUDIO_EVENT_PW_ERROR,           /**< PipeWire stream entered ERROR state */
    AUDIO_EVENT_SCHEDULING_DELAY,   /**< Callback arrived >2x period late */
} audio_event_type_t;

/**
 * Performance event recorded by audio pipeline.
 * Union-based storage for space efficiency.
 */
typedef struct {
    uint64_t timestamp_ns;   /**< Monotonic timestamp in nanoseconds (from time_ns()) */
    audio_event_type_t type; /**< Event type */
    int player_id;           /**< Player that generated event (0-3) */
    int64_t track_id;        /**< Track ID associated with event */

    /* Context-specific data (union for space efficiency) */
    union {
        struct { /* BUFFER_UNDERRUN, SCRUBBER_UNDERFLOW */
            uint32_t requested_frames;
            uint32_t available_frames;
            uint32_t scrub_fill; /**< Only for scrubber underflow */
            float speed;
        } underrun;

        struct { /* DEQUEUE_FAILURE */
            uint32_t queue_size;
        } dequeue;

        struct { /* BUDGET_OVERRUN */
            uint64_t elapsed_ns;
            uint64_t budget_ns;
        } budget;

        struct { /* ZONE_TRANSITION, INSTANT_ADVANCE, DEFERRED_ADVANCE */
            float speed;
            uint32_t old_queue_size;
            uint32_t new_queue_size;
            uint32_t scrub_fill; /**< For deferred advance */
        } transition;

        struct { /* PW_XRUN */
            uint32_t avail_buffers;
            uint32_t queued_buffers;
        } pw_xrun;

        struct {                  /* SCHEDULING_DELAY */
            int64_t deviation_ns; /**< How late the callback arrived (positive = late) */
            int64_t expected_ns;  /**< Expected callback period */
        } scheduling;
    } data;
} audio_pipeline_event_t;

/* =============================================================================
 * Track Changed Callback
 * ============================================================================= */

/**
 * Callback invoked when a player's track changes (via auto-advance or skip).
 *
 * @param player_id  Player that changed
 * @param track_id   New track ID (0 if playback ended)
 * @param user_data  User data from set_track_changed_callback
 */
typedef void (*audio_track_changed_cb)(int player_id, int64_t track_id, void *user_data);

/* =============================================================================
 * Pipeline Lifecycle
 * ============================================================================= */

/**
 * Create a pipewire audio pipeline.
 *
 * @param library      Library cache for track_id -> path resolution
 * @param sample_rate  Output sample rate
 * @param pipeline     Output pointer to created pipeline
 * @return QUADRATURE_OK on success
 */
quadrature_result_t
audio_pipeline_create(library_cache_t *library, uint32_t sample_rate, audio_pipeline_t **pipeline);

void audio_pipeline_destroy(audio_pipeline_t *pipeline);

/* =============================================================================
 * Player Control - Track ID Based
 * ============================================================================= */

/**
 * Set the track for a player by track ID.
 *
 * Handles locking/unlocking of old and new tracks in the cache.
 * Also preloads the next track in the album for instant advance.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param track_id   Track ID to load
 * @return QUADRATURE_OK on success
 */
quadrature_result_t
audio_pipeline_set_player_track(audio_pipeline_t *pipeline, int player_id, int64_t track_id);

/**
 * Get current track ID for a player.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @return Current track ID or 0 if no track loaded
 */
int64_t audio_pipeline_get_player_track_id(audio_pipeline_t *pipeline, int player_id);

/**
 * Set track changed callback.
 *
 * Called when track changes via auto-advance or skip.
 *
 * @param pipeline   Pipeline instance
 * @param callback   Callback function (NULL to clear)
 * @param user_data  User data passed to callback
 */
void audio_pipeline_set_track_changed_callback(audio_pipeline_t *pipeline,
                                               audio_track_changed_cb callback,
                                               void *user_data);

/* =============================================================================
 * Playback Control
 * ============================================================================= */

quadrature_result_t audio_pipeline_player_play(audio_pipeline_t *pipeline, int player_id);
quadrature_result_t audio_pipeline_player_stop(audio_pipeline_t *pipeline, int player_id);
quadrature_result_t
audio_pipeline_player_seek(audio_pipeline_t *pipeline, int player_id, uint64_t position);

/**
 * Seek a player to an absolute position in seconds.
 * Clamps to [0, length_seconds).
 *
 * @return QUADRATURE_OK on success
 */
quadrature_result_t
audio_pipeline_player_seek_seconds(audio_pipeline_t *pipeline, int player_id, double seconds);

/**
 * Toggle between play and pause.
 * If playing, pauses. If stopped/paused, plays.
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_pipeline_player_toggle_play(audio_pipeline_t *pipeline, int player_id);

/* =============================================================================
 * Playback Speed Control
 *
 * Variable-speed playback (-4x to +4x). Processing is determined by mode:
 *   - SHUTTLE_MODE_OFF: Direct copy at 1.0x (no processing)
 *   - SHUTTLE_MODE_KEYLOCK: Rubberband always active (pitch preserved)
 *   - SHUTTLE_MODE_PITCHED: Cubic interpolation always active (pitch shifts with speed)
 * ============================================================================= */

/**
 * Set playback speed for a player.
 *
 * @param speed  Playback speed: -4.0 to +4.0 (1.0 = normal)
 * @return QUADRATURE_OK on success, error if track not cached
 */
quadrature_result_t
audio_pipeline_player_set_speed(audio_pipeline_t *pipeline, int player_id, float speed);

/**
 * Set shuttle mode for a player.
 *
 * Controls how variable-speed playback is processed:
 *   - SHUTTLE_MODE_OFF: Speed locked at 1.0x, no processing
 *   - SHUTTLE_MODE_KEYLOCK: Variable speed, pitch preserved (Rubberband)
 *   - SHUTTLE_MODE_PITCHED: Variable speed, pitch shifts with speed (Turntable)
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param mode       Shuttle mode
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_pipeline_player_set_shuttle_mode(audio_pipeline_t *pipeline,
                                                           int player_id,
                                                           shuttle_mode_t mode);

/* =============================================================================
 * End-of-Track Behavior
 *
 * Single enum replaces the old mutually-exclusive repeat/autoplay booleans.
 * ============================================================================= */

quadrature_result_t audio_pipeline_player_set_end_mode(audio_pipeline_t *pipeline,
                                                       int player_id,
                                                       track_end_mode_t mode);
track_end_mode_t audio_pipeline_player_get_end_mode(audio_pipeline_t *pipeline, int player_id);

/* =============================================================================
 * Device Routing
 * ============================================================================= */

quadrature_result_t audio_pipeline_set_player_device(audio_pipeline_t *pipeline,
                                                     int player_id,
                                                     const char *device_name);

/**
 * Set PipeWire exclusive mode for a player.
 * When exclusive, the player claims sole access to the output device.
 * Takes effect on next device change or stream recreate.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param exclusive  true to enable exclusive mode
 */
void audio_pipeline_set_player_exclusive(audio_pipeline_t *pipeline, int player_id, bool exclusive);

/**
 * Set PipeWire quantum (buffer size) for a player, recreating the stream.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param quantum_frames  Buffer size in frames (power-of-2, 32-2048)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_pipeline_set_player_quantum(audio_pipeline_t *pipeline,
                                                      int player_id,
                                                      uint32_t quantum_frames);

/* =============================================================================
 * Monitoring
 * ============================================================================= */

/**
 * UI-facing snapshot of a player's current display state.
 *
 * All times are in seconds (pre-converted from samples) and `position_seconds`
 * is smooth-interpolated between audio callbacks using sample-count elapsed
 * since the last RT update. The UI never needs sample rate.
 *
 * On invalid pipeline/player_id the struct is zero-filled and `state` is
 * CHANNEL_ERROR.
 */
typedef struct {
    channel_state_t state;
    double position_seconds; /**< Smooth-interpolated position */
    double length_seconds;   /**< Track length, 0 if not loaded */
    float speed;             /**< Signed playback speed (-4.0..+4.0) */
} audio_player_display_t;

/**
 * Fetch a UI display snapshot for a player.
 *
 * Thread-safe; reads atomics and a seqlocked position snapshot. Intended to
 * be called once per UI frame per channel strip.
 */
void audio_pipeline_get_player_display(audio_pipeline_t *pipeline,
                                       int player_id,
                                       audio_player_display_t *out);

/* =============================================================================
 * Spectrum Analyzer
 * ============================================================================= */

/**
 * Get stereo frequency band levels for spectrum display.
 *
 * @param pipeline  Pipeline instance
 * @param player_id Player index (0-3)
 * @param left      Output buffer for left channel bar values (0.0-1.0)
 * @param right     Output buffer for right channel bar values (0.0-1.0)
 * @param num_bars  Number of bars per channel to retrieve (max 24)
 */
void audio_pipeline_get_player_spectrum(
    audio_pipeline_t *pipeline, int player_id, float *left, float *right, int num_bars);

/**
 * Set the target FFT update rate for all players' spectrum analyzers.
 * Clamped to [30, 165] Hz. Safe to call from any thread.
 */
void audio_pipeline_set_spectrum_refresh_hz(audio_pipeline_t *pipeline, double hz);

/* =============================================================================
 * Statistics
 * ============================================================================= */

/* =============================================================================
 * Per-Player Statistics
 * ============================================================================= */

/**
 * Per-player audio health snapshot.
 *
 * All values are computed rates/percentages -- no raw monotonic counters.
 * Fault counts (budget_overruns, etc.) represent rare events that should be 0.
 */
typedef struct {
    /* Callback performance */
    float callback_time_avg_us; /**< Average processing time per callback */
    float callback_time_max_us; /**< All-time peak processing time */
    float budget_pct;           /**< Avg processing time as % of period budget */
    uint64_t budget_overruns;   /**< Callbacks exceeding 50% of period budget */

    /* Audio health */
    float underrun_rate_pct; /**< Underruns as % of total callbacks (0 = healthy) */
    float jitter_ms;         /**< Average callback scheduling jitter */

    /* Fault events (should be 0 in normal operation) */
    uint64_t dequeue_failures;    /**< PipeWire couldn't provide output buffer */
    uint64_t scrubber_underflows; /**< Rubberband couldn't fill requested frames */
    uint64_t deferred_advances;   /**< Track advance with audible gap (preload miss) */

    /* Advance quality */
    float advance_hit_rate_pct; /**< Preloaded advances as % of total (100 = perfect) */
} audio_player_stats_t;

/**
 * Get per-player health snapshot.
 *
 * Thread-safe. Computes rates from atomic counters.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param stats      Output stats
 */
void audio_pipeline_get_player_stats(audio_pipeline_t *pipeline,
                                     int player_id,
                                     audio_player_stats_t *stats);

/* =============================================================================
 * Performance Event Polling
 * ============================================================================= */

/**
 * Read recent events from audio pipeline for performance monitoring.
 * Thread-safe, can be called from any thread.
 *
 * Events are stored in a lock-free ring buffer and consumed on read.
 * If the UI falls behind, older events may be overwritten.
 *
 * @param pipeline  Audio pipeline
 * @param out       Output buffer for events
 * @param max       Maximum events to read
 * @return          Number of events read (0 to max)
 */
int audio_pipeline_get_events(audio_pipeline_t *pipeline, audio_pipeline_event_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_H */
