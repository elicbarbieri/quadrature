/**
 * Quadrature Application Settings
 *
 * GKeyFile-based settings persistence.
 */

#include "quadrature/settings.h"
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
#define KEY_QUANTUM "quantum"
#define KEY_EXCLUSIVE "exclusive"

/* Global Axia GPIO credentials */
#define GROUP_AXIA "axia"
#define KEY_AXIA_USERNAME "username"
#define KEY_AXIA_PASSWORD "password"

#define DEFAULT_QUANTUM_FRAMES APP_SETTINGS_DEFAULT_QUANTUM

#define KEY_SHOW_SPECTRUM "show_spectrum"
#define KEY_TIME_WARNING "time_warning_threshold"

#define KEY_AUTO_SCAN "auto_scan_on_startup"
#define KEY_PROCESS_ARTWORK "process_artwork"
#define KEY_INDEXER_THREADS "indexer_threads"
#define KEY_ART_THUMB_SIZE "art_thumb_size"
#define KEY_MAX_CONCURRENT_SCANS "max_concurrent_library_scans"

#define GROUP_MUSICBRAINZ "MusicBrainz"
#define KEY_MB_RESOLVE "musicbrainz_resolve"
#define KEY_MB_PG_CONNINFO "pg_conninfo"

#define KEY_FANART_RESOLVE "fanart_resolve"
#define KEY_ACOUSTID_FINGERPRINT "acoustid_fingerprint"

#define GROUP_FANART "Fanart"
#define KEY_FANART_API_KEY "api_key"

#define GROUP_ACOUSTID "AcoustID"
#define KEY_ACOUSTID_PG_CONNINFO "pg_conninfo"
#define KEY_ACOUSTID_INDEX_URL "index_url"

