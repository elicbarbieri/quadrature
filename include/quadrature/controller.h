/**
 * Quadrature Channel Controller
 *
 * Abstracts a *source of channel control commands*. The application receives
 * high-level operations (play/pause, preview, on-air, skip) for a channel and
 * pushes channel state back so the source can mirror it (e.g. console LEDs).
 *
 * The command vocabulary is independent of how a source is wired: an Axia
 * console (LWRP/GPIO over TCP), a CLI command, a network API, etc. Each source
 * is a backend that maps its native input to control_command_t and renders
 * control_state_t in whatever way it can.
 *
 * Threading model (guaranteed by all backends):
 * - Each controller_t runs a dedicated listener thread managed by the core.
 * - Callbacks are marshaled to the GLib main thread via g_main_context_invoke().
 * - Network-backed sources auto-reconnect on drop with exponential backoff.
 *
 * Adding a backend: implement the internal controller_driver_t vtable and expose
 * a single controller_<name>_create() constructor. The generic control surface
 * and existing callers are unaffected.
 */

#ifndef QUADRATURE_CONTROLLER_H
#define QUADRATURE_CONTROLLER_H

#include "quadrature/quadrature.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque controller handle. Created via a backend constructor (e.g.
 * controller_axia_create), driven through the generic functions below. */
typedef struct controller_t controller_t;

/* Control commands issued by a source (source -> app).
 * These are intents; the app resolves them against current channel state. */
typedef enum {
    CONTROL_CMD_PLAY_PAUSE,     /* Toggle playback (fader on/off, spacebar, ...) */
    CONTROL_CMD_PREVIEW_TOGGLE, /* Toggle preview / PFL monitoring */
    CONTROL_CMD_ON_AIR,         /* Take the channel to air */
    CONTROL_CMD_SKIP,           /* Skip to the next track */
} control_command_t;

/* Channel state the app pushes to a source (app -> source) for indicators.
 * Mirrors the application's channel modes. */
typedef enum {
    CONTROL_STATE_IDLE,    /* Stopped / nothing happening */
    CONTROL_STATE_PREVIEW, /* Previewing (PFL), not on air */
    CONTROL_STATE_QUEUED,  /* Cued and ready, not on air */
    CONTROL_STATE_ON_AIR,  /* Live to air */
} control_state_t;

/* Incoming command callback (source -> app).
 * Invoked on the GLib main thread (marshaled via g_main_context_invoke()).
 * @param channel Channel the command targets (0-based)
 * @param command The control intent
 * @param user_data Opaque data passed to controller_set_command_callback()
 */
typedef void (*controller_command_callback_t)(int channel,
                                              control_command_t command,
                                              void *user_data);

/* Connection status callback (optional).
 * Invoked on the GLib main thread when the source's connection state changes.
 * @param channel Channel (0-based, as supplied at construction)
 * @param connected true if connected/available, false otherwise
 * @param user_data Opaque data passed to controller_set_status_callback()
 */
typedef void (*controller_status_callback_t)(int channel, bool connected, void *user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle (generic — works on any backend)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Destroy a controller and stop its listener thread.
 * @param controller Handle (NULL is safe)
 */
void controller_destroy(controller_t *controller);

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback & Thread Control (generic)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Set the callback for incoming control commands.
 * @param controller Handle
 * @param callback Function to call on commands (NULL to disable)
 * @param user_data Opaque pointer passed to callback
 */
void controller_set_command_callback(controller_t *controller,
                                     controller_command_callback_t callback,
                                     void *user_data);

/**
 * Set the callback for connection status changes.
 * @param controller Handle
 * @param callback Function to call on status changes (NULL to disable)
 * @param user_data Opaque pointer passed to callback
 */
void controller_set_status_callback(controller_t *controller,
                                    controller_status_callback_t callback,
                                    void *user_data);

/**
 * Start the listener thread (network sources auto-reconnect on disconnect).
 * @param controller Handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t controller_start(controller_t *controller);

/**
 * Stop the listener thread and close the source connection.
 * @param controller Handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t controller_stop(controller_t *controller);

/**
 * Check whether the source is currently connected/available.
 * @param controller Handle
 * @return true if connected, false otherwise
 */
bool controller_is_connected(const controller_t *controller);

/* ═══════════════════════════════════════════════════════════════════════════
 * State Feedback (app -> source indicators)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Push a channel's state to the source so it can mirror it (e.g. console LEDs).
 * Sources without indicators (e.g. CLI) may treat this as a no-op.
 * Thread-safe: may be called from any thread.
 * @param controller Handle
 * @param channel Channel to update (0-based)
 * @param state Current channel state
 * @return QUADRATURE_OK on success, QUADRATURE_ERROR_DEVICE_BUSY if not connected
 */
quadrature_result_t
controller_set_channel_state(controller_t *controller, int channel, control_state_t state);

/* ═══════════════════════════════════════════════════════════════════════════
 * Backends
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create a controller backed by an Axia Livewire+ console (LWRP/GPIO over TCP).
 * @param address Console address as "ip:port" or just "ip" (default port 93).
 *                Examples: "10.1.73.12:93", "10.1.73.12", "192.168.1.100:9300"
 * @param channel Channel number (0-based; sent as 1-based to the console)
 * @param password LWRP password for the LOGIN command (NULL if auth not required)
 * @param out Output pointer for the created handle
 * @return QUADRATURE_OK on success, QUADRATURE_ERROR_INVALID_PARAM for bad input
 */
quadrature_result_t
controller_axia_create(const char *address, int channel, const char *password, controller_t **out);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_CONTROLLER_H */
