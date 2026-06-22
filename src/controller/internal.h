/**
 * Quadrature Controller Internal Declarations
 *
 * Private types shared between the generic core (controller.c) and concrete
 * backends (axia.c). Layering:
 *
 *   controller.c    — generic control surface: opaque handle, callback
 *                     marshaling, listener thread, reconnect/backoff.
 *   axia.c          — Axia Livewire+ source: maps LWRP/GPIO <-> control commands
 *                     and channel state, implementing controller_driver_t. The
 *                     pure LWRP wire parse/format helpers live at its bottom
 *                     (no I/O; unit-tested directly in test_axia_protocol.c).
 *
 * A backend implements controller_driver_t and exposes a single
 * controller_<name>_create() that builds its context and wraps it via
 * controller_core_new().
 */

#ifndef QUADRATURE_CONTROLLER_INTERNAL_H
#define QUADRATURE_CONTROLLER_INTERNAL_H

#include "quadrature/controller.h"
#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>

/* Reconnect/backoff tuning (owned by the generic core). */
#define RECONNECT_INITIAL_DELAY_MS 100
#define RECONNECT_MAX_DELAY_MS     5000
#define RECONNECT_BACKOFF_FACTOR   2

/* LWRP protocol constants (Axia backend). */
#define LWRP_PORT     93
#define LWRP_MAX_LINE 512

/* ═══════════════════════════════════════════════════════════════════════════
 * Backend driver interface
 *
 * The core calls these from the listener thread (connect/pump/wake/disconnect/
 * destroy) and from arbitrary caller threads (set_state). A backend must make
 * set_state safe to call concurrently with the listener thread's read loop.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Emitter passed into pump(): the backend calls this for each decoded command.
 * The core marshals it to the main thread and invokes the user callback. */
typedef void (*controller_emit_fn)(controller_t *core, int channel, control_command_t command);

typedef struct controller_driver_t {
    /* Establish the full session (transport connect + auth + subscribe).
     * Returns true when ready to pump; cleans up internally on failure.
     * Non-network sources may simply return true. */
    bool (*connect)(void *ctx);

    /* Block reading the source once; decode and emit() any commands.
     * Returns false on disconnect/error (the core will reconnect). */
    bool (*pump)(void *ctx, controller_emit_fn emit, controller_t *core);

    /* Render a channel's state on the source (e.g. LEDs). Returns true on
     * success. Sources without indicators may return true unconditionally. */
    bool (*set_state)(void *ctx, int channel, control_state_t state);

    /* Interrupt a blocked pump() so the listener can observe shutdown (e.g.
     * shutdown() the socket). Must be safe to call from another thread. */
    void (*wake)(void *ctx);

    /* Tear down the source connection. Idempotent. */
    void (*disconnect)(void *ctx);

    /* Free the backend context. */
    void (*destroy)(void *ctx);
} controller_driver_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Generic core handle (defined here, opaque to public callers)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct controller_t {
    const controller_driver_t *driver; // Backend vtable
    void *ctx;                         // Backend-private context
    int channel;                       // 0-based channel reported to callbacks/logs

    /* Listener thread */
    pthread_t listener_thread;
    _Atomic bool running;   // Thread should keep running
    _Atomic bool connected; // Source currently connected/available

    /* Callbacks (protected by callback_mutex) */
    GMutex callback_mutex;
    controller_command_callback_t command_callback;
    void *command_user_data;
    controller_status_callback_t status_callback;
    void *status_user_data;

    /* Reconnection state (listener thread only) */
    int reconnect_delay_ms;
};

/* Allocate a core handle wrapping a backend. Takes ownership of ctx: a later
 * controller_destroy() calls driver->destroy(ctx). Returns NULL on
 * allocation/param failure (caller must then free ctx itself). */
controller_t *controller_core_new(const controller_driver_t *driver, void *ctx, int channel);

/* ═══════════════════════════════════════════════════════════════════════════
 * Axia Livewire+ backend (axia.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char *console_ip; // Heap-allocated console IP
    uint16_t port;    // TCP port (default: LWRP_PORT)
    int channel;      // 0-based (internal), sent as 1-based to console
    char *password;   // LWRP password for LOGIN (NULL if not required)
    int sockfd;       // TCP socket fd (-1 if not connected)

    /* Partial-line reassembly buffer for the read loop (per-instance: each
     * channel has its own listener thread, so this must not be shared). */
    char line_buffer[LWRP_MAX_LINE];
    int line_pos;
} axia_ctx_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * LWRP protocol helpers (axia_protocol.c)
 *
 * Livewire Routing Protocol GPIO. Each GPI/GPO port has exactly 5 pins. A pin
 * state line is "GPI <port> <5 chars>" / "GPO <port> <5 chars>", one character
 * per pin (pin 1 first). In notifications each character is:
 *   'H' high (inactive/off), changing      'h' high (inactive/off), steady
 *   'L' low  (active/on),    changing      'l' low  (active/on),    steady
 * i.e. LWRP GPIO is active-LOW, and an UPPERCASE letter flags the pin that just
 * changed. When *setting* a GPO, 'x' means "leave this pin unchanged".
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AXIA_GPIO_PINS 5

/* Decoded GPI/GPO state line. Pin index 0 == pin 1. */
typedef struct {
    int port;                       /* 1-based LWRP port number */
    bool active[AXIA_GPIO_PINS];    /* true => pin is LOW (on/active) */
    bool changed[AXIA_GPIO_PINS];   /* true => char was UPPERCASE (just changed) */
} axia_gpio_line_t;

/* Desired value of a pin when formatting a GPO set command. */
typedef enum {
    AXIA_GPO_UNCHANGED = 0, /* 'x' — leave this pin as-is */
    AXIA_GPO_LOW,           /* 'l' — drive low (on/active) */
    AXIA_GPO_HIGH,          /* 'h' — drive high (off/inactive) */
} axia_gpo_pin_t;

/**
 * Parse a GPI notification line: "GPI <port> <5 pin chars>".
 * Tolerates leading whitespace; requires exactly AXIA_GPIO_PINS pin characters,
 * each in {H,h,L,l}. Rejects wrong prefix, bad port (<1), wrong pin count, or
 * any invalid pin character.
 * @return true on success (fills *out), false otherwise
 */
bool axia_protocol_parse_gpi(const char *line, axia_gpio_line_t *out);

/**
 * Format a GPO set command: "GPO <port> <5 chars>\n".
 * @param buffer Output buffer
 * @param buflen Size of buffer
 * @param port 1-based LWRP port number
 * @param pins Desired value for each of the AXIA_GPIO_PINS pins
 * @return Number of bytes written (>0), or 0 on invalid args / truncation
 */
int axia_protocol_format_gpo(char *buffer,
                             size_t buflen,
                             int port,
                             const axia_gpo_pin_t pins[AXIA_GPIO_PINS]);

/**
 * Format a LOGIN command: "LOGIN <password>\n".
 * @return Number of bytes written (>0), or 0 on invalid args / truncation
 */
int axia_protocol_format_login(char *buffer, size_t buflen, const char *password);

#endif /* QUADRATURE_CONTROLLER_INTERNAL_H */
