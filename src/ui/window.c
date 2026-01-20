/**
 * Quadrature Main Window
 *
 * GtkApplicationWindow subclass: nav bar + content stack + channel panel.
 */

#include "internal.h"
#include "indexer_controller.h"
#include "perf/perf_view.h"
#include "quadrature/audio/audio_devices.h"
#include "quadrature/database/database.h"
#include <string.h>

/* View names for content stack pages */
static const char *VIEW_SEARCH = "search";
static const char *VIEW_ARTISTS = "artists";

/* Library entry */
typedef struct {
    int64_t id;
    char *path;
    char *name;
    size_t tracks;
    size_t errors;
    GtkWidget *card;
    GtkWidget *progress;
} LibEntry;

/* Progress phase widgets */
typedef struct {
    GtkWidget *container;
    GtkWidget *title;
    GtkWidget *bar;
    GtkWidget *label;
    GtkWidget *rate_label;
} ProgressPhaseWidgets;

struct _UiWindow {
    GtkApplicationWindow parent;

    audio_pipeline_t *pipeline;
    app_settings_t *settings;
    quadrature_db_t *db;
    IndexerController *indexer;
    LibraryCache *library_cache;
    ArtworkManager *artwork_mgr;

    /* Template-bound layout widgets */
    GtkWidget *main_box;
    GtkWidget *content_stack;
    GtkWidget *channel_strips_box;
    GtkWidget *toast_overlay;
    GtkWidget *toast_label;
    GtkWidget *errors_overlay;
    GtkWidget *errors_panel;
    GtkWidget *errors_view_container;
    GtkWidget *errors_close_btn;

    /* Runtime layout */
    GtkWidget *nav_bar;
    GSimpleAction *navigate_action;
    GtkWidget *stack;  /* Alias for content_stack */
    UiChannelStrip *channels[MAX_CHANNELS];
    int focused_channel;
    gboolean show_spectrum;
    const char *current_view;

    /* Toast timer */
    guint toast_timer;

    /* Errors */
    GtkWidget *errors_view;
    char *errors_library_path;

    /* Library views */
    GtkWidget *artists_view;
    GtkWidget *albums_view;
    GtkWidget *detail_view;     /* Unified detail view */
    const char *previous_view;  /* For back navigation */

    /* Search */
    GtkWidget *search_entry;
    GtkWidget *filter_btns[4];
    int filter_active;
    guint search_debounce_timer;
    char *last_search_query;
    GtkWidget *search_results_box;
    GtkWidget *search_artists_section;
    GtkWidget *search_albums_section;
    GtkWidget *search_tracks_section;
    GtkWidget *search_empty_label;

    /* Settings - devices */
    GtkWidget *device_drops[MAX_CHANNELS];
    GtkWidget *format_drops[MAX_CHANNELS];
    GtkWidget *gpio_entries[MAX_CHANNELS];
    GtkStringList *device_model;
    GtkStringList *format_model;
    char **device_names;
    int device_count;
    gboolean settings_initializing;  /* Prevents spurious saves during init */

    /* Libraries */
    LibEntry *libs;
    size_t lib_count;
    GtkWidget *libs_box;
    GtkWidget *libs_empty;

    /* Indexing Progress */
    GtkWidget *progress_container;
    ProgressPhaseWidgets scan_phase;
    ProgressPhaseWidgets metadata_phase;
    ProgressPhaseWidgets artwork_phase;
    guint progress_pulse_timer;

    guint update_timer;
    GtkCssProvider *css;
};

G_DEFINE_FINAL_TYPE(UiWindow, ui_window, GTK_TYPE_APPLICATION_WINDOW)

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_ui(UiWindow *w);
static void load_css(UiWindow *w);
static GtkWidget *make_search_view(UiWindow *w);
static GtkWidget *make_settings_view(UiWindow *w);
static GtkWidget *make_help_view(void);
static GtkWidget *make_libraries_view(UiWindow *w);
static void libs_load(UiWindow *w);
static void libs_rebuild(UiWindow *w);
static void populate_devices_async(UiWindow *w);

/* Library view callbacks */
static void on_library_navigate(LibraryItemKind kind, int64_t id, gpointer data);
static void on_library_play(const char *path, const char *title, const char *artist, const char *album, int64_t track_id, gpointer data);
static void on_library_back(gpointer data);
static void on_track_info(int64_t track_id, gpointer data);
static void on_channel_strip_clicked(UiChannelStrip *strip, int channel_id, gpointer data);
static void on_channel_strip_mode_changed(UiChannelStrip *strip, int channel_id, int new_mode, gpointer data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean on_update(gpointer data) {
    if (!UI_IS_WINDOW(data)) return G_SOURCE_REMOVE;
    UiWindow *w = UI_WINDOW(data);
    if (!w->pipeline) return G_SOURCE_CONTINUE;

    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->channels[i] && UI_IS_CHANNEL_STRIP(w->channels[i]))
            ui_channel_strip_update(w->channels[i], w->pipeline);
    }
    return G_SOURCE_CONTINUE;
}

/* Navigate action handler - triggered by nav bar buttons */
static void on_navigate_action(GSimpleAction *action, GVariant *param, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    const char *view = g_variant_get_string(param, NULL);

    /* Update action state (auto-updates toggle button :checked states) */
    g_simple_action_set_state(action, param);

    /* Switch stack page */
    w->current_view = view;
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), view);
}

static void do_search(UiWindow *w);  /* Forward declaration */

static void on_filter_clicked(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    for (int i = 0; i < 4; i++) {
        gboolean active = (GTK_WIDGET(btn) == w->filter_btns[i]);
        ui_toggle_css(w->filter_btns[i], "search-filter-active", active);
        if (active) w->filter_active = i;
    }
    /* Re-run search with new filter */
    do_search(w);
}

static void on_spectrum_toggled(GtkCheckButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    gboolean on = gtk_check_button_get_active(btn);
    ui_window_set_spectrum_visible(w, on);
    if (w->settings) {
        w->settings->show_spectrum = on;
        app_settings_save(w->settings);
    }
}

static void on_channel_strip_clicked(UiChannelStrip *strip, int channel_id, gpointer data) {
    (void)strip;
    UiWindow *w = UI_WINDOW(data);
    ui_window_set_focused_channel(w, channel_id);
}

static void on_channel_strip_mode_changed(UiChannelStrip *strip, int channel_id, int new_mode, gpointer data) {
    (void)strip;
    UiWindow *w = UI_WINDOW(data);

    /* Clear focus if this channel was focused and entered QUEUED/ON_AIR */
    if (w->focused_channel == channel_id) {
        if (new_mode == CHANNEL_MODE_QUEUED || new_mode == CHANNEL_MODE_ON_AIR) {
            ui_window_clear_focus(w);
        }
    }
}

static void on_channel_album_clicked(UiChannelStrip *strip, int channel_id, int64_t album_id, gpointer data) {
    (void)strip; (void)channel_id;
    UiWindow *w = UI_WINDOW(data);

    if (album_id > 0) {
        /* Navigate to album detail view */
        w->previous_view = w->current_view;
        library_unified_detail_navigate_to_album(w->detail_view, album_id, w->current_view);
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
        w->current_view = "detail";
    }
}

static void on_channel_artist_clicked(UiChannelStrip *strip, int channel_id, int64_t artist_id, gpointer data) {
    (void)strip; (void)channel_id;
    UiWindow *w = UI_WINDOW(data);

    if (artist_id > 0) {
        /* Navigate to artist detail view */
        w->previous_view = w->current_view;
        library_unified_detail_navigate_to_artist(w->detail_view, artist_id, w->current_view);
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
        w->current_view = "detail";
    }
}

