/**
 * Unified Filter Bar
 *
 * Shared filter/sort UI for Artists, Albums, and Search views.
 * Loads filter_sort_bar.ui + optionally advanced_search_bar.ui.
 * Consolidates duplicated filter logic from views.c and window.c.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/database.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *DECADE_NAMES[] = {
    "2020s", "2010s", "2000s", "1990s", "1980s", "1970s", "1960s", "Pre-1960",
};
#define NUM_DECADES 8

/* MusicBrainz l_artist_recording link_type GIDs (stable UUIDs) */
typedef struct {
    const char *name;
    const char *gid;
} RoleFilter;

static const RoleFilter ROLE_FILTERS[] = {
    { "All Roles",      NULL },
    { "Producer",       "5c0ceac3-feb4-41f0-868d-dc06f6e27fc0" },
    { "Vocalist",       "0fdbe3c6-7700-4a31-ae54-b53f06ae1cfa" },
    { "Instrumentalist","59054b12-01ac-43ee-a618-285fd397e461" },
    { "Engineer",       "0cd6aa63-c297-42ed-8725-c16d31913a98" },
    { "Remixer",        "7950be4d-13a3-48e7-906b-5af562e39544" },
};
#define NUM_ROLE_FILTERS (int)(sizeof(ROLE_FILTERS) / sizeof(ROLE_FILTERS[0]))

/* ═══════════════════════════════════════════════════════════════════════════
 * Label Updates
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_genre_label(FilterBarState *fb) {
    if (!fb->filter_genre) return;
    guint count = fb->selected_genres ? g_hash_table_size(fb->selected_genres) : 0;
    if (count == 0) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_genre), "All");
    } else if (count == 1) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, fb->selected_genres);
        g_hash_table_iter_next(&iter, &key, NULL);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_genre), (const char *)key);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u selected", count);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_genre), buf);
    }
}

static void update_year_label(FilterBarState *fb) {
    if (!fb->filter_year) return;
    int count = __builtin_popcount(fb->selected_years_mask);
    if (count == 0) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_year), "All");
    } else if (count == 1) {
        for (int i = 0; i < NUM_DECADES; i++) {
            if (fb->selected_years_mask & (1 << i)) {
                gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_year), DECADE_NAMES[i]);
                break;
            }
        }
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d selected", count);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_year), buf);
    }
}

static void update_sort_label(FilterBarState *fb) {
    if (!fb->sort_dropdown || fb->sort_option_count == 0) return;
    const char *label = fb->sort_options[fb->current_sort_index].label;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->sort_dropdown), label);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Notify Consumer
 * ═══════════════════════════════════════════════════════════════════════════ */

