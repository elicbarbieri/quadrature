/**
 * Quadrature Libraries View
 *
 * Library cards, stats display, add/remove/rename library management.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "../library/internal.h"
#include "internal.h"
#include <string.h>

LibEntry *
find_lib_entry(UiWindow *w, const char *path)
{
    if (!path)
        return NULL;
    for (size_t i = 0; i < w->lib_count; i++) {
        if (w->libs[i].path && g_strcmp0(w->libs[i].path, path) == 0)
            return &w->libs[i];
    }
    return NULL;
}

void
libs_free(UiWindow *w)
{
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

void
libs_load(UiWindow *w)
{
    libs_free(w);
    if (!w->settings || w->settings->library_count == 0)
        return;

    size_t count = (size_t)w->settings->library_count;
    w->libs = g_new0(LibEntry, count);
    w->lib_count = count;

    for (size_t i = 0; i < count; i++) {
        w->libs[i].id = (int64_t)i;
        w->libs[i].path = g_strdup(w->settings->libraries[i].path);
        w->libs[i].name = app_settings_get_library_name(w->settings, (int)i);
        const char *dp = app_settings_get_library_data_path(w->settings, (int)i);
        w->libs[i].data_path = (dp && g_strcmp0(dp, w->libs[i].path) != 0) ? g_strdup(dp) : NULL;
        libs_load_entry_stats(&w->libs[i], w);
    }
}

static void
on_rescan(GtkButton *btn, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (e && w->indexer && !indexer_controller_is_running(w->indexer)) {
        const char *paths[] = { e->path };
        const char *data_paths[] = { e->data_path ? e->data_path : e->path };
        indexer_controller_start(w->indexer, paths, data_paths, 1);
    }
}

static void
on_remove(GtkButton *btn, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (e && w->settings) {
        /* Cancel any running indexer for this library before tearing down */
        if (w->indexer)
            indexer_controller_cancel_library(w->indexer, e->path);

        /* Remove cache/artwork slots BEFORE settings removal (which shifts indices).
         * find_lib_idx uses settings, so must resolve while settings are still intact. */
        int lib_idx = find_lib_idx(w, e->path);
        if (lib_idx >= 0) {
            int bitmap = w->settings->libraries[lib_idx].library_index;
            if (w->library_cache)
                library_cache_remove_slot(w->library_cache, bitmap);
            if (w->artwork_mgr)
                artwork_manager_remove_library(w->artwork_mgr, bitmap);
        }

        app_settings_remove_library(w->settings, e->path);
        settings_save_debounced(w);
        libs_load(w);
        libs_rebuild(w);
        refresh_library_views(w);
    }
}

