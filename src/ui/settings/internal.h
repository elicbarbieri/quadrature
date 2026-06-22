/**
 * Settings View / Device Settings / GPIO Bridge Internal Header
 *
 * Functions called from window.c to build settings views and manage GPIO.
 */

#pragma once

#include "../internal.h"

/* Settings view */
GtkWidget *make_settings_view(UiWindow *w);
GtkWidget *make_help_view(void);

/* Device settings */
GtkWidget *make_channel_settings_frame(UiWindow *w, int channel);
void populate_devices_async(UiWindow *w);
void setup_device_monitor(UiWindow *w);
void teardown_device_monitor(UiWindow *w);

/* Controller bridge — callbacks connected in window.c */
void restart_controller(UiWindow *w, int channel_id);
void on_gpio_changed(GtkEditable *editable, gpointer data);
void on_controller_command(int channel, control_command_t command, void *user_data);
void on_controller_status(int channel, bool connected, void *user_data);
void on_channel_mode_changed(UiChannelStrip *strip, int channel_id, int mode, gpointer user_data);
