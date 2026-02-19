/**
 * Quadrature Libraries View
 *
 * Library cards, stats display, add/remove/rename library management.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "internal.h"
#include <string.h>

LibEntry *find_lib_entry(UiWindow *w, const char *path) {
    if (!path) return NULL;
    for (size_t i = 0; i < w->lib_count; i++) {
        if (w->libs[i].path && strcmp(w->libs[i].path, path) == 0)
            return &w->libs[i];
    }
    return NULL;
}


void libs_free(UiWindow *w) {
    if (w->libs) {
        for (size_t i = 0; i < w->lib_count; i++) {
            if (w->libs[i].pulse_timer) {
                g_source_remove(w->libs[i].pulse_timer);
                w->libs[i].pulse_timer = 0;
            }
            if (w->libs[i].hide_timer) {
                g_source_remove(w->libs[i].hide_timer);
                w->libs[i].hide_timer = 0;
            }
            g_free(w->libs[i].path);
            g_free(w->libs[i].data_path);
            g_free(w->libs[i].name);
        }
        g_free(w->libs);
        w->libs = NULL;
    }
    w->lib_count = 0;
}

void libs_load(UiWindow *w) {
    libs_free(w);
    if (!w->settings || w->settings->library_path_count == 0) return;

    size_t count = (size_t)w->settings->library_path_count;
    w->libs = g_new0(LibEntry, count);
    w->lib_count = count;

    for (size_t i = 0; i < count; i++) {
        w->libs[i].id   = (int64_t)i;
        w->libs[i].path = g_strdup(w->settings->library_paths[i]);
        w->libs[i].name = app_settings_get_effective_library_name(w->settings, (int)i);
        const char *dp = app_settings_get_library_data_path(w->settings, (int)i);
        w->libs[i].data_path = (dp && strcmp(dp, w->libs[i].path) != 0)
                                ? g_strdup(dp) : NULL;
        libs_load_entry_stats(&w->libs[i], w);
    }
}

static void on_rescan(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (e && w->indexer && !indexer_controller_is_running(w->indexer)) {
        const char *paths[] = { e->path };
        const char *data_paths[] = { e->data_path ? e->data_path : e->path };
        indexer_controller_start(w->indexer, paths, data_paths, 1);
    }
}

static void on_remove(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (e && w->settings) {
        /* Cancel any running indexer for this library before tearing down */
        if (w->indexer)
            indexer_controller_cancel_library(w->indexer, e->path);

        app_settings_remove_library_path(w->settings, e->path);
        settings_save_debounced(w);
        libs_load(w);
        libs_rebuild(w);
    }
}

/** Called when the errors popover is closed — clean up DB and destroy popover. */
static void on_errors_popover_closed(GtkPopover *popover, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->errors_lib_db) {
        db_close(w->errors_lib_db);
        w->errors_lib_db = NULL;
    }
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void on_errors(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e) return;

    /* Close any previously opened errors DB */
    if (w->errors_lib_db) {
        db_close(w->errors_lib_db);
        w->errors_lib_db = NULL;
    }

    /* Open the library's DB (use data_path if set, else library path) */
    const char *db_root = e->data_path ? e->data_path : e->path;
    char *dbpath = g_build_filename(db_root, "quadrature.sqlite", NULL);
    if (db_open(dbpath, &w->errors_lib_db) != QUADRATURE_OK) {
        g_warning("on_errors: failed to open DB for %s", db_root);
        w->errors_lib_db = NULL;
    }
    g_free(dbpath);

    /* Store library path for filtering */
    g_free(w->errors_library_path);
    w->errors_library_path = g_strdup(e->path);

    /* Load popover structure from template */
    GtkBuilder *b = gtk_builder_new_from_resource("/org/quadrature/ui/errors_popover.ui");
    GtkWidget *popover = GTK_WIDGET(gtk_builder_get_object(b, "errors_popover"));
    GtkWidget *content = GTK_WIDGET(gtk_builder_get_object(b, "errors_content"));
    g_object_ref(popover);
    g_object_unref(b);

    ui_popover_install_shortcuts(GTK_POPOVER(popover));

    /* Insert fresh errors view */
    GtkWidget *errors_view = errors_view_new(w->errors_lib_db);
    errors_view_set_path_filter(errors_view, w->errors_library_path);
    errors_view_refresh(errors_view);
    gtk_widget_set_vexpand(errors_view, TRUE);
    gtk_box_append(GTK_BOX(content), errors_view);

    /* Size the errors view to fill the popover (same margins as track info popover) */
    static const int MARGIN_TOP  = 100;
    static const int MARGIN_SIDE = 100;
    static const int MARGIN_BOT  = 100;
    int stack_w = gtk_widget_get_width(w->stack);
    int stack_h = gtk_widget_get_height(w->stack);
    int pop_w = MAX(400, stack_w - 2 * MARGIN_SIDE);
    int pop_h = MAX(300, stack_h - MARGIN_TOP - MARGIN_BOT);
    gtk_widget_set_size_request(errors_view, pop_w, pop_h);

    gtk_widget_set_parent(popover, w->stack);
    g_object_unref(popover);  /* parent now owns it */

    GdkRectangle anchor = { stack_w / 2, MARGIN_TOP, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);

    g_signal_connect(popover, "closed", G_CALLBACK(on_errors_popover_closed), w);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_lib_name_editing_done(GtkEditableLabel *label, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    if (gtk_editable_label_get_editing(label)) return; /* still in edit mode */
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(label), "entry");
    if (!e || !w->settings) return;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(label));
    app_settings_set_library_name(w->settings, (int)e->id, new_name);
    settings_save_debounced(w);
    g_free(e->name);
    e->name = app_settings_get_effective_library_name(w->settings, (int)e->id);
}