#define KEY_LIBRARY_PATHS "library_paths"
#define KEY_LIBRARY_NAMES "library_names"
#define KEY_LIBRARY_DATA_PATHS "library_data_paths"

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
        settings->channels[i].exclusive = TRUE;  // Default: exclusive access
        settings->channels[i].output_format = OUTPUT_FORMAT_16BIT_48000;  // Broadcast default
        settings->channels[i].quantum_frames = DEFAULT_QUANTUM_FRAMES;
        settings->channels[i].livewire_gpio_address = NULL;
    }
    settings->show_spectrum = TRUE;
    settings->time_warning_threshold_ms = DEFAULT_TIME_WARNING_MS;

    // Library defaults
    settings->auto_scan_on_startup = TRUE;
    settings->process_artwork = TRUE;
    settings->indexer_thread_count = 0;  // Auto
    settings->art_thumb_size = 48;
    settings->max_concurrent_library_scans = 2;

    // Integration toggle defaults
    settings->musicbrainz_resolve = FALSE;
    settings->fanart_resolve = FALSE;
    settings->acoustid_fingerprint = FALSE;

    // Integration connection defaults
    settings->musicbrainz_pg_conninfo = g_strdup("host=localhost dbname=musicbrainz_db user=musicbrainz");
    settings->acoustid_pg_conninfo = NULL;
    settings->acoustid_index_url = NULL;

    // Library paths, names, and data paths
    settings->library_paths = NULL;
    settings->library_path_count = 0;
    settings->library_names = NULL;
    settings->library_name_count = 0;
    settings->library_data_paths = NULL;
    settings->library_data_path_count = 0;
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

        // Quantum (buffer size) — must be power-of-2 in [32, 2048]
        int quantum = g_key_file_get_integer(keyfile, group, KEY_QUANTUM, &error);
        if (!error && quantum >= 32 && quantum <= 2048 && (quantum & (quantum - 1)) == 0) {
            settings->channels[i].quantum_frames = (uint32_t)quantum;
        }
        g_clear_error(&error);

        // Exclusive mode (default: TRUE if not present)
        gboolean excl = g_key_file_get_boolean(keyfile, group, KEY_EXCLUSIVE, &error);
        if (!error) {
            settings->channels[i].exclusive = excl;
        }
        g_clear_error(&error);
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

    int art_thumb_size = g_key_file_get_integer(keyfile, GROUP_LIBRARY, KEY_ART_THUMB_SIZE, &error);
    if (!error && art_thumb_size >= 16 && art_thumb_size <= 300) {
        settings->art_thumb_size = art_thumb_size;
    }
    g_clear_error(&error);

    int max_concurrent = g_key_file_get_integer(keyfile, GROUP_LIBRARY, KEY_MAX_CONCURRENT_SCANS, &error);
    if (!error && max_concurrent >= 1 && max_concurrent <= 8) {
        settings->max_concurrent_library_scans = max_concurrent;
    }
    g_clear_error(&error);

    // Load MusicBrainz settings
    gboolean mb_resolve = g_key_file_get_boolean(keyfile, GROUP_MUSICBRAINZ, KEY_MB_RESOLVE, &error);
    if (!error) {
        settings->musicbrainz_resolve = mb_resolve;
    }
    g_clear_error(&error);

    char *pg_conninfo = g_key_file_get_string(keyfile, GROUP_MUSICBRAINZ, KEY_MB_PG_CONNINFO, NULL);
    if (pg_conninfo && pg_conninfo[0] != '\0') {
        g_free(settings->musicbrainz_pg_conninfo);
        settings->musicbrainz_pg_conninfo = pg_conninfo;
    } else {
        g_free(pg_conninfo);
    }

    // Load fanart.tv settings
    gboolean fanart_resolve = g_key_file_get_boolean(keyfile, GROUP_FANART, KEY_FANART_RESOLVE, &error);
    if (!error) {
        settings->fanart_resolve = fanart_resolve;
    }
    g_clear_error(&error);

    char *fanart_key = g_key_file_get_string(keyfile, GROUP_FANART, KEY_FANART_API_KEY, NULL);
    if (fanart_key && fanart_key[0] != '\0') {
        settings->fanart_api_key = fanart_key;
    } else {
        g_free(fanart_key);
    }
    
    /* Load Axia GPIO credentials (global) */
    settings->axia_username = g_key_file_get_string(keyfile, GROUP_AXIA, KEY_AXIA_USERNAME, NULL);
    settings->axia_password = g_key_file_get_string(keyfile, GROUP_AXIA, KEY_AXIA_PASSWORD, NULL);

    // Load AcoustID settings
    gboolean acoustid_fp = g_key_file_get_boolean(keyfile, GROUP_ACOUSTID, KEY_ACOUSTID_FINGERPRINT, &error);
    if (!error) {
        settings->acoustid_fingerprint = acoustid_fp;
    }
    g_clear_error(&error);

    char *acoustid_pg = g_key_file_get_string(keyfile, GROUP_ACOUSTID, KEY_ACOUSTID_PG_CONNINFO, NULL);
    if (acoustid_pg && acoustid_pg[0] != '\0') {
        settings->acoustid_pg_conninfo = acoustid_pg;
    } else {
        g_free(acoustid_pg);
    }

    char *acoustid_url = g_key_file_get_string(keyfile, GROUP_ACOUSTID, KEY_ACOUSTID_INDEX_URL, NULL);
    if (acoustid_url && acoustid_url[0] != '\0') {
        settings->acoustid_index_url = acoustid_url;
    } else {
        g_free(acoustid_url);
    }

    // Load library paths
    gsize path_count = 0;
    char **lib_paths = g_key_file_get_string_list(keyfile, GROUP_LIBRARY,
                                                   KEY_LIBRARY_PATHS, &path_count, NULL);
    if (lib_paths && path_count > 0) {
        settings->library_paths = g_malloc((path_count + 1) * sizeof(char *));
        for (gsize i = 0; i < path_count; i++) {
            settings->library_paths[i] = g_strdup(lib_paths[i]);
        }
        settings->library_paths[path_count] = NULL;
        settings->library_path_count = (int)path_count;
        g_strfreev(lib_paths);
    }

    // Load library names (parallel to library_paths; may be absent or shorter)
    gsize name_count = 0;
    char **lib_names = g_key_file_get_string_list(keyfile, GROUP_LIBRARY,
                                                   KEY_LIBRARY_NAMES, &name_count, NULL);
    if (lib_names && name_count > 0) {
        settings->library_names = g_malloc((name_count + 1) * sizeof(char *));
        for (gsize i = 0; i < name_count; i++) {
            /* Store NULL for empty strings so basename fallback kicks in */
            settings->library_names[i] = (lib_names[i] && lib_names[i][0])
                                         ? g_strdup(lib_names[i]) : NULL;
        }
        settings->library_names[name_count] = NULL;
        settings->library_name_count = (int)name_count;
        g_strfreev(lib_names);
    }

    // Load library data paths (parallel to library_paths; may be absent or shorter)
    gsize data_path_count = 0;
    char **lib_data_paths = g_key_file_get_string_list(keyfile, GROUP_LIBRARY,
                                                        KEY_LIBRARY_DATA_PATHS, &data_path_count, NULL);
    if (lib_data_paths && data_path_count > 0) {
        settings->library_data_paths = g_malloc((data_path_count + 1) * sizeof(char *));
        for (gsize i = 0; i < data_path_count; i++) {
            settings->library_data_paths[i] = (lib_data_paths[i] && lib_data_paths[i][0])
                                               ? g_strdup(lib_data_paths[i]) : NULL;
        }
        settings->library_data_paths[data_path_count] = NULL;
        settings->library_data_path_count = (int)data_path_count;
        g_strfreev(lib_data_paths);
    }

    g_key_file_free(keyfile);
    g_free(path);

    g_info("Loaded settings from disk");
    return settings;
}