static void notify_changed(FilterBarState *fb) {
    if (fb->on_changed)
        fb->on_changed(fb->on_changed_data);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Checkbox Popover Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void uncheck_popover(GtkMenuButton *mb) {
    GtkPopover *popover = gtk_menu_button_get_popover(mb);
    if (!popover) return;
    GtkWidget *scroll = gtk_popover_get_child(GTK_POPOVER(popover));
    if (!scroll) return;
    GtkWidget *box = GTK_IS_SCROLLED_WINDOW(scroll)
        ? gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll))
        : scroll;
    if (!box) return;
    for (GtkWidget *child = gtk_widget_get_first_child(box);
         child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_CHECK_BUTTON(child))
            gtk_check_button_set_active(GTK_CHECK_BUTTON(child), FALSE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Toggle Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_genre_toggled(GtkCheckButton *check, gpointer data) {
    FilterBarState *fb = data;
    const char *genre = gtk_check_button_get_label(check);
    if (gtk_check_button_get_active(check))
        g_hash_table_add(fb->selected_genres, g_strdup(genre));
    else
        g_hash_table_remove(fb->selected_genres, genre);
    update_genre_label(fb);
    notify_changed(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Year Toggle Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_year_toggled(GtkCheckButton *check, gpointer data) {
    FilterBarState *fb = data;
    int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "bit-index"));
    if (gtk_check_button_get_active(check))
        fb->selected_years_mask |= (uint16_t)(1 << bit);
    else
        fb->selected_years_mask &= (uint16_t)~(1 << bit);
    update_year_label(fb);
    notify_changed(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Search Filter (debounced)
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean on_search_debounce(gpointer data) {
    FilterBarState *fb = data;
    fb->filter_debounce_timer = 0;
    notify_changed(fb);
    return G_SOURCE_REMOVE;
}

static void on_filter_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    FilterBarState *fb = data;
    if (fb->filter_debounce_timer)
        g_source_remove(fb->filter_debounce_timer);
    fb->filter_debounce_timer = g_timeout_add(200, on_search_debounce, fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Clear All Filters
 * ═══════════════════════════════════════════════════════════════════════════ */

static void clear_multiselect(FilterBarState *fb) {
    if (fb->selected_genres) g_hash_table_remove_all(fb->selected_genres);
    fb->selected_years_mask = 0;
    if (fb->filter_genre) {
        uncheck_popover(GTK_MENU_BUTTON(fb->filter_genre));
        update_genre_label(fb);
    }
    if (fb->filter_year) {
        uncheck_popover(GTK_MENU_BUTTON(fb->filter_year));
        update_year_label(fb);
    }
}

static void on_clear_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    FilterBarState *fb = data;
    filter_bar_clear(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sort Dropdown Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_sort_selected(GtkCheckButton *check, gpointer data) {
    FilterBarState *fb = data;
    if (!gtk_check_button_get_active(check)) return;

    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "sort-index"));
    fb->current_sort_index = idx;
    update_sort_label(fb);

    /* Close popover */
    GtkWidget *pop = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(fb->sort_dropdown)));
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    notify_changed(fb);
}

static void build_sort_popover(FilterBarState *fb) {
    if (!fb->sort_dropdown || fb->sort_option_count == 0) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkCheckButton *group = NULL;

    for (int i = 0; i < fb->sort_option_count; i++) {
        GtkWidget *radio = gtk_check_button_new_with_label(fb->sort_options[i].label);
        g_object_set_data(G_OBJECT(radio), "sort-index", GINT_TO_POINTER(i));

        if (group)
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), group);
        else
            group = GTK_CHECK_BUTTON(radio);

        if (i == 0)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);

        g_signal_connect(radio, "toggled", G_CALLBACK(on_sort_selected), fb);
        gtk_box_append(GTK_BOX(box), radio);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->sort_dropdown), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Popover Builder
 * ═══════════════════════════════════════════════════════════════════════════ */

void filter_bar_rebuild_genre_popover(FilterBarState *fb) {
    if (!fb->filter_genre || !fb->cache) return;

    GPtrArray *albums = library_cache_get_albums_filtered(fb->cache, LIBRARY_SORT_NAME_ASC, NULL, NULL);
    if (!albums) return;

    /* Collect unique genres */
    GHashTable *genre_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *album = g_ptr_array_index(albums, i);
        if (!album->genres || !album->genres[0]) continue;
        char **parts = g_strsplit(album->genres, ";", -1);
        for (int j = 0; parts[j]; j++) {
            char *trimmed = g_strstrip(g_strdup(parts[j]));
            if (*trimmed && !g_hash_table_contains(genre_set, trimmed))
                g_hash_table_add(genre_set, trimmed);
            else
                g_free(trimmed);
        }
        g_strfreev(parts);
    }

    /* Sort alphabetically and store in genre_list */
    GList *sorted = g_hash_table_get_keys(genre_set);
    sorted = g_list_sort(sorted, (GCompareFunc)g_ascii_strcasecmp);

    if (fb->genre_list) g_ptr_array_unref(fb->genre_list);
    fb->genre_list = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = sorted; l; l = l->next)
        g_ptr_array_add(fb->genre_list, g_strdup((const char *)l->data));
    g_list_free(sorted);
    g_hash_table_unref(genre_set);
    g_ptr_array_unref(albums);

    /* Build popover with checkboxes */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (guint i = 0; i < fb->genre_list->len; i++) {
        const char *genre = g_ptr_array_index(fb->genre_list, i);
        GtkWidget *check = gtk_check_button_new_with_label(genre);
        g_signal_connect(check, "toggled", G_CALLBACK(on_genre_toggled), fb);
        gtk_box_append(GTK_BOX(box), check);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->filter_genre), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Year Popover Builder
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_year_popover(FilterBarState *fb) {
    if (!fb->filter_year) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (int i = 0; i < NUM_DECADES; i++) {
        GtkWidget *check = gtk_check_button_new_with_label(DECADE_NAMES[i]);
        g_object_set_data(G_OBJECT(check), "bit-index", GINT_TO_POINTER(i));
        g_signal_connect(check, "toggled", G_CALLBACK(on_year_toggled), fb);
        gtk_box_append(GTK_BOX(box), check);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->filter_year), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Advanced Panel (Credit Search + Role Filter)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_advanced_toggled(GtkToggleButton *btn, gpointer data) {
    FilterBarState *fb = data;
    gboolean active = gtk_toggle_button_get_active(btn);
    if (fb->advanced_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(fb->advanced_revealer), active);
    if (!active && fb->credit_search_entry) {
        gtk_editable_set_text(GTK_EDITABLE(fb->credit_search_entry), "");
        fb->selected_role_index = 0;
        if (fb->filter_role)
            gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_role), "All Roles");
        notify_changed(fb);
    }
}