static GtkWidget *make_lib_card(UiWindow *w, LibEntry *e) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_card.ui");

    GtkWidget *card = GTK_WIDGET(gtk_builder_get_object(builder, "library_card"));
    g_object_ref(card);
    e->card = card;

    /* Progress revealer */
    e->progress_revealer = GTK_WIDGET(gtk_builder_get_object(builder, "progress_revealer"));

    /* Stats labels */
    e->stat_tracks       = GTK_WIDGET(gtk_builder_get_object(builder, "stat_tracks"));
    e->stat_albums       = GTK_WIDGET(gtk_builder_get_object(builder, "stat_albums"));
    e->stat_artists      = GTK_WIDGET(gtk_builder_get_object(builder, "stat_artists"));
    e->stat_last_scanned = GTK_WIDGET(gtk_builder_get_object(builder, "stat_last_scanned"));
    e->stat_errors_btn   = GTK_WIDGET(gtk_builder_get_object(builder, "stat_errors_btn"));

    /* Phase rows */
    for (int i = 0; i < 7; i++) {
        char id[32];
        snprintf(id, sizeof(id), "phase_%d_row",    i); e->phases[i].container  = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_title",  i); e->phases[i].title      = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_status", i); e->phases[i].label      = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_bar",    i); e->phases[i].bar        = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_rate",   i); e->phases[i].rate_label = GTK_WIDGET(gtk_builder_get_object(builder, id));
    }

    /* Grab action widgets before releasing builder */
    GtkWidget *name      = GTK_WIDGET(gtk_builder_get_object(builder, "card_name"));
    GtkWidget *path      = GTK_WIDGET(gtk_builder_get_object(builder, "card_path"));
    GtkWidget *data_path = GTK_WIDGET(gtk_builder_get_object(builder, "card_data_path"));
    GtkWidget *rescan    = GTK_WIDGET(gtk_builder_get_object(builder, "card_rescan"));
    GtkWidget *remove    = GTK_WIDGET(gtk_builder_get_object(builder, "card_remove"));

    g_object_unref(builder);

    /* Set dynamic content */
    gtk_editable_set_text(GTK_EDITABLE(name), e->name);
    gtk_label_set_text(GTK_LABEL(path), e->path);

    /* Show data path if different from music path */
    if (e->data_path) {
        char *dp_display = g_strdup_printf("Data: %s", e->data_path);
        gtk_label_set_text(GTK_LABEL(data_path), dp_display);
        gtk_widget_set_visible(data_path, TRUE);
        g_free(dp_display);
    }

    /* Wire signals */
    g_object_set_data(G_OBJECT(name), "entry", e);
    g_signal_connect(name, "notify::editing", G_CALLBACK(on_lib_name_editing_done), w);

    g_object_set_data(G_OBJECT(rescan), "entry", e);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan), w);

    g_object_set_data(G_OBJECT(remove), "entry", e);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), w);

    g_object_set_data(G_OBJECT(e->stat_errors_btn), "entry", e);
    g_signal_connect(e->stat_errors_btn, "clicked", G_CALLBACK(on_errors), w);

    update_card_stats_labels(e);
    return card;
}

void libs_rebuild(UiWindow *w) {
    if (!w->libs_box) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(w->libs_box)))
        gtk_box_remove(GTK_BOX(w->libs_box), child);

    if (w->lib_count == 0) {
        gtk_widget_set_visible(w->libs_empty, TRUE);
        return;
    }

    gtk_widget_set_visible(w->libs_empty, FALSE);
    for (size_t i = 0; i < w->lib_count; i++)
        gtk_box_append(GTK_BOX(w->libs_box), make_lib_card(w, &w->libs[i]));
}

/* ── Add-library popover state ────────────────────────────────────────── */

