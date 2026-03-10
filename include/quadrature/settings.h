/**
 * Quadrature Application Settings
 *
 * GKeyFile-based settings persistence for user preferences.
 * Settings are stored in ~/.config/quadrature/settings.ini
 */

#ifndef QUADRATURE_SETTINGS_H
#define QUADRATURE_SETTINGS_H

#include <glib.h>
#include <stdint.h>
#include "quadrature/quadrature.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_MAX_CHANNELS 4
#define APP_SETTINGS_DEFAULT_QUANTUM 256

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
    gboolean exclusive;              // PipeWire exclusive mode (default: TRUE)
    output_format_t output_format;   // audio output format
    uint32_t quantum_frames;         // PipeWire quantum / buffer size (default: 512)
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
    int indexer_thread_count;         // Number of indexer threads per library (0 = auto)
    int art_thumb_size;               // Album art thumbnail size in pixels (default: 48)
    int max_concurrent_library_scans; // Max libraries scanning simultaneously (default: 2)

    // Integration toggles (global defaults; per-library overrides below)
    gboolean musicbrainz_resolve;     // Resolve MusicBrainz metadata after indexing
    gboolean fanart_resolve;          // Resolve fanart.tv artist images after indexing
    gboolean acoustid_fingerprint;    // Fingerprint tracks missing MUSICBRAINZ_ tags
    gboolean wikipedia_bios;          // Fetch artist biographies from Wikipedia

    // Integration connection strings
    char *musicbrainz_pg_conninfo;    // libpq conninfo for MusicBrainz PostgreSQL
    char *acoustid_pg_conninfo;       // libpq conninfo for AcoustID PostgreSQL
    char *acoustid_index_url;         // AcoustID index HTTP endpoint URL
    char *fanart_api_key;             // fanart.tv personal API key
    
    // Axia Livewire+ GPIO credentials (global for all channels)
    char *axia_username;              // LWRP username (NULL if not required)
    char *axia_password;              // LWRP password (NULL if not required)

    // Library root paths (client-side; not stored in SQLite)
    char **library_paths;             // NULL-terminated array of library root paths (heap-owned)
    int    library_path_count;        // Number of entries in library_paths

    // Library display names (parallel to library_paths[]; NULL entry = use basename)
    char **library_names;             // Heap-owned; may be NULL or shorter than library_paths
    int    library_name_count;        // Number of entries in library_names

    // Library data paths (parallel to library_paths[]; NULL entry = use library_path)
    // For read-only library roots, point this to a writable location for DB + artwork
    char **library_data_paths;        // Heap-owned; may be NULL or shorter than library_paths
    int    library_data_path_count;   // Number of entries in library_data_paths

    // Per-library integration overrides (parallel to library_paths[])
    // Values: -1 = inherit global default, 0 = disabled, 1 = enabled
    int   *library_mb_resolve;
    int    library_mb_resolve_count;
    int   *library_acoustid;
    int    library_acoustid_count;
    int   *library_fanart;
    int    library_fanart_count;
    int   *library_wikipedia;
    int    library_wikipedia_count;
} app_settings_t;

/**
 * Create a new settings structure with defaults.
 * @return New settings structure (caller must free with app_settings_destroy())
 */
app_settings_t *app_settings_create(void);

/**
 * Free a settings structure and all its data.
 * @param settings Settings structure to free
 */
void app_settings_destroy(app_settings_t *settings);

/**
 * Load settings from disk (~/.config/quadrature/settings.ini).
 * @return New settings structure (caller must free), or NULL on error
 */
app_settings_t *app_settings_load(void);

/**
 * Save settings to disk (~/.config/quadrature/settings.ini).
 * @param settings Settings to save
 * @return TRUE on success, FALSE on error
 */
gboolean app_settings_save(const app_settings_t *settings);

/**
 * Get a channel's device name (PipeWire node.name).
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @return Device name (NULL if not configured), do not free
 */
const char *app_settings_get_channel_device(const app_settings_t *settings, int channel);

/**
 * Set a channel's device name (PipeWire node.name).
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @param device_name Device name (NULL to clear)
 */
void app_settings_set_channel_device(app_settings_t *settings, int channel, const char *device_name);

/**
 * Get a channel's exclusive mode setting.
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @return TRUE if exclusive mode enabled
 */
gboolean app_settings_get_channel_exclusive(const app_settings_t *settings, int channel);

/**
 * Set a channel's exclusive mode setting.
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @param exclusive TRUE to enable exclusive mode
 */
void app_settings_set_channel_exclusive(app_settings_t *settings, int channel, gboolean exclusive);