/** Called when the errors popover is closed — clean up DB and destroy popover. */
static void
on_errors_popover_closed(GtkPopover *popover, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (w->errors_lib_db) {
        db_close(w->errors_lib_db);
        w->errors_lib_db = NULL;
    }
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void
on_errors(GtkButton *btn, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e)
        return;

    /* Close any previously opened errors DB */
    if (w->errors_lib_db) {
        db_close(w->errors_lib_db);
        w->errors_lib_db = NULL;
    }

    /* Open the library's DB (use data_path if set, else library path) */
    const char *db_root = e->data_path ? e->data_path : e->path;
    char *dbpath = g_build_filename(db_root, "quadrature.sqlite", NULL);
    if (db_open(dbpath, false, &w->errors_lib_db) != QUADRATURE_OK) {
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
    static const int MARGIN_TOP = 100;
    static const int MARGIN_SIDE = 100;
    static const int MARGIN_BOT = 100;
    int stack_w = gtk_widget_get_width(w->stack);
    int stack_h = gtk_widget_get_height(w->stack);
    int pop_w = MAX(400, stack_w - 2 * MARGIN_SIDE);
    int pop_h = MAX(300, stack_h - MARGIN_TOP - MARGIN_BOT);
    gtk_widget_set_size_request(errors_view, pop_w, pop_h);

    gtk_widget_set_parent(popover, w->stack);
    g_object_unref(popover); /* parent now owns it */

    GdkRectangle anchor = { stack_w / 2, MARGIN_TOP, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);

    g_signal_connect(popover, "closed", G_CALLBACK(on_errors_popover_closed), w);
    gtk_popover_popup(GTK_POPOVER(popover));
}

/* ── Reorder callbacks ──────────────────────────────────────────────── */

static void
update_card_lock_visual(LibEntry *e, int lib_count)
{
    if (!e->lock_btn)
        return;
    if (e->locked) {
        gtk_button_set_icon_name(GTK_BUTTON(e->lock_btn), "changes-prevent-symbolic");
        gtk_widget_remove_css_class(e->lock_btn, "library-unlocked");
        gtk_widget_add_css_class(e->lock_btn, "library-locked");
        gtk_widget_set_tooltip_text(e->lock_btn, "Locked (double-click to unlock)");
        gtk_widget_add_css_class(e->card, "library-card-locked");
    } else {
        gtk_button_set_icon_name(GTK_BUTTON(e->lock_btn), "changes-allow-symbolic");
        gtk_widget_remove_css_class(e->lock_btn, "library-locked");
        gtk_widget_add_css_class(e->lock_btn, "library-unlocked");
        gtk_widget_set_tooltip_text(e->lock_btn, "Click to lock");
        gtk_widget_remove_css_class(e->card, "library-card-locked");
    }
    /* Sensitive = unlocked AND not at boundary */
    gboolean can_up = !e->locked && e->id > 0;
    gboolean can_down = !e->locked && (int)e->id < lib_count - 1;
    gtk_widget_set_sensitive(e->move_up_btn, can_up);
    gtk_widget_set_sensitive(e->move_down_btn, can_down);
    gtk_widget_set_sensitive(e->edit_btn, !e->locked);
    /* Close edit section when locking */
    if (e->locked && e->edit_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(e->edit_revealer), FALSE);
}

static void
on_lock_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
    (void)gesture;
    (void)x;
    (void)y;
    UiWindow *w = UI_WINDOW(data);
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !w->settings)
        return;

    if (!e->locked && n_press >= 1) {
        /* Single-click to lock */
        e->locked = TRUE;
    } else if (e->locked && n_press >= 2) {
        /* Double-click to unlock */
        e->locked = FALSE;
    } else {
        return;
    }
    w->settings->libraries[(int)e->id].locked = e->locked ? 1 : 0;
    settings_save_debounced(w);
    update_card_lock_visual(e, w->settings->library_count);
}

static void
on_move_up(GtkButton *btn, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !w->settings || e->id <= 0)
        return;

    app_settings_swap_libraries(w->settings, (int)e->id, (int)e->id - 1);
    settings_save_debounced(w);
    libs_load(w);
    libs_rebuild(w);
}

static void
on_move_down(GtkButton *btn, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !w->settings || (int)e->id >= w->settings->library_count - 1)
        return;

    app_settings_swap_libraries(w->settings, (int)e->id, (int)e->id + 1);
    settings_save_debounced(w);
    libs_load(w);
    libs_rebuild(w);
}

/* ── Edit callbacks ─────────────────────────────────────────────────── */

static void
on_edit_toggle(GtkButton *btn, gpointer data)
{
    (void)data;
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !e->edit_revealer)
        return;
    gboolean revealed = gtk_revealer_get_reveal_child(GTK_REVEALER(e->edit_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(e->edit_revealer), !revealed);
}

static void
on_edit_done(GtkButton *btn, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !w->settings)
        return;

    /* Save name from entry */
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(e->edit_name_entry));
    g_free(w->settings->libraries[(int)e->id].name);
    w->settings->libraries[(int)e->id].name = (new_name && new_name[0]) ? g_strdup(new_name) : NULL;
    g_free(e->name);
    e->name = app_settings_get_library_name(w->settings, (int)e->id);
    gtk_label_set_text(GTK_LABEL(e->card_name_label), e->name);

    if (w->library_cache)
        library_cache_set_library_name(
            w->library_cache, w->settings->libraries[(int)e->id].library_index, e->name);

    settings_save_debounced(w);
    gtk_revealer_set_reveal_child(GTK_REVEALER(e->edit_revealer), FALSE);
    refresh_library_views(w);
}

