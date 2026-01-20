#ifndef QUADRATURE_AUDIO_SPECTRUM_H
#define QUADRATURE_AUDIO_SPECTRUM_H

/**
 * Spectrum analyzer that processes audio from players.
 *
 * Runs a background thread that reads samples from each player's ring buffer,
 * processes through cavacore FFT, and writes spectrum bars to player structs.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types - full definitions in src/audio/internal.h */
typedef struct spectrum_channel spectrum_channel_t;
typedef struct spectrum_analyzer spectrum_analyzer_t;

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

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_AUDIO_SPECTRUM_H
