/**
 * Quadrature Controller Bridge
 *
 * Wires channel control sources (e.g. an Axia console) to the UI: lifecycle,
 * command handling (source -> app), and channel-state feedback (app -> source).
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "internal.h"
#include <string.h>

void
on_gpio_changed(GtkEditable *editable, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);

    if (w->settings_initializing)
        return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(editable) == w->gpio_entries[i]) {
            ch = i;
            break;
        }
    }
    if (ch < 0)
        return;

    const char *text = gtk_editable_get_text(editable);
    if (w->settings) {
        app_settings_set_channel_gpio(w->settings, ch, text);
        settings_save_debounced(w);
    }

    /* Restart the controller with the new address */
    restart_controller(w, ch);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Control Command Handling (source -> app)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Map a channel's playback/preview state to a control_state_t for indicators. */
static control_state_t
channel_mode_to_control_state(ChannelMode mode)
{
    switch (mode) {
    case CHANNEL_MODE_PREVIEW:
        return CONTROL_STATE_PREVIEW;
    case CHANNEL_MODE_QUEUED:
        return CONTROL_STATE_QUEUED;
    case CHANNEL_MODE_ON_AIR:
        return CONTROL_STATE_ON_AIR;
    case CHANNEL_MODE_IDLE:
    default:
        return CONTROL_STATE_IDLE;
    }
}

/* Called when a control source issues a command for a channel. */
void
on_controller_command(int channel, control_command_t command, void *user_data)
{
    UiWindow *w = (UiWindow *)user_data;

    g_info("Controller command: channel=%d, command=%d", channel + 1, command);

    UiChannelStrip *strip = w->channels[channel];
    if (!strip)
        return;

    switch (command) {
    case CONTROL_CMD_PLAY_PAUSE: {
        channel_state_t player_state = ui_channel_strip_get_player_state(strip);
        if (player_state == CHANNEL_PLAYING) {
            ui_channel_strip_stop(strip);
            g_info("Controller -> Channel %d: Pause", channel + 1);
        } else {
            ui_channel_strip_play(strip);
            g_info("Controller -> Channel %d: Play", channel + 1);
        }
        break;
    }

    case CONTROL_CMD_PREVIEW_TOGGLE: {
        bool preview_active = ui_channel_strip_get_preview_active(strip);
        if (preview_active) {
            ui_channel_strip_preview_off(strip);
            g_info("Controller -> Channel %d: Preview OFF", channel + 1);
        } else {
            ui_channel_strip_preview_on(strip);
            g_info("Controller -> Channel %d: Preview ON", channel + 1);
        }
        break;
    }

    case CONTROL_CMD_ON_AIR:
        ui_channel_strip_play(strip);
        g_info("Controller -> Channel %d: On air", channel + 1);
        break;

    case CONTROL_CMD_SKIP:
        ui_channel_strip_next_track(strip);
        g_info("Controller -> Channel %d: Skip", channel + 1);
        break;

    default:
        g_warning("Controller: unknown command %d", command);
        break;
    }
}

/* Called when a control source's connection status changes */
void
on_controller_status(int channel, bool connected, void *user_data)
{
    UiWindow *w = (UiWindow *)user_data;

    g_info("Controller status: channel=%d, connected=%s", channel + 1, connected ? "YES" : "NO");

    UiChannelStrip *strip = w->channels[channel];
    if (!strip)
        return;

    if (connected) {
        /* Connected — restore normal device state */
        const char *device = app_settings_get_channel_device(w->settings, channel);
        if (device && device[0] != '\0') {
            ui_channel_strip_set_device_state(strip, DEVICE_STATE_VALID);
        } else {
            ui_channel_strip_set_device_state(strip, DEVICE_STATE_UNCONFIGURED);
        }
    } else {
        /* Disconnected — mark as invalid */
        ui_channel_strip_set_device_state(strip, DEVICE_STATE_INVALID);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Channel State Feedback (app -> source)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Called when a channel mode changes; mirror it on the control source. */
void
on_channel_mode_changed(UiChannelStrip *strip, int channel_id, int mode, gpointer user_data)
{
    (void)strip;
    UiWindow *w = (UiWindow *)user_data;
    controller_t *controller = w->controllers[channel_id];

    if (!controller || !controller_is_connected(controller))
        return;

    g_info("Mode changed: channel=%d, mode=%d -> updating source", channel_id + 1, mode);

    controller_set_channel_state(
        controller, channel_id, channel_mode_to_control_state((ChannelMode)mode));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Start or restart the control source for a channel */
void
restart_controller(UiWindow *w, int channel_id)
{
    /* Stop existing controller */
    if (w->controllers[channel_id]) {
        controller_stop(w->controllers[channel_id]);
        controller_destroy(w->controllers[channel_id]);
        w->controllers[channel_id] = NULL;
    }

    /* Get the console address from settings */
    const char *gpio_addr = app_settings_get_channel_gpio(w->settings, channel_id);
    if (!gpio_addr || !gpio_addr[0]) {
        g_info("Channel %d: controller address cleared", channel_id + 1);
        return; /* No address configured */
    }

    /* Global Axia password (LWRP LOGIN command) */
    const char *password = app_settings_get_axia_password(w->settings);

    /* Create the Axia-backed controller */
    controller_t *controller = NULL;
    quadrature_result_t res = controller_axia_create(gpio_addr, channel_id, password, &controller);
    if (res != QUADRATURE_OK) {
        g_warning("Failed to create controller for channel %d: %d", channel_id + 1, res);
        return;
    }

    controller_set_command_callback(controller, on_controller_command, w);
    controller_set_status_callback(controller, on_controller_status, w);

    res = controller_start(controller);
    if (res != QUADRATURE_OK) {
        g_warning("Failed to start controller for channel %d: %d", channel_id + 1, res);
        controller_destroy(controller);
        return;
    }

    w->controllers[channel_id] = controller;
    g_info("Channel %d: controller started (%s)", channel_id + 1, gpio_addr);
}