static void on_channel_track_changed(UiChannelStrip *strip, int channel_id, int new_index, gpointer data) {
    (void)strip; (void)channel_id; (void)new_index; (void)data;
    /* Log for debugging, could be used for future features like play history */
    g_debug("Track changed on channel %d to index %d", channel_id, new_index);
}

static void on_channel_track_ended(UiChannelStrip *strip, int channel_id, gpointer data) {
    (void)strip; (void)channel_id; (void)data;
    /* Log for debugging, could be used for future features like play history */
    g_debug("Track ended on channel %d", channel_id);
}

static gboolean on_key(GtkEventControllerKey *ctl, guint key, guint code,
                       GdkModifierType state, gpointer data) {
    (void)ctl; (void)code;
    UiWindow *w = UI_WINDOW(data);
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    /* Escape: close errors overlay if open */
    if (key == GDK_KEY_Escape && w->errors_overlay &&
        gtk_widget_get_visible(w->errors_overlay)) {
        gtk_widget_set_visible(w->errors_overlay, FALSE);
        return TRUE;
    }

    if (ctrl) {
        if (key == GDK_KEY_f) { ui_window_navigate_to(w, VIEW_SEARCH); return TRUE; }
    }

    /* 1-4: Set focus to channel (if channel is active) */
    if (key >= GDK_KEY_1 && key <= GDK_KEY_4) {
        int ch = key - GDK_KEY_1;
        if (ch < MAX_CHANNELS && w->channels[ch]) {
            /* Check if channel can be focused */
            if (!ui_channel_strip_is_active(w->channels[ch])) {
                /* Show toast explaining why */
                DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
                ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);

                if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
                    ui_window_show_toast(w, "Channel has no Audio Output Configured");
                } else if (mode == CHANNEL_MODE_QUEUED) {
                    ui_window_show_toast(w, "Channel is Queued");
                } else if (mode == CHANNEL_MODE_ON_AIR) {
                    ui_window_show_toast(w, "Channel is On-Air");
                }
                return TRUE;
            }
        }
        ui_window_set_focused_channel(w, ch);
        return TRUE;
    }

    if (key >= GDK_KEY_F1 && key <= GDK_KEY_F4 && w->pipeline) {
        int ch = key - GDK_KEY_F1;
        if (shift) audio_pipeline_player_stop(w->pipeline, ch);
        else       audio_pipeline_player_play(w->pipeline, ch);
        return TRUE;
    }

    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Enumeration
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_pipeline_t *pipeline;
    audio_device_list_t devices;
    quadrature_result_t result;
} DeviceEnumData;

static void device_enum_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    DeviceEnumData *d = data;
    d->result = audio_devices_enumerate(d->pipeline, &d->devices);
    g_task_return_pointer(task, d, NULL);
}

static void device_enum_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    UiWindow *w = UI_WINDOW(data);
    GError *err = NULL;
    DeviceEnumData *d = g_task_propagate_pointer(G_TASK(res), &err);

    if (err) { g_error_free(err); return; }

    /* Block callbacks while restoring settings */
    w->settings_initializing = TRUE;

    /* Free old */
    if (w->device_names) {
        for (int i = 0; i < w->device_count; i++) g_free(w->device_names[i]);
        g_free(w->device_names);
    }
    g_clear_object(&w->device_model);

    /* Build new model */
    w->device_model = gtk_string_list_new(NULL);
    gtk_string_list_append(w->device_model, "None");

    if (d->result == QUADRATURE_OK && d->devices.count > 0) {
        w->device_names = g_new0(char*, d->devices.count);
        w->device_count = d->devices.count;
        for (int i = 0; i < d->devices.count; i++) {
            gtk_string_list_append(w->device_model, d->devices.devices[i].description);
            w->device_names[i] = g_strdup(d->devices.devices[i].node_name);
        }
        audio_devices_free(&d->devices);
    } else {
        w->device_names = NULL;
        w->device_count = 0;
    }

    /* Update dropdowns with saved settings */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->device_drops[i]) {
            gtk_drop_down_set_model(GTK_DROP_DOWN(w->device_drops[i]),
                                    G_LIST_MODEL(w->device_model));

            const char *saved = w->settings ? app_settings_get_channel_device(w->settings, i) : NULL;
            guint sel = 0;
            if (saved && saved[0]) {
                for (int j = 0; j < w->device_count; j++) {
                    if (strcmp(w->device_names[j], saved) == 0) {
                        sel = j + 1;
                        break;
                    }
                }
            }
            gtk_drop_down_set_selected(GTK_DROP_DOWN(w->device_drops[i]), sel);

            /* Update channel state using new layered API */
            if (w->channels[i]) {
                if (!saved || !saved[0]) {
                    ui_channel_strip_set_device_name(w->channels[i], NULL);
                    ui_channel_strip_set_device_state(w->channels[i], DEVICE_STATE_UNCONFIGURED);
                } else if (sel == 0) {
                    /* Device was configured but not found - set name for error message */
                    ui_channel_strip_set_device_name(w->channels[i], saved);
                    ui_channel_strip_set_device_state(w->channels[i], DEVICE_STATE_INVALID);
                } else {
                    ui_channel_strip_set_device_name(w->channels[i], NULL);
                    ui_channel_strip_set_device_state(w->channels[i], DEVICE_STATE_VALID);
                    audio_pipeline_set_player_device(w->pipeline, i, saved);
                }
            }
        }

        /* Restore format selection */
        if (w->format_drops[i] && w->settings) {
            output_format_t fmt = app_settings_get_channel_format(w->settings, i);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(w->format_drops[i]), (guint)fmt);
        }

        /* Restore GPIO address */
        if (w->gpio_entries[i] && w->settings) {
            const char *gpio = app_settings_get_channel_gpio(w->settings, i);
            gtk_editable_set_text(GTK_EDITABLE(w->gpio_entries[i]), gpio ? gpio : "");
        }
    }

    /* Re-enable callbacks */
    w->settings_initializing = FALSE;

    g_free(d);
}

static void populate_devices_async(UiWindow *w) {
    if (!w->pipeline) return;
    DeviceEnumData *d = g_new0(DeviceEnumData, 1);
    d->pipeline = w->pipeline;
    GTask *task = g_task_new(NULL, NULL, device_enum_done, w);
    g_task_set_task_data(task, d, NULL);
    g_task_run_in_thread(task, device_enum_thread);
    g_object_unref(task);
}

static void on_device_changed(GtkDropDown *drop, GParamSpec *p, gpointer data) {
    (void)p;
    UiWindow *w = UI_WINDOW(data);

    /* Skip save during initialization - settings are being restored, not changed */
    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(drop) == w->device_drops[i]) { ch = i; break; }
    }
    if (ch < 0 || !w->pipeline) return;

    guint sel = gtk_drop_down_get_selected(drop);
    const char *name = (sel > 0 && (int)(sel - 1) < w->device_count)
                       ? w->device_names[sel - 1] : NULL;

    audio_pipeline_set_player_device(w->pipeline, ch, name);

    if (w->channels[ch]) {
        ui_channel_strip_set_device_name(w->channels[ch], NULL);
        ui_channel_strip_set_device_state(w->channels[ch],
            name ? DEVICE_STATE_VALID : DEVICE_STATE_UNCONFIGURED);
    }

    if (w->settings) {
        app_settings_set_channel_device(w->settings, ch, name);
        app_settings_save(w->settings);
    }
}