gboolean app_settings_save(const app_settings_t *settings) {
    g_assert(settings != NULL);  /* Caller must provide valid settings */

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
        g_key_file_set_integer(keyfile, group, KEY_QUANTUM, (int)settings->channels[i].quantum_frames);
        g_key_file_set_boolean(keyfile, group, KEY_EXCLUSIVE, settings->channels[i].exclusive);
    }

    // Save display settings
    g_key_file_set_boolean(keyfile, GROUP_DISPLAY, KEY_SHOW_SPECTRUM, settings->show_spectrum);
    g_key_file_set_integer(keyfile, GROUP_DISPLAY, KEY_TIME_WARNING, settings->time_warning_threshold_ms);

    // Save library settings
    g_key_file_set_boolean(keyfile, GROUP_LIBRARY, KEY_AUTO_SCAN, settings->auto_scan_on_startup);
    g_key_file_set_boolean(keyfile, GROUP_LIBRARY, KEY_PROCESS_ARTWORK, settings->process_artwork);
    g_key_file_set_integer(keyfile, GROUP_LIBRARY, KEY_INDEXER_THREADS, settings->indexer_thread_count);
    g_key_file_set_integer(keyfile, GROUP_LIBRARY, KEY_ART_THUMB_SIZE, settings->art_thumb_size);
    g_key_file_set_integer(keyfile, GROUP_LIBRARY, KEY_MAX_CONCURRENT_SCANS, settings->max_concurrent_library_scans);

    // Save library paths
    if (settings->library_paths && settings->library_path_count > 0) {
        g_key_file_set_string_list(keyfile, GROUP_LIBRARY, KEY_LIBRARY_PATHS,
                                   (const char * const *)settings->library_paths,
                                   (gsize)settings->library_path_count);
    }

    // Save library names (write empty string for NULL entries)
    if (settings->library_name_count > 0) {
        const char **name_list = g_new0(const char *, settings->library_name_count);
        for (int i = 0; i < settings->library_name_count; i++) {
            name_list[i] = (settings->library_names && settings->library_names[i])
                           ? settings->library_names[i] : "";
        }
        g_key_file_set_string_list(keyfile, GROUP_LIBRARY, KEY_LIBRARY_NAMES,
                                   name_list, (gsize)settings->library_name_count);
        g_free(name_list);
    }

    // Save library data paths (write empty string for NULL entries)
    if (settings->library_data_path_count > 0) {
        const char **dp_list = g_new0(const char *, settings->library_data_path_count);
        for (int i = 0; i < settings->library_data_path_count; i++) {
            dp_list[i] = (settings->library_data_paths && settings->library_data_paths[i])
                         ? settings->library_data_paths[i] : "";
        }
        g_key_file_set_string_list(keyfile, GROUP_LIBRARY, KEY_LIBRARY_DATA_PATHS,
                                   dp_list, (gsize)settings->library_data_path_count);
        g_free(dp_list);
    }

    // Save MusicBrainz settings
    g_key_file_set_boolean(keyfile, GROUP_MUSICBRAINZ, KEY_MB_RESOLVE, settings->musicbrainz_resolve);
    g_key_file_set_string(keyfile, GROUP_MUSICBRAINZ, KEY_MB_PG_CONNINFO,
                          settings->musicbrainz_pg_conninfo ? settings->musicbrainz_pg_conninfo : "");

    // Save fanart.tv settings
    g_key_file_set_boolean(keyfile, GROUP_FANART, KEY_FANART_RESOLVE, settings->fanart_resolve);
    g_key_file_set_string(keyfile, GROUP_FANART, KEY_FANART_API_KEY,
                          settings->fanart_api_key ? settings->fanart_api_key : "");
    
    // Save Axia GPIO credentials
    g_key_file_set_string(keyfile, GROUP_AXIA, KEY_AXIA_USERNAME,
                          settings->axia_username ? settings->axia_username : "");
    g_key_file_set_string(keyfile, GROUP_AXIA, KEY_AXIA_PASSWORD,
                          settings->axia_password ? settings->axia_password : "");

    // Save AcoustID settings
    g_key_file_set_boolean(keyfile, GROUP_ACOUSTID, KEY_ACOUSTID_FINGERPRINT, settings->acoustid_fingerprint);
    g_key_file_set_string(keyfile, GROUP_ACOUSTID, KEY_ACOUSTID_PG_CONNINFO,
                          settings->acoustid_pg_conninfo ? settings->acoustid_pg_conninfo : "");
    g_key_file_set_string(keyfile, GROUP_ACOUSTID, KEY_ACOUSTID_INDEX_URL,
                          settings->acoustid_index_url ? settings->acoustid_index_url : "");

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