/* Helper: read a 3-state toggle group and return -1/0/1 */
static int
read_toggle_value(GtkWidget *toggles[3])
{
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggles[1])))
        return 1; /* On */
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggles[2])))
        return 0; /* Off */
    return -1;    /* Default */
}

/* Helper: set a 3-state toggle group from -1/0/1 */
static void
set_toggle_value(GtkWidget *toggles[3], int value)
{
    if (value == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggles[1]), TRUE);
    else if (value == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggles[2]), TRUE);
    else
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggles[0]), TRUE);
}

static void
on_integration_toggled(GtkToggleButton *btn, gpointer data)
{
    if (!gtk_toggle_button_get_active(btn))
        return; /* only handle activation */
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = g_object_get_data(G_OBJECT(btn), "entry");
    if (!e || !w->settings)
        return;

    library_config_t *lib = &w->settings->libraries[(int)e->id];
    lib->mb_resolve = read_toggle_value(e->mb_toggles);
    lib->acoustid = read_toggle_value(e->acoustid_toggles);
    lib->fanart = read_toggle_value(e->fanart_toggles);
    lib->wikipedia = read_toggle_value(e->wikipedia_toggles);
    settings_save_debounced(w);
}

/* Wire a 3-state toggle group (Default / On / Off) with mutual exclusion */
static void
setup_toggle_group(GtkBuilder *b, LibEntry *e, UiWindow *w, const char *prefix, GtkWidget *out[3])
{
    char id[64];
    snprintf(id, sizeof(id), "%s_default_btn", prefix);
    out[0] = GTK_WIDGET(gtk_builder_get_object(b, id));
    snprintf(id, sizeof(id), "%s_on_btn", prefix);
    out[1] = GTK_WIDGET(gtk_builder_get_object(b, id));
    snprintf(id, sizeof(id), "%s_off_btn", prefix);
    out[2] = GTK_WIDGET(gtk_builder_get_object(b, id));

    /* Mutual exclusion via toggle group */
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(out[1]), GTK_TOGGLE_BUTTON(out[0]));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(out[2]), GTK_TOGGLE_BUTTON(out[0]));

    for (int i = 0; i < 3; i++) {
        g_object_set_data(G_OBJECT(out[i]), "entry", e);
        g_signal_connect(out[i], "toggled", G_CALLBACK(on_integration_toggled), w);
    }
}