static void on_format_changed(GtkDropDown *drop, GParamSpec *p, gpointer data) {
    (void)p;
    UiWindow *w = UI_WINDOW(data);

    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(drop) == w->format_drops[i]) { ch = i; break; }
    }
    if (ch < 0) return;

    guint sel = gtk_drop_down_get_selected(drop);
    if (w->settings && sel < OUTPUT_FORMAT_COUNT) {
        app_settings_set_channel_format(w->settings, ch, (output_format_t)sel);
        app_settings_save(w->settings);
    }
}

static void on_gpio_changed(GtkEditable *editable, gpointer data) {
    UiWindow *w = UI_WINDOW(data);

    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(editable) == w->gpio_entries[i]) { ch = i; break; }
    }
    if (ch < 0) return;

    const char *text = gtk_editable_get_text(editable);
    if (w->settings) {
        app_settings_set_channel_gpio(w->settings, ch, text);
        app_settings_save(w->settings);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Progress Display Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void format_rate_eta(int64_t phase_start_time,
                            size_t processed, size_t total,
                            const char *unit,
                            char *out_buf, size_t buf_size) {
    int64_t now = g_get_monotonic_time();
    double elapsed_sec = (now - phase_start_time) / 1000000.0;

    if (elapsed_sec < 0.1 || processed == 0) {
        snprintf(out_buf, buf_size, "Calculating...");
        return;
    }

    double rate = processed / elapsed_sec;
    size_t remaining = total - processed;
    double eta_sec = (rate > 0) ? remaining / rate : 0;

    if (eta_sec < 60) {
        snprintf(out_buf, buf_size, "%.1f %s/sec · ~%.0fs remaining", rate, unit, eta_sec);
    } else {
        snprintf(out_buf, buf_size, "%.1f %s/sec · ~%.1fm remaining", rate, unit, eta_sec / 60.0);
    }
}

static void set_phase_state(ProgressPhaseWidgets *phase, const char *state_class) {
    /* Remove all state classes */
    gtk_widget_remove_css_class(phase->container, "progress-phase-dim");
    gtk_widget_remove_css_class(phase->container, "progress-phase-active");
    gtk_widget_remove_css_class(phase->container, "progress-phase-complete");

    /* Add the new state class */
    if (state_class && state_class[0])
        gtk_widget_add_css_class(phase->container, state_class);
}

/* Helper to load phase widgets from builder */
static void load_phase_widgets(GtkBuilder *builder, ProgressPhaseWidgets *phase, const char *prefix) {
    char id[64];

    snprintf(id, sizeof(id), "%s_phase_container", prefix);
    phase->container = GTK_WIDGET(gtk_builder_get_object(builder, id));

    snprintf(id, sizeof(id), "%s_phase_title", prefix);
    phase->title = GTK_WIDGET(gtk_builder_get_object(builder, id));

    snprintf(id, sizeof(id), "%s_phase_bar", prefix);
    phase->bar = GTK_WIDGET(gtk_builder_get_object(builder, id));

    snprintf(id, sizeof(id), "%s_phase_label", prefix);
    phase->label = GTK_WIDGET(gtk_builder_get_object(builder, id));

    snprintf(id, sizeof(id), "%s_phase_rate", prefix);
    phase->rate_label = GTK_WIDGET(gtk_builder_get_object(builder, id));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Libraries
 * ═══════════════════════════════════════════════════════════════════════════ */

static void libs_free(UiWindow *w) {
    if (w->libs) {
        for (size_t i = 0; i < w->lib_count; i++) {
            g_free(w->libs[i].path);
            g_free(w->libs[i].name);
        }
        g_free(w->libs);
        w->libs = NULL;
    }
    w->lib_count = 0;
}

static void libs_load(UiWindow *w) {
    libs_free(w);
    if (!w->db) return;

    db_watch_path_t *paths = NULL;
    size_t count = 0;
    if (db_get_watch_paths(w->db, &paths, &count) != QUADRATURE_OK || count == 0)
        return;

    w->libs = g_new0(LibEntry, count);
    w->lib_count = count;

    for (size_t i = 0; i < count; i++) {
        w->libs[i].id = paths[i].id;
        w->libs[i].path = g_strdup(paths[i].path);
        const char *slash = strrchr(paths[i].path, '/');
        w->libs[i].name = g_strdup(slash ? slash + 1 : paths[i].path);
        db_get_track_count_for_path(w->db, paths[i].path, &w->libs[i].tracks);
        db_get_error_count(w->db, paths[i].path, &w->libs[i].errors);
    }
    db_free_watch_paths(paths, count);
}

static void on_rescan(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (e && w->indexer && !indexer_controller_is_running(w->indexer)) {
        const char *paths[] = { e->path };
        indexer_controller_start(w->indexer, paths, 1);
    }
}

static void on_remove(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (e && w->db) {
        db_remove_watch_path(w->db, e->path);
        libs_load(w);
        libs_rebuild(w);
    }
}

static void on_errors(GtkButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !w->db || !w->errors_overlay) return;

    /* Store library path for filtering */
    g_free(w->errors_library_path);
    w->errors_library_path = g_strdup(e->path);

    /* Update view with library filter */
    errors_view_set_path_filter(w->errors_view, w->errors_library_path);
    errors_view_refresh(w->errors_view);

    /* Show overlay */
    gtk_widget_set_visible(w->errors_overlay, TRUE);
}

static void on_errors_close(GtkButton *btn, gpointer data) {
    (void)btn;
    UiWindow *w = UI_WINDOW(data);
    gtk_widget_set_visible(w->errors_overlay, FALSE);
}

static GtkWidget *make_lib_card(UiWindow *w, LibEntry *e) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(card, "library-card");
    e->card = card;

    GtkWidget *name = gtk_label_new(e->name);
    gtk_widget_add_css_class(name, "library-card-name");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_box_append(GTK_BOX(card), name);

    GtkWidget *path = gtk_label_new(e->path);
    gtk_widget_add_css_class(path, "library-card-path");
    gtk_label_set_xalign(GTK_LABEL(path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(card), path);

    char detail[64];
    snprintf(detail, sizeof(detail), "%zu tracks", e->tracks);
    GtkWidget *det = gtk_label_new(detail);
    gtk_widget_add_css_class(det, "library-card-detail");
    gtk_label_set_xalign(GTK_LABEL(det), 0.0);
    gtk_box_append(GTK_BOX(card), det);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);

    /* Errors button - show count if > 0 */
    GtkWidget *errors_btn;
    if (e->errors > 0) {
        char label[32];
        snprintf(label, sizeof(label), "Errors (%zu)", e->errors);
        errors_btn = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(errors_btn, "error-button");
    } else {
        errors_btn = gtk_button_new_with_label("Errors");
    }
    g_object_set_data(G_OBJECT(errors_btn), "entry", e);
    g_signal_connect(errors_btn, "clicked", G_CALLBACK(on_errors), w);
    gtk_box_append(GTK_BOX(btns), errors_btn);

    GtkWidget *rescan = gtk_button_new_with_label("Rescan");
    g_object_set_data(G_OBJECT(rescan), "entry", e);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan), w);
    gtk_box_append(GTK_BOX(btns), rescan);

    GtkWidget *remove = gtk_button_new_with_label("Remove");
    g_object_set_data(G_OBJECT(remove), "entry", e);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), w);
    gtk_box_append(GTK_BOX(btns), remove);

    gtk_box_append(GTK_BOX(card), btns);
    return card;
}