void app_settings_destroy(app_settings_t *settings) {
    if (!settings) return;

    for (int i = 0; i < APP_SETTINGS_MAX_CHANNELS; i++) {
        g_free(settings->channels[i].device_name);
        g_free(settings->channels[i].livewire_gpio_address);
    }
    g_free(settings->musicbrainz_pg_conninfo);
    g_free(settings->acoustid_pg_conninfo);
    g_free(settings->acoustid_index_url);
    g_free(settings->fanart_api_key);
    g_free(settings->axia_username);
    g_free(settings->axia_password);
    if (settings->library_paths) {
        for (int i = 0; i < settings->library_path_count; i++)
            g_free(settings->library_paths[i]);
        g_free(settings->library_paths);
    }
    if (settings->library_names) {
        for (int i = 0; i < settings->library_name_count; i++)
            g_free(settings->library_names[i]);
        g_free(settings->library_names);
    }
    if (settings->library_data_paths) {
        for (int i = 0; i < settings->library_data_path_count; i++)
            g_free(settings->library_data_paths[i]);
        g_free(settings->library_data_paths);
    }
    g_free(settings);
}

char *app_settings_get_effective_library_name(const app_settings_t *settings, int idx) {
    g_assert(settings != NULL);
    g_assert(idx >= 0 && idx < settings->library_path_count);

    if (idx < settings->library_name_count
        && settings->library_names
        && settings->library_names[idx]
        && settings->library_names[idx][0]) {
        return g_strdup(settings->library_names[idx]);
    }
    return g_path_get_basename(settings->library_paths[idx]);
}

void app_settings_set_library_name(app_settings_t *settings, int idx, const char *name) {
    if (!settings || idx < 0 || idx >= settings->library_path_count) return;

    /* Grow names array to cover all paths if needed */
    if (idx >= settings->library_name_count) {
        int new_count = settings->library_path_count;
        settings->library_names = g_realloc(settings->library_names,
                                             (new_count + 1) * sizeof(char *));
        for (int i = settings->library_name_count; i < new_count; i++)
            settings->library_names[i] = NULL;
        settings->library_names[new_count] = NULL;
        settings->library_name_count = new_count;
    }

    g_free(settings->library_names[idx]);
    settings->library_names[idx] = (name && name[0]) ? g_strdup(name) : NULL;
}

void app_settings_add_library_path(app_settings_t *settings, const char *path) {
    if (!settings || !path) return;

    // No-op if already present
    for (int i = 0; i < settings->library_path_count; i++) {
        if (strcmp(settings->library_paths[i], path) == 0) return;
    }

    int new_count = settings->library_path_count + 1;
    settings->library_paths = g_realloc(settings->library_paths,
                                         (new_count + 1) * sizeof(char *));
    settings->library_paths[new_count - 1] = g_strdup(path);
    settings->library_paths[new_count] = NULL;
    settings->library_path_count = new_count;
}

