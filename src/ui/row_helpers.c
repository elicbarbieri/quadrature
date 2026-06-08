/**
 * Quadrature UI Row Helpers
 *
 * Creates template-based list rows from LibraryCache data.
 * Rows are stateless - click handlers attached separately via ui_row_attach_handlers().
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * SelectionGroup — mutual-exclusion selection across GtkListBox widgets
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _SelectionGroup {
    GPtrArray *lists;
    gboolean deselecting;
};

SelectionGroup *ui_selection_group_new(void) {
    SelectionGroup *g = g_new0(SelectionGroup, 1);
    g->lists = g_ptr_array_new();
    return g;
}

static void on_group_row_selected(GtkListBox *list_box, GtkListBoxRow *row, gpointer data) {
    SelectionGroup *g = data;
    if (!row || g->deselecting) return;
    g->deselecting = TRUE;
    for (guint i = 0; i < g->lists->len; i++) {
        GtkListBox *other = g_ptr_array_index(g->lists, i);
        if (other != list_box)
            gtk_list_box_unselect_all(other);
    }
    g->deselecting = FALSE;
}

void ui_selection_group_add(SelectionGroup *g, GtkListBox *list) {
    g_ptr_array_add(g->lists, list);
    g_signal_connect(list, "row-selected", G_CALLBACK(on_group_row_selected), g);
}

void ui_selection_group_remove(SelectionGroup *g, GtkListBox *list) {
    g_signal_handlers_disconnect_by_func(list, on_group_row_selected, g);
    g_ptr_array_remove(g->lists, list);
}

void ui_selection_group_free(SelectionGroup *g) {
    if (!g) return;
    for (guint i = 0; i < g->lists->len; i++)
        g_signal_handlers_disconnect_by_func(g_ptr_array_index(g->lists, i),
                                              on_group_row_selected, g);
    g_ptr_array_free(g->lists, TRUE);
    g_free(g);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Widget Tree Lookup
 *
 * Generic traversal to find a widget by its GtkBuildable ID. Used by
 * detail_view.c and credits_view.c to locate named children in cards
 * built from GtkBuilder templates.
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *find_widget_by_name(GtkWidget *parent, const char *name) {
    const char *widget_name = gtk_buildable_get_buildable_id(GTK_BUILDABLE(parent));
    if (widget_name && g_strcmp0(widget_name, name) == 0)
        return parent;

    GtkWidget *child = gtk_widget_get_first_child(parent);
    while (child) {
        GtkWidget *found = find_widget_by_name(child, name);
        if (found) return found;
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Badge Helper
 *
 * Creates pill labels showing library display names. Uses overflow box
 * for consistent infrastructure with other badge types.
 * ═══════════════════════════════════════════════════════════════════════════ */

static GtkWidget *make_library_badge_label(library_cache_t *cache, int lib_idx,
                                            int max_chars) {
    const char *name = library_cache_get_library_name(cache, lib_idx);
    if (!name) return NULL;

    char truncated[48];
    glong char_count = g_utf8_strlen(name, -1);
    if (char_count > max_chars) {
        gchar *end = g_utf8_offset_to_pointer(name, max_chars);
        gsize byte_len = (gsize)(end - name);
        memcpy(truncated, name, byte_len);
        truncated[byte_len] = '\0';
        g_strlcat(truncated, "\xe2\x80\xa6", sizeof(truncated));
    } else {
        g_strlcpy(truncated, name, sizeof(truncated));
    }

    GtkWidget *badge = gtk_label_new(truncated);
    gtk_widget_add_css_class(badge, "library-badge");
    return badge;
}

GtkWidget *ui_create_library_badge(library_cache_t *cache, int library_index) {
    if (!cache || library_index < 0) return NULL;
    if (library_cache_get_library_count(cache) <= 1) return NULL;

    GtkWidget *badge = make_library_badge_label(cache, library_index, 8);
    if (badge)
        gtk_label_set_xalign(GTK_LABEL(badge), 0.0f);
    return badge;
}