static void libs_rebuild(UiWindow *w) {
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

static void on_folder_selected(GObject *src, GAsyncResult *res, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (err) { g_error_free(err); return; }
    if (!folder) return;

    char *path = g_file_get_path(folder);
    g_object_unref(folder);
    if (!path) return;

    if (w->db && db_add_watch_path(w->db, path) == QUADRATURE_OK) {
        libs_load(w);
        libs_rebuild(w);
        if (w->indexer) {
            const char *paths[] = { path };
            indexer_controller_start(w->indexer, paths, 1);
        }
    }
    g_free(path);
}

static void on_add_library(GtkButton *btn, gpointer data) {
    (void)btn;
    UiWindow *w = UI_WINDOW(data);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Music Folder");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(w), NULL, on_folder_selected, w);
    g_object_unref(dlg);
}

static gboolean on_scan_pulse(gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->progress_container && gtk_widget_get_visible(w->progress_container)) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(w->scan_phase.bar));
        return G_SOURCE_CONTINUE;
    }
    w->progress_pulse_timer = 0;
    return G_SOURCE_REMOVE;
}

static void on_indexer_started(IndexerController *idx, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);

    /* Show progress section */
    gtk_widget_set_visible(w->progress_container, TRUE);

    /* Reset all phases */
    set_phase_state(&w->scan_phase, "progress-phase-active");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->scan_phase.bar), 0.0);
    gtk_label_set_text(GTK_LABEL(w->scan_phase.label), "Starting...");
    gtk_label_set_text(GTK_LABEL(w->scan_phase.rate_label), "");

    set_phase_state(&w->metadata_phase, "progress-phase-dim");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->metadata_phase.bar), 0.0);
    gtk_label_set_text(GTK_LABEL(w->metadata_phase.label), "0/0 tracks");
    gtk_label_set_text(GTK_LABEL(w->metadata_phase.rate_label), "");

    set_phase_state(&w->artwork_phase, "progress-phase-dim");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->artwork_phase.bar), 0.0);
    gtk_label_set_text(GTK_LABEL(w->artwork_phase.label), "0/0 albums");
    gtk_label_set_text(GTK_LABEL(w->artwork_phase.rate_label), "");

    /* Start pulse animation for scan phase */
    if (w->progress_pulse_timer == 0)
        w->progress_pulse_timer = g_timeout_add(100, on_scan_pulse, w);
}

static void on_indexer_progress(IndexerController *idx, indexer_progress_t *p, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);
    char buf[128];
    char rate_buf[128];

    switch (p->phase) {
    case INDEXER_PHASE_SCANNING:
        /* Show current directory and file progress */
        if (p->current_path && p->current_path[0]) {
            /* Extract directory name from path for display */
            const char* dir_name = strrchr(p->current_path, '/');
            dir_name = dir_name ? dir_name + 1 : p->current_path;

            snprintf(buf, sizeof(buf), "Scanning: %.48s", dir_name);
        } else {
            snprintf(buf, sizeof(buf), "Scanned %zu directories", p->dirs_scanned);
        }
        gtk_label_set_text(GTK_LABEL(w->scan_phase.label), buf);

        /* Also update metadata counts if available */
        snprintf(buf, sizeof(buf), "%zu/%zu tracks", p->files_processed, p->files_total);
        gtk_label_set_text(GTK_LABEL(w->metadata_phase.label), buf);
        if (p->files_total > 0) {
            double frac = (double)p->files_processed / p->files_total;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->metadata_phase.bar), frac);
        }
        break;

    case INDEXER_PHASE_METADATA:
        /* Scanning complete */
        if (w->progress_pulse_timer) {
            g_source_remove(w->progress_pulse_timer);
            w->progress_pulse_timer = 0;
        }
        set_phase_state(&w->scan_phase, "progress-phase-complete");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->scan_phase.bar), 1.0);
        gtk_label_set_text(GTK_LABEL(w->scan_phase.rate_label), "Complete");

        /* Update metadata phase */
        set_phase_state(&w->metadata_phase, "progress-phase-active");
        snprintf(buf, sizeof(buf), "%zu/%zu tracks", p->files_processed, p->files_total);
        gtk_label_set_text(GTK_LABEL(w->metadata_phase.label), buf);

        if (p->files_total > 0) {
            double frac = (double)p->files_processed / p->files_total;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->metadata_phase.bar), frac);
            format_rate_eta(p->phase_start_time, p->files_processed, p->files_total,
                           "tracks", rate_buf, sizeof(rate_buf));
            gtk_label_set_text(GTK_LABEL(w->metadata_phase.rate_label), rate_buf);
        }
        break;

    case INDEXER_PHASE_ARTWORK:
        /* Metadata complete */
        set_phase_state(&w->metadata_phase, "progress-phase-complete");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->metadata_phase.bar), 1.0);
        gtk_label_set_text(GTK_LABEL(w->metadata_phase.rate_label), "Complete");

        /* Update artwork phase */
        set_phase_state(&w->artwork_phase, "progress-phase-active");
        snprintf(buf, sizeof(buf), "%zu/%zu albums", p->albums_processed, p->albums_total);
        gtk_label_set_text(GTK_LABEL(w->artwork_phase.label), buf);

        if (p->albums_total > 0) {
            double frac = (double)p->albums_processed / p->albums_total;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->artwork_phase.bar), frac);
            format_rate_eta(p->phase_start_time, p->albums_processed, p->albums_total,
                           "albums", rate_buf, sizeof(rate_buf));
            gtk_label_set_text(GTK_LABEL(w->artwork_phase.rate_label), rate_buf);
        }
        break;

    case INDEXER_PHASE_FINALIZE:
        /* Artwork complete, finalizing DB */
        set_phase_state(&w->artwork_phase, "progress-phase-complete");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->artwork_phase.bar), 1.0);
        gtk_label_set_text(GTK_LABEL(w->artwork_phase.rate_label), "Complete");
        break;

    case INDEXER_PHASE_COMPLETE:
        /* All complete */
        set_phase_state(&w->artwork_phase, "progress-phase-complete");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->artwork_phase.bar), 1.0);
        gtk_label_set_text(GTK_LABEL(w->artwork_phase.rate_label), "Complete");
        break;
    }
}

static void on_indexer_done(IndexerController *idx, gboolean ok, indexer_progress_t *p, gpointer data) {
    (void)idx; (void)ok; (void)p;
    UiWindow *w = UI_WINDOW(data);

    /* Stop pulse timer if still running */
    if (w->progress_pulse_timer) {
        g_source_remove(w->progress_pulse_timer);
        w->progress_pulse_timer = 0;
    }

    /* Hide progress after a short delay so user can see final state */
    /* For now, just hide immediately */
    gtk_widget_set_visible(w->progress_container, FALSE);

    /* Reload library data */
    libs_load(w);
    libs_rebuild(w);

    /* Reload artwork atlas (new thumbnails may have been generated) */
    if (w->artwork_mgr) {
        artwork_manager_clear(w->artwork_mgr);
        artwork_manager_reload_atlas(w->artwork_mgr);
    }

    /* Invalidate library cache so views fetch fresh data */
    if (w->library_cache)
        library_cache_invalidate(w->library_cache);

    /* Refresh library views */
    library_view_refresh(w->artists_view);
    library_view_refresh(w->albums_view);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Search Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

static void clear_search_section(GtkWidget *section) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(section)))
        gtk_box_remove(GTK_BOX(section), child);
}


