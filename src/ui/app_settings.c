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
#define KEY_MB_SOLR_URL "solr_url"

#define KEY_FANART_RESOLVE "fanart_resolve"
#define KEY_ACOUSTID_FINGERPRINT "acoustid_fingerprint"

#define GROUP_FANART "Fanart"
#define KEY_FANART_API_KEY "api_key"

#define GROUP_ACOUSTID "AcoustID"
#define KEY_ACOUSTID_PG_CONNINFO "pg_conninfo"
#define KEY_ACOUSTID_INDEX_URL "index_url"

/* Per-library settings now use [Library.N] groups with key names
 * matching library_config_t field names (path, name, data_path, etc.) */

#define GROUP_WIKIPEDIA "Wikipedia"
#define KEY_WIKIPEDIA_BIOS "wikipedia_bios"

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
    settings->mb_solr_url = NULL;
    settings->acoustid_pg_conninfo = NULL;
    settings->acoustid_index_url = NULL;

    // Libraries
    settings->libraries = NULL;
    settings->library_count = 0;
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

    char *solr_url = g_key_file_get_string(keyfile, GROUP_MUSICBRAINZ, KEY_MB_SOLR_URL, NULL);
    if (solr_url && solr_url[0] != '\0') {
        settings->mb_solr_url = solr_url;
    } else {
        g_free(solr_url);
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

    // Load Wikipedia bios global toggle
    if (g_key_file_has_key(keyfile, GROUP_WIKIPEDIA, KEY_WIKIPEDIA_BIOS, NULL))
        settings->wikipedia_bios = g_key_file_get_boolean(keyfile, GROUP_WIKIPEDIA, KEY_WIKIPEDIA_BIOS, NULL);

    // Load libraries from [Library.N] groups
    {
        /* First pass: count library groups */
        gsize group_count = 0;
        char **groups = g_key_file_get_groups(keyfile, &group_count);
        int lib_count = 0;
        for (gsize i = 0; i < group_count; i++) {
            if (g_str_has_prefix(groups[i], "Library."))
                lib_count++;
        }

        if (lib_count > 0) {
            settings->libraries = g_new0(library_config_t, lib_count);
            settings->library_count = lib_count;

            int idx = 0;
            for (gsize i = 0; i < group_count && idx < lib_count; i++) {
                if (!g_str_has_prefix(groups[i], "Library."))
                    continue;
                library_config_t *lib = &settings->libraries[idx];

                char *p = g_key_file_get_string(keyfile, groups[i], "path", NULL);
                lib->path = (p && p[0]) ? p : (g_free(p), NULL);
                if (!lib->path) { idx++; continue; } /* skip entries without a path */

                char *n = g_key_file_get_string(keyfile, groups[i], "name", NULL);
                lib->name = (n && n[0]) ? n : (g_free(n), NULL);

                char *dp = g_key_file_get_string(keyfile, groups[i], "data_path", NULL);
                lib->data_path = (dp && dp[0]) ? dp : (g_free(dp), NULL);

                lib->mb_resolve = g_key_file_get_integer(keyfile, groups[i], "mb_resolve", NULL);
                lib->acoustid   = g_key_file_get_integer(keyfile, groups[i], "acoustid", NULL);
                lib->fanart     = g_key_file_get_integer(keyfile, groups[i], "fanart", NULL);
                lib->wikipedia  = g_key_file_get_integer(keyfile, groups[i], "wikipedia", NULL);
                lib->locked     = g_key_file_get_integer(keyfile, groups[i], "locked", NULL);

                GError *idx_err = NULL;
                lib->library_index = g_key_file_get_integer(keyfile, groups[i], "library_index", &idx_err);
                if (idx_err) {
                    lib->library_index = idx; /* fallback: position = index */
                    g_error_free(idx_err);
                }
                idx++;
            }

            /* Sort by library_index so array order matches persisted order */
            for (int a = 0; a < settings->library_count - 1; a++) {
                for (int b = a + 1; b < settings->library_count; b++) {
                    if (settings->libraries[b].library_index < settings->libraries[a].library_index) {
                        library_config_t tmp = settings->libraries[a];
                        settings->libraries[a] = settings->libraries[b];
                        settings->libraries[b] = tmp;
                    }
                }
            }
        }
        g_strfreev(groups);
    }

    // Migration: load old parallel-array format if no Library.N groups found
    if (settings->library_count == 0) {
        gsize path_count = 0;
        char **lib_paths = g_key_file_get_string_list(keyfile, GROUP_LIBRARY,
                                                       "library_paths", &path_count, NULL);
        if (lib_paths && path_count > 0) {
            settings->libraries = g_new0(library_config_t, (int)path_count);
            settings->library_count = (int)path_count;

            gsize name_count = 0, dp_count = 0;
            char **lib_names = g_key_file_get_string_list(keyfile, GROUP_LIBRARY,
                                                           "library_names", &name_count, NULL);
            char **lib_dps   = g_key_file_get_string_list(keyfile, GROUP_LIBRARY,
                                                           "library_data_paths", &dp_count, NULL);
            gsize mb_cnt = 0, ac_cnt = 0, fa_cnt = 0, wp_cnt = 0;
            int *mb_vals = g_key_file_get_integer_list(keyfile, GROUP_LIBRARY, "library_mb_resolve", &mb_cnt, NULL);
            int *ac_vals = g_key_file_get_integer_list(keyfile, GROUP_LIBRARY, "library_acoustid", &ac_cnt, NULL);
            int *fa_vals = g_key_file_get_integer_list(keyfile, GROUP_LIBRARY, "library_fanart", &fa_cnt, NULL);
            int *wp_vals = g_key_file_get_integer_list(keyfile, GROUP_LIBRARY, "library_wikipedia", &wp_cnt, NULL);

            for (gsize i = 0; i < path_count; i++) {
                library_config_t *lib = &settings->libraries[i];
                lib->path = g_strdup(lib_paths[i]);
                lib->library_index = (int)i;
                if (i < name_count && lib_names && lib_names[i] && lib_names[i][0])
                    lib->name = g_strdup(lib_names[i]);
                if (i < dp_count && lib_dps && lib_dps[i] && lib_dps[i][0])
                    lib->data_path = g_strdup(lib_dps[i]);
                if (i < mb_cnt && mb_vals) lib->mb_resolve = mb_vals[i]; else lib->mb_resolve = -1;
                if (i < ac_cnt && ac_vals) lib->acoustid   = ac_vals[i]; else lib->acoustid   = -1;
                if (i < fa_cnt && fa_vals) lib->fanart     = fa_vals[i]; else lib->fanart     = -1;
                if (i < wp_cnt && wp_vals) lib->wikipedia  = wp_vals[i]; else lib->wikipedia  = -1;
            }
            g_strfreev(lib_paths);
            g_strfreev(lib_names);
            g_strfreev(lib_dps);
            g_free(mb_vals); g_free(ac_vals); g_free(fa_vals); g_free(wp_vals);
        }
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

    // Save libraries as [Library.N] groups
    for (int i = 0; i < settings->library_count; i++) {
        const library_config_t *lib = &settings->libraries[i];
        char group[32];
        snprintf(group, sizeof(group), "Library.%d", i);

        g_key_file_set_string(keyfile, group, "path", lib->path ? lib->path : "");
        g_key_file_set_string(keyfile, group, "name", lib->name ? lib->name : "");
        g_key_file_set_string(keyfile, group, "data_path", lib->data_path ? lib->data_path : "");
        g_key_file_set_integer(keyfile, group, "mb_resolve", lib->mb_resolve);
        g_key_file_set_integer(keyfile, group, "acoustid", lib->acoustid);
        g_key_file_set_integer(keyfile, group, "fanart", lib->fanart);
        g_key_file_set_integer(keyfile, group, "wikipedia", lib->wikipedia);
        g_key_file_set_integer(keyfile, group, "locked", lib->locked);
        g_key_file_set_integer(keyfile, group, "library_index", lib->library_index);
    }

    // Save MusicBrainz settings
    g_key_file_set_boolean(keyfile, GROUP_MUSICBRAINZ, KEY_MB_RESOLVE, settings->musicbrainz_resolve);
    g_key_file_set_string(keyfile, GROUP_MUSICBRAINZ, KEY_MB_PG_CONNINFO,
                          settings->musicbrainz_pg_conninfo ? settings->musicbrainz_pg_conninfo : "");
    g_key_file_set_string(keyfile, GROUP_MUSICBRAINZ, KEY_MB_SOLR_URL,
                          settings->mb_solr_url ? settings->mb_solr_url : "");

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

    // Save Wikipedia settings
    g_key_file_set_boolean(keyfile, GROUP_WIKIPEDIA, KEY_WIKIPEDIA_BIOS, settings->wikipedia_bios);

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
    g_free(settings->mb_solr_url);
    g_free(settings->acoustid_pg_conninfo);
    g_free(settings->acoustid_index_url);
    g_free(settings->fanart_api_key);
    g_free(settings->axia_username);
    g_free(settings->axia_password);
    if (settings->libraries) {
        for (int i = 0; i < settings->library_count; i++) {
            g_free(settings->libraries[i].path);
            g_free(settings->libraries[i].name);
            g_free(settings->libraries[i].data_path);
        }
        g_free(settings->libraries);
    }
    g_free(settings);
}

char *app_settings_get_library_name(const app_settings_t *settings, int idx) {
    g_assert(settings != NULL);
    g_assert(idx >= 0 && idx < settings->library_count);

    const library_config_t *lib = &settings->libraries[idx];
    if (lib->name && lib->name[0])
        return g_strdup(lib->name);
    return g_path_get_basename(lib->path);
}

const char *app_settings_get_library_data_path(const app_settings_t *settings, int idx) {
    g_assert(settings != NULL);
    g_assert(idx >= 0 && idx < settings->library_count);

    const library_config_t *lib = &settings->libraries[idx];
    return (lib->data_path && lib->data_path[0]) ? lib->data_path : lib->path;
}

void app_settings_add_library(app_settings_t *settings, const char *path) {
    if (!settings || !path) return;
    if (app_settings_find_library(settings, path) >= 0) return;

    /* Find next available library_index */
    int max_idx = -1;
    for (int i = 0; i < settings->library_count; i++) {
        if (settings->libraries[i].library_index > max_idx)
            max_idx = settings->libraries[i].library_index;
    }

    int new_count = settings->library_count + 1;
    settings->libraries = g_realloc(settings->libraries,
                                     new_count * sizeof(library_config_t));
    library_config_t *lib = &settings->libraries[new_count - 1];
    memset(lib, 0, sizeof(*lib));
    lib->path          = g_strdup(path);
    lib->mb_resolve    = -1;
    lib->acoustid      = -1;
    lib->fanart        = -1;
    lib->wikipedia     = -1;
    lib->library_index = max_idx + 1;
    settings->library_count = new_count;
}

void app_settings_remove_library(app_settings_t *settings, const char *path) {
    int idx = app_settings_find_library(settings, path);
    if (idx < 0) return;

    g_free(settings->libraries[idx].path);
    g_free(settings->libraries[idx].name);
    g_free(settings->libraries[idx].data_path);

    for (int j = idx; j < settings->library_count - 1; j++)
        settings->libraries[j] = settings->libraries[j + 1];
    settings->library_count--;
}

void app_settings_swap_libraries(app_settings_t *settings, int pos_a, int pos_b) {
    if (!settings || pos_a < 0 || pos_b < 0
        || pos_a >= settings->library_count || pos_b >= settings->library_count
        || pos_a == pos_b) return;

    library_config_t tmp = settings->libraries[pos_a];
    settings->libraries[pos_a] = settings->libraries[pos_b];
    settings->libraries[pos_b] = tmp;
}

int app_settings_find_library(const app_settings_t *settings, const char *path) {
    if (!settings || !path) return -1;
    for (int i = 0; i < settings->library_count; i++) {
        if (settings->libraries[i].path && strcmp(settings->libraries[i].path, path) == 0)
            return i;
    }
    return -1;
}

int app_settings_find_library_by_index(const app_settings_t *settings, int library_index) {
    if (!settings) return -1;
    for (int i = 0; i < settings->library_count; i++) {
        if (settings->libraries[i].library_index == library_index)
            return i;
    }
    return -1;
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

/* Old getter/setter functions removed — callers now access
 * settings->libraries[i].field directly.
 * Kept: app_settings_get_library_name() and app_settings_get_library_data_path()
 * as convenience functions with fallback logic. */

const char *app_settings_format_name(output_format_t format) {
    if (format < 0 || format >= OUTPUT_FORMAT_COUNT) return "Unknown";
    return OUTPUT_FORMAT_NAMES[format];
}
