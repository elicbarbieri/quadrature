#ifndef QUADRATURE_AUDIO_PIPELINE_H
#define QUADRATURE_AUDIO_PIPELINE_H

#include "../core/types.h"
#include "../core/perf_dashboard.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Opaque Types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct audio_pipeline audio_pipeline_t;
typedef struct audio_player audio_player_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Pipeline Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_create(uint32_t sample_rate, audio_pipeline_t** pipeline);
void audio_pipeline_destroy(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_start(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_stop(audio_pipeline_t* pipeline);

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Control
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Load an audio file for playback (async).
 *
 * Delegates to audio_buffer_store for decode. Returns immediately.
 * If buffer is already cached, acquires immediately and transitions to STOPPED.
 * Otherwise, player enters LOADING state until decode completes.
 * Poll is_ready() to check when buffer is acquired and playback can begin.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @param path       Path to audio file
 * @return QUADRATURE_OK on success (load started or cache hit)
 */
quadrature_result_t audio_pipeline_player_load(audio_pipeline_t* pipeline,
                                                int player_id,
                                                const char* path);

/**
 * Check if a player is ready to play.
 *
 * @param pipeline   Pipeline instance
 * @param player_id  Player index (0-3)
 * @return true if buffer is loaded and ready for playback
 */
bool audio_pipeline_player_is_ready(audio_pipeline_t* pipeline, int player_id);

quadrature_result_t audio_pipeline_player_play(audio_pipeline_t* pipeline, int player_id);
quadrature_result_t audio_pipeline_player_stop(audio_pipeline_t* pipeline, int player_id);
quadrature_result_t audio_pipeline_player_seek(audio_pipeline_t* pipeline, int player_id, uint64_t position);

/**
 * Toggle between play and pause.
 * If playing, pauses. If stopped/paused, plays.
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_pipeline_player_toggle_play(audio_pipeline_t* pipeline, int player_id);

/* ═══════════════════════════════════════════════════════════════════════════
 * Playback Speed Control
 *
 * Variable-speed playback (-4x to +4x) with three processing zones:
 *   - Passthrough (≈1.0): Zero CPU, direct copy
 *   - Turntable (0.8-1.2x): Low CPU, pitch shifts with speed (like vinyl)
 *   - Rubberband (<0.8x or >1.2x): Higher CPU, pitch preserved
 *
 * Speed mapping (UI slider value + 1.0):
 *   speed=4.0  -> 4x forward   (rubberband, pitch preserved)
 *   speed=2.0  -> 2x forward   (rubberband, pitch preserved)
 *   speed=1.1  -> 1.1x forward (turntable, pitch shifts)
 *   speed=1.0  -> normal       (passthrough, zero CPU)
 *   speed=0.9  -> 0.9x         (turntable, pitch shifts)
 *   speed=0.5  -> half speed   (rubberband, pitch preserved)
 *   speed=0.0  -> stopped
 *   speed=-1.0 -> 1x reverse
 *   speed=-3.0 -> 3x reverse   (rubberband, pitch preserved)
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Repeat Control
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_player_set_repeat(audio_pipeline_t* pipeline, int player_id, bool repeat);

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Routing
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_pipeline_set_player_device(audio_pipeline_t* pipeline, int player_id, const char* device_name);

/* ═══════════════════════════════════════════════════════════════════════════
 * Monitoring
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Spectrum Analyzer
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get frequency band levels for spectrum display.
 *
 * @param pipeline  Pipeline instance
 * @param player_id Player index (0-3)
 * @param bars      Output buffer for bar values (0.0-1.0)
 * @param num_bars  Number of bars to retrieve (max 24)
 */
void audio_pipeline_get_player_spectrum(audio_pipeline_t* pipeline, int player_id, float* bars, int num_bars);

/* ═══════════════════════════════════════════════════════════════════════════
 * Performance Dashboard
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get the performance dashboard for the pipeline.
 *
 * @param pipeline  Pipeline instance
 * @return Performance dashboard (may be NULL if not enabled)
 */
perf_dashboard_t* audio_pipeline_get_perf(audio_pipeline_t* pipeline);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_PIPELINE_H */