static GtkWidget *
make_lib_card(UiWindow *w, LibEntry *e)
{
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_card.ui");

    GtkWidget *card = GTK_WIDGET(gtk_builder_get_object(builder, "library_card"));
    g_object_ref(card);
    e->card = card;

    /* Reorder / lock controls */
    e->move_up_btn = GTK_WIDGET(gtk_builder_get_object(builder, "move_up_btn"));
    e->move_down_btn = GTK_WIDGET(gtk_builder_get_object(builder, "move_down_btn"));
    e->lock_btn = GTK_WIDGET(gtk_builder_get_object(builder, "lock_btn"));

    /* Edit controls */
    e->edit_btn = GTK_WIDGET(gtk_builder_get_object(builder, "card_edit_btn"));
    e->edit_revealer = GTK_WIDGET(gtk_builder_get_object(builder, "edit_revealer"));
    e->edit_name_entry = GTK_WIDGET(gtk_builder_get_object(builder, "edit_name_entry"));
    e->card_name_label = GTK_WIDGET(gtk_builder_get_object(builder, "card_name"));

    /* Progress revealer */
    e->progress_revealer = GTK_WIDGET(gtk_builder_get_object(builder, "progress_revealer"));

    /* Stats labels */
    e->stat_tracks = GTK_WIDGET(gtk_builder_get_object(builder, "stat_tracks"));
    e->stat_albums = GTK_WIDGET(gtk_builder_get_object(builder, "stat_albums"));
    e->stat_artists = GTK_WIDGET(gtk_builder_get_object(builder, "stat_artists"));
    e->stat_last_scanned = GTK_WIDGET(gtk_builder_get_object(builder, "stat_last_scanned"));
    e->stat_errors_btn = GTK_WIDGET(gtk_builder_get_object(builder, "stat_errors_btn"));

    /* Phase rows */
    for (int i = 0; i < 7; i++) {
        char id[32];
        snprintf(id, sizeof(id), "phase_%d_row", i);
        e->phases[i].container = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_title", i);
        e->phases[i].title = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_status", i);
        e->phases[i].label = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_bar", i);
        e->phases[i].bar = GTK_WIDGET(gtk_builder_get_object(builder, id));
        snprintf(id, sizeof(id), "phase_%d_rate", i);
        e->phases[i].rate_label = GTK_WIDGET(gtk_builder_get_object(builder, id));
    }

    /* Integration toggle groups */
    setup_toggle_group(builder, e, w, "mb", e->mb_toggles);
    setup_toggle_group(builder, e, w, "acoustid", e->acoustid_toggles);
    setup_toggle_group(builder, e, w, "fanart", e->fanart_toggles);
    setup_toggle_group(builder, e, w, "wikipedia", e->wikipedia_toggles);

    GtkWidget *path_label = GTK_WIDGET(gtk_builder_get_object(builder, "card_path"));
    GtkWidget *data_path = GTK_WIDGET(gtk_builder_get_object(builder, "card_data_path"));
    GtkWidget *rescan = GTK_WIDGET(gtk_builder_get_object(builder, "card_rescan"));
    GtkWidget *remove = GTK_WIDGET(gtk_builder_get_object(builder, "card_remove"));
    GtkWidget *edit_done = GTK_WIDGET(gtk_builder_get_object(builder, "card_edit_done"));

    g_object_unref(builder);

    /* ── Set dynamic content ── */
    gtk_label_set_text(GTK_LABEL(e->card_name_label), e->name);
    gtk_label_set_text(GTK_LABEL(path_label), e->path);

    /* Pre-fill edit name entry */
    const library_config_t *lib = &w->settings->libraries[(int)e->id];
    if (lib->name && lib->name[0])
        gtk_editable_set_text(GTK_EDITABLE(e->edit_name_entry), lib->name);

    /* Show data path if different from music path */
    if (e->data_path) {
        char *dp_display = g_strdup_printf("Data: %s", e->data_path);
        gtk_label_set_text(GTK_LABEL(data_path), dp_display);
        gtk_widget_set_visible(data_path, TRUE);
        g_free(dp_display);
    }

    /* Set integration toggle state from settings */
    set_toggle_value(e->mb_toggles, lib->mb_resolve);
    set_toggle_value(e->acoustid_toggles, lib->acoustid);
    set_toggle_value(e->fanart_toggles, lib->fanart);
    set_toggle_value(e->wikipedia_toggles, lib->wikipedia);

    /* ── Wire signals ── */

    /* Reorder buttons */
    g_object_set_data(G_OBJECT(e->move_up_btn), "entry", e);
    g_signal_connect(e->move_up_btn, "clicked", G_CALLBACK(on_move_up), w);
    g_object_set_data(G_OBJECT(e->move_down_btn), "entry", e);
    g_signal_connect(e->move_down_btn, "clicked", G_CALLBACK(on_move_down), w);

    /* Lock button — uses GtkGestureClick for double-click detection.
     * CAPTURE phase so our gesture sees events before GtkButton's internal
     * gesture can claim the sequence (which would deny us n_press=2). */
    g_object_set_data(G_OBJECT(e->lock_btn), "entry", e);
    GtkGesture *lock_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lock_gesture), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(lock_gesture),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(lock_gesture, "pressed", G_CALLBACK(on_lock_pressed), w);
    gtk_widget_add_controller(e->lock_btn, GTK_EVENT_CONTROLLER(lock_gesture));

    /* Edit / Done */
    g_object_set_data(G_OBJECT(e->edit_btn), "entry", e);
    g_signal_connect(e->edit_btn, "clicked", G_CALLBACK(on_edit_toggle), w);
    g_object_set_data(G_OBJECT(edit_done), "entry", e);
    g_signal_connect(edit_done, "clicked", G_CALLBACK(on_edit_done), w);

    /* Rescan */
    g_object_set_data(G_OBJECT(rescan), "entry", e);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan), w);
    g_object_set_data(G_OBJECT(card), "rescan-btn", rescan);

    /* Remove */
    g_object_set_data(G_OBJECT(remove), "entry", e);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove), w);

    /* Errors */
    g_object_set_data(G_OBJECT(e->stat_errors_btn), "entry", e);
    g_signal_connect(e->stat_errors_btn, "clicked", G_CALLBACK(on_errors), w);

    update_card_stats_labels(e);

    /* ── Apply initial state ── */

    /* Lock state from settings — also handles boundary sensitivity (first/last) */
    e->locked = (lib->locked != 0);
    update_card_lock_visual(e, w->settings->library_count);

    /* Availability */
    e->available = library_cache_get_available(w->library_cache,
                                               w->settings->libraries[(int)e->id].library_index);
    if (!e->available) {
        gtk_widget_add_css_class(card, "library-disconnected");
        gtk_widget_set_sensitive(rescan, FALSE);
    }

    return card;
}

