/**
 * Quadrature UI Control API
 *
 * Public interface for external control of UI elements (e.g., GPIO/Axia integration).
 * All functions are thread-safe when called from the GTK main thread.
 * GPIO threads must use g_main_context_invoke() to marshal calls to main thread.
 */

#ifndef QUADRATURE_UI_H
#define QUADRATURE_UI_H

#include "quadrature/quadrature.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations (opaque types) */
typedef struct _UiChannelStrip UiChannelStrip;

/* Channel operational modes (broadcast automation states) */
typedef enum {
    CHANNEL_MODE_IDLE = 0,       /* Normal operation */
    CHANNEL_MODE_PREVIEW,        /* PFL (Pre-Fader Listen) active */
    CHANNEL_MODE_QUEUED,         /* Ready for on-air */
    CHANNEL_MODE_ON_AIR          /* Live broadcast */
} ChannelMode;

/* ═══════════════════════════════════════════════════════════════════════════
 * Preview/PFL Control
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Enable preview (PFL) mode on a channel
 * - Switches mode to CHANNEL_MODE_PREVIEW
 * - Does nothing if already in QUEUED or ON_AIR mode (blocked)
 * - Updates preview button state without triggering signal handler
 */
void ui_channel_strip_preview_on(UiChannelStrip *strip);

/**
 * Disable preview (PFL) mode on a channel
 * - Returns to CHANNEL_MODE_IDLE
 * - Does nothing if not in PREVIEW mode
 * - Updates preview button state without triggering signal handler
 */
void ui_channel_strip_preview_off(UiChannelStrip *strip);

/**
 * Query current preview state
 * Returns: true if channel is in PREVIEW mode, false otherwise
 */
bool ui_channel_strip_get_preview_active(UiChannelStrip *strip);

/* ═══════════════════════════════════════════════════════════════════════════
 * Mode Control & Queries
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get current channel mode
 * Used by GPIO thread to send LED feedback to hardware
 */
ChannelMode ui_channel_strip_get_mode(UiChannelStrip *strip);

/**
 * Set channel mode explicitly
 * Used internally by UI; GPIO thread should use specific action functions
 */
void ui_channel_strip_set_mode(UiChannelStrip *strip, ChannelMode mode);

/* ═══════════════════════════════════════════════════════════════════════════
 * Playback Control (for GPIO integration)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Start playback on channel
 * - If in QUEUED mode: transitions to ON_AIR and starts playing
 * - If in IDLE/PREVIEW: starts playing without changing mode
 * - If already playing: does nothing
 */
void ui_channel_strip_play(UiChannelStrip *strip);

/**
 * Stop playback on channel
 * - Stops audio playback
 * - If in ON_AIR mode: returns to IDLE
 * - If in QUEUED mode: returns to IDLE
 */
void ui_channel_strip_stop(UiChannelStrip *strip);

/**
 * Query current playback state
 * Returns: CHANNEL_PLAYING, CHANNEL_STOPPED, CHANNEL_PAUSED, or CHANNEL_ERROR
 */
channel_state_t ui_channel_strip_get_player_state(UiChannelStrip *strip);

/* ═══════════════════════════════════════════════════════════════════════════
 * Mode Change Notifications
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Signal: "mode-changed"
 * Emitted when channel mode changes (IDLE, PREVIEW, QUEUED, ON_AIR)
 * 
 * Callback signature:
 *   void callback(UiChannelStrip *strip, int channel_id, int mode, gpointer user_data)
 * 
 * GPIO thread can connect to this signal to detect UI-initiated mode changes
 * and send LED feedback to hardware panels.
 * 
 * Example:
 *   g_signal_connect(strip, "mode-changed", G_CALLBACK(on_mode_changed), gpio_context);
 */

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_UI_H */
