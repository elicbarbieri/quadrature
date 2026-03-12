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

typedef struct {
    library_cache_t *cache;
    int             *lib_indices;   /* Owned deduplicated array */
    guint            count;
} LibBadgeData;

static void lib_badge_data_free(gpointer data) {
    LibBadgeData *d = data;
    g_free(d->lib_indices);
    g_free(d);
}

static GtkWidget *lib_badge_create_item(guint index, gpointer user_data) {
    LibBadgeData *d = user_data;
    return make_library_badge_label(d->cache, d->lib_indices[index], 12);
}

/* Max libraries for stack-based dedup (matches PERF_MAX_LIBRARIES) */
#define BADGE_MAX_LIBS 8

void ui_populate_library_badges(GtkWidget *badges_box,
                                 library_cache_t *cache,
                                 int library_index,
                                 int64_t entity_global_id,
                                 const int64_t *merged_source_ids,
                                 int merged_source_count) {
    ui_box_clear(GTK_BOX(badges_box));

    if (!cache || library_cache_get_library_count(cache) <= 1) {
        gtk_widget_set_visible(badges_box, FALSE);
        return;
    }

    /* Stack-based dedup — no heap allocation for typical library counts */
    int deduped[BADGE_MAX_LIBS];
    guint count = 0;

    int primary_lib = (library_index >= 0)
        ? library_index
        : LIBRARY_GLOBAL_ID_LIB(entity_global_id);
    if (primary_lib >= 0 && primary_lib < library_cache_get_library_count(cache)
        && count < BADGE_MAX_LIBS) {
        if (library_cache_get_library_name(cache, primary_lib)) {
            deduped[count++] = primary_lib;
        }
    }

    for (int i = 0; i < merged_source_count && count < BADGE_MAX_LIBS; i++) {
        int src_lib = LIBRARY_GLOBAL_ID_LIB(merged_source_ids[i]);
        /* Linear scan dedup — fine for N ≤ 8 */
        gboolean seen = FALSE;
        for (guint j = 0; j < count; j++) {
            if (deduped[j] == src_lib) { seen = TRUE; break; }
        }
        if (seen) continue;
        if (library_cache_get_library_name(cache, src_lib))
            deduped[count++] = src_lib;
    }

    if (count == 0) {
        gtk_widget_set_visible(badges_box, FALSE);
        return;
    }

    gtk_widget_set_visible(badges_box, TRUE);

    LibBadgeData *d = g_new0(LibBadgeData, 1);
    d->cache = cache;
    d->count = count;
    d->lib_indices = g_memdup2(deduped, count * sizeof(int));

    ui_overflow_box_setup(&(UiOverflowBoxParams){
        .box               = badges_box,
        .default_max_width = 300,
        .pinned_children   = 0,
        .create_item       = lib_badge_create_item,
        .create_overflow   = NULL,   /* Just stop — badges are small */
        .item_count        = count,
        .user_data         = d,
        .user_data_destroy = lib_badge_data_free,
    });
}

/* ui_format_duration → ui_math.c */


/* ═══════════════════════════════════════════════════════════════════════════
 * Overflow Box — Generic Width-Aware Pill/Badge Layout
 *
 * Populates a horizontal GtkBox with as many items as fit within a width
 * budget, collapsing the rest into a caller-provided overflow indicator.
 * Automatically re-lays out when the constraint widget resizes (10px
 * hysteresis to avoid jitter).
 *
 * Callers provide two callbacks:
 *   create_item(index, user_data) → a floating GtkWidget for item #index
 *   create_overflow(first_hidden, total, user_data) → overflow indicator widget
 *
 * The first `pinned_children` widgets already in the box are preserved
 * (their width counts toward the budget but they are never removed).
 * ═══════════════════════════════════════════════════════════════════════════ */

static int measure_natural_width(GtkWidget *w) {
    int min_w = 0, nat_w = 0;
    gtk_widget_measure(w, GTK_ORIENTATION_HORIZONTAL, -1, &min_w, &nat_w, NULL, NULL);
    return nat_w;
}

typedef struct {
    GtkWidget  *box;
    GtkWidget  *constraint_widget;
    double      constraint_fraction;
    int         default_max_width;
    int         pinned_children;

    UiOverflowCreateItem     create_item;
    UiOverflowCreateOverflow create_overflow;
    guint       item_count;
    gpointer    user_data;
    GDestroyNotify user_data_destroy;

    gulong      width_signal_id;
    gulong      map_signal_id;
    int         last_width;
} OverflowBoxData;