/* Max libraries for stack-based dedup (matches PERF_MAX_LIBRARIES) */
#define BADGE_MAX_LIBS 8

void ui_populate_library_badges(GtkWidget *badges_box,
                                 library_cache_t *cache,
                                 int64_t entity_global_id,
                                 badge_entity_type_t entity_type,
                                 GtkWidget *constraint_widget) {
    (void)constraint_widget;  /* QuadratureOverflowBox plans from its own allocation */
    QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(badges_box);
    quadrature_overflow_box_clear_all(ofb);

    if (!cache || library_cache_get_library_count(cache) <= 1) {
        gtk_widget_set_visible(badges_box, FALSE);
        return;
    }

    int deduped[BADGE_MAX_LIBS];
    int count = 0;

    switch (entity_type) {
    case BADGE_ENTITY_ARTIST:
        count = library_cache_get_artist_libraries(
            cache, entity_global_id, deduped, BADGE_MAX_LIBS);
        break;
    case BADGE_ENTITY_ALBUM:
        count = library_cache_get_album_libraries(
            cache, entity_global_id, deduped, BADGE_MAX_LIBS);
        break;
    case BADGE_ENTITY_TRACK:
        count = library_cache_get_track_libraries(
            cache, entity_global_id, deduped, BADGE_MAX_LIBS);
        break;
    }

    if (count == 0) {
        gtk_widget_set_visible(badges_box, FALSE);
        return;
    }

    gtk_widget_set_visible(badges_box, TRUE);

    for (int i = 0; i < count; i++) {
        GtkWidget *badge = make_library_badge_label(cache, deduped[i], 12);
        quadrature_overflow_box_append(ofb, badge);
    }
    quadrature_overflow_box_set_item_count(ofb, (guint)count);
    /* No overflow indicator — badges are small; any hidden ones simply drop. */
}

/* ui_format_duration → ui_math.c */


/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Pills — QuadratureOverflowBox + Pre-Allocated Labels
 *
 * Uses QuadratureOverflowBox which does overflow planning inside its own
 * size_allocate — no signals, no deferred visibility, no clipping.
 * Pre-allocate GENRE_PILL_MAX labels + 1 "…" overflow label once at setup.
 * On bind, set label text — the overflow box handles the rest.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GENRE_PILL_MAX 6

GtkWidget *ui_genre_pills_new(int spacing) {
    g_type_ensure(QUADRATURE_TYPE_OVERFLOW_BOX);
    GtkWidget *box = g_object_new(QUADRATURE_TYPE_OVERFLOW_BOX,
                                   "spacing", spacing, NULL);
    gtk_widget_add_css_class(box, "genre-pills");

    /* Pre-allocate pill label slots + overflow indicator (last child) */
    QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(box);
    for (int i = 0; i < GENRE_PILL_MAX; i++) {
        GtkWidget *pill = gtk_label_new("");
        gtk_widget_add_css_class(pill, "genre-pill");
        quadrature_overflow_box_append(ofb, pill);
    }
    GtkWidget *overflow = gtk_label_new("…");
    gtk_widget_add_css_class(overflow, "genre-pill");
    quadrature_overflow_box_append(ofb, overflow);

    return box;
}

void ui_genre_pills_bind(GtkWidget *genres_box, const char *genres) {
    if (!genres_box) return;

    guint genre_count = 0;
    if (genres && genres[0]) {
        gchar **raw = g_strsplit(genres, ";", GENRE_PILL_MAX + 1);
        GtkWidget *child = gtk_widget_get_first_child(genres_box);
        for (guint i = 0; raw[i] && genre_count < GENRE_PILL_MAX; i++) {
            g_strstrip(raw[i]);
            if (!raw[i][0]) continue;
            if (child) {
                gtk_label_set_text(GTK_LABEL(child), raw[i]);
                child = gtk_widget_get_next_sibling(child);
            }
            genre_count++;
        }
        g_strfreev(raw);
    }

    /* Tell the overflow box how many items are populated */
    quadrature_overflow_box_set_item_count(QUADRATURE_OVERFLOW_BOX(genres_box), genre_count);
    gtk_widget_set_visible(genres_box, genre_count > 0);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Row Interaction Handler Attachment
 *
 * Attach handlers to row widgets for activate (double-click) and queue (right-click).
 * Selection is handled by GTK's GtkSelectionModel automatically.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    RowCallbacks cbs;
} RowHandlerData;

