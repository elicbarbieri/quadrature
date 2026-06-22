/**
 * Quadrature Controller — Generic Core
 *
 * Source-independent control surface shared by all backends:
 *  - opaque controller_t handle + lifecycle
 *  - callback storage and marshaling to the GLib main thread
 *  - the listener thread and reconnect/backoff state machine
 *
 * Backends supply behaviour through controller_driver_t; this file never
 * touches sockets, wire protocols, or input devices directly.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <string.h>
#include <unistd.h>

/* Forward declarations */
static void *listener_thread_func(void *arg);
static void dispatch_command(controller_t *c, int channel, control_command_t command);
static void dispatch_status(controller_t *c, bool connected);
static void exponential_backoff_sleep(controller_t *c);

/* ═══════════════════════════════════════════════════════════════════════════
 * Construction / Destruction
 * ═══════════════════════════════════════════════════════════════════════════ */

controller_t *
controller_core_new(const controller_driver_t *driver, void *ctx, int channel)
{
    if (!driver || !ctx)
        return NULL;

    controller_t *c = g_new0(controller_t, 1);
    c->driver = driver;
    c->ctx = ctx;
    c->channel = channel;

    atomic_init(&c->running, false);
    atomic_init(&c->connected, false);

    g_mutex_init(&c->callback_mutex);
    c->reconnect_delay_ms = RECONNECT_INITIAL_DELAY_MS;

    return c;
}