static gboolean on_credit_debounce(gpointer data) {
    FilterBarState *fb = data;
    fb->credit_debounce_timer = 0;
    notify_changed(fb);
    return G_SOURCE_REMOVE;
}

static void on_credit_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    FilterBarState *fb = data;
    if (fb->credit_debounce_timer)
        g_source_remove(fb->credit_debounce_timer);
    fb->credit_debounce_timer = g_timeout_add(200, on_credit_debounce, fb);
}

static void on_credit_search_activate(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    FilterBarState *fb = data;
    if (fb->credit_debounce_timer) {
        g_source_remove(fb->credit_debounce_timer);
        fb->credit_debounce_timer = 0;
    }
    notify_changed(fb);
}

static void on_role_selected(GtkCheckButton *check, gpointer data) {
    FilterBarState *fb = data;
    if (!gtk_check_button_get_active(check)) return;

    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "role-index"));
    fb->selected_role_index = idx;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_role), ROLE_FILTERS[idx].name);

    GtkWidget *pop = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(fb->filter_role)));
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    notify_changed(fb);
}

static void build_role_popover(FilterBarState *fb) {
    if (!fb->filter_role) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkCheckButton *group = NULL;

    for (int i = 0; i < NUM_ROLE_FILTERS; i++) {
        GtkWidget *radio = gtk_check_button_new_with_label(ROLE_FILTERS[i].name);
        g_object_set_data(G_OBJECT(radio), "role-index", GINT_TO_POINTER(i));

        if (group)
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), group);
        else
            group = GTK_CHECK_BUTTON(radio);

        if (i == 0)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);

        g_signal_connect(radio, "toggled", G_CALLBACK(on_role_selected), fb);
        gtk_box_append(GTK_BOX(box), radio);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->filter_role), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *filter_bar_init(FilterBarState *fb, library_cache_t *cache,
                            const FilterBarSortOption *sort_options, int sort_count,
                            filter_bar_changed_cb on_changed, gpointer user_data) {
    memset(fb, 0, sizeof(*fb));

    fb->cache = cache;
    fb->on_changed = on_changed;
    fb->on_changed_data = user_data;
    fb->sort_options = sort_options;
    fb->sort_option_count = sort_count;

    /* Load template */
    GtkBuilder *builder = gtk_builder_new_from_resource(
        "/org/quadrature/ui/filter_sort_bar.ui");

    fb->bar_widget = GTK_WIDGET(gtk_builder_get_object(builder, "filter_sort_bar"));
    g_object_ref(fb->bar_widget);

    fb->filter_genre = GTK_WIDGET(gtk_builder_get_object(builder, "filter_genre"));
    fb->filter_year = GTK_WIDGET(gtk_builder_get_object(builder, "filter_year"));
    fb->filter_search = GTK_WIDGET(gtk_builder_get_object(builder, "filter_search"));
    fb->filter_search_box = GTK_WIDGET(gtk_builder_get_object(builder, "filter_search_box"));
    fb->filter_clear = GTK_WIDGET(gtk_builder_get_object(builder, "filter_clear"));
    fb->advanced_toggle = GTK_WIDGET(gtk_builder_get_object(builder, "advanced_toggle"));
    fb->sort_dropdown = GTK_WIDGET(gtk_builder_get_object(builder, "sort_dropdown"));
    fb->sort_dropdown_box = GTK_WIDGET(gtk_builder_get_object(builder, "sort_dropdown_box"));

    g_object_unref(builder);

    /* Initialize state */
    fb->selected_genres = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Build popovers */
    filter_bar_rebuild_genre_popover(fb);
    build_year_popover(fb);

    /* Sort dropdown */
    if (sort_options && sort_count > 0) {
        gtk_widget_set_visible(fb->sort_dropdown_box, TRUE);
        build_sort_popover(fb);
        update_sort_label(fb);
    }

    /* Connect signals */
    if (fb->filter_search)
        g_signal_connect(fb->filter_search, "search-changed",
                         G_CALLBACK(on_filter_search_changed), fb);
    if (fb->filter_clear)
        g_signal_connect(fb->filter_clear, "clicked",
                         G_CALLBACK(on_clear_clicked), fb);
    if (fb->advanced_toggle)
        g_signal_connect(fb->advanced_toggle, "toggled",
                         G_CALLBACK(on_advanced_toggled), fb);

    return fb->bar_widget;
}