void app_settings_remove_library_path(app_settings_t *settings, const char *path) {
    if (!settings || !path) return;

    for (int i = 0; i < settings->library_path_count; i++) {
        if (strcmp(settings->library_paths[i], path) == 0) {
            g_free(settings->library_paths[i]);
            for (int j = i; j < settings->library_path_count - 1; j++)
                settings->library_paths[j] = settings->library_paths[j + 1];
            settings->library_path_count--;
            settings->library_paths[settings->library_path_count] = NULL;

            // Shift parallel names array
            if (i < settings->library_name_count) {
                g_free(settings->library_names[i]);
                for (int j = i; j < settings->library_name_count - 1; j++)
                    settings->library_names[j] = settings->library_names[j + 1];
                settings->library_name_count--;
                settings->library_names[settings->library_name_count] = NULL;
            }

            // Shift parallel data paths array
            if (i < settings->library_data_path_count) {
                g_free(settings->library_data_paths[i]);
                for (int j = i; j < settings->library_data_path_count - 1; j++)
                    settings->library_data_paths[j] = settings->library_data_paths[j + 1];
                settings->library_data_path_count--;
                settings->library_data_paths[settings->library_data_path_count] = NULL;
            }
            return;
        }
    }
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

void app_settings_set_channel_exclusive(app_settings_t *settings, int channel, gboolean exclusive) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return;
    settings->channels[channel].exclusive = exclusive;
}

gboolean app_settings_get_channel_exclusive(const app_settings_t *settings, int channel) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return TRUE;
    return settings->channels[channel].exclusive;
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
    g_return_val_if_fail(settings && channel >= 0 && channel < APP_SETTINGS_MAX_CHANNELS, NULL);
    return settings->channels[channel].livewire_gpio_address;
}

/* Axia GPIO credentials */

void app_settings_set_axia_username(app_settings_t *settings, const char *username) {
    g_return_if_fail(settings);
    g_free(settings->axia_username);
    
    if (username && username[0] != '\0') {
        settings->axia_username = g_strdup(username);
    } else {
        settings->axia_username = NULL;
    }
}

const char *app_settings_get_axia_username(const app_settings_t *settings) {
    g_return_val_if_fail(settings, NULL);
    return settings->axia_username;
}

void app_settings_set_axia_password(app_settings_t *settings, const char *password) {
    g_return_if_fail(settings);
    g_free(settings->axia_password);
    
    if (password && password[0] != '\0') {
        settings->axia_password = g_strdup(password);
    } else {
        settings->axia_password = NULL;
    }
}

const char *app_settings_get_axia_password(const app_settings_t *settings) {
    g_return_val_if_fail(settings, NULL);
    return settings->axia_password;
}

void app_settings_set_channel_quantum(app_settings_t *settings, int channel, uint32_t quantum_frames) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return;
    if (quantum_frames < 32 || quantum_frames > 2048 || (quantum_frames & (quantum_frames - 1)) != 0) return;
    settings->channels[channel].quantum_frames = quantum_frames;
}

uint32_t app_settings_get_channel_quantum(const app_settings_t *settings, int channel) {
    if (!settings || channel < 0 || channel >= APP_SETTINGS_MAX_CHANNELS) return DEFAULT_QUANTUM_FRAMES;
    return settings->channels[channel].quantum_frames;
}

const char *app_settings_get_library_data_path(const app_settings_t *settings, int idx) {
    g_assert(settings != NULL);
    g_assert(idx >= 0 && idx < settings->library_path_count);

    if (idx < settings->library_data_path_count
        && settings->library_data_paths
        && settings->library_data_paths[idx]
        && settings->library_data_paths[idx][0]) {
        return settings->library_data_paths[idx];
    }
    return settings->library_paths[idx];
}

void app_settings_set_library_data_path(app_settings_t *settings, int idx, const char *path) {
    if (!settings || idx < 0 || idx >= settings->library_path_count) return;

    /* Grow data paths array to cover all paths if needed */
    if (idx >= settings->library_data_path_count) {
        int new_count = settings->library_path_count;
        settings->library_data_paths = g_realloc(settings->library_data_paths,
                                                  (new_count + 1) * sizeof(char *));
        for (int i = settings->library_data_path_count; i < new_count; i++)
            settings->library_data_paths[i] = NULL;
        settings->library_data_paths[new_count] = NULL;
        settings->library_data_path_count = new_count;
    }

    g_free(settings->library_data_paths[idx]);
    settings->library_data_paths[idx] = (path && path[0]) ? g_strdup(path) : NULL;
}

const char *app_settings_format_name(output_format_t format) {
    if (format < 0 || format >= OUTPUT_FORMAT_COUNT) return "Unknown";
    return OUTPUT_FORMAT_NAMES[format];
}