void
controller_destroy(controller_t *c)
{
    if (!c)
        return;

    g_debug("Controller: destroying handler for channel %d", c->channel + 1);

    if (atomic_load(&c->running))
        controller_stop(c);

    c->driver->destroy(c->ctx);
    g_mutex_clear(&c->callback_mutex);
    g_free(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback & Thread Control
 * ═══════════════════════════════════════════════════════════════════════════ */

void
controller_set_command_callback(controller_t *c,
                                controller_command_callback_t callback,
                                void *user_data)
{
    if (!c)
        return;

    g_mutex_lock(&c->callback_mutex);
    c->command_callback = callback;
    c->command_user_data = user_data;
    g_mutex_unlock(&c->callback_mutex);
}

void
controller_set_status_callback(controller_t *c,
                               controller_status_callback_t callback,
                               void *user_data)
{
    if (!c)
        return;

    g_mutex_lock(&c->callback_mutex);
    c->status_callback = callback;
    c->status_user_data = user_data;
    g_mutex_unlock(&c->callback_mutex);
}

quadrature_result_t
controller_start(controller_t *c)
{
    if (!c)
        return QUADRATURE_ERROR_INVALID_PARAM;

    if (atomic_load(&c->running)) {
        g_warning("Controller: channel %d already running", c->channel + 1);
        return QUADRATURE_OK;
    }

    atomic_store(&c->running, true);

    int result = pthread_create(&c->listener_thread, NULL, listener_thread_func, c);
    if (result != 0) {
        g_warning("Controller: channel %d failed to create listener thread: %s",
                  c->channel + 1,
                  strerror(result));
        atomic_store(&c->running, false);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_info("Controller: channel %d listener thread started", c->channel + 1);
    return QUADRATURE_OK;
}

quadrature_result_t
controller_stop(controller_t *c)
{
    if (!c)
        return QUADRATURE_ERROR_INVALID_PARAM;

    if (!atomic_load(&c->running))
        return QUADRATURE_OK;

    g_info("Controller: channel %d stopping listener thread", c->channel + 1);

    atomic_store(&c->running, false);

    /* Unblock the listener's read so it observes running == false. */
    c->driver->wake(c->ctx);

    pthread_join(c->listener_thread, NULL);

    c->driver->disconnect(c->ctx);
    atomic_store(&c->connected, false);

    g_info("Controller: channel %d listener thread stopped", c->channel + 1);
    return QUADRATURE_OK;
}

bool
controller_is_connected(const controller_t *c)
{
    if (!c)
        return false;
    return atomic_load(&c->connected);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * State Feedback (app -> source indicators)
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
controller_set_channel_state(controller_t *c, int channel, control_state_t state)
{
    if (!c)
        return QUADRATURE_ERROR_INVALID_PARAM;

    if (!atomic_load(&c->connected)) {
        g_debug("Controller: channel %d cannot set state (not connected)", c->channel + 1);
        return QUADRATURE_ERROR_DEVICE_BUSY;
    }

    if (!c->driver->set_state(c->ctx, channel, state)) {
        /* Transport failure — drop the connection so the listener reconnects. */
        atomic_store(&c->connected, false);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_info("Controller: channel %d state -> %d", channel + 1, state);
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Listener Thread + Reconnect State Machine
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *
listener_thread_func(void *arg)
{
    controller_t *c = (controller_t *)arg;

    g_debug("Controller: channel %d listener thread running", c->channel + 1);

    while (atomic_load(&c->running)) {
        /* Establish a full session (connect + auth + subscribe). */
        if (!c->driver->connect(c->ctx)) {
            exponential_backoff_sleep(c);
            continue;
        }

        /* Connected — reset backoff and announce. */
        c->reconnect_delay_ms = RECONNECT_INITIAL_DELAY_MS;
        atomic_store(&c->connected, true);
        dispatch_status(c, true);
        g_info("Controller: channel %d connected", c->channel + 1);

        /* Read loop. */
        while (atomic_load(&c->running) && atomic_load(&c->connected)) {
            if (!c->driver->pump(c->ctx, dispatch_command, c))
                break; /* disconnected or error */
        }

        atomic_store(&c->connected, false);
        c->driver->disconnect(c->ctx);
        dispatch_status(c, false);
        g_info("Controller: channel %d disconnected", c->channel + 1);
    }

    g_debug("Controller: channel %d listener thread exiting", c->channel + 1);
    return NULL;
}

static void
exponential_backoff_sleep(controller_t *c)
{
    if (!atomic_load(&c->running))
        return;

    g_debug("Controller: channel %d reconnecting in %dms", c->channel + 1, c->reconnect_delay_ms);

    /* Sleep in small chunks so shutdown is observed promptly. */
    int remaining_ms = c->reconnect_delay_ms;
    while (remaining_ms > 0 && atomic_load(&c->running)) {
        int sleep_ms = (remaining_ms > 100) ? 100 : remaining_ms;
        usleep(sleep_ms * 1000);
        remaining_ms -= sleep_ms;
    }

    c->reconnect_delay_ms *= RECONNECT_BACKOFF_FACTOR;
    if (c->reconnect_delay_ms > RECONNECT_MAX_DELAY_MS)
        c->reconnect_delay_ms = RECONNECT_MAX_DELAY_MS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback Dispatch (marshaled to the GLib main thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    controller_t *controller;
    int channel;
    control_command_t command;
} command_dispatch_t;

typedef struct {
    controller_t *controller;
    bool connected;
} status_dispatch_t;

static gboolean
invoke_command_on_main_thread(gpointer data)
{
    command_dispatch_t *d = (command_dispatch_t *)data;

    g_mutex_lock(&d->controller->callback_mutex);
    if (d->controller->command_callback) {
        d->controller->command_callback(d->channel, d->command, d->controller->command_user_data);
    }
    g_mutex_unlock(&d->controller->callback_mutex);

    g_free(d);
    return G_SOURCE_REMOVE;
}

static gboolean
invoke_status_on_main_thread(gpointer data)
{
    status_dispatch_t *d = (status_dispatch_t *)data;

    g_mutex_lock(&d->controller->callback_mutex);
    if (d->controller->status_callback) {
        d->controller->status_callback(
            d->controller->channel, d->connected, d->controller->status_user_data);
    }
    g_mutex_unlock(&d->controller->callback_mutex);

    g_free(d);
    return G_SOURCE_REMOVE;
}

/* Emitter handed to the backend's pump(). */
static void
dispatch_command(controller_t *c, int channel, control_command_t command)
{
    g_info("Controller: channel %d command %d", channel + 1, command);

    command_dispatch_t *d = g_new0(command_dispatch_t, 1);
    d->controller = c;
    d->channel = channel;
    d->command = command;
    g_main_context_invoke(NULL, invoke_command_on_main_thread, d);
}

static void
dispatch_status(controller_t *c, bool connected)
{
    status_dispatch_t *d = g_new0(status_dispatch_t, 1);
    d->controller = c;
    d->connected = connected;
    g_main_context_invoke(NULL, invoke_status_on_main_thread, d);
}
