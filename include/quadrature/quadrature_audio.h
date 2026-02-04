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
 * Track Changed Callback
 * ============================================================================= */

/**
 * Callback invoked when a player's track changes (via auto-advance or skip).
 *
 * @param player_id  Player that changed
 * @param track_id   New track ID (0 if playback ended)
 * @param user_data  User data from set_track_changed_callback
 */
typedef void (*audio_track_changed_cb)(int player_id, int64_t track_id, void* user_data);

/* =============================================================================
 * Pipeline Lifecycle
 * ============================================================================= */

/**
 * Create an audio pipeline with LibraryCache for track ID support.
 *
 * @param library      Library cache for track_id -> path resolution (may be NULL)
 * @param sample_rate  Output sample rate
 * @param pipeline     Output pointer to created pipeline
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_pipeline_create(library_cache_t* library,
                                           uint32_t sample_rate,
                                           audio_pipeline_t** pipeline);

void audio_pipeline_destroy(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_start(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_stop(audio_pipeline_t* pipeline);

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
quadrature_result_t audio_pipeline_set_player_track(audio_pipeline_t* pipeline,
                                                     int player_id,
                                                     int64_t track_id);

/**
 * Get current track ID for a player.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @return Current track ID or 0 if no track loaded
 */
int64_t audio_pipeline_get_player_track_id(audio_pipeline_t* pipeline, int player_id);

/**
 * Set track changed callback.
 *
 * Called when track changes via auto-advance or skip.
 *
 * @param pipeline   Pipeline instance
 * @param callback   Callback function (NULL to clear)
 * @param user_data  User data passed to callback
 */
void audio_pipeline_set_track_changed_callback(audio_pipeline_t* pipeline,
                                                audio_track_changed_cb callback,
                                                void* user_data);


/* =============================================================================
 * Playback Control
 * ============================================================================= */

quadrature_result_t audio_pipeline_player_play(audio_pipeline_t* pipeline, int player_id);
quadrature_result_t audio_pipeline_player_stop(audio_pipeline_t* pipeline, int player_id);
quadrature_result_t audio_pipeline_player_seek(audio_pipeline_t* pipeline, int player_id, uint64_t position);

/**
 * Toggle between play and pause.
 * If playing, pauses. If stopped/paused, plays.
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_pipeline_player_toggle_play(audio_pipeline_t* pipeline, int player_id);

/* =============================================================================
 * Playback Speed Control
 *
 * Variable-speed playback (-4x to +4x) with three processing zones:
 *   - Passthrough (≈1.0): Zero CPU, direct copy
 *   - Turntable (0.8-1.2x): Low CPU, pitch shifts with speed (like vinyl)
 *   - Rubberband (<0.8x or >1.2x): Higher CPU, pitch preserved
 * ============================================================================= */

/**
 * Set playback speed for a player.
 *
 * @param speed  Playback speed: -4.0 to +4.0 (1.0 = normal)
 * @return QUADRATURE_OK on success, error if track not cached
 */
quadrature_result_t audio_pipeline_player_set_speed(audio_pipeline_t* pipeline,
                                                     int player_id, float speed);

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
quadrature_result_t audio_pipeline_player_set_shuttle_mode(audio_pipeline_t* pipeline,
                                                           int player_id, shuttle_mode_t mode);

/* =============================================================================
 * Repeat Control
 * ============================================================================= */

quadrature_result_t audio_pipeline_player_set_repeat(audio_pipeline_t* pipeline, int player_id, bool repeat);
bool audio_pipeline_player_get_repeat(audio_pipeline_t* pipeline, int player_id);
quadrature_result_t audio_pipeline_player_set_autoplay(audio_pipeline_t* pipeline, int player_id, bool autoplay);
bool audio_pipeline_player_get_autoplay(audio_pipeline_t* pipeline, int player_id);

/* =============================================================================
 * Device Routing
 * ============================================================================= */

quadrature_result_t audio_pipeline_set_player_device(audio_pipeline_t* pipeline, int player_id, const char* device_name);

/* =============================================================================
 * Monitoring
 * ============================================================================= */

channel_state_t audio_pipeline_get_player_state(audio_pipeline_t* pipeline, int player_id);
uint64_t audio_pipeline_get_player_position(audio_pipeline_t* pipeline, int player_id);
uint64_t audio_pipeline_get_player_length(audio_pipeline_t* pipeline, int player_id);
uint32_t audio_pipeline_get_sample_rate(audio_pipeline_t* pipeline);

/**
 * Get interpolated player position for smooth UI display.
 * Interpolates between audio updates using wall-clock time.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param out_speed  Optional: receives current playback speed
 * @return Position in samples (double for sub-sample accuracy)
 */
double audio_pipeline_get_player_position_smooth(audio_pipeline_t* pipeline,
                                                  int player_id,
                                                  float* out_speed);

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
void audio_pipeline_get_player_spectrum(audio_pipeline_t* pipeline, int player_id,
                                        float* left, float* right, int num_bars);

/* =============================================================================
 * Statistics
 * ============================================================================= */

/**
 * Pipeline performance statistics.
 */
typedef struct {
    /* Callback performance */
    uint64_t callback_count;         /**< Total callbacks since start */
    uint64_t underrun_count;         /**< Callbacks where buffer wasn't ready (audio gap) */

    /* Callback timing (microseconds) */
    float callback_time_avg_us;      /**< Average processing time per callback */
    float callback_time_max_us;      /**< Peak processing time (budget: ~21ms) */

    /* Track transitions */
    uint64_t track_changes;          /**< Total auto-advances + user skips */
    uint64_t instant_advances;       /**< Advances where next was preloaded */
} audio_pipeline_stats_t;

/**
 * Get pipeline performance statistics.
 *
 * @param pipeline  Pipeline instance
 * @param stats     Output stats structure
 */
void audio_pipeline_get_stats(audio_pipeline_t* pipeline, audio_pipeline_stats_t* stats);

/* =============================================================================
 * Per-Player Statistics
 * ============================================================================= */

/**
 * Per-player audio health snapshot.
 *
 * All values are computed rates/percentages — no raw monotonic counters.
 * Fault counts (budget_overruns, etc.) represent rare events that should be 0.
 */
typedef struct {
    /* Callback performance */
    float callback_time_avg_us;   /**< Average processing time per callback */
    float callback_time_max_us;   /**< All-time peak processing time */
    float budget_pct;             /**< Avg processing time as % of period budget */
    uint64_t budget_overruns;     /**< Callbacks exceeding 50% of period budget */

    /* Audio health */
    float underrun_rate_pct;      /**< Underruns as % of total callbacks (0 = healthy) */
    float jitter_ms;              /**< Average callback scheduling jitter */

    /* Fault events (should be 0 in normal operation) */
    uint64_t dequeue_failures;    /**< PipeWire couldn't provide output buffer */
    uint64_t scrubber_underflows; /**< Rubberband couldn't fill requested frames */
    uint64_t deferred_advances;   /**< Track advance with audible gap (preload miss) */

    /* Advance quality */
    float advance_hit_rate_pct;   /**< Preloaded advances as % of total (100 = perfect) */
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
void audio_pipeline_get_player_stats(audio_pipeline_t* pipeline,
                                      int player_id,
                                      audio_player_stats_t* stats);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_H */
