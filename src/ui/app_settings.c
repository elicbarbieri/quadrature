/**
 * Quadrature Application Settings
 *
 * GKeyFile-based settings persistence.
 */

#include "../../include/quadrature/ui/app_settings.h"
#include <stdio.h>

#define CONFIG_DIR "quadrature"
#define CONFIG_FILE "settings.ini"

#define GROUP_CHANNEL_FMT "Channel%d"
#define GROUP_DISPLAY "Display"
#define GROUP_LIBRARY "Library"

#define KEY_DEVICE "device"
#define KEY_ENABLED "enabled"
#define KEY_OUTPUT_FORMAT "output_format"
#define KEY_GPIO_ADDRESS "gpio_address"

#define KEY_SHOW_SPECTRUM "show_spectrum"
#define KEY_TIME_WARNING "time_warning_threshold"

#define KEY_AUTO_SCAN "auto_scan_on_startup"
#define KEY_PROCESS_ARTWORK "process_artwork"
#define KEY_INDEXER_THREADS "indexer_threads"

#define DEFAULT_TIME_WARNING_MS 30000

/* Output format names for display and serialization */
static const char *OUTPUT_FORMAT_NAMES[] = {
    "16-bit 44.1 kHz",
    "16-bit 48.0 kHz",
    "24-bit 44.1 kHz",
    "24-bit 48.0 kHz",
    "24-bit 96.0 kHz"
};

char *app_settings_get_path(void) {
    const char *config_home = g_get_user_config_dir();
    return g_build_filename(config_home, CONFIG_DIR, CONFIG_FILE, NULL);
}

static char *get_config_dir(void) {
    const char *config_home = g_get_user_config_dir();
    return g_build_filename(config_home, CONFIG_DIR, NULL);
}

static void init_defaults(app_settings_t *settings) {
    for (int i = 0; i < APP_SETTINGS_MAX_CHANNELS; i++) {
        settings->channels[i].device_name = NULL;
        settings->channels[i].enabled = FALSE;
        settings->channels[i].output_format = OUTPUT_FORMAT_16BIT_48000;  // Broadcast default
        settings->channels[i].livewire_gpio_address = NULL;
    }
    settings->show_spectrum = TRUE;
    settings->time_warning_threshold_ms = DEFAULT_TIME_WARNING_MS;

    // Library defaults
    settings->auto_scan_on_startup = TRUE;
    settings->process_artwork = TRUE;
    settings->indexer_thread_count = 0;  // Auto
}

app_settings_t *app_settings_load(void) {
    app_settings_t *settings = g_new0(app_settings_t, 1);
    init_defaults(settings);

    char *path = app_settings_get_path();
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning("Failed to load settings from %s: %s", path, error->message);
        }
        g_clear_error(&error);
        g_key_file_free(keyfile);
        g_free(path);
        return settings;
    }

    // Load per-channel settings (each channel has its own group)
    for (int i = 0; i < APP_SETTINGS_MAX_CHANNELS; i++) {
        char group[32];
        snprintf(group, sizeof(group), GROUP_CHANNEL_FMT, i + 1);  // Channel1, Channel2, etc.

        // Device name
        char *device = g_key_file_get_string(keyfile, group, KEY_DEVICE, NULL);
        if (device && device[0] != '\0') {
            settings->channels[i].device_name = device;
        } else {
            g_free(device);
            settings->channels[i].device_name = NULL;
        }

        // Enabled state
        settings->channels[i].enabled = g_key_file_get_boolean(keyfile, group, KEY_ENABLED, &error);
        if (error) {
            settings->channels[i].enabled = (settings->channels[i].device_name != NULL);
            g_clear_error(&error);
        }

        // Output format
        int format = g_key_file_get_integer(keyfile, group, KEY_OUTPUT_FORMAT, &error);
        if (!error && format >= 0 && format < OUTPUT_FORMAT_COUNT) {
            settings->channels[i].output_format = (output_format_t)format;
        }
        g_clear_error(&error);

        // Livewire+ GPIO address
        char *gpio = g_key_file_get_string(keyfile, group, KEY_GPIO_ADDRESS, NULL);
        if (gpio && gpio[0] != '\0') {
            settings->channels[i].livewire_gpio_address = gpio;
        } else {
            g_free(gpio);
            settings->channels[i].livewire_gpio_address = NULL;
        }
    }

    // Load display settings
    gboolean show_spectrum = g_key_file_get_boolean(keyfile, GROUP_DISPLAY, KEY_SHOW_SPECTRUM, &error);
    if (!error) {
        settings->show_spectrum = show_spectrum;
    }
    g_clear_error(&error);

    int time_warning = g_key_file_get_integer(keyfile, GROUP_DISPLAY, KEY_TIME_WARNING, &error);
    if (!error && time_warning > 0) {
        settings->time_warning_threshold_ms = time_warning;
    }
    g_clear_error(&error);

    // Load library settings
    gboolean auto_scan = g_key_file_get_boolean(keyfile, GROUP_LIBRARY, KEY_AUTO_SCAN, &error);
    if (!error) {
        settings->auto_scan_on_startup = auto_scan;
    }
    g_clear_error(&error);

    gboolean process_artwork = g_key_file_get_boolean(keyfile, GROUP_LIBRARY, KEY_PROCESS_ARTWORK, &error);
    if (!error) {
        settings->process_artwork = process_artwork;
    }
    g_clear_error(&error);

    int indexer_threads = g_key_file_get_integer(keyfile, GROUP_LIBRARY, KEY_INDEXER_THREADS, &error);
    if (!error && indexer_threads >= 0) {
        settings->indexer_thread_count = indexer_threads;
    }
    g_clear_error(&error);

    g_key_file_free(keyfile);
    g_free(path);

    g_message("Loaded settings from disk");
    return settings;
}