/**
 * Get a channel's output format.
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @return Output format preset
 */
output_format_t app_settings_get_channel_format(const app_settings_t *settings, int channel);

/**
 * Set a channel's output format.
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @param format Output format preset
 */
void app_settings_set_channel_format(app_settings_t *settings, int channel, output_format_t format);

/**
 * Get a channel's Livewire+ GPIO address.
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @return GPIO address (NULL if not configured), do not free
 */
const char *app_settings_get_channel_gpio(const app_settings_t *settings, int channel);

/**
 * Set a channel's Livewire+ GPIO address.
 * @param settings Settings structure
 * @param channel Channel index (0-3)
 * @param address GPIO address (NULL to clear)
 */
void app_settings_set_channel_gpio(app_settings_t *settings, int channel, const char *address);

/**
 * Set Axia Livewire+ GPIO username (global for all channels).
 * @param settings Settings structure
 * @param username Username (NULL to clear)
 */
void app_settings_set_axia_username(app_settings_t *settings, const char *username);

/**
 * Get Axia Livewire+ GPIO username.
 * @param settings Settings structure
 * @return Username (NULL if not configured), do not free
 */
const char *app_settings_get_axia_username(const app_settings_t *settings);

/**
 * Set Axia Livewire+ GPIO password (global for all channels).
 * @param settings Settings structure
 * @param password Password (NULL to clear)
 */
void app_settings_set_axia_password(app_settings_t *settings, const char *password);

/**
 * Get Axia Livewire+ GPIO password.
 * @param settings Settings structure
 * @return Password (NULL if not configured), do not free
 */
const char *app_settings_get_axia_password(const app_settings_t *settings);

/**
 * Get display name for library at index.
 * Returns the custom name if set, otherwise the basename of the path.
 * Caller must g_free() the result.
 */
char *app_settings_get_effective_library_name(const app_settings_t *settings, int idx);

/**
 * Set the display name for library at index.
 * Pass NULL or "" to clear (falls back to basename).
 * Does not save to disk -- caller must call app_settings_save().
 */
void app_settings_set_library_name(app_settings_t *settings, int idx, const char *name);

/**
 * Get data path for library at index.
 * Returns the custom data path if set, otherwise the library path itself.
 * Do not free the result.
 */
const char *app_settings_get_library_data_path(const app_settings_t *settings, int idx);

/**
 * Set the data path for library at index (where DB + artwork are stored).
 * Pass NULL or "" to clear (falls back to library path).
 * Does not save to disk -- caller must call app_settings_save().
 */
void app_settings_set_library_data_path(app_settings_t *settings, int idx, const char *path);

/**
 * Per-library integration overrides.
 * Getter returns: -1 = inherit global, 0 = disabled, 1 = enabled.
 * Setter accepts the same values.
 */
int  app_settings_get_library_mb_resolve(const app_settings_t *settings, int idx);
void app_settings_set_library_mb_resolve(app_settings_t *settings, int idx, int value);
int  app_settings_get_library_acoustid(const app_settings_t *settings, int idx);
void app_settings_set_library_acoustid(app_settings_t *settings, int idx, int value);
int  app_settings_get_library_fanart(const app_settings_t *settings, int idx);
void app_settings_set_library_fanart(app_settings_t *settings, int idx, int value);
int  app_settings_get_library_wikipedia(const app_settings_t *settings, int idx);
void app_settings_set_library_wikipedia(app_settings_t *settings, int idx, int value);

/**
 * Add a library root path (no-op if already present; copies the string).
 *
 * @param settings Settings to modify
 * @param path Library root path
 */
void app_settings_add_library_path(app_settings_t *settings, const char *path);

/**
 * Remove a library root path (no-op if not present).
 *
 * @param settings Settings to modify
 * @param path Library root path to remove
 */
void app_settings_remove_library_path(app_settings_t *settings, const char *path);

/**
 * Set a channel's PipeWire quantum (buffer size in frames).
 *
 * @param settings Settings to modify
 * @param channel Channel index (0-3)
 * @param quantum_frames Buffer size (must be power-of-2, 32-2048)
 */
void app_settings_set_channel_quantum(app_settings_t *settings, int channel, uint32_t quantum_frames);

/**
 * Get a channel's PipeWire quantum (buffer size in frames).
 *
 * @param settings Settings to read
 * @param channel Channel index (0-3)
 * @return Quantum in frames (default: 512)
 */
uint32_t app_settings_get_channel_quantum(const app_settings_t *settings, int channel);

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

#endif // QUADRATURE_SETTINGS_H
