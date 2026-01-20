/**
 * Quadrature - 4-Channel Broadcast Audio Player
 *
 * GTK4 UI entry point.
 */

#include "internal.h"

#define SAMPLE_RATE 48000

typedef struct {
    GtkApplication *app;
    audio_pipeline_t *pipeline;
} AppData;

static void on_activate(GtkApplication *gtkapp, gpointer data) {
    AppData *d = data;

    /* Ensure widget types are registered */
    g_type_ensure(UI_TYPE_SPECTRUM);
    g_type_ensure(UI_TYPE_CHANNEL_STRIP);

    app_settings_t *settings = app_settings_load();
    GtkWidget *win = ui_window_new(gtkapp, d->pipeline, settings);
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
}

int main(int argc, char *argv[]) {
    AppData data = {0};

    if (audio_pipeline_create(SAMPLE_RATE, &data.pipeline) != QUADRATURE_OK) {
        g_critical("Failed to create audio pipeline");
        return 1;
    }

    if (audio_pipeline_start(data.pipeline) != QUADRATURE_OK) {
        g_critical("Failed to start audio pipeline");
        audio_pipeline_destroy(data.pipeline);
        return 1;
    }

    data.app = gtk_application_new("org.quadrature.player", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(data.app, "activate", G_CALLBACK(on_activate), &data);
    g_signal_connect(data.app, "shutdown", G_CALLBACK(on_shutdown), &data);

    int status = g_application_run(G_APPLICATION(data.app), argc, argv);
    g_object_unref(data.app);

    return status;
}
