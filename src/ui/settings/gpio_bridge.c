/**
 * Quadrature GPIO Bridge
 *
 * Axia GPIO handler lifecycle, event callbacks, status monitoring.
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

    /* Restart GPIO handler with new address */
    restart_gpio_handler(w, ch);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Axia GPIO Integration
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Called when Axia console button is pressed (console → app) */
void
on_axia_gpio_event(int channel_id, axia_pin_t pin, axia_state_t state, void *user_data)
{
    UiWindow *w = (UiWindow *)user_data;

    g_info("Axia GPIO Event: channel=%d, pin=%d, state=%s",
           channel_id + 1,
           pin,
           state == AXIA_STATE_HIGH ? "HIGH" : "LOW");

    UiChannelStrip *strip = w->channels[channel_id];
    if (!strip)
        return;

    /* Only respond to rising edge (button press, not release) */
    if (state != AXIA_STATE_HIGH)
        return;

    switch (pin) {
    case AXIA_PIN_ON_AIR:
        /* Fader ON/OFF from console */
        {
            channel_state_t player_state = ui_channel_strip_get_player_state(strip);
            if (player_state == CHANNEL_PLAYING) {
                ui_channel_strip_stop(strip);
                g_info("Axia → Channel %d: Stop (fader off)", channel_id + 1);
            } else {
                ui_channel_strip_play(strip);
                g_info("Axia → Channel %d: Play (fader on)", channel_id + 1);
            }
        }
        break;

    case AXIA_PIN_PREVIEW:
        /* Toggle preview from console */
        {
            bool preview_active = ui_channel_strip_get_preview_active(strip);
            if (preview_active) {
                ui_channel_strip_preview_off(strip);
                g_info("Axia → Channel %d: Preview OFF", channel_id + 1);
            } else {
                ui_channel_strip_preview_on(strip);
                g_info("Axia → Channel %d: Preview ON", channel_id + 1);
            }
        }
        break;

    default:
        g_warning("Axia GPIO: Unknown pin %d", pin);
        break;
    }
}

/* Called when GPIO connection status changes */
void
on_axia_status_changed(int channel_id, bool connected, void *user_data)
{
    UiWindow *w = (UiWindow *)user_data;

    g_info("Axia Status: channel=%d, connected=%s", channel_id + 1, connected ? "YES" : "NO");

    UiChannelStrip *strip = w->channels[channel_id];
    if (!strip)
        return;

    /* Update device state based on GPIO connection status */
    if (connected) {
        /* GPIO connected - restore normal device state */
        const char *device = app_settings_get_channel_device(w->settings, channel_id);
        if (device && device[0] != '\0') {
            ui_channel_strip_set_device_state(strip, DEVICE_STATE_VALID);
        } else {
            ui_channel_strip_set_device_state(strip, DEVICE_STATE_UNCONFIGURED);
        }
    } else {
        /* GPIO disconnected - mark as invalid */
        ui_channel_strip_set_device_state(strip, DEVICE_STATE_INVALID);
    }
}

/* Called when channel mode changes (app → console LED feedback) */
void
on_channel_mode_changed(UiChannelStrip *strip, int channel_id, int mode, gpointer user_data)
{
    (void)strip;
    UiWindow *w = (UiWindow *)user_data;
    axia_gpio_t *gpio = w->gpio_handlers[channel_id];

    if (!gpio || !axia_gpio_is_connected(gpio))
        return;

    g_info("Mode Changed: channel=%d, mode=%d → updating console LEDs", channel_id + 1, mode);

    /* Update console LEDs based on new mode */
    switch ((ChannelMode)mode) {
    case CHANNEL_MODE_IDLE:
        axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_LOW);
        axia_gpio_set(gpio, AXIA_PIN_PREVIEW, AXIA_STATE_LOW);
        break;

    case CHANNEL_MODE_PREVIEW:
        axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_LOW);
        axia_gpio_set(gpio, AXIA_PIN_PREVIEW, AXIA_STATE_HIGH);
        break;

    case CHANNEL_MODE_QUEUED:
        /* QUEUED doesn't have a dedicated LED - just turn off ON_AIR */
        axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_LOW);
        axia_gpio_set(gpio, AXIA_PIN_PREVIEW, AXIA_STATE_LOW);
        break;

    case CHANNEL_MODE_ON_AIR:
        axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_HIGH);
        axia_gpio_set(gpio, AXIA_PIN_PREVIEW, AXIA_STATE_LOW);
        break;
    }
}

/* Start or restart GPIO handler for a channel */
void
restart_gpio_handler(UiWindow *w, int channel_id)
{
    /* Stop existing handler */
    if (w->gpio_handlers[channel_id]) {
        axia_gpio_stop(w->gpio_handlers[channel_id]);
        axia_gpio_destroy(w->gpio_handlers[channel_id]);
        w->gpio_handlers[channel_id] = NULL;
    }

    /* Get GPIO address from settings */
    const char *gpio_addr = app_settings_get_channel_gpio(w->settings, channel_id);
    if (!gpio_addr || !gpio_addr[0]) {
        g_info("Channel %d: GPIO address cleared", channel_id + 1);
        return; /* No address configured */
    }

    /* Get global Axia password (LWRP LOGIN command) */
    const char *password = app_settings_get_axia_password(w->settings);

    /* Create new handler */
    axia_gpio_t *gpio = NULL;
    quadrature_result_t res = axia_gpio_create(gpio_addr, channel_id, password, &gpio);
    if (res != QUADRATURE_OK) {
        g_warning("Failed to create Axia GPIO for channel %d: %d", channel_id + 1, res);
        return;
    }

    /* Set callbacks */
    axia_gpio_set_callback(gpio, on_axia_gpio_event, w);
    axia_gpio_set_status_callback(gpio, on_axia_status_changed, w);

    /* Start listener thread */
    res = axia_gpio_start(gpio);
    if (res != QUADRATURE_OK) {
        g_warning("Failed to start Axia GPIO for channel %d: %d", channel_id + 1, res);
        axia_gpio_destroy(gpio);
        return;
    }

    w->gpio_handlers[channel_id] = gpio;
    g_info("Channel %d: Axia GPIO started (%s)", channel_id + 1, gpio_addr);
}