static void do_search(UiWindow *w) {
    if (!w->db) return;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(w->search_entry));
    if (!query || strlen(query) < 1) {
        /* Clear results */
        clear_search_section(w->search_artists_section);
        clear_search_section(w->search_albums_section);
        clear_search_section(w->search_tracks_section);
        gtk_widget_set_visible(w->search_results_box, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "Type to search...");
        return;
    }

    /* Determine search type from filter */
    db_search_type_t type = DB_SEARCH_ALL;
    size_t limit = 0; /* 0 = use defaults */
    switch (w->filter_active) {
        case 1: type = DB_SEARCH_ARTISTS; break;
        case 2: type = DB_SEARCH_ALBUMS; break;
        case 3: type = DB_SEARCH_TRACKS; break;
    }

    db_search_results_t results = {0};
    if (db_search_typed(w->db, query, type, limit, &results) != QUADRATURE_OK) {
        gtk_widget_set_visible(w->search_results_box, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "Search failed");
        return;
    }

    /* Check if any results */
    if (results.artist_count == 0 && results.album_count == 0 && results.track_count == 0) {
        clear_search_section(w->search_artists_section);
        clear_search_section(w->search_albums_section);
        clear_search_section(w->search_tracks_section);
        gtk_widget_set_visible(w->search_results_box, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "No results found");
        db_search_results_free(&results);
        return;
    }

    gtk_widget_set_visible(w->search_empty_label, FALSE);
    gtk_widget_set_visible(w->search_results_box, TRUE);

    /* Set up callbacks for search results - uses same handlers as library views */
    LibraryCallbacks search_cbs = {
        .on_navigate = on_library_navigate,
        .on_play = on_library_play,
        .on_back = on_library_back,
        .on_track_info = on_track_info,
        .user_data = w
    };

    /* Populate artists section */
    clear_search_section(w->search_artists_section);
    if (results.artist_count > 0) {
        GtkWidget *header = gtk_label_new("ARTISTS");
        gtk_widget_add_css_class(header, "search-results-section-header");
        gtk_label_set_xalign(GTK_LABEL(header), 0.0);
        gtk_box_append(GTK_BOX(w->search_artists_section), header);

        for (size_t i = 0; i < results.artist_count; i++) {
            GtkWidget *row = ui_create_artist_row(&results.artists[i], FALSE, &search_cbs);
            gtk_box_append(GTK_BOX(w->search_artists_section), row);
        }
        gtk_widget_set_visible(w->search_artists_section, TRUE);
    } else {
        gtk_widget_set_visible(w->search_artists_section, FALSE);
    }

    /* Populate albums section */
    clear_search_section(w->search_albums_section);
    if (results.album_count > 0) {
        GtkWidget *header = gtk_label_new("ALBUMS");
        gtk_widget_add_css_class(header, "search-results-section-header");
        gtk_label_set_xalign(GTK_LABEL(header), 0.0);
        gtk_box_append(GTK_BOX(w->search_albums_section), header);

        for (size_t i = 0; i < results.album_count; i++) {
            GtkWidget *row = ui_create_album_row(&results.albums[i], w->artwork_mgr, FALSE,
                                                  w->db, &search_cbs);
            gtk_box_append(GTK_BOX(w->search_albums_section), row);
        }
        gtk_widget_set_visible(w->search_albums_section, TRUE);
    } else {
        gtk_widget_set_visible(w->search_albums_section, FALSE);
    }

    /* Populate tracks section */
    clear_search_section(w->search_tracks_section);
    if (results.track_count > 0) {
        GtkWidget *header = gtk_label_new("SONGS");
        gtk_widget_add_css_class(header, "search-results-section-header");
        gtk_label_set_xalign(GTK_LABEL(header), 0.0);
        gtk_box_append(GTK_BOX(w->search_tracks_section), header);

        for (size_t i = 0; i < results.track_count; i++) {
            GtkWidget *row = ui_create_track_row(&results.tracks[i], w->artwork_mgr, FALSE,
                                                  &search_cbs);
            gtk_box_append(GTK_BOX(w->search_tracks_section), row);
        }
        gtk_widget_set_visible(w->search_tracks_section, TRUE);
    } else {
        gtk_widget_set_visible(w->search_tracks_section, FALSE);
    }

    db_search_results_free(&results);
}

static gboolean on_search_debounce(gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    w->search_debounce_timer = 0;
    do_search(w);
    return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    UiWindow *w = UI_WINDOW(data);

    /* Cancel previous timer */
    if (w->search_debounce_timer) {
        g_source_remove(w->search_debounce_timer);
        w->search_debounce_timer = 0;
    }

    /* Start new debounce timer (200ms) */
    w->search_debounce_timer = g_timeout_add(200, on_search_debounce, w);
}

static void on_search_activate(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    UiWindow *w = UI_WINDOW(data);

    /* Cancel debounce and search immediately */
    if (w->search_debounce_timer) {
        g_source_remove(w->search_debounce_timer);
        w->search_debounce_timer = 0;
    }
    do_search(w);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * View Builders
 * ═══════════════════════════════════════════════════════════════════════════ */

static GtkWidget *make_search_view(UiWindow *w) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/search_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "search_view"));
    g_object_ref(view);

    /* Get widget references */
    w->search_entry = GTK_WIDGET(gtk_builder_get_object(builder, "search_entry"));
    w->filter_btns[0] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_all"));
    w->filter_btns[1] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_artists"));
    w->filter_btns[2] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_albums"));
    w->filter_btns[3] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_songs"));
    w->search_empty_label = GTK_WIDGET(gtk_builder_get_object(builder, "search_empty_label"));
    w->search_results_box = GTK_WIDGET(gtk_builder_get_object(builder, "search_results_box"));
    w->search_artists_section = GTK_WIDGET(gtk_builder_get_object(builder, "search_artists_section"));
    w->search_albums_section = GTK_WIDGET(gtk_builder_get_object(builder, "search_albums_section"));
    w->search_tracks_section = GTK_WIDGET(gtk_builder_get_object(builder, "search_tracks_section"));

    /* Connect signals */
    g_signal_connect(w->search_entry, "search-changed", G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->search_entry, "activate", G_CALLBACK(on_search_activate), w);

    for (int i = 0; i < 4; i++) {
        g_signal_connect(w->filter_btns[i], "clicked", G_CALLBACK(on_filter_clicked), w);
    }

    g_object_unref(builder);
    return view;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library View Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_library_navigate(LibraryItemKind kind, int64_t id, gpointer data) {
    UiWindow *w = UI_WINDOW(data);

    if (kind == LIBRARY_ITEM_ARTIST) {
        g_message("Artist selected: id=%" G_GINT64_FORMAT, id);
        w->previous_view = VIEW_ARTISTS;
        library_unified_detail_navigate_to_artist(w->detail_view, id, "Artists");
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
        w->current_view = "detail";
    } else if (kind == LIBRARY_ITEM_ALBUM) {
        g_message("Album selected: id=%" G_GINT64_FORMAT, id);
        const char *source = (w->current_view == VIEW_ARTISTS) ? "Artists" : "Albums";
        if (strcmp(w->current_view, "detail") != 0)
            w->previous_view = w->current_view;
        library_unified_detail_navigate_to_album(w->detail_view, id, source);
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
        w->current_view = "detail";
    }
}