static void row_handler_data_free(gpointer data) {
    g_free(data);
}

static int64_t get_row_entity_id(GtkWidget *row) {
    gpointer p;
    if ((p = g_object_get_data(G_OBJECT(row), "track-id")))
        return (int64_t)GPOINTER_TO_SIZE(p);
    if ((p = g_object_get_data(G_OBJECT(row), "album-id")))
        return (int64_t)GPOINTER_TO_SIZE(p);
    if ((p = g_object_get_data(G_OBJECT(row), "artist-id")))
        return (int64_t)GPOINTER_TO_SIZE(p);
    return 0;
}

static void on_row_secondary(GtkGestureClick *gesture, int n_press,
                              double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;

    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    g_assert(row != NULL);  /* Gesture must be attached to a widget */

    RowHandlerData *data = g_object_get_data(G_OBJECT(row), "row-handler-data");
    if (!data || !data->cbs.on_secondary) return;

    int64_t id = get_row_entity_id(row);
    if (id > 0)
        data->cbs.on_secondary(id, data->cbs.user_data);
}

void ui_row_attach_handlers(GtkWidget *row, RowCallbacks *callbacks) {
    g_assert(row != NULL);
    g_assert(callbacks != NULL);

    RowHandlerData *data = g_new0(RowHandlerData, 1);
    data->cbs = *callbacks;
    g_object_set_data_full(G_OBJECT(row), "row-handler-data", data, row_handler_data_free);

    /* Activation (double-click/Enter) is handled by GtkListBox::row-activated signal.
     * Selection (single-click) is handled automatically by GtkListBox.
     * We only need to handle secondary click (right-click) here. */

    if (callbacks->on_secondary) {
        GtkGesture *secondary = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
        g_signal_connect(secondary, "pressed", G_CALLBACK(on_row_secondary), NULL);
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(secondary));
    }
}

typedef struct {
    int64_t id;
    RowCallbacks cbs;
} RowActivateIdleData;

static gboolean row_activate_idle(gpointer data) {
    RowActivateIdleData *d = data;
    if (d->cbs.on_activate)
        d->cbs.on_activate(d->id, d->cbs.user_data);
    g_free(d);
    return G_SOURCE_REMOVE;
}