void filter_bar_attach_advanced(FilterBarState *fb, GtkWidget *parent_box) {
    GtkBuilder *builder = gtk_builder_new_from_resource(
        "/org/quadrature/ui/advanced_search_bar.ui");

    fb->advanced_revealer = GTK_WIDGET(gtk_builder_get_object(builder, "advanced_revealer"));
    fb->credit_search_entry = GTK_WIDGET(gtk_builder_get_object(builder, "credit_search_entry"));
    fb->filter_role = GTK_WIDGET(gtk_builder_get_object(builder, "filter_role"));

    g_object_ref(fb->advanced_revealer);
    g_object_unref(builder);

    /* Insert revealer into parent box right after the filter bar */
    /* Find position of bar_widget in parent and insert after it */
    GtkWidget *sibling = fb->bar_widget;
    gtk_box_insert_child_after(GTK_BOX(parent_box), fb->advanced_revealer, sibling);

    /* Build role popover */
    build_role_popover(fb);

    /* Connect signals */
    g_signal_connect(fb->credit_search_entry, "search-changed",
                     G_CALLBACK(on_credit_search_changed), fb);
    g_signal_connect(fb->credit_search_entry, "activate",
                     G_CALLBACK(on_credit_search_activate), fb);
}

void filter_bar_clear(FilterBarState *fb) {
    clear_multiselect(fb);

    if (fb->filter_search)
        gtk_editable_set_text(GTK_EDITABLE(fb->filter_search), "");
    if (fb->filter_debounce_timer) {
        g_source_remove(fb->filter_debounce_timer);
        fb->filter_debounce_timer = 0;
    }

    /* Reset sort to first option */
    fb->current_sort_index = 0;
    update_sort_label(fb);

    /* Clear advanced panel */
    if (fb->credit_search_entry)
        gtk_editable_set_text(GTK_EDITABLE(fb->credit_search_entry), "");
    if (fb->credit_debounce_timer) {
        g_source_remove(fb->credit_debounce_timer);
        fb->credit_debounce_timer = 0;
    }
    fb->selected_role_index = 0;
    if (fb->filter_role)
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_role), "All Roles");
    if (fb->advanced_toggle)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fb->advanced_toggle), FALSE);
    if (fb->advanced_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(fb->advanced_revealer), FALSE);

    notify_changed(fb);
}

