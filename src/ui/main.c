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

#include <adwaita.h>
#include <errno.h>
#include <glib-unix.h>
#ifdef QUADRATURE_USE_LIBPQ
#include <libpq-fe.h>
#endif
#include <sqlite3.h>
#include <sys/stat.h>

#define SAMPLE_RATE 48000

typedef struct {
    GtkApplication *app;
    audio_pipeline_t *pipeline;
    library_cache_t *library_cache;
    app_settings_t *settings;
} AppData;

/* ═══════════════════════════════════════════════════════════════════════════
 * Startup Health Checks — early validation before subsystem init
 * ═══════════════════════════════════════════════════════════════════════════ */

static void startup_health_check(const app_settings_t *settings) {
    if (!settings) return;

    /* 1. Library path accessibility + 2. SQLite quick_check */
    for (int i = 0; i < settings->library_count; i++) {
        const char *path = settings->libraries[i].path;
        struct stat st;

        if (stat(path, &st) != 0) {
            g_warning("Startup: library '%s' is not accessible: %s",
                      path, strerror(errno));
            continue;
        }

        const char *data_root = app_settings_get_library_data_path(settings, i);
        g_autofree char *db_path = g_build_filename(data_root,
                                                     "quadrature.sqlite", NULL);

        if (stat(db_path, &st) != 0)
            continue;  /* DB doesn't exist yet — first run for this library */

        sqlite3 *db = NULL;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            g_critical("Startup: cannot open database %s: %s",
                       db_path, db ? sqlite3_errmsg(db) : "unknown error");
            if (db) sqlite3_close(db);
            continue;
        }

        sqlite3_stmt *stmt = NULL;
        gboolean ok = FALSE;
        if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *result = (const char *)sqlite3_column_text(stmt, 0);
                ok = result && g_strcmp0(result, "ok") == 0;
            }
            sqlite3_finalize(stmt);
        }

        if (!ok)
            g_critical("Startup: database integrity check failed for %s", db_path);

        sqlite3_close(db);
    }

    /* 3. PostgreSQL connectivity (skipped in HTTP-only builds) */
#ifdef QUADRATURE_USE_LIBPQ
    const char *pg_conninfos[] = {
        settings->musicbrainz_pg_conninfo,
        settings->acoustid_pg_conninfo,
    };
    const char *pg_labels[] = {
        "MusicBrainz",
        "AcoustID",
    };

    for (int i = 0; i < 2; i++) {
        if (!pg_conninfos[i] || pg_conninfos[i][0] == '\0')
            continue;

        g_autofree char *conninfo = g_strdup_printf(
            "%s connect_timeout=2", pg_conninfos[i]);

        PGconn *conn = PQconnectdb(conninfo);
        if (PQstatus(conn) != CONNECTION_OK)
            g_warning("Startup: %s PostgreSQL unreachable — "
                      "MB resolution will be disabled", pg_labels[i]);
        PQfinish(conn);
    }
#else
    (void)settings;  /* HTTP-only build — no PG check */
#endif
}

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

/**
 * on_startup — initialise settings, library cache, and audio pipeline.
 *
 * Fires after GApplication processes local options (--help / --version)
 * so those flags exit cleanly without touching pipewire, dbus, or sqlite.
 * On fatal init failure, calls g_application_quit() — on_shutdown still
 * runs to clean up whatever was constructed.
 */
static void on_startup(GtkApplication *gtkapp, gpointer data) {
    AppData *d = data;

    adw_init();

    d->settings = app_settings_load();
    startup_health_check(d->settings);

    int lib_count = d->settings ? d->settings->library_count : 0;
    library_cache_source_t *sources = NULL;
    char **dbpaths = NULL;
    char **names   = NULL;

    if (lib_count > 0) {
        sources = g_new0(library_cache_source_t, lib_count);
        dbpaths = g_new0(char *, lib_count);
        names   = g_new0(char *, lib_count);
        for (int i = 0; i < lib_count; i++) {
            const char *data_root = app_settings_get_library_data_path(d->settings, i);
            dbpaths[i] = g_build_filename(data_root, "quadrature.sqlite", NULL);
            names[i]   = app_settings_get_library_name(d->settings, i);
            sources[i].db_path      = dbpaths[i];
            sources[i].music_base   = d->settings->libraries[i].path;
            sources[i].display_name = names[i];
            sources[i].bitmap_index = d->settings->libraries[i].library_index;
        }
    }

    if (library_cache_create_multi(sources, lib_count, &d->library_cache) != QUADRATURE_OK) {
        g_warning("Failed to create library cache - audio will not resolve track IDs");
        d->library_cache = NULL;
    }

    for (int i = 0; i < lib_count; i++) {
        g_free(dbpaths[i]);
        g_free(names[i]);
    }
    g_free(dbpaths);
    g_free(names);
    g_free(sources);

    if (audio_pipeline_create(d->library_cache, SAMPLE_RATE, &d->pipeline) != QUADRATURE_OK) {
        g_critical("Failed to create audio pipeline");
        g_application_quit(G_APPLICATION(gtkapp));
        return;
    }
}

static void on_activate(GtkApplication *gtkapp, gpointer data) {
    AppData *d = data;

    /* Startup may have aborted before constructing the pipeline. */
    if (!d->pipeline) return;

    /* Ensure widget types are registered */
    g_type_ensure(UI_TYPE_SPECTRUM);
    g_type_ensure(UI_TYPE_CHANNEL_STRIP);
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);
    g_type_ensure(QUADRATURE_TYPE_OVERFLOW_BOX);

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

    /* Destroy audio pipeline first — halts PipeWire thread loop and removes
     * the advance timer, ensuring no RT or GLib callbacks fire during
     * subsequent destruction */
    if (d->pipeline) {
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

static gint on_local_options(GApplication *gapp, GVariantDict *opts, gpointer user_data) {
    (void)gapp; (void)user_data;
    if (g_variant_dict_contains(opts, "version")) {
        g_print("quadrature %s\n", QUADRATURE_VERSION);
        return 0;  /* exit success — startup never fires */
    }
    return -1;  /* continue to startup */
}

int main(int argc, char *argv[]) {
    AppData data = {0};

    data.app = gtk_application_new("org.quadrature.player", G_APPLICATION_NON_UNIQUE);

    const GOptionEntry option_entries[] = {
        { "version", 0, 0, G_OPTION_ARG_NONE, NULL, "Print version and exit", NULL },
        { 0 },
    };
    g_application_add_main_option_entries(G_APPLICATION(data.app), option_entries);

    g_signal_connect(data.app, "handle-local-options", G_CALLBACK(on_local_options), &data);
    g_signal_connect(data.app, "startup",  G_CALLBACK(on_startup),  &data);
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
