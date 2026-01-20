/**
 * Quadrature Application Settings
 *
 * GKeyFile-based settings persistence for user preferences.
 * Settings are stored in ~/.config/quadrature/settings.ini
 */

#ifndef QUADRATURE_APP_SETTINGS_H
#define QUADRATURE_APP_SETTINGS_H

#include <glib.h>
#include "../core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_MAX_CHANNELS 4

/**
 * Audio output format presets.
 * Index corresponds to dropdown selection order.
 */
typedef enum {
    OUTPUT_FORMAT_16BIT_44100 = 0,   // CD quality
    OUTPUT_FORMAT_16BIT_48000,       // Standard broadcast
    OUTPUT_FORMAT_24BIT_44100,       // High-res CD rate
    OUTPUT_FORMAT_24BIT_48000,       // High-res broadcast
    OUTPUT_FORMAT_24BIT_96000,       // Studio quality
    OUTPUT_FORMAT_COUNT
} output_format_t;

/**
 * Per-channel configuration structure.
 */
typedef struct {
    char *device_name;               // node.name (NULL = unconfigured)
    gboolean enabled;                // explicit enable state
    output_format_t output_format;   // audio output format
    char *livewire_gpio_address;     // Livewire+ GPIO address (NULL = not configured)
} channel_config_t;

/**
 * Application settings structure.
 * Loaded from and saved to settings.ini file.
 */
typedef struct {
    // Per-channel settings (replaces flat arrays)
    channel_config_t channels[APP_SETTINGS_MAX_CHANNELS];

    // Display settings
    gboolean show_spectrum;
    int time_warning_threshold_ms;

    // Library settings
    gboolean auto_scan_on_startup;    // Auto-scan watch paths on startup (default: TRUE)
    gboolean process_artwork;         // Extract and process album artwork (default: TRUE)
    int indexer_thread_count;         // Number of indexer threads (0 = auto)
} app_settings_t;

/**
 * Load settings from disk.
 * Creates default settings if file doesn't exist.
 *
 * @return Newly allocated settings (caller must free with app_settings_free)
 */
app_settings_t *app_settings_load(void);

/**
 * Save settings to disk.
 * Creates config directory if needed.
 *
 * @param settings Settings to save
 * @return TRUE on success
 */
gboolean app_settings_save(const app_settings_t *settings);

/**
 * Free settings structure.
 *
 * @param settings Settings to free (may be NULL)
 */
void app_settings_free(app_settings_t *settings);

/**
 * Get the path to the settings file.
 *
 * @return Newly allocated path string (caller must g_free)
 */
char *app_settings_get_path(void);

/**
 * Set a channel's device (copies the string).
 *
 * @param settings Settings to modify
 * @param channel Channel index (0-3)
 * @param device_name Device node.name (NULL to clear)
 */
void app_settings_set_channel_device(app_settings_t *settings, int channel, const char *device_name);

/**
 * Get a channel's device.
 *
 * @param settings Settings to read
 * @param channel Channel index (0-3)
 * @return Device node.name (NULL if unconfigured), do not free
 */
const char *app_settings_get_channel_device(const app_settings_t *settings, int channel);

/**
 * Set a channel's output format.
 *
 * @param settings Settings to modify
 * @param channel Channel index (0-3)
 * @param format Output format preset
 */
void app_settings_set_channel_format(app_settings_t *settings, int channel, output_format_t format);

/**
 * Get a channel's output format.
 *
 * @param settings Settings to read
 * @param channel Channel index (0-3)
 * @return Output format preset
 */
output_format_t app_settings_get_channel_format(const app_settings_t *settings, int channel);

/**
 * Set a channel's Livewire+ GPIO address (copies the string).
 *
 * @param settings Settings to modify
 * @param channel Channel index (0-3)
 * @param address GPIO address (NULL to clear)
 */
void app_settings_set_channel_gpio(app_settings_t *settings, int channel, const char *address);

/**
 * Get a channel's Livewire+ GPIO address.
 *
 * @param settings Settings to read
 * @param channel Channel index (0-3)
 * @return GPIO address (NULL if not configured), do not free
 */
const char *app_settings_get_channel_gpio(const app_settings_t *settings, int channel);

/**
 * Get human-readable name for an output format.
 *
 * @param format Output format preset
 * @return Static string describing the format
 */
const char *app_settings_format_name(output_format_t format);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_APP_SETTINGS_H