static void overflow_box_data_free(gpointer data) {
    OverflowBoxData *obd = data;
    if (obd->map_signal_id && obd->box)
        g_signal_handler_disconnect(obd->box, obd->map_signal_id);
    if (obd->width_signal_id && obd->constraint_widget)
        g_signal_handler_disconnect(obd->constraint_widget, obd->width_signal_id);
    if (obd->user_data_destroy && obd->user_data)
        obd->user_data_destroy(obd->user_data);
    g_free(obd);
}

static void overflow_box_relayout(OverflowBoxData *obd) {
    GtkBox *box = GTK_BOX(obd->box);

    /* Remove all children after the pinned ones */
    int skip = obd->pinned_children;
    GtkWidget *child = gtk_widget_get_first_child(obd->box);
    for (int i = 0; i < skip && child; i++)
        child = gtk_widget_get_next_sibling(child);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(box, child);
        child = next;
    }

    if (obd->item_count == 0) return;

    /* Calculate width budget */
    int raw_width = obd->constraint_widget
        ? gtk_widget_get_width(obd->constraint_widget) : 0;
    int max_width = (raw_width > 0)
        ? (int)(raw_width * obd->constraint_fraction)
        : obd->default_max_width;

    /* Measure pinned children's accumulated width */
    int pinned_width = 0;
    child = gtk_widget_get_first_child(obd->box);
    for (int i = 0; i < skip && child; i++) {
        pinned_width += measure_natural_width(child);
        child = gtk_widget_get_next_sibling(child);
    }

    /* ── Measure everything upfront ── */
    GtkWidget **items = g_newa(GtkWidget *, obd->item_count);
    int *item_widths  = g_newa(int, obd->item_count);
    for (guint i = 0; i < obd->item_count; i++) {
        items[i] = obd->create_item(i, obd->user_data);
        item_widths[i] = measure_natural_width(items[i]);
    }

    GtkWidget *overflow = obd->create_overflow
        ? obd->create_overflow(0, obd->item_count, obd->user_data)
        : NULL;
    int overflow_w = overflow ? measure_natural_width(overflow) : 0;

    /* ── Plan with pure integer math ── */
    gboolean needs_overflow = FALSE;
    guint show_count = ui_overflow_box_plan_layout(
        max_width - pinned_width, item_widths, obd->item_count,
        overflow_w, &needs_overflow);

    /* ── Execute: append only what the plan selected ── */
    for (guint i = 0; i < show_count; i++)
        gtk_box_append(box, items[i]);

    /* Discard items that didn't make the cut */
    for (guint i = show_count; i < obd->item_count; i++) {
        g_object_ref_sink(items[i]);
        g_object_unref(items[i]);
    }

    if (needs_overflow && overflow) {
        /* Recreate with correct first_hidden index if it differs */
        if (show_count > 0) {
            g_object_ref_sink(overflow);
            g_object_unref(overflow);
            overflow = obd->create_overflow(show_count, obd->item_count,
                                            obd->user_data);
        }
        if (overflow)
            gtk_box_append(box, overflow);
    } else if (overflow) {
        g_object_ref_sink(overflow);
        g_object_unref(overflow);
    }
}

static void on_overflow_box_map(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    overflow_box_relayout(user_data);
}

static void on_overflow_box_width_changed(GObject *object, GParamSpec *pspec,
                                           gpointer user_data) {
    (void)pspec;
    OverflowBoxData *obd = user_data;

    int raw_width = gtk_widget_get_width(GTK_WIDGET(object));
    int box_width = (int)(raw_width * obd->constraint_fraction);
    if (box_width <= 0) return;

    if (abs(box_width - obd->last_width) < 10) return;
    obd->last_width = box_width;

    overflow_box_relayout(obd);
}

void ui_overflow_box_setup(const UiOverflowBoxParams *p) {
    g_assert(p->box != NULL);
    g_assert(p->create_item != NULL);

    OverflowBoxData *obd = g_new0(OverflowBoxData, 1);
    obd->box                = p->box;
    obd->constraint_widget  = p->constraint_widget;
    obd->constraint_fraction = p->constraint_fraction;
    obd->default_max_width  = p->default_max_width > 0 ? p->default_max_width : 300;
    obd->pinned_children    = p->pinned_children;
    obd->create_item        = p->create_item;
    obd->create_overflow    = p->create_overflow;
    obd->item_count         = p->item_count;
    obd->user_data          = p->user_data;
    obd->user_data_destroy  = p->user_data_destroy;

    g_object_set_data_full(G_OBJECT(p->box), "overflow-box-data",
                           obd, overflow_box_data_free);
    obd->map_signal_id = g_signal_connect(p->box, "map",
                                           G_CALLBACK(on_overflow_box_map), obd);

    if (p->constraint_widget)
        obd->width_signal_id = g_signal_connect(
            p->constraint_widget, "notify::width",
            G_CALLBACK(on_overflow_box_width_changed), obd);

    /* Immediate layout if already mapped */
    overflow_box_relayout(obd);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Pills — Width-Aware via Overflow Box
 *
 * Splits genre string on semicolons. Each genre becomes an ellipsized pill.
 * Overflow collapses into a "…" button with a popover listing all genres.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  **genres;       /* Owned, null-terminated */
    guint   count;
} GenrePillData;