static void on_library_play(const char *path, const char *title,
                            const char *artist, const char *album,
                            int64_t track_id, gpointer data) {
    UiWindow *w = UI_WINDOW(data);

    /* Check if a channel is focused */
    if (w->focused_channel < 0) {
        ui_window_show_toast(w, "Focus a channel first (click channel number or press 1-4)");
        return;
    }

    int ch = w->focused_channel;

    /* Check if target channel can receive tracks */
    if (ch >= 0 && ch < MAX_CHANNELS && w->channels[ch]) {
        if (!ui_channel_strip_is_active(w->channels[ch])) {
            DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
            ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);

            if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
                ui_window_show_toast(w, "Channel has no Audio Output Configured");
            } else if (mode == CHANNEL_MODE_QUEUED) {
                ui_window_show_toast(w, "Channel is Queued");
            } else if (mode == CHANNEL_MODE_ON_AIR) {
                ui_window_show_toast(w, "Channel is On-Air");
            }
            return;
        }
    }

    g_message("Track queued to channel %d: \"%s\" by %s (%s)", ch + 1, title, artist, album);
    g_debug("Track path: %s", path);

    if (!w->channels[ch])
        return;

    /* Load the track */
    ui_channel_strip_load_track(w->channels[ch], path, title, artist, album);

    /* Load album context for track navigation */
    if (w->db && track_id > 0) {
        db_track_t *track = NULL;
        if (db_get_track(w->db, track_id, &track) == QUADRATURE_OK && track) {
            if (track->album_id > 0) {
                db_track_t *album_tracks = NULL;
                size_t count = 0;
                if (db_get_tracks_by_album(w->db, track->album_id, &album_tracks, &count) == QUADRATURE_OK
                    && album_tracks && count > 0) {
                    /* Find current track index in album */
                    int idx = 0;
                    for (size_t i = 0; i < count; i++) {
                        if (album_tracks[i].id == track_id) {
                            idx = (int)i;
                            break;
                        }
                    }

                    ui_channel_strip_set_album_context(w->channels[ch],
                                                        track->album_id,
                                                        track->album,
                                                        album_tracks,
                                                        (int)count,
                                                        idx);
                    db_tracks_free(album_tracks, count);
                } else {
                    /* No album tracks found - clear context */
                    ui_channel_strip_clear_album_context(w->channels[ch]);
                }
            } else {
                /* No album_id - clear context */
                ui_channel_strip_clear_album_context(w->channels[ch]);
            }
            db_track_free(track);
        } else {
            /* Couldn't get track info - clear context */
            ui_channel_strip_clear_album_context(w->channels[ch]);
        }
    } else {
        /* No database or track_id - clear context */
        ui_channel_strip_clear_album_context(w->channels[ch]);
    }
}

static void on_library_back(gpointer data) {
    UiWindow *w = UI_WINDOW(data);

    /* If detail view handles back internally, we're done */
    if (library_unified_detail_go_back(w->detail_view))
        return;

    /* Otherwise return to previous main view */
    if (w->previous_view) {
        ui_window_navigate_to(w, w->previous_view);
        library_unified_detail_show_empty(w->detail_view);
    }
}

static void on_track_info(int64_t track_id, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (!w->db || track_id <= 0) return;

    /* Get track info */
    db_track_t *track = NULL;
    if (db_get_track(w->db, track_id, &track) != QUADRATURE_OK) {
        return;
    }

    /* Get extended metadata */
    db_track_metadata_t *metadata = NULL;
    db_get_track_metadata(w->db, track_id, &metadata);  /* May be NULL if not available */

    /* Create and show metadata dialog */
    GtkWidget *dialog = ui_metadata_dialog_new(GTK_WINDOW(w));
    ui_metadata_dialog_set_track(UI_METADATA_DIALOG(dialog), track, metadata);
    gtk_window_present(GTK_WINDOW(dialog));

    /* Cleanup */
    db_track_free(track);
    if (metadata) db_track_metadata_free(metadata);
}

static GtkWidget *make_libraries_view(UiWindow *w) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/libraries_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "libraries_view"));
    g_object_ref(view);

    /* Get widget references */
    GtkWidget *add_btn = GTK_WIDGET(gtk_builder_get_object(builder, "add_library_btn"));
    w->progress_container = GTK_WIDGET(gtk_builder_get_object(builder, "progress_container"));
    w->libs_box = GTK_WIDGET(gtk_builder_get_object(builder, "libs_box"));
    w->libs_empty = GTK_WIDGET(gtk_builder_get_object(builder, "libs_empty"));

    /* Load phase widgets from template */
    load_phase_widgets(builder, &w->scan_phase, "scan");
    load_phase_widgets(builder, &w->metadata_phase, "metadata");
    load_phase_widgets(builder, &w->artwork_phase, "artwork");

    /* Connect signals */
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_library), w);

    g_object_unref(builder);

    libs_load(w);
    libs_rebuild(w);

    return view;
}

static GtkWidget *make_channel_settings_frame(UiWindow *w, int channel) {
    char title[32];
    snprintf(title, sizeof(title), "Channel %d", channel + 1);

    GtkWidget *frame = gtk_frame_new(title);
    gtk_widget_add_css_class(frame, "settings-channel-frame");

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 12);

    /* Row 0: Audio Device */
    GtkWidget *device_label = gtk_label_new("Audio Device");
    gtk_widget_add_css_class(device_label, "settings-label");
    gtk_label_set_xalign(GTK_LABEL(device_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), device_label, 0, 0, 1, 1);

    w->device_drops[channel] = gtk_drop_down_new(NULL, NULL);
    gtk_widget_set_hexpand(w->device_drops[channel], TRUE);
    g_signal_connect(w->device_drops[channel], "notify::selected",
                     G_CALLBACK(on_device_changed), w);
    gtk_grid_attach(GTK_GRID(grid), w->device_drops[channel], 1, 0, 1, 1);

    /* Row 1: Output Format */
    GtkWidget *format_label = gtk_label_new("Output Format");
    gtk_widget_add_css_class(format_label, "settings-label");
    gtk_label_set_xalign(GTK_LABEL(format_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), format_label, 0, 1, 1, 1);

    /* Build format model (shared across all channels) */
    if (!w->format_model) {
        w->format_model = gtk_string_list_new(NULL);
        for (int i = 0; i < OUTPUT_FORMAT_COUNT; i++) {
            gtk_string_list_append(w->format_model, app_settings_format_name((output_format_t)i));
        }
    }

    w->format_drops[channel] = gtk_drop_down_new(G_LIST_MODEL(w->format_model), NULL);
    gtk_widget_set_hexpand(w->format_drops[channel], TRUE);
    /* Set default selection (will be overwritten when settings are restored) */
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->format_drops[channel]), OUTPUT_FORMAT_16BIT_48000);
    g_signal_connect(w->format_drops[channel], "notify::selected",
                     G_CALLBACK(on_format_changed), w);
    gtk_grid_attach(GTK_GRID(grid), w->format_drops[channel], 1, 1, 1, 1);

    /* Row 2: Livewire+ GPIO Address */
    GtkWidget *gpio_label = gtk_label_new("Livewire+ GPIO");
    gtk_widget_add_css_class(gpio_label, "settings-label");
    gtk_label_set_xalign(GTK_LABEL(gpio_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), gpio_label, 0, 2, 1, 1);

    w->gpio_entries[channel] = gtk_entry_new();
    gtk_widget_set_hexpand(w->gpio_entries[channel], TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->gpio_entries[channel]), "e.g., 192.168.1.100:5001");
    gtk_widget_add_css_class(w->gpio_entries[channel], "settings-entry");
    g_signal_connect(w->gpio_entries[channel], "changed",
                     G_CALLBACK(on_gpio_changed), w);
    gtk_grid_attach(GTK_GRID(grid), w->gpio_entries[channel], 1, 2, 1, 1);

    gtk_frame_set_child(GTK_FRAME(frame), grid);
    return frame;
}

