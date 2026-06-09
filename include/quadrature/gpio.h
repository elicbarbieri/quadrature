/**
 * Quadrature Axia Livewire+ GPIO Integration
 *
 * Manages TCP connection to Axia console for hardware button control and LED feedback.
 * Uses LWRP (Livewire Routing Protocol) over TCP port 93.
 *
 * Protocol Overview:
 * - ASCII commands, terminated with \r\n or \n
 * - Channel IDs: 1-4 (LWRP protocol, converted from internal 0-3)
 * - Pin IDs: 1-5 available, we use 1 (ON-AIR) and 2 (PREVIEW)
 * - State: L (low/0) or H (high/1)
 *
 * Threading:
 * - Each axia_gpio_t runs a dedicated listener thread
 * - Callbacks are marshaled to GTK main thread via g_main_context_invoke()
 * - Auto-reconnects on disconnect with exponential backoff
 */

#ifndef QUADRATURE_GPIO_H
#define QUADRATURE_GPIO_H

#include "quadrature/quadrature.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct axia_gpio_t axia_gpio_t;

/* Pin assignments (Axia Livewire+ GPIO pins) */
typedef enum {
    AXIA_PIN_ON_AIR = 1,  /* Fader on/off state (console → app: play/stop) */
    AXIA_PIN_PREVIEW = 2, /* Preview/PFL state (bidirectional) */
} axia_pin_t;

/* Pin state */
typedef enum {
    AXIA_STATE_LOW = 0,  /* Pin inactive (LED off, button not pressed) */
    AXIA_STATE_HIGH = 1, /* Pin active (LED on, button pressed) */
} axia_state_t;

/* Callback for incoming GPIO state changes (console → app)
 * Called from GTK main thread (automatically marshaled via g_main_context_invoke())
 * @param channel_id Channel ID (0-3, internal representation)
 * @param pin Pin that changed state
 * @param state New state of the pin
 * @param user_data Opaque user data passed to axia_gpio_set_callback()
 */
typedef void (*axia_gpio_callback_t)(int channel_id,
                                     axia_pin_t pin,
                                     axia_state_t state,
                                     void *user_data);

/* Connection status callback (optional)
 * Called from GTK main thread when connection state changes
 * @param channel_id Channel ID (0-3, internal representation)
 * @param connected true if connected, false if disconnected
 * @param user_data Opaque user data passed to axia_gpio_set_status_callback()
 */
typedef void (*axia_gpio_status_callback_t)(int channel_id, bool connected, void *user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create Axia GPIO handler for a single channel
 * @param address Console address in format "ip:port" or just "ip" (default port 93)
 *                Examples: "10.1.73.12:93", "10.1.73.12", "192.168.1.100:9300"
 * @param channel_id Channel number (0-3 internally, sent as 1-4 to console)
 * @param password LWRP password for LOGIN command (NULL if auth not required)
 * @param out Output pointer for created gpio handle
 * @return QUADRATURE_OK on success, QUADRATURE_ERROR_INVALID_PARAM for invalid address
 */
quadrature_result_t
axia_gpio_create(const char *address, int channel_id, const char *password, axia_gpio_t **out);

/**
 * Destroy GPIO handler and stop listener thread
 * @param gpio GPIO handle (NULL is safe)
 */
void axia_gpio_destroy(axia_gpio_t *gpio);

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback & Thread Control
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Set callback for console → app GPIO events
 * @param gpio GPIO handle
 * @param callback Function to call on GPIO state changes (may be NULL to disable)
 * @param user_data Opaque pointer passed to callback
 */
void axia_gpio_set_callback(axia_gpio_t *gpio, axia_gpio_callback_t callback, void *user_data);

/**
 * Set callback for connection status changes
 * @param gpio GPIO handle
 * @param callback Function to call on status changes (may be NULL to disable)
 * @param user_data Opaque pointer passed to callback
 */
void axia_gpio_set_status_callback(axia_gpio_t *gpio,
                                   axia_gpio_status_callback_t callback,
                                   void *user_data);

/**
 * Start listener thread (auto-reconnects on disconnect)
 * @param gpio GPIO handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t axia_gpio_start(axia_gpio_t *gpio);

/**
 * Stop listener thread and close connection
 * @param gpio GPIO handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t axia_gpio_stop(axia_gpio_t *gpio);

/**
 * Check if GPIO is currently connected
 * @param gpio GPIO handle
 * @return true if connected, false otherwise
 */
bool axia_gpio_is_connected(const axia_gpio_t *gpio);

/* ═══════════════════════════════════════════════════════════════════════════
 * Send GPIO State (app → console LED feedback)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Send GPIO state to console (updates console LEDs)
 * Thread-safe: Can be called from any thread
 * @param gpio GPIO handle
 * @param pin Pin to update (AXIA_PIN_ON_AIR or AXIA_PIN_PREVIEW)
 * @param state State to set (AXIA_STATE_LOW or AXIA_STATE_HIGH)
 * @return QUADRATURE_OK on success, error if not connected
 */
quadrature_result_t axia_gpio_set(axia_gpio_t *gpio, axia_pin_t pin, axia_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_GPIO_H */