typedef struct {
    UiWindow   *w;
    GtkWidget  *popover;
    GtkWidget  *music_label;
    GtkWidget  *data_label;
    GtkWidget  *data_revealer;
    GtkWidget  *confirm_btn;
    char       *music_path;      /* required */
    char       *data_path;       /* NULL = same as music_path */
    gboolean    portable_mode;   /* TRUE when "Portable Drive" is active */
} AddLibState;

static void add_lib_state_free(AddLibState *s) {
    g_free(s->music_path);
    g_free(s->data_path);
    g_free(s);
}

static char *generate_portable_data_path(const char *music_path) {
    char *basename = g_path_get_basename(music_path);
    char *data_path = g_build_filename(
        g_get_user_data_dir(), "quadrature", "libraries", basename, NULL);
    g_free(basename);
    return data_path;
}

static void update_auto_data_path(AddLibState *s) {
    if (!s->portable_mode || !s->music_path) return;
    g_free(s->data_path);
    s->data_path = generate_portable_data_path(s->music_path);
    gtk_label_set_text(GTK_LABEL(s->data_label), s->data_path);
    gtk_widget_set_opacity(s->data_label, 1.0);
}

static void on_mode_toggled(GtkToggleButton *btn, gpointer data) {
    AddLibState *s = data;
    if (!gtk_toggle_button_get_active(btn)) return; /* ignore deactivation */

    const char *label = gtk_button_get_label(GTK_BUTTON(btn));
    s->portable_mode = (g_strcmp0(label, "Portable Drive") == 0);

    gtk_revealer_set_reveal_child(GTK_REVEALER(s->data_revealer), s->portable_mode);

    if (s->portable_mode) {
        update_auto_data_path(s);
    } else {
        g_free(s->data_path);
        s->data_path = NULL;
        gtk_label_set_text(GTK_LABEL(s->data_label), "Select a music folder first");
        gtk_widget_set_opacity(s->data_label, 0.5);
    }
}

static void on_music_folder_selected(GObject *src, GAsyncResult *res, gpointer data) {
    AddLibState *s = data;
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (err) { g_error_free(err); return; }
    if (!folder) return;

    g_free(s->music_path);
    s->music_path = g_file_get_path(folder);
    g_object_unref(folder);

    if (s->music_path) {
        gtk_label_set_text(GTK_LABEL(s->music_label), s->music_path);
        gtk_widget_set_opacity(s->music_label, 1.0);
        gtk_widget_set_sensitive(s->confirm_btn, TRUE);
        update_auto_data_path(s);
    }
}

static void on_data_folder_selected(GObject *src, GAsyncResult *res, gpointer data) {
    AddLibState *s = data;
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (err) { g_error_free(err); return; }
    if (!folder) return;

    g_free(s->data_path);
    s->data_path = g_file_get_path(folder);
    g_object_unref(folder);

    if (s->data_path) {
        gtk_label_set_text(GTK_LABEL(s->data_label), s->data_path);
        gtk_widget_set_opacity(s->data_label, 1.0);
    }
}

static void on_music_browse(GtkButton *btn, gpointer data) {
    (void)btn;
    AddLibState *s = data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Music Folder");
    GFile *root = g_file_new_for_path("/");
    gtk_file_dialog_set_initial_folder(dlg, root);
    g_object_unref(root);
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(s->w), NULL, on_music_folder_selected, s);
    g_object_unref(dlg);
}

static void on_data_browse(GtkButton *btn, gpointer data) {
    (void)btn;
    AddLibState *s = data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Data Directory");
    /* Start at the music folder if already chosen, else root */
    GFile *initial = s->music_path
        ? g_file_new_for_path(s->music_path)
        : g_file_new_for_path("/");
    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(s->w), NULL, on_data_folder_selected, s);
    g_object_unref(dlg);
}

static void on_add_confirm(GtkButton *btn, gpointer data) {
    (void)btn;
    AddLibState *s = data;
    if (!s->music_path || !s->w->settings) return;

    /* In local mode, data_path is implicitly the same as music_path */
    if (!s->portable_mode) {
        g_free(s->data_path);
        s->data_path = NULL;
    }

    /* In portable mode, create data path now to fail fast on permission errors */
    if (s->data_path) {
        if (g_mkdir_with_parents(s->data_path, 0755) != 0) {
            g_warning("on_add_confirm: failed to create data directory: %s", s->data_path);
            return;
        }
    }

    app_settings_add_library_path(s->w->settings, s->music_path);

    /* Set data path if different from music path */
    if (s->data_path && strcmp(s->data_path, s->music_path) != 0) {
        int idx = s->w->settings->library_path_count - 1;
        app_settings_set_library_data_path(s->w->settings, idx, s->data_path);
    }

    settings_save_debounced(s->w);
    libs_load(s->w);
    libs_rebuild(s->w);

    if (s->w->indexer) {
        const char *paths[] = { s->music_path };
        const char *data_paths[] = { s->data_path ? s->data_path : s->music_path };
        indexer_controller_start(s->w->indexer, paths, data_paths, 1);
    }

    gtk_popover_popdown(GTK_POPOVER(s->popover));
}