static GtkWidget *make_settings_view(UiWindow *w) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/settings_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "settings_view"));
    g_object_ref(view);

    /* Get widget references */
    GtkWidget *channel_frames_box = GTK_WIDGET(gtk_builder_get_object(builder, "channel_frames_box"));
    GtkWidget *spectrum_checkbox = GTK_WIDGET(gtk_builder_get_object(builder, "spectrum_checkbox"));

    /* Create a frame for each channel */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        GtkWidget *frame = make_channel_settings_frame(w, i);
        gtk_box_append(GTK_BOX(channel_frames_box), frame);
    }

    /* Connect spectrum toggle */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(spectrum_checkbox), w->show_spectrum);
    g_signal_connect(spectrum_checkbox, "toggled", G_CALLBACK(on_spectrum_toggled), w);

    g_object_unref(builder);
    return view;
}

static GtkWidget *make_help_view(void) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/help_view.ui");
    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "help_view"));
    g_object_ref(view);  /* prevent destruction when builder is freed */
    g_object_unref(builder);
    return view;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UI Building
 * ═══════════════════════════════════════════════════════════════════════════ */

static void load_css(UiWindow *w) {
    w->css = gtk_css_provider_new();

    GBytes *bytes = g_resources_lookup_data("/org/quadrature/ui/quadrature.css",
                                            G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (bytes) {
        gsize size;
        const char *data = g_bytes_get_data(bytes, &size);
        char *str = g_strndup(data, size);
        gtk_css_provider_load_from_string(w->css, str);
        g_free(str);
        g_bytes_unref(bytes);
    }

    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(w->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void build_ui(UiWindow *w) {
    /* Template already set window title and size, load CSS */
    load_css(w);

    /* Connect template button signals */
    g_signal_connect(w->errors_close_btn, "clicked", G_CALLBACK(on_errors_close), w);

    /* Create and add errors view to its container */
    w->errors_view = errors_view_new(w->db);
    gtk_widget_set_vexpand(w->errors_view, TRUE);
    gtk_box_append(GTK_BOX(w->errors_view_container), w->errors_view);

    /* Create navigate action (stateful - state is current view name) */
    w->navigate_action = g_simple_action_new_stateful(
        "navigate", G_VARIANT_TYPE_STRING, g_variant_new_string("search"));
    g_signal_connect(w->navigate_action, "activate", G_CALLBACK(on_navigate_action), w);
    g_action_map_add_action(G_ACTION_MAP(w), G_ACTION(w->navigate_action));

    /* Load nav bar from template and prepend to main_box */
    GtkBuilder *nav_builder = gtk_builder_new_from_resource("/org/quadrature/ui/nav_bar.ui");
    w->nav_bar = GTK_WIDGET(gtk_builder_get_object(nav_builder, "nav_bar"));
    gtk_widget_set_size_request(w->nav_bar, 56, -1);
    gtk_widget_set_hexpand(w->nav_bar, FALSE);
    gtk_box_prepend(GTK_BOX(w->main_box), w->nav_bar);
    g_object_unref(nav_builder);

    /* Add views to content stack */
    gtk_stack_add_named(GTK_STACK(w->stack), make_search_view(w), "search");

    LibraryCallbacks lib_cbs = {
        .on_navigate = on_library_navigate,
        .on_play = on_library_play,
        .on_back = on_library_back,
        .on_track_info = on_track_info,
        .user_data = w
    };

    w->artists_view = library_view_new(LIBRARY_ITEM_ARTIST, w->db, w->library_cache,
                                        w->artwork_mgr, &lib_cbs);
    gtk_stack_add_named(GTK_STACK(w->stack), w->artists_view, "artists");

    w->albums_view = library_view_new(LIBRARY_ITEM_ALBUM, w->db, w->library_cache,
                                       w->artwork_mgr, &lib_cbs);
    gtk_stack_add_named(GTK_STACK(w->stack), w->albums_view, "albums");

    gtk_stack_add_named(GTK_STACK(w->stack), make_libraries_view(w), "libraries");
    gtk_stack_add_named(GTK_STACK(w->stack), make_settings_view(w), "settings");
    gtk_stack_add_named(GTK_STACK(w->stack), perf_view_new(audio_pipeline_get_perf(w->pipeline)), "perf");
    gtk_stack_add_named(GTK_STACK(w->stack), make_help_view(), "help");

    /* Unified detail view */
    w->detail_view = library_unified_detail_view_new(w->db, w->library_cache,
                                                      w->artwork_mgr, &lib_cbs);
    gtk_stack_add_named(GTK_STACK(w->stack), w->detail_view, "detail");

    /* Create channel strips and add to template container */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        GtkWidget *strip = ui_channel_strip_new(i, w->pipeline);
        w->channels[i] = UI_CHANNEL_STRIP(strip);
        gtk_widget_set_vexpand(strip, TRUE);
        g_signal_connect(strip, "clicked", G_CALLBACK(on_channel_strip_clicked), w);
        g_signal_connect(strip, "mode-changed", G_CALLBACK(on_channel_strip_mode_changed), w);
        g_signal_connect(strip, "album-clicked", G_CALLBACK(on_channel_album_clicked), w);
        g_signal_connect(strip, "artist-clicked", G_CALLBACK(on_channel_artist_clicked), w);
        g_signal_connect(strip, "track-changed", G_CALLBACK(on_channel_track_changed), w);
        g_signal_connect(strip, "track-ended", G_CALLBACK(on_channel_track_ended), w);
        gtk_box_append(GTK_BOX(w->channel_strips_box), strip);
    }

    /* Set fixed width on channels panel - CSS min/max-width alone isn't reliable */
    GtkWidget *channels_panel = gtk_widget_get_parent(w->channel_strips_box);
    if (channels_panel) {
        gtk_widget_set_size_request(channels_panel, 720, -1);
        gtk_widget_set_hexpand(channels_panel, FALSE);
    }

    /* Key controller */
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w), key);

    /* Update timer */
    w->update_timer = g_timeout_add(16, on_update, w);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ui_window_dispose(GObject *obj) {
    UiWindow *w = UI_WINDOW(obj);

    if (w->update_timer) { g_source_remove(w->update_timer); w->update_timer = 0; }
    if (w->progress_pulse_timer) { g_source_remove(w->progress_pulse_timer); w->progress_pulse_timer = 0; }
    if (w->search_debounce_timer) { g_source_remove(w->search_debounce_timer); w->search_debounce_timer = 0; }
    if (w->toast_timer) { g_source_remove(w->toast_timer); w->toast_timer = 0; }

    g_clear_object(&w->css);
    g_free(w->last_search_query);
    w->last_search_query = NULL;

    if (w->device_names) {
        for (int i = 0; i < w->device_count; i++) g_free(w->device_names[i]);
        g_free(w->device_names);
        w->device_names = NULL;
    }
    g_clear_object(&w->device_model);
    g_clear_object(&w->format_model);

    if (w->settings) { app_settings_free(w->settings); w->settings = NULL; }

    libs_free(w);
    g_clear_object(&w->indexer);

    g_free(w->errors_library_path);
    w->errors_library_path = NULL;

    if (w->artwork_mgr) { artwork_manager_free(w->artwork_mgr); w->artwork_mgr = NULL; }
    if (w->library_cache) { library_cache_free(w->library_cache); w->library_cache = NULL; }
    if (w->db) { db_close(w->db); w->db = NULL; }

    G_OBJECT_CLASS(ui_window_parent_class)->dispose(obj);
}

static void ui_window_class_init(UiWindowClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    G_OBJECT_CLASS(klass)->dispose = ui_window_dispose;

    /* Set up composite template */
    gtk_widget_class_set_template_from_resource(widget_class,
        "/org/quadrature/ui/quadrature_window.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, UiWindow, main_box);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, content_stack);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, channel_strips_box);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, toast_label);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, errors_overlay);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, errors_panel);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, errors_view_container);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, errors_close_btn);
}

static void ui_window_init(UiWindow *w) {
    /* Initialize template - this populates all bound children */
    gtk_widget_init_template(GTK_WIDGET(w));

    /* Set alias for stack */
    w->stack = w->content_stack;

    /* Initialize non-template fields */
    w->pipeline = NULL;
    w->settings = NULL;
    w->db = NULL;
    w->indexer = NULL;
    w->library_cache = NULL;
    w->artwork_mgr = NULL;
    w->focused_channel = -1;
    w->show_spectrum = TRUE;
    w->current_view = VIEW_SEARCH;
    w->previous_view = VIEW_SEARCH;
    w->filter_active = 0;
    w->device_model = NULL;
    w->format_model = NULL;
    w->device_names = NULL;
    w->device_count = 0;
    w->settings_initializing = FALSE;
    w->libs = NULL;
    w->lib_count = 0;
    w->update_timer = 0;
    w->progress_pulse_timer = 0;
    w->css = NULL;

    /* Toast */
    w->toast_timer = 0;

    /* Errors */
    w->errors_view = NULL;
    w->errors_library_path = NULL;

    /* Search */
    w->search_debounce_timer = 0;
    w->last_search_query = NULL;
    w->search_results_box = NULL;
    w->search_artists_section = NULL;
    w->search_albums_section = NULL;
    w->search_tracks_section = NULL;
    w->search_empty_label = NULL;

    /* Library views */
    w->artists_view = NULL;
    w->albums_view = NULL;
    w->detail_view = NULL;

    for (int i = 0; i < MAX_CHANNELS; i++) {
        w->channels[i] = NULL;
        w->device_drops[i] = NULL;
        w->format_drops[i] = NULL;
        w->gpio_entries[i] = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean init_devices_idle(gpointer data) {
    populate_devices_async(UI_WINDOW(data));
    return G_SOURCE_REMOVE;
}

static gboolean auto_scan_idle(gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->indexer && w->settings && w->settings->auto_scan_on_startup)
        indexer_controller_start_all(w->indexer);
    return G_SOURCE_REMOVE;
}

GtkWidget *ui_window_new(GtkApplication *app, audio_pipeline_t *pipeline, app_settings_t *settings) {
    UiWindow *w = g_object_new(UI_TYPE_WINDOW, "application", app, NULL);
    w->pipeline = pipeline;
    w->settings = settings;

    if (settings) w->show_spectrum = settings->show_spectrum;

    /* Open database */
    char *dir = g_build_filename(g_get_user_data_dir(), "quadrature", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *dbpath = g_build_filename(dir, "library.db", NULL);
    g_free(dir);

    if (db_open(dbpath, &w->db) != QUADRATURE_OK) w->db = NULL;
    g_free(dbpath);

    /* Create indexer */
    if (w->db) {
        w->indexer = indexer_controller_new(w->db);
        if (w->indexer && settings) {
            indexer_controller_set_thread_count(w->indexer, settings->indexer_thread_count);
            indexer_controller_set_process_artwork(w->indexer, settings->process_artwork);
        }
    }

    /* Create library cache for lazy loading */
    w->library_cache = library_cache_new();

    /* Create artwork manager for async image loading */
    w->artwork_mgr = artwork_manager_new(w->db, 0);

    build_ui(w);

    if (w->indexer) {
        g_signal_connect(w->indexer, "started", G_CALLBACK(on_indexer_started), w);
        g_signal_connect(w->indexer, "progress", G_CALLBACK(on_indexer_progress), w);
        g_signal_connect(w->indexer, "completed", G_CALLBACK(on_indexer_done), w);
    }

    g_idle_add(init_devices_idle, w);

    if (w->indexer && settings && settings->auto_scan_on_startup)
        g_idle_add(auto_scan_idle, w);

    return GTK_WIDGET(w);
}

void ui_window_navigate_to(UiWindow *w, const char *view) {
    g_return_if_fail(UI_IS_WINDOW(w));
    g_return_if_fail(view != NULL);

    w->current_view = view;
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), view);

    /* Update action state to sync nav bar toggle buttons */
    if (w->navigate_action)
        g_simple_action_set_state(w->navigate_action, g_variant_new_string(view));
}

void ui_window_set_spectrum_visible(UiWindow *w, gboolean visible) {
    g_return_if_fail(UI_IS_WINDOW(w));
    w->show_spectrum = visible;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->channels[i])
            ui_channel_strip_set_spectrum_visible(w->channels[i], visible);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Focus Management (New API)
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_window_set_focused_channel(UiWindow *w, int ch) {
    g_return_if_fail(UI_IS_WINDOW(w));

    /* Check if the channel can be focused */
    if (ch >= 0 && ch < MAX_CHANNELS && w->channels[ch]) {
        if (!ui_channel_strip_is_active(w->channels[ch])) {
            /* Cannot focus this channel - show toast and return */
            DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
            ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);

            if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
                ui_window_show_toast(w, "Channel has no Audio Output Configured");
            } else if (mode == CHANNEL_MODE_QUEUED) {
                ui_window_show_toast(w, "Channel is Queued");
            } else if (mode == CHANNEL_MODE_ON_AIR) {
                ui_window_show_toast(w, "Channel is On-Air");
            }
            return;
        }
    }

    /* Toggle off if clicking the same channel */
    if (ch == w->focused_channel) {
        ui_window_clear_focus(w);
        return;
    }

    /* Clear previous focus */
    if (w->focused_channel >= 0 && w->focused_channel < MAX_CHANNELS && w->channels[w->focused_channel])
        ui_channel_strip_set_focused(w->channels[w->focused_channel], FALSE);

    w->focused_channel = ch;

    if (ch >= 0 && ch < MAX_CHANNELS && w->channels[ch])
        ui_channel_strip_set_focused(w->channels[ch], TRUE);
}

