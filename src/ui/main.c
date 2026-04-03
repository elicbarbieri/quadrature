/**
 * Quadrature - 4-Channel Broadcast Audio Player
 *
 * GTK4 UI entry point.
 *
 * Shutdown sequence:
 *   1. SIGINT/SIGTERM → g_application_quit() (enters orderly shutdown)
 *   2. GTK disposes all windows → ui_window_dispose():
 *      - Deregisters callbacks (track_changed, cache_ready, indexer signals)
 *      - Cancels indexer, removes timers
 *      - NULLs borrowed pointers (pipeline, cache, settings)
 *   3. GtkApplication::shutdown → on_shutdown():
 *      - Stops + destroys audio pipeline (PipeWire threads, streams, cache)
 *      - Destroys library cache (joins warming threads, closes DBs)
 *      - Frees settings
 */

#include "internal.h"
#include "quadrature/library.h"

#include <glib-unix.h>

#define SAMPLE_RATE 48000

typedef struct {
    GtkApplication *app;
    audio_pipeline_t *pipeline;
    library_cache_t *library_cache;
    app_settings_t *settings;
} AppData;

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handling — graceful shutdown on SIGINT/SIGTERM
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean on_unix_signal(gpointer user_data) {
    GApplication *app = G_APPLICATION(user_data);
    g_message("Received signal, shutting down gracefully...");
    g_application_quit(app);
    return G_SOURCE_REMOVE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GtkApplication Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_activate(GtkApplication *gtkapp, gpointer data) {
    AppData *d = data;

    /* Ensure widget types are registered */
    g_type_ensure(UI_TYPE_SPECTRUM);
    g_type_ensure(UI_TYPE_CHANNEL_STRIP);
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkWidget *win = ui_window_new(gtkapp, d->pipeline, d->library_cache, d->settings);
    gtk_window_present(GTK_WINDOW(win));
}

/**
 * on_shutdown — destroy backend resources.
 *
 * Runs AFTER all windows have been disposed. ui_window_dispose has already
 * deregistered its callbacks from pipeline/cache/indexer, so no UI code
 * will touch these resources during or after teardown.
 */
static void on_shutdown(GtkApplication *gtkapp, gpointer data) {
    (void)gtkapp;
    AppData *d = data;

    /* Stop audio pipeline first — halts PipeWire thread loop and removes
     * the advance timer, ensuring no RT or GLib callbacks fire during
     * subsequent destruction */
    if (d->pipeline) {
        audio_pipeline_stop(d->pipeline);
        audio_pipeline_destroy(d->pipeline);
        d->pipeline = NULL;
    }

    /* Destroy library cache — joins any warming threads, closes DBs */
    if (d->library_cache) {
        library_cache_destroy(d->library_cache);
        d->library_cache = NULL;
    }

    if (d->settings) {
        app_settings_destroy(d->settings);
        d->settings = NULL;
    }

    g_message("Shutdown complete");
}

int main(int argc, char *argv[]) {
    AppData data = {0};

    /* Load settings first to get library paths */
    data.settings = app_settings_load();

    /* Create library cache for ALL configured libraries.
     * Each library gets its own slot (indexed 0..N-1) in the cache.
     * If no libraries are configured, the cache starts empty but non-NULL
     * so slots can be added dynamically when the user adds libraries. */
    {
        int lib_count = data.settings ? data.settings->library_count : 0;
        library_cache_source_t *sources = NULL;
        char **dbpaths = NULL;
        char **names   = NULL;

        if (lib_count > 0) {
            sources = g_new0(library_cache_source_t, lib_count);
            dbpaths = g_new0(char *, lib_count);
            names   = g_new0(char *, lib_count);
            for (int i = 0; i < lib_count; i++) {
                const char *data_root = app_settings_get_library_data_path(data.settings, i);
                dbpaths[i] = g_build_filename(data_root, "quadrature.sqlite", NULL);
                names[i]   = app_settings_get_library_name(data.settings, i);
                sources[i].db_path      = dbpaths[i];
                sources[i].music_base   = data.settings->libraries[i].path;
                sources[i].display_name = names[i];
                sources[i].bitmap_index = data.settings->libraries[i].library_index;
            }
        }

        if (library_cache_create_multi(sources, lib_count, &data.library_cache) != QUADRATURE_OK) {
            g_warning("Failed to create library cache - audio will not resolve track IDs");
            data.library_cache = NULL;
        }

        for (int i = 0; i < lib_count; i++) {
            g_free(dbpaths[i]);
            g_free(names[i]);
        }
        g_free(dbpaths);
        g_free(names);
        g_free(sources);
    }

    /* Create audio pipeline with library cache */
    if (audio_pipeline_create(data.library_cache, SAMPLE_RATE, &data.pipeline) != QUADRATURE_OK) {
        g_critical("Failed to create audio pipeline");
        if (data.library_cache) library_cache_destroy(data.library_cache);
        app_settings_destroy(data.settings);
        return 1;
    }

    if (audio_pipeline_start(data.pipeline) != QUADRATURE_OK) {
        g_critical("Failed to start audio pipeline");
        audio_pipeline_destroy(data.pipeline);
        if (data.library_cache) library_cache_destroy(data.library_cache);
        app_settings_destroy(data.settings);
        return 1;
    }

    data.app = gtk_application_new("org.quadrature.player", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(data.app, "activate", G_CALLBACK(on_activate), &data);
    g_signal_connect(data.app, "shutdown", G_CALLBACK(on_shutdown), &data);

    /* Install signal handlers so Ctrl+C triggers orderly shutdown
     * instead of immediate process termination */
    g_unix_signal_add(SIGINT,  on_unix_signal, data.app);
    g_unix_signal_add(SIGTERM, on_unix_signal, data.app);

    int status = g_application_run(G_APPLICATION(data.app), argc, argv);
    g_object_unref(data.app);

    return status;
}