void filter_bar_destroy(FilterBarState *fb) {
    if (fb->filter_debounce_timer) {
        g_source_remove(fb->filter_debounce_timer);
        fb->filter_debounce_timer = 0;
    }
    if (fb->credit_debounce_timer) {
        g_source_remove(fb->credit_debounce_timer);
        fb->credit_debounce_timer = 0;
    }
    if (fb->selected_genres) {
        g_hash_table_unref(fb->selected_genres);
        fb->selected_genres = NULL;
    }
    if (fb->genre_list) {
        g_ptr_array_unref(fb->genre_list);
        fb->genre_list = NULL;
    }
    if (fb->bar_widget) {
        g_object_unref(fb->bar_widget);
        fb->bar_widget = NULL;
    }
    if (fb->advanced_revealer) {
        g_object_unref(fb->advanced_revealer);
        fb->advanced_revealer = NULL;
    }
}

gboolean filter_bar_is_active(const FilterBarState *fb) {
    if (fb->selected_genres && g_hash_table_size(fb->selected_genres) > 0)
        return TRUE;
    if (fb->selected_years_mask != 0)
        return TRUE;
    if (fb->filter_search) {
        const char *text = gtk_editable_get_text(GTK_EDITABLE(fb->filter_search));
        if (text && *text) return TRUE;
    }
    if (fb->advanced_toggle &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb->advanced_toggle))) {
        if (fb->credit_search_entry) {
            const char *ct = gtk_editable_get_text(GTK_EDITABLE(fb->credit_search_entry));
            if (ct && *ct) return TRUE;
        }
        if (fb->selected_role_index > 0) return TRUE;
    }
    return FALSE;
}

library_sort_t filter_bar_get_sort(const FilterBarState *fb) {
    if (fb->sort_options && fb->sort_option_count > 0)
        return fb->sort_options[fb->current_sort_index].sort;
    return LIBRARY_SORT_NAME_ASC;
}

const char *filter_bar_get_search_text(const FilterBarState *fb) {
    if (!fb->filter_search) return NULL;
    if (fb->filter_search_box && !gtk_widget_get_visible(fb->filter_search_box))
        return NULL;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(fb->filter_search));
    return (text && *text) ? text : NULL;
}

const char *filter_bar_get_credit_text(const FilterBarState *fb) {
    if (!fb->advanced_toggle || !fb->credit_search_entry) return NULL;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb->advanced_toggle)))
        return NULL;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(fb->credit_search_entry));
    return (text && *text) ? text : NULL;
}

int filter_bar_get_role_index(const FilterBarState *fb) {
    return fb->selected_role_index;
}

const char *filter_bar_get_role_gid(const FilterBarState *fb) {
    if (fb->selected_role_index <= 0 || fb->selected_role_index >= NUM_ROLE_FILTERS)
        return NULL;
    return ROLE_FILTERS[fb->selected_role_index].gid;
}

db_search_opts_t filter_bar_build_search_opts(const FilterBarState *fb,
                                               const char ***genres_out,
                                               size_t *genre_count_out) {
    *genres_out = NULL;
    *genre_count_out = 0;

    if (fb->selected_genres && g_hash_table_size(fb->selected_genres) > 0) {
        size_t n = g_hash_table_size(fb->selected_genres);
        const char **arr = g_new(const char *, n);
        GHashTableIter iter;
        gpointer key;
        size_t i = 0;
        g_hash_table_iter_init(&iter, fb->selected_genres);
        while (g_hash_table_iter_next(&iter, &key, NULL))
            arr[i++] = (const char *)key;
        *genres_out = arr;
        *genre_count_out = n;
    }

    return (db_search_opts_t){
        .genres = *genres_out,
        .genre_count = *genre_count_out,
        .year_mask = fb->selected_years_mask,
    };
}

void filter_bar_hide_search(FilterBarState *fb) {
    if (fb->filter_search_box)
        gtk_widget_set_visible(fb->filter_search_box, FALSE);
}