void ui_list_box_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data) {
    (void)list; (void)user_data;

    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child) return;

    RowHandlerData *data = g_object_get_data(G_OBJECT(child), "row-handler-data");
    if (!data || !data->cbs.on_activate) return;

    int64_t id = get_row_entity_id(child);
    if (id > 0) {
        RowActivateIdleData *d = g_new0(RowActivateIdleData, 1);
        d->id = id;
        d->cbs = data->cbs;
        g_idle_add(row_activate_idle, d);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * List View Loading States
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_list_view_set_loading(GtkWidget *list, gboolean loading) {
    g_assert(list != NULL);
    ui_toggle_css(list, "loading", loading);
}

void ui_list_view_set_empty(GtkWidget *list, const char *message) {
    g_assert(list != NULL);
    g_object_set_data_full(G_OBJECT(list), "empty-message",
                           message ? g_strdup(message) : NULL, g_free);
    gtk_widget_add_css_class(list, "empty");
}

void ui_list_view_set_error(GtkWidget *list, const char *message, GCallback retry_cb) {
    g_assert(list != NULL);
    g_object_set_data_full(G_OBJECT(list), "error-message",
                           message ? g_strdup(message) : NULL, g_free);
    g_object_set_data(G_OBJECT(list), "retry-callback", retry_cb);
    gtk_widget_add_css_class(list, "error");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Popover Shortcut Passthrough
 *
 * GTK4 popovers with autohide capture keyboard focus, blocking window-level
 * accelerators. This installs a capture-phase key controller that intercepts
 * navigation shortcuts, dismisses the popover, and activates the corresponding
 * win.* action so hotkeys work seamlessly from any popover.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    guint keyval;
    GdkModifierType mods;
    const char *action;
} PopoverShortcut;

static const PopoverShortcut popover_shortcuts[] = {
    { GDK_KEY_f, GDK_CONTROL_MASK, "win.search" },
    { GDK_KEY_a, GDK_CONTROL_MASK, "win.filter-artists" },
    { GDK_KEY_b, GDK_CONTROL_MASK, "win.filter-albums" },
    { GDK_KEY_t, GDK_CONTROL_MASK, "win.filter-tracks" },
    { GDK_KEY_r, GDK_CONTROL_MASK, "win.clear-filters" },
    { GDK_KEY_Escape, 0,           "win.close-errors" },
};

static gboolean on_popover_shortcut_key(GtkEventControllerKey *ctl,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer data) {
    (void)ctl; (void)keycode;
    GtkPopover *popover = GTK_POPOVER(data);

    /* Mask off lock keys (NumLock, CapsLock, ScrollLock) */
    GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK);

    for (size_t i = 0; i < G_N_ELEMENTS(popover_shortcuts); i++) {
        if (keyval == popover_shortcuts[i].keyval && mods == popover_shortcuts[i].mods) {
            gtk_popover_popdown(popover);

            GtkWidget *win = GTK_WIDGET(popover);
            while (win && !GTK_IS_WINDOW(win))
                win = gtk_widget_get_parent(win);
            if (win)
                gtk_widget_activate_action_variant(win, popover_shortcuts[i].action + 4,
                                                   NULL);
            return TRUE;
        }
    }
    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Section Header — "── TITLE ──────────────────" rule style
 *
 * Snapshot-based separator line bypasses GTK4 CSS cascade issues inside
 * gtk_list_box_row_set_header(). Uses gtk_snapshot_append_color (GPU-composited).
 * ═══════════════════════════════════════════════════════════════════════════ */

static void sep_snapshot(GtkWidget *widget, GtkSnapshot *snap) {
    float w = (float)gtk_widget_get_width(widget);
    float h = (float)gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0) return;
    float line_h = 3.0f;
    float y = (h - line_h) / 2.0f;
    GdkRGBA color = { 0.5f, 0.5f, 0.5f, 0.5f };
    graphene_rect_t rect = GRAPHENE_RECT_INIT(0, y, w, line_h);
    gtk_snapshot_append_color(snap, &color, &rect);
}

typedef struct { GtkWidget parent; } QuadSepLine;
typedef struct { GtkWidgetClass parent_class; } QuadSepLineClass;
GType quad_sep_line_get_type(void) G_GNUC_CONST;
G_DEFINE_FINAL_TYPE(QuadSepLine, quad_sep_line, GTK_TYPE_WIDGET)
static void quad_sep_line_init(QuadSepLine *self) { (void)self; }
static void quad_sep_line_class_init(QuadSepLineClass *klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = sep_snapshot;
}

GtkWidget *ui_make_section_header(const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "search-section-header");

    GtkWidget *label = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped(
        "<span foreground='#888888' weight='bold' size='22528' letter_spacing='1229'>%s</span>",
        title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);

    GtkWidget *sep = g_object_new(quad_sep_line_get_type(), NULL);
    gtk_widget_set_hexpand(sep, TRUE);
    gtk_widget_set_valign(sep, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(sep, -1, 12);

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), sep);
    return box;
}

void ui_popover_install_shortcuts(GtkPopover *popover) {
    g_assert(GTK_IS_POPOVER(popover));

    GtkEventController *ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(ctl, "key-pressed", G_CALLBACK(on_popover_shortcut_key), popover);
    gtk_widget_add_controller(GTK_WIDGET(popover), ctl);
}