static void on_add_cancel(GtkButton *btn, gpointer data) {
    (void)btn;
    AddLibState *s = data;
    gtk_popover_popdown(GTK_POPOVER(s->popover));
}

static void on_add_popover_closed(GtkPopover *popover, gpointer data) {
    AddLibState *s = data;
    gtk_widget_unparent(GTK_WIDGET(popover));
    add_lib_state_free(s);
}

static void on_add_library(GtkButton *btn, gpointer data) {
    (void)btn;
    UiWindow *w = UI_WINDOW(data);

    GtkBuilder *b = gtk_builder_new_from_resource("/org/quadrature/ui/add_library_popover.ui");
    GtkWidget *popover        = GTK_WIDGET(gtk_builder_get_object(b, "add_library_popover"));
    GtkWidget *music_label    = GTK_WIDGET(gtk_builder_get_object(b, "music_path_label"));
    GtkWidget *data_label     = GTK_WIDGET(gtk_builder_get_object(b, "data_path_label"));
    GtkWidget *data_revealer  = GTK_WIDGET(gtk_builder_get_object(b, "data_section_revealer"));
    GtkWidget *music_browse   = GTK_WIDGET(gtk_builder_get_object(b, "music_browse_btn"));
    GtkWidget *data_browse    = GTK_WIDGET(gtk_builder_get_object(b, "data_browse_btn"));
    GtkWidget *confirm_btn    = GTK_WIDGET(gtk_builder_get_object(b, "add_confirm_btn"));
    GtkWidget *cancel_btn     = GTK_WIDGET(gtk_builder_get_object(b, "add_cancel_btn"));
    GtkWidget *mode_local     = GTK_WIDGET(gtk_builder_get_object(b, "mode_local_btn"));
    GtkWidget *mode_portable  = GTK_WIDGET(gtk_builder_get_object(b, "mode_portable_btn"));
    g_object_ref(popover);
    g_object_unref(b);

    /* Group toggle buttons for mutual exclusion */
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(mode_portable),
                                GTK_TOGGLE_BUTTON(mode_local));

    AddLibState *s  = g_new0(AddLibState, 1);
    s->w            = w;
    s->popover      = popover;
    s->music_label  = music_label;
    s->data_label   = data_label;
    s->data_revealer = data_revealer;
    s->confirm_btn  = confirm_btn;

    g_signal_connect(mode_local,    "toggled", G_CALLBACK(on_mode_toggled), s);
    g_signal_connect(mode_portable, "toggled", G_CALLBACK(on_mode_toggled), s);
    g_signal_connect(music_browse,  "clicked", G_CALLBACK(on_music_browse), s);
    g_signal_connect(data_browse,   "clicked", G_CALLBACK(on_data_browse),  s);
    g_signal_connect(confirm_btn,   "clicked", G_CALLBACK(on_add_confirm),  s);
    g_signal_connect(cancel_btn,    "clicked", G_CALLBACK(on_add_cancel),   s);
    g_signal_connect(popover,       "closed",  G_CALLBACK(on_add_popover_closed), s);

    /* Size content to match errors popover pattern */
    static const int MARGIN_SIDE = 100;
    int stack_w = gtk_widget_get_width(w->stack);
    int pop_w = MAX(400, stack_w - 2 * MARGIN_SIDE);
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));
    if (content)
        gtk_widget_set_size_request(content, pop_w, -1);

    /* Anchor to the content stack (like errors/metadata popovers) so the
     * popover stays alive when the native file dialog steals focus. */
    gtk_widget_set_parent(popover, w->stack);
    g_object_unref(popover);  /* parent now owns it */

    GdkRectangle anchor = { stack_w / 2, 100, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);

    gtk_popover_popup(GTK_POPOVER(popover));
}

GtkWidget *make_libraries_view(UiWindow *w) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/libraries_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "libraries_view"));
    g_object_ref(view);

    /* Get widget references */
    GtkWidget *add_btn = GTK_WIDGET(gtk_builder_get_object(builder, "add_library_btn"));
    w->libs_box   = GTK_WIDGET(gtk_builder_get_object(builder, "libs_box"));
    w->libs_empty = GTK_WIDGET(gtk_builder_get_object(builder, "libs_empty"));

    /* Connect signals */
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_library), w);

    g_object_unref(builder);

    libs_load(w);
    libs_rebuild(w);

    return view;
}
