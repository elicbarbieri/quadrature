/**
 * Quadrature - 4-Channel Broadcast Audio Player
 *
 * GTK4 UI entry point.
 */

#include "internal.h"
#include "quadrature/quadrature_library.h"

#define SAMPLE_RATE 48000

typedef struct {
    GtkApplication *app;
    audio_pipeline_t *pipeline;
    library_cache_t *library_cache;
} AppData;

static void on_activate(GtkApplication *gtkapp, gpointer data) {
    AppData *d = data;

    /* Ensure widget types are registered */
    g_type_ensure(UI_TYPE_SPECTRUM);
    g_type_ensure(UI_TYPE_CHANNEL_STRIP);

    app_settings_t *settings = app_settings_load();
    GtkWidget *win = ui_window_new(gtkapp, d->pipeline, d->library_cache, settings);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_shutdown(GtkApplication *gtkapp, gpointer data) {
    (void)gtkapp;
    AppData *d = data;

    if (d->pipeline) {
        audio_pipeline_stop(d->pipeline);
        audio_pipeline_destroy(d->pipeline);
        d->pipeline = NULL;
    }

    if (d->library_cache) {
        library_cache_destroy(d->library_cache);
        d->library_cache = NULL;
    }
}

int main(int argc, char *argv[]) {
    AppData data = {0};

    /* Build database path */
    char *dir = g_build_filename(g_get_user_data_dir(), "quadrature", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *dbpath = g_build_filename(dir, "library.db", NULL);
    g_free(dir);

    /* Create library cache for track_id -> path resolution */
    if (library_cache_create(dbpath, NULL, &data.library_cache) != QUADRATURE_OK) {
        g_warning("Failed to create library cache - audio will not resolve track IDs");
        data.library_cache = NULL;
    }
    g_free(dbpath);

    /* Create audio pipeline with library cache */
    if (audio_pipeline_create(data.library_cache, SAMPLE_RATE, &data.pipeline) != QUADRATURE_OK) {
        g_critical("Failed to create audio pipeline");
        if (data.library_cache) library_cache_destroy(data.library_cache);
        return 1;
    }

    if (audio_pipeline_start(data.pipeline) != QUADRATURE_OK) {
        g_critical("Failed to start audio pipeline");
        audio_pipeline_destroy(data.pipeline);
        if (data.library_cache) library_cache_destroy(data.library_cache);
        return 1;
    }

    data.app = gtk_application_new("org.quadrature.player", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(data.app, "activate", G_CALLBACK(on_activate), &data);
    g_signal_connect(data.app, "shutdown", G_CALLBACK(on_shutdown), &data);

    int status = g_application_run(G_APPLICATION(data.app), argc, argv);
    g_object_unref(data.app);

    return status;
}