static void genre_pill_data_free(gpointer data) {
    GenrePillData *d = data;
    g_strfreev(d->genres);
    g_free(d);
}

static GtkWidget *genre_create_item(guint index, gpointer user_data) {
    GenrePillData *d = user_data;
    GtkWidget *pill = gtk_label_new(d->genres[index]);
    gtk_label_set_ellipsize(GTK_LABEL(pill), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(pill), 18);
    gtk_widget_add_css_class(pill, "genre-pill");
    return pill;
}

/* Unparent popover when overflow button leaves the widget tree */
static void on_genre_overflow_unroot(GObject *obj, GParamSpec *pspec, gpointer popover) {
    (void)pspec;
    if (gtk_widget_get_root(GTK_WIDGET(obj)) == NULL)
        gtk_widget_unparent(GTK_WIDGET(popover));
}

static void on_genre_overflow_clicked(GtkButton *button, gpointer popover) {
    (void)button;
    gtk_popover_popup(GTK_POPOVER(popover));
}

static GtkWidget *genre_create_overflow(guint first_hidden, guint total,
                                         gpointer user_data) {
    (void)first_hidden;
    GenrePillData *d = user_data;

    /* Overflow button styled like a genre pill */
    GtkWidget *btn = gtk_button_new_with_label("…");
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "genre-pill");
    gtk_widget_add_css_class(btn, "genre-overflow-btn");

    /* Popover with all genres */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(vbox, 4);
    gtk_widget_set_margin_end(vbox, 4);
    gtk_widget_set_margin_top(vbox, 4);
    gtk_widget_set_margin_bottom(vbox, 4);

    for (guint i = 0; i < total; i++) {
        GtkWidget *label = gtk_label_new(d->genres[i]);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
        gtk_widget_add_css_class(label, "genre-pill");
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), label);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), vbox);
    gtk_widget_set_parent(popover, btn);

    g_signal_connect(btn, "notify::root",
                     G_CALLBACK(on_genre_overflow_unroot), popover);
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_genre_overflow_clicked), popover);

    return btn;
}

void ui_populate_genre_pills(GtkWidget *box, const char *genres,
                              GtkWidget *constraint_widget,
                              double constraint_fraction) {
    ui_box_clear(GTK_BOX(box));

    if (!genres || !genres[0]) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    /* Parse genres, skip empty entries */
    gchar **raw = g_strsplit(genres, ";", 0);
    GPtrArray *clean = g_ptr_array_new();
    for (guint i = 0; raw[i]; i++) {
        g_strstrip(raw[i]);
        if (raw[i][0])
            g_ptr_array_add(clean, g_strdup(raw[i]));
    }
    g_strfreev(raw);

    if (clean->len == 0) {
        g_ptr_array_free(clean, TRUE);
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    gtk_widget_set_visible(box, TRUE);

    /* Build null-terminated strv for GenrePillData */
    g_ptr_array_add(clean, NULL);
    char **genre_strv = (char **)g_ptr_array_free(clean, FALSE);

    guint count = 0;
    for (char **p = genre_strv; *p; p++) count++;

    GenrePillData *d = g_new0(GenrePillData, 1);
    d->genres = genre_strv;
    d->count  = count;

    ui_overflow_box_setup(&(UiOverflowBoxParams){
        .box                = box,
        .constraint_widget  = constraint_widget,
        .constraint_fraction = constraint_fraction,
        .default_max_width  = 250,
        .pinned_children    = 0,
        .create_item        = genre_create_item,
        .create_overflow    = genre_create_overflow,
        .item_count         = count,
        .user_data          = d,
        .user_data_destroy  = genre_pill_data_free,
    });
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

void ui_popover_install_shortcuts(GtkPopover *popover) {
    g_assert(GTK_IS_POPOVER(popover));

    GtkEventController *ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(ctl, "key-pressed", G_CALLBACK(on_popover_shortcut_key), popover);
    gtk_widget_add_controller(GTK_WIDGET(popover), ctl);
}
