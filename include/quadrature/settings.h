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
 * Per-library configuration.
 * All per-library state lives here — no parallel arrays.
 */
typedef struct {
    char *path;           // Music folder root (required, heap-owned)
    char *name;           // Display name (NULL = use basename of path)
    char *data_path;      // Index/DB location (NULL = same as path)
    int   mb_resolve;     // -1 = inherit global, 0 = disabled, 1 = enabled
    int   acoustid;       // -1 = inherit global, 0 = disabled, 1 = enabled
    int   fanart;         // -1 = inherit global, 0 = disabled, 1 = enabled
    int   wikipedia;      // -1 = inherit global, 0 = disabled, 1 = enabled
    int   locked;         // 0 = unlocked, 1 = locked (prevents reorder + edit)
    int   library_index;  // Stable slot ID; persisted as sort key in settings.ini
} library_config_t;

/**
 * Application settings structure.
 * Loaded from and saved to settings.ini file.
 */
typedef struct {
    // Per-channel settings
    channel_config_t channels[APP_SETTINGS_MAX_CHANNELS];

    // Display settings
    gboolean show_spectrum;
    int time_warning_threshold_ms;

    // Library settings (global)
    gboolean auto_scan_on_startup;    // Auto-scan watch paths on startup (default: TRUE)
    gboolean process_artwork;         // Extract and process album artwork (default: TRUE)
    int indexer_thread_count;         // Number of indexer threads per library (0 = auto)
    int art_thumb_size;               // Album art thumbnail size in pixels (default: 48)
    int max_concurrent_library_scans; // Max libraries scanning simultaneously (default: 2)

    // Integration toggles (global defaults; per-library overrides in library_config_t)
    gboolean musicbrainz_resolve;     // Resolve MusicBrainz metadata after indexing
    gboolean fanart_resolve;          // Resolve fanart.tv artist images after indexing
    gboolean acoustid_fingerprint;    // Fingerprint tracks missing MUSICBRAINZ_ tags
    gboolean wikipedia_bios;          // Fetch artist biographies from Wikipedia

    // Integration connection strings
    char *musicbrainz_pg_conninfo;    // libpq conninfo for self-hosted MB PG (NULL → use HTTP)
    char *acoustid_pg_conninfo;       // libpq conninfo for AcoustID PostgreSQL
    char *acoustid_index_url;         // AcoustID index HTTP endpoint URL
    char *mb_solr_url;                // MusicBrainz Solr search URL (e.g., "http://host:8983")
    char *fanart_api_key;             // fanart.tv personal API key

    // Axia Livewire+ GPIO credentials (global for all channels)
    char *axia_username;              // LWRP username (NULL if not required)
    char *axia_password;              // LWRP password (NULL if not required)

    // Libraries — ordered array, position = display order.
    // library_config_t.library_index is the stable slot ID for cache/artwork.
    library_config_t *libraries;      // Heap-owned array (NULL if none)
    int               library_count;  // Number of configured libraries
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
char *app_settings_get_library_name(const app_settings_t *settings, int idx);

/**
 * Get effective data path for library at index.
 * Returns data_path if set, otherwise the library path itself.
 * Do not free the result.
 */
const char *app_settings_get_library_data_path(const app_settings_t *settings, int idx);

/**
 * Add a library with the given music path. Assigns the next available library_index.
 * No-op if the path is already present.
 * Does not save to disk.
 */
void app_settings_add_library(app_settings_t *settings, const char *path);

/**
 * Remove the library with the given music path. No-op if not found.
 * Does not save to disk.
 */
void app_settings_remove_library(app_settings_t *settings, const char *path);

/**
 * Swap two libraries by array position (display order).
 * Both positions must be valid (0 .. library_count-1).
 * Does not save to disk.
 */
void app_settings_swap_libraries(app_settings_t *settings, int pos_a, int pos_b);

/**
 * Find library array index by path. Returns -1 if not found.
 */
int app_settings_find_library(const app_settings_t *settings, const char *path);

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