int ui_window_get_focused_channel(UiWindow *w) {
    g_return_val_if_fail(UI_IS_WINDOW(w), -1);
    return w->focused_channel;
}

void ui_window_clear_focus(UiWindow *w) {
    g_return_if_fail(UI_IS_WINDOW(w));

    if (w->focused_channel >= 0 && w->focused_channel < MAX_CHANNELS && w->channels[w->focused_channel])
        ui_channel_strip_set_focused(w->channels[w->focused_channel], FALSE);

    w->focused_channel = -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Toast Notifications
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean hide_toast(gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->toast_overlay)
        gtk_widget_set_visible(w->toast_overlay, FALSE);
    w->toast_timer = 0;
    return G_SOURCE_REMOVE;
}

void ui_window_show_toast(UiWindow *w, const char *message) {
    g_return_if_fail(UI_IS_WINDOW(w));
    g_return_if_fail(message != NULL);

    if (!w->toast_overlay || !w->toast_label) return;

    gtk_label_set_text(GTK_LABEL(w->toast_label), message);
    gtk_widget_set_visible(w->toast_overlay, TRUE);

    /* Cancel previous timer */
    if (w->toast_timer)
        g_source_remove(w->toast_timer);

    /* Hide after 2 seconds */
    w->toast_timer = g_timeout_add(2000, hide_toast, w);
}