gboolean app_settings_save(const app_settings_t *settings) {
    if (!settings) return FALSE;

    // Ensure config directory exists
    char *config_dir = get_config_dir();
    if (g_mkdir_with_parents(config_dir, 0755) != 0) {
        g_warning("Failed to create config directory: %s", config_dir);
        g_free(config_dir);
        return FALSE;
    }
    g_free(config_dir);

    GKeyFile *keyfile = g_key_file_new();

    // Save per-channel settings (each channel has its own group)
    for (int i = 0; i < APP_SETTINGS_MAX_CHANNELS; i++) {
        char group[32];
        snprintf(group, sizeof(group), GROUP_CHANNEL_FMT, i + 1);  // Channel1, Channel2, etc.

        g_key_file_set_string(keyfile, group, KEY_DEVICE,
                              settings->channels[i].device_name ? settings->channels[i].device_name : "");
        g_key_file_set_boolean(keyfile, group, KEY_ENABLED, settings->channels[i].enabled);
        g_key_file_set_integer(keyfile, group, KEY_OUTPUT_FORMAT, (int)settings->channels[i].output_format);
        g_key_file_set_string(keyfile, group, KEY_GPIO_ADDRESS,
                              settings->channels[i].livewire_gpio_address ? settings->channels[i].livewire_gpio_address : "");
    }

    // Save display settings
    g_key_file_set_boolean(keyfile, GROUP_DISPLAY, KEY_SHOW_SPECTRUM, settings->show_spectrum);
    g_key_file_set_integer(keyfile, GROUP_DISPLAY, KEY_TIME_WARNING, settings->time_warning_threshold_ms);

    // Save library settings
    g_key_file_set_boolean(keyfile, GROUP_LIBRARY, KEY_AUTO_SCAN, settings->auto_scan_on_startup);
    g_key_file_set_boolean(keyfile, GROUP_LIBRARY, KEY_PROCESS_ARTWORK, settings->process_artwork);
    g_key_file_set_integer(keyfile, GROUP_LIBRARY, KEY_INDEXER_THREADS, settings->indexer_thread_count);

    // Write to file
    char *path = app_settings_get_path();
    GError *error = NULL;
    gboolean success = g_key_file_save_to_file(keyfile, path, &error);

    if (!success) {
        g_warning("Failed to save settings to %s: %s", path, error->message);
        g_error_free(error);
    } else {
        g_debug("Saved settings to %s", path);
    }

    g_key_file_free(keyfile);
    g_free(path);

    return success;
}

void app_settings_free(app_settings_t *settings) {
    if (!settings) return;

    for (int i = 0; i < APP_SETTINGS_MAX_CHANNELS; i++) {
        g_free(settings->channels[i].device_name);
        g_free(settings->channels[i].livewire_gpio_address);
    }
    g_free(settings);
}

void app_settings_set_channel_device(app_settings_t *settings, int channel, const char *device_name) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return;

    g_free(settings->channels[channel].device_name);

    if (device_name && device_name[0] != '\0') {
        settings->channels[channel].device_name = g_strdup(device_name);
        settings->channels[channel].enabled = TRUE;
    } else {
        settings->channels[channel].device_name = NULL;
        settings->channels[channel].enabled = FALSE;
    }
}

const char *app_settings_get_channel_device(const app_settings_t *settings, int channel) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return NULL;
    return settings->channels[channel].device_name;
}

void app_settings_set_channel_format(app_settings_t *settings, int channel, output_format_t format) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return;
    if (format < 0 || format >= OUTPUT_FORMAT_COUNT) return;
    settings->channels[channel].output_format = format;
}

output_format_t app_settings_get_channel_format(const app_settings_t *settings, int channel) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return OUTPUT_FORMAT_16BIT_48000;
    return settings->channels[channel].output_format;
}

void app_settings_set_channel_gpio(app_settings_t *settings, int channel, const char *address) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return;

    g_free(settings->channels[channel].livewire_gpio_address);

    if (address && address[0] != '\0') {
        settings->channels[channel].livewire_gpio_address = g_strdup(address);
    } else {
        settings->channels[channel].livewire_gpio_address = NULL;
    }
}

const char *app_settings_get_channel_gpio(const app_settings_t *settings, int channel) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return NULL;
    return settings->channels[channel].livewire_gpio_address;
}

const char *app_settings_format_name(output_format_t format) {
    if (format < 0 || format >= OUTPUT_FORMAT_COUNT) return "Unknown";
    return OUTPUT_FORMAT_NAMES[format];
}