void
update_lib_card_availability(UiWindow *w, int lib_idx, gboolean available)
{
    if (!w || (size_t)lib_idx >= w->lib_count)
        return;
    LibEntry *e = &w->libs[lib_idx];
    e->available = available;

    if (!e->card)
        return;

    if (available) {
        gtk_widget_remove_css_class(e->card, "library-disconnected");
    } else {
        gtk_widget_add_css_class(e->card, "library-disconnected");
    }

    GtkWidget *rescan = g_object_get_data(G_OBJECT(e->card), "rescan-btn");
    if (rescan)
        gtk_widget_set_sensitive(rescan, available);
}

void
libs_rebuild(UiWindow *w)
{
    if (!w->libs_box)
        return;

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

/* ── Add-library popover ─────────────────────────────────────────────── */

typedef enum {
    LIB_TYPE_LOCAL = 0, /* DB alongside music */
    LIB_TYPE_USB,       /* DB in ~/.local/share/quadrature/libraries/{name} */
    LIB_TYPE_NETWORK,   /* DB at user-chosen path */
} LibType;

typedef struct {
    UiWindow *w;
    GtkWidget *popover;
    GtkWidget *music_label;
    GtkWidget *data_label;
    GtkWidget *data_browse_btn;
    GtkWidget *type_description;
    GtkWidget *index_hint_label;
    GtkWidget *confirm_btn;
    GtkWidget *mb_switch;
    GtkWidget *acoustid_switch;
    GtkWidget *fanart_switch;
    GtkWidget *wikipedia_switch;
    char *music_path;
    char *data_path;
    LibType lib_type;
} AddLibState;

static void
add_lib_state_free(AddLibState *s)
{
    g_free(s->music_path);
    g_free(s->data_path);
    g_free(s);
}

static char *
generate_portable_data_path(const char *music_path)
{
    char *basename = g_path_get_basename(music_path);
    char *data_path
        = g_build_filename(g_get_user_data_dir(), "quadrature", "libraries", basename, NULL);
    g_free(basename);
    return data_path;
}

static void
update_confirm_sensitivity(AddLibState *s)
{
    gboolean ok = (s->music_path != NULL);
    if (s->lib_type == LIB_TYPE_NETWORK)
        ok = ok && (s->data_path != NULL);
    gtk_widget_set_sensitive(s->confirm_btn, ok);
}

static void
update_auto_data_path(AddLibState *s)
{
    if (s->lib_type != LIB_TYPE_USB || !s->music_path)
        return;
    g_free(s->data_path);
    s->data_path = generate_portable_data_path(s->music_path);
    gtk_label_set_text(GTK_LABEL(s->data_label), s->data_path);
    gtk_widget_set_opacity(s->data_label, 1.0);
}

static const char *TYPE_DESCRIPTIONS[] = {
    [LIB_TYPE_LOCAL] = "Index stored alongside your music files.",
    [LIB_TYPE_USB] = "Music on a removable drive. Index stored locally on this computer.",
    [LIB_TYPE_NETWORK] = "Music on a network mount. Choose where to store the index.",
};

static void
on_type_toggled(GtkToggleButton *btn, gpointer data)
{
    AddLibState *s = data;
    if (!gtk_toggle_button_get_active(btn))
        return;

    const char *label = gtk_button_get_label(GTK_BUTTON(btn));
    if (g_strcmp0(label, "Local Folder") == 0)
        s->lib_type = LIB_TYPE_LOCAL;
    else if (g_strcmp0(label, "USB Drive") == 0)
        s->lib_type = LIB_TYPE_USB;
    else
        s->lib_type = LIB_TYPE_NETWORK;

    gtk_label_set_text(GTK_LABEL(s->type_description), TYPE_DESCRIPTIONS[s->lib_type]);

    /* Update index section content — use opacity to avoid popover resize on Wayland */
    gboolean show_browse = (s->lib_type == LIB_TYPE_NETWORK);
    gtk_widget_set_opacity(s->data_browse_btn, show_browse ? 1.0 : 0.0);
    gtk_widget_set_sensitive(s->data_browse_btn, show_browse);

    if (s->lib_type == LIB_TYPE_USB) {
        update_auto_data_path(s);
        if (!s->music_path) {
            gtk_label_set_text(GTK_LABEL(s->data_label), "Select a music folder first");
            gtk_widget_set_opacity(s->data_label, 0.5);
        }
        gtk_label_set_text(GTK_LABEL(s->index_hint_label),
                           "Stored locally so data persists when the drive is disconnected.");
    } else if (s->lib_type == LIB_TYPE_NETWORK) {
        g_free(s->data_path);
        s->data_path = NULL;
        gtk_label_set_text(GTK_LABEL(s->data_label), "Select a location for the index");
        gtk_widget_set_opacity(s->data_label, 0.5);
        gtk_label_set_text(GTK_LABEL(s->index_hint_label),
                           "Database and artwork will be stored at this location.");
    } else {
        g_free(s->data_path);
        s->data_path = NULL;
        gtk_label_set_text(GTK_LABEL(s->data_label), "Alongside your music files");
        gtk_widget_set_opacity(s->data_label, 0.5);
        gtk_label_set_text(GTK_LABEL(s->index_hint_label),
                           "Index stored alongside your music files.");
    }

    update_confirm_sensitivity(s);
}

static void
on_music_folder_selected(GObject *src, GAsyncResult *res, gpointer data)
{
    AddLibState *s = data;
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (err) {
        g_error_free(err);
        return;
    }
    if (!folder)
        return;

    g_free(s->music_path);
    s->music_path = g_file_get_path(folder);
    g_object_unref(folder);

    if (s->music_path) {
        gtk_label_set_text(GTK_LABEL(s->music_label), s->music_path);
        gtk_widget_set_opacity(s->music_label, 1.0);
        update_auto_data_path(s);
        update_confirm_sensitivity(s);
    }
}

static void
on_data_folder_selected(GObject *src, GAsyncResult *res, gpointer data)
{
    AddLibState *s = data;
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (err) {
        g_error_free(err);
        return;
    }
    if (!folder)
        return;

    g_free(s->data_path);
    s->data_path = g_file_get_path(folder);
    g_object_unref(folder);

    if (s->data_path) {
        gtk_label_set_text(GTK_LABEL(s->data_label), s->data_path);
        gtk_widget_set_opacity(s->data_label, 1.0);
        update_confirm_sensitivity(s);
    }
}

static void
on_music_browse(GtkButton *btn, gpointer data)
{
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

static void
on_data_browse(GtkButton *btn, gpointer data)
{
    (void)btn;
    AddLibState *s = data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select Index Location");
    GFile *initial = s->music_path ? g_file_new_for_path(s->music_path) : g_file_new_for_path("/");
    gtk_file_dialog_set_initial_folder(dlg, initial);
    g_object_unref(initial);
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(s->w), NULL, on_data_folder_selected, s);
    g_object_unref(dlg);
}

static void
on_add_confirm(GtkButton *btn, gpointer data)
{
    (void)btn;
    AddLibState *s = data;
    if (!s->music_path || !s->w->settings)
        return;

    /* Determine effective data_path based on library type */
    char *effective_data = NULL;
    if (s->lib_type == LIB_TYPE_USB)
        effective_data = generate_portable_data_path(s->music_path);
    else if (s->lib_type == LIB_TYPE_NETWORK && s->data_path)
        effective_data = g_strdup(s->data_path);
    /* LIB_TYPE_LOCAL: effective_data stays NULL (same as music_path) */

    /* Create data directory if separate from music path */
    if (effective_data) {
        if (g_mkdir_with_parents(effective_data, 0755) != 0) {
            g_warning("on_add_confirm: failed to create data directory: %s", effective_data);
            g_free(effective_data);
            return;
        }
    }

    app_settings_add_library(s->w->settings, s->music_path);
    int idx = s->w->settings->library_count - 1;

    if (effective_data) {
        g_free(s->w->settings->libraries[idx].data_path);
        s->w->settings->libraries[idx].data_path = effective_data ? g_strdup(effective_data) : NULL;
    }

    /* Store per-library integration flags */
    s->w->settings->libraries[idx].mb_resolve
        = gtk_switch_get_active(GTK_SWITCH(s->mb_switch)) ? 1 : 0;
    s->w->settings->libraries[idx].acoustid
        = gtk_switch_get_active(GTK_SWITCH(s->acoustid_switch)) ? 1 : 0;
    s->w->settings->libraries[idx].fanart
        = gtk_switch_get_active(GTK_SWITCH(s->fanart_switch)) ? 1 : 0;
    s->w->settings->libraries[idx].wikipedia
        = gtk_switch_get_active(GTK_SWITCH(s->wikipedia_switch)) ? 1 : 0;

    settings_save_debounced(s->w);

    /* Add cache and artwork slots for the new library */
    const char *dp = effective_data ? effective_data : s->music_path;
    if (s->w->library_cache) {
        char *dbpath = g_build_filename(dp, "quadrature.sqlite", NULL);
        library_cache_source_t src = {
            .db_path = dbpath,
            .music_base = s->music_path,
            .display_name = NULL,
            .bitmap_index
            = s->w->settings->libraries[s->w->settings->library_count - 1].library_index,
        };
        library_cache_add_slot(s->w->library_cache, &src);
        g_free(dbpath);
    }
    if (s->w->artwork_mgr) {
        int new_bitmap = s->w->settings->libraries[s->w->settings->library_count - 1].library_index;
        artwork_manager_add_library(s->w->artwork_mgr, new_bitmap, dp, s->music_path);
    }

    libs_load(s->w);
    libs_rebuild(s->w);

    if (s->w->indexer) {
        const char *paths[] = { s->music_path };
        const char *dpaths[] = { dp };
        indexer_controller_start(s->w->indexer, paths, dpaths, 1);
    }

    g_free(effective_data);
    gtk_popover_popdown(GTK_POPOVER(s->popover));
}

static void
on_add_cancel(GtkButton *btn, gpointer data)
{
    (void)btn;
    AddLibState *s = data;
    gtk_popover_popdown(GTK_POPOVER(s->popover));
}

static void
on_add_popover_closed(GtkPopover *popover, gpointer data)
{
    AddLibState *s = data;
    gtk_widget_unparent(GTK_WIDGET(popover));
    add_lib_state_free(s);
}

static void
on_add_library(GtkButton *btn, gpointer data)
{
    (void)btn;
    UiWindow *w = UI_WINDOW(data);

    GtkBuilder *b = gtk_builder_new_from_resource("/org/quadrature/ui/add_library_popover.ui");
    GtkWidget *popover = GTK_WIDGET(gtk_builder_get_object(b, "add_library_popover"));
    GtkWidget *music_label = GTK_WIDGET(gtk_builder_get_object(b, "music_path_label"));
    GtkWidget *data_label = GTK_WIDGET(gtk_builder_get_object(b, "data_path_label"));
    GtkWidget *data_browse = GTK_WIDGET(gtk_builder_get_object(b, "data_browse_btn"));
    GtkWidget *type_desc = GTK_WIDGET(gtk_builder_get_object(b, "type_description"));
    GtkWidget *index_hint = GTK_WIDGET(gtk_builder_get_object(b, "index_hint_label"));
    GtkWidget *music_browse = GTK_WIDGET(gtk_builder_get_object(b, "music_browse_btn"));
    GtkWidget *confirm_btn = GTK_WIDGET(gtk_builder_get_object(b, "add_confirm_btn"));
    GtkWidget *cancel_btn = GTK_WIDGET(gtk_builder_get_object(b, "add_cancel_btn"));
    GtkWidget *type_local = GTK_WIDGET(gtk_builder_get_object(b, "type_local_btn"));
    GtkWidget *type_usb = GTK_WIDGET(gtk_builder_get_object(b, "type_usb_btn"));
    GtkWidget *type_network = GTK_WIDGET(gtk_builder_get_object(b, "type_network_btn"));
    GtkWidget *mb_switch = GTK_WIDGET(gtk_builder_get_object(b, "mb_switch"));
    GtkWidget *acoustid_sw = GTK_WIDGET(gtk_builder_get_object(b, "acoustid_switch"));
    GtkWidget *fanart_sw = GTK_WIDGET(gtk_builder_get_object(b, "fanart_switch"));
    GtkWidget *wikipedia_sw = GTK_WIDGET(gtk_builder_get_object(b, "wikipedia_switch"));
    g_object_ref(popover);
    g_object_unref(b);

    /* Group toggle buttons for mutual exclusion */
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(type_usb), GTK_TOGGLE_BUTTON(type_local));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(type_network), GTK_TOGGLE_BUTTON(type_local));

    /* Initialize integration switches from global defaults */
    if (w->settings) {
        gtk_switch_set_active(GTK_SWITCH(mb_switch), w->settings->musicbrainz_resolve);
        gtk_switch_set_active(GTK_SWITCH(acoustid_sw), w->settings->acoustid_fingerprint);
        gtk_switch_set_active(GTK_SWITCH(fanart_sw), w->settings->fanart_resolve);
        gtk_switch_set_active(GTK_SWITCH(wikipedia_sw), w->settings->wikipedia_bios);
    }

    AddLibState *s = g_new0(AddLibState, 1);
    s->w = w;
    s->popover = popover;
    s->music_label = music_label;
    s->data_label = data_label;
    s->data_browse_btn = data_browse;
    s->type_description = type_desc;
    s->index_hint_label = index_hint;
    s->confirm_btn = confirm_btn;
    s->mb_switch = mb_switch;
    s->acoustid_switch = acoustid_sw;
    s->fanart_switch = fanart_sw;
    s->wikipedia_switch = wikipedia_sw;
    s->lib_type = LIB_TYPE_LOCAL;

    g_signal_connect(type_local, "toggled", G_CALLBACK(on_type_toggled), s);
    g_signal_connect(type_usb, "toggled", G_CALLBACK(on_type_toggled), s);
    g_signal_connect(type_network, "toggled", G_CALLBACK(on_type_toggled), s);
    g_signal_connect(music_browse, "clicked", G_CALLBACK(on_music_browse), s);
    g_signal_connect(data_browse, "clicked", G_CALLBACK(on_data_browse), s);
    g_signal_connect(confirm_btn, "clicked", G_CALLBACK(on_add_confirm), s);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_add_cancel), s);
    g_signal_connect(popover, "closed", G_CALLBACK(on_add_popover_closed), s);

    /* Size content to match errors popover pattern */
    static const int MARGIN_SIDE = 100;
    int stack_w = gtk_widget_get_width(w->stack);
    int pop_w = MAX(400, stack_w - 2 * MARGIN_SIDE);
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));
    if (content)
        gtk_widget_set_size_request(content, pop_w, -1);

    gtk_widget_set_parent(popover, w->stack);
    g_object_unref(popover); /* parent now owns it */

    GdkRectangle anchor = { stack_w / 2, 100, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);

    gtk_popover_popup(GTK_POPOVER(popover));
}

GtkWidget *
make_libraries_view(UiWindow *w)
{
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/libraries_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "libraries_view"));
    g_object_ref(view);

    /* Get widget references */
    GtkWidget *add_btn = GTK_WIDGET(gtk_builder_get_object(builder, "add_library_btn"));
    w->libs_box = GTK_WIDGET(gtk_builder_get_object(builder, "libs_box"));
    w->libs_empty = GTK_WIDGET(gtk_builder_get_object(builder, "libs_empty"));

    /* Connect signals */
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_library), w);

    g_object_unref(builder);

    libs_load(w);
    libs_rebuild(w);

    return view;
}
