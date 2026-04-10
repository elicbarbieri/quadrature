/**
 * Unified Filter Bar
 *
 * Shared filter/sort UI for Artists, Albums, and Search views.
 * Two-row layout:
 *   Row 1: [Genre][Year] ...padding... [Clear][Sort]
 *          (metadata mode swaps Genre/Year for Role)
 *   Row 2: [Search][Search Mode]  (hidden in search view)
 *
 * All multi-select dropdowns (Genre, Year, Role) use custom GtkPopover
 * with GtkCheckButton — stays open until click-away.
 * Single-select dropdowns (Sort, Search Mode) use GtkPopoverMenu
 * with GMenu — closes on selection.
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
    { "Producer",       "5c0ceac3-feb4-41f0-868d-dc06f6e27fc0" },
    { "Vocalist",       "0fdbe3c6-7700-4a31-ae54-b53f06ae1cfa" },
    { "Instrumentalist","59054b12-01ac-43ee-a618-285fd397e461" },
    { "Engineer",       "0cd6aa63-c297-42ed-8725-c16d31913a98" },
    { "Remixer",        "7950be4d-13a3-48e7-906b-5af562e39544" },
};
#define NUM_ROLE_FILTERS (int)(sizeof(ROLE_FILTERS) / sizeof(ROLE_FILTERS[0]))

static const char *SEARCH_MODE_LABELS[] = { "Default", "Metadata" };
#define NUM_SEARCH_MODES 2

/* ═══════════════════════════════════════════════════════════════════════════
 * Label Updates
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Toggle .filter-field--active on the parent GtkBox of a menu button */
static void set_field_active(GtkWidget *menu_button, gboolean active) {
    GtkWidget *parent = gtk_widget_get_parent(menu_button);
    if (!parent) return;
    if (active)
        gtk_widget_add_css_class(parent, "filter-field--active");
    else
        gtk_widget_remove_css_class(parent, "filter-field--active");
}

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
    set_field_active(fb->filter_genre, count > 0);
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
    set_field_active(fb->filter_year, count > 0);
}

static void update_role_label(FilterBarState *fb) {
    if (!fb->filter_role) return;
    int count = __builtin_popcount(fb->selected_roles_mask);
    if (count == 0) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_role), "All Roles");
    } else if (count == 1) {
        for (int i = 0; i < NUM_ROLE_FILTERS; i++) {
            if (fb->selected_roles_mask & (1u << i)) {
                gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_role),
                                          ROLE_FILTERS[i].name);
                break;
            }
        }
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d selected", count);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->filter_role), buf);
    }
    set_field_active(fb->filter_role, count > 0);
}

static void sync_sort_dropdown(FilterBarState *fb) {
    if (!fb->sort_dropdown || fb->sort_option_count == 0) return;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->sort_dropdown),
                              fb->sort_options[fb->current_sort_index].label);
    if (fb->sort_actions) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(fb->sort_actions), "order");
        if (a) {
            char idx_str[8];
            snprintf(idx_str, sizeof(idx_str), "%d", fb->current_sort_index);
            g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_string(idx_str));
        }
    }
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
 *
 * Used for all multi-select filters (Genre, Year, Role).
 * Custom GtkPopover with GtkCheckButton children stays open until
 * the user clicks outside — unlike GtkPopoverMenu which closes on
 * item activation.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void uncheck_all_in_container(GtkWidget *widget) {
    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_CHECK_BUTTON(child))
            gtk_check_button_set_active(GTK_CHECK_BUTTON(child), FALSE);
        else if (GTK_IS_SCROLLED_WINDOW(child))
            uncheck_all_in_container(
                gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(child)));
        else if (GTK_IS_BOX(child))
            uncheck_all_in_container(child);
    }
}

static void uncheck_popover(GtkMenuButton *mb) {
    GtkPopover *popover = gtk_menu_button_get_popover(mb);
    if (!popover) return;
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));
    if (!content) return;
    uncheck_all_in_container(content);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Popover Search Filter
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_genre_search_changed(GtkSearchEntry *entry, gpointer data) {
    GtkWidget *checkbox_box = GTK_WIDGET(data);
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));

    for (GtkWidget *child = gtk_widget_get_first_child(checkbox_box);
         child; child = gtk_widget_get_next_sibling(child)) {
        if (!GTK_IS_CHECK_BUTTON(child)) continue;
        const char *label = gtk_check_button_get_label(GTK_CHECK_BUTTON(child));
        gboolean visible = (!query || !*query ||
                            strcasestr(label, query) != NULL);
        gtk_widget_set_visible(child, visible);
    }
}

/** Reset search filter when popover opens so all items are visible. */
static void on_genre_popover_show(GtkWidget *popover, gpointer data) {
    (void)popover;
    GtkSearchEntry *search = GTK_SEARCH_ENTRY(data);
    gtk_editable_set_text(GTK_EDITABLE(search), "");
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
 * Year Toggle Handler (checkbox popover — multi-select, stays open)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_year_toggled(GtkCheckButton *check, gpointer data) {
    FilterBarState *fb = data;
    int decade_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "decade-idx"));
    if (gtk_check_button_get_active(check))
        fb->selected_years_mask |= (uint16_t)(1 << decade_idx);
    else
        fb->selected_years_mask &= (uint16_t)~(1 << decade_idx);
    update_year_label(fb);
    notify_changed(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Role Toggle Handler (checkbox popover — multi-select, stays open)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_role_toggled(GtkCheckButton *check, gpointer data) {
    FilterBarState *fb = data;
    int role_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "role-idx"));
    if (gtk_check_button_get_active(check))
        fb->selected_roles_mask |= (1u << role_idx);
    else
        fb->selected_roles_mask &= ~(1u << role_idx);
    update_role_label(fb);
    notify_changed(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Search Filter (debounced)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void debounce_restart(guint *timer, GSourceFunc fire_cb, gpointer data) {
    if (*timer) g_source_remove(*timer);
    *timer = g_timeout_add(200, fire_cb, data);
}

static gboolean on_filter_debounce_fire(gpointer data) {
    FilterBarState *fb = data;
    fb->filter_debounce_timer = 0;
    notify_changed(fb);
    return G_SOURCE_REMOVE;
}

static void on_filter_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    FilterBarState *fb = data;
    debounce_restart(&fb->filter_debounce_timer, on_filter_debounce_fire, fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Clear All Filters
 * ═══════════════════════════════════════════════════════════════════════════ */

static void clear_multiselect(FilterBarState *fb) {
    if (fb->selected_genres) g_hash_table_remove_all(fb->selected_genres);
    fb->selected_years_mask = 0;
    fb->selected_roles_mask = 0;

    if (fb->filter_genre) {
        uncheck_popover(GTK_MENU_BUTTON(fb->filter_genre));
        update_genre_label(fb);
    }
    if (fb->filter_year) {
        uncheck_popover(GTK_MENU_BUTTON(fb->filter_year));
        update_year_label(fb);
    }
    if (fb->filter_role) {
        uncheck_popover(GTK_MENU_BUTTON(fb->filter_role));
        update_role_label(fb);
    }
}

static void on_clear_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    FilterBarState *fb = data;
    filter_bar_clear(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sort Menu (GMenu + radio-style string-state GAction — single-select)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_sort_action_activate(GSimpleAction *action, GVariant *param,
                                     gpointer data) {
    FilterBarState *fb = data;
    const char *idx_str = g_variant_get_string(param, NULL);
    int idx = atoi(idx_str);
    if (idx < 0 || idx >= fb->sort_option_count) return;
    g_simple_action_set_state(action, g_variant_new_string(idx_str));
    fb->current_sort_index = idx;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->sort_dropdown),
                              fb->sort_options[idx].label);
    notify_changed(fb);
}

static void setup_sort_menu(FilterBarState *fb) {
    if (!fb->sort_dropdown || fb->sort_option_count == 0) return;

    fb->sort_actions = g_simple_action_group_new();
    GSimpleAction *action = g_simple_action_new_stateful(
        "order", G_VARIANT_TYPE_STRING, g_variant_new_string("0"));
    g_signal_connect(action, "activate",
                     G_CALLBACK(on_sort_action_activate), fb);
    g_action_map_add_action(G_ACTION_MAP(fb->sort_actions), G_ACTION(action));
    g_object_unref(action);

    GMenu *menu = g_menu_new();
    for (int i = 0; i < fb->sort_option_count; i++) {
        char idx_str[8];
        snprintf(idx_str, sizeof(idx_str), "%d", i);
        GMenuItem *item = g_menu_item_new(fb->sort_options[i].label, NULL);
        g_menu_item_set_action_and_target_value(item, "sort.order",
            g_variant_new_string(idx_str));
        g_menu_append_item(menu, item);
        g_object_unref(item);
    }

    gtk_widget_insert_action_group(GTK_WIDGET(fb->sort_dropdown),
                                   "sort", G_ACTION_GROUP(fb->sort_actions));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(fb->sort_dropdown),
                                    G_MENU_MODEL(menu));
    gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->sort_dropdown),
                              fb->sort_options[0].label);
    g_object_unref(menu);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Search Mode Menu (GMenu + radio-style — single-select, closes on click)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_search_mode_activate(GSimpleAction *action, GVariant *param,
                                     gpointer data) {
    FilterBarState *fb = data;
    const char *mode_str = g_variant_get_string(param, NULL);
    int mode = atoi(mode_str);
    if (mode < 0 || mode >= NUM_SEARCH_MODES) return;
    g_simple_action_set_state(action, g_variant_new_string(mode_str));
    fb->search_mode = (FilterSearchMode)mode;
    gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->search_mode_dropdown),
                              SEARCH_MODE_LABELS[mode]);

    /* Toggle metadata mode: swap Genre/Year for Role */
    filter_bar_set_metadata_mode(fb, fb->search_mode == FILTER_SEARCH_METADATA);
    notify_changed(fb);
}

static void setup_search_mode_menu(FilterBarState *fb) {
    if (!fb->search_mode_dropdown) return;

    GSimpleActionGroup *group = g_simple_action_group_new();
    GSimpleAction *action = g_simple_action_new_stateful(
        "mode", G_VARIANT_TYPE_STRING, g_variant_new_string("0"));
    g_signal_connect(action, "activate",
                     G_CALLBACK(on_search_mode_activate), fb);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);

    GMenu *menu = g_menu_new();
    for (int i = 0; i < NUM_SEARCH_MODES; i++) {
        char idx_str[8];
        snprintf(idx_str, sizeof(idx_str), "%d", i);
        GMenuItem *item = g_menu_item_new(SEARCH_MODE_LABELS[i], NULL);
        g_menu_item_set_action_and_target_value(item, "smode.mode",
            g_variant_new_string(idx_str));
        g_menu_append_item(menu, item);
        g_object_unref(item);
    }

    gtk_widget_insert_action_group(GTK_WIDGET(fb->search_mode_dropdown),
                                   "smode", G_ACTION_GROUP(group));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(fb->search_mode_dropdown),
                                    G_MENU_MODEL(menu));
    g_object_unref(group);
    g_object_unref(menu);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Popover Builder (checkbox — multi-select)
 * ═══════════════════════════════════════════════════════════════════════════ */

void filter_bar_rebuild_genre_popover(FilterBarState *fb) {
    if (!fb->filter_genre || !fb->cache) return;

    GPtrArray *albums = library_cache_get_albums_filtered(fb->cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, LIBRARY_MASK_ALL);
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

    /* Build popover with search entry + checkboxes */
    GtkWidget *checkbox_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (guint i = 0; i < fb->genre_list->len; i++) {
        const char *genre = g_ptr_array_index(fb->genre_list, i);
        GtkWidget *check = gtk_check_button_new_with_label(genre);
        g_signal_connect(check, "toggled", G_CALLBACK(on_genre_toggled), fb);
        gtk_box_append(GTK_BOX(checkbox_box), check);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), checkbox_box);

    /* Search entry for type-ahead filtering */
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_add_css_class(search, "genre-search");
    gtk_widget_set_hexpand(search, TRUE);
    g_signal_connect(search, "search-changed",
                     G_CALLBACK(on_genre_search_changed), checkbox_box);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(outer), search);
    gtk_box_append(GTK_BOX(outer), scroll);

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), outer);
    g_signal_connect(popover, "show",
                     G_CALLBACK(on_genre_popover_show), search);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->filter_genre), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Year Popover Builder (checkbox — multi-select, stays open)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_year_popover(FilterBarState *fb) {
    if (!fb->filter_year) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (int i = 0; i < NUM_DECADES; i++) {
        GtkWidget *check = gtk_check_button_new_with_label(DECADE_NAMES[i]);
        g_object_set_data(G_OBJECT(check), "decade-idx", GINT_TO_POINTER(i));
        g_signal_connect(check, "toggled", G_CALLBACK(on_year_toggled), fb);
        gtk_box_append(GTK_BOX(box), check);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->filter_year), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Role Popover Builder (checkbox — multi-select, stays open)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_role_popover(FilterBarState *fb) {
    if (!fb->filter_role) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (int i = 0; i < NUM_ROLE_FILTERS; i++) {
        GtkWidget *check = gtk_check_button_new_with_label(ROLE_FILTERS[i].name);
        g_object_set_data(G_OBJECT(check), "role-idx", GINT_TO_POINTER(i));
        g_signal_connect(check, "toggled", G_CALLBACK(on_role_toggled), fb);
        gtk_box_append(GTK_BOX(box), check);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(fb->filter_role), popover);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Show Featuring Toggle
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_show_featuring_toggled(GtkToggleButton *btn, gpointer data) {
    (void)btn;
    FilterBarState *fb = data;
    notify_changed(fb);
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
    fb->filter_role = GTK_WIDGET(gtk_builder_get_object(builder, "filter_role"));
    fb->genre_box = GTK_WIDGET(gtk_builder_get_object(builder, "genre_box"));
    fb->year_box = GTK_WIDGET(gtk_builder_get_object(builder, "year_box"));
    fb->role_box = GTK_WIDGET(gtk_builder_get_object(builder, "role_box"));
    fb->filter_search = GTK_WIDGET(gtk_builder_get_object(builder, "filter_search"));
    fb->filter_search_row = GTK_WIDGET(gtk_builder_get_object(builder, "filter_search_row"));
    fb->filter_search_box = GTK_WIDGET(gtk_builder_get_object(builder, "filter_search_box"));
    fb->filter_clear = GTK_WIDGET(gtk_builder_get_object(builder, "filter_clear"));
    fb->sort_box = GTK_WIDGET(gtk_builder_get_object(builder, "sort_box"));
    fb->sort_dropdown = GTK_WIDGET(gtk_builder_get_object(builder, "sort_dropdown"));
    fb->search_mode_dropdown = GTK_WIDGET(gtk_builder_get_object(builder, "search_mode_dropdown"));
    fb->show_featuring_toggle = GTK_WIDGET(gtk_builder_get_object(builder, "show_featuring_toggle"));

    g_object_unref(builder);

    /* Initialize state */
    fb->selected_genres = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Build multi-select popovers (checkbox-based, stays open) */
    filter_bar_rebuild_genre_popover(fb);
    build_year_popover(fb);
    build_role_popover(fb);

    /* Sort menu (GMenu with radio-style action — single-select, closes on click) */
    if (sort_options && sort_count > 0) {
        gtk_widget_set_visible(fb->sort_box, TRUE);
        setup_sort_menu(fb);
    }

    /* Search mode menu (single-select, closes on click) */
    setup_search_mode_menu(fb);

    /* Connect signals */
    if (fb->filter_search)
        g_signal_connect(fb->filter_search, "search-changed",
                         G_CALLBACK(on_filter_search_changed), fb);
    if (fb->filter_clear)
        g_signal_connect(fb->filter_clear, "clicked",
                         G_CALLBACK(on_clear_clicked), fb);
    if (fb->show_featuring_toggle)
        g_signal_connect(fb->show_featuring_toggle, "toggled",
                         G_CALLBACK(on_show_featuring_toggled), fb);

    return fb->bar_widget;
}

void filter_bar_set_metadata_mode(FilterBarState *fb, gboolean metadata) {
    if (fb->role_box) gtk_widget_set_visible(fb->role_box, metadata);
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
    sync_sort_dropdown(fb);

    /* Reset search mode to default */
    fb->search_mode = FILTER_SEARCH_DEFAULT;
    if (fb->search_mode_dropdown)
        gtk_menu_button_set_label(GTK_MENU_BUTTON(fb->search_mode_dropdown),
                                  SEARCH_MODE_LABELS[0]);
    filter_bar_set_metadata_mode(fb, FALSE);

    if (fb->show_featuring_toggle)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fb->show_featuring_toggle), TRUE);

    notify_changed(fb);
}

void filter_bar_destroy(FilterBarState *fb) {
    if (fb->filter_debounce_timer) {
        g_source_remove(fb->filter_debounce_timer);
        fb->filter_debounce_timer = 0;
    }
    if (fb->selected_genres) {
        g_hash_table_unref(fb->selected_genres);
        fb->selected_genres = NULL;
    }
    if (fb->genre_list) {
        g_ptr_array_unref(fb->genre_list);
        fb->genre_list = NULL;
    }
    g_clear_object(&fb->sort_actions);
    if (fb->bar_widget) {
        g_object_unref(fb->bar_widget);
        fb->bar_widget = NULL;
    }
}

gboolean filter_bar_is_active(const FilterBarState *fb) {
    if (fb->selected_genres && g_hash_table_size(fb->selected_genres) > 0)
        return TRUE;
    if (fb->selected_years_mask != 0)
        return TRUE;
    if (fb->selected_roles_mask != 0)
        return TRUE;
    if (fb->search_mode != FILTER_SEARCH_DEFAULT)
        return TRUE;
    if (fb->filter_search) {
        const char *text = gtk_editable_get_text(GTK_EDITABLE(fb->filter_search));
        if (text && *text) return TRUE;
    }
    if (fb->show_featuring_toggle &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb->show_featuring_toggle)))
        return TRUE;
    return FALSE;
}

gboolean filter_bar_get_show_featuring(const FilterBarState *fb) {
    if (!fb->show_featuring_toggle) return TRUE;
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb->show_featuring_toggle));
}

library_sort_t filter_bar_get_sort(const FilterBarState *fb) {
    if (fb->sort_options && fb->sort_option_count > 0)
        return fb->sort_options[fb->current_sort_index].sort;
    return LIBRARY_SORT_NAME_ASC;
}

const char *filter_bar_get_search_text(const FilterBarState *fb) {
    if (!fb->filter_search) return NULL;
    if (fb->filter_search_row && !gtk_widget_get_visible(fb->filter_search_row))
        return NULL;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(fb->filter_search));
    return (text && *text) ? text : NULL;
}

FilterSearchMode filter_bar_get_search_mode(const FilterBarState *fb) {
    return fb->search_mode;
}

uint32_t filter_bar_get_selected_roles_mask(const FilterBarState *fb) {
    return fb->selected_roles_mask;
}

const char **filter_bar_get_selected_role_gids(const FilterBarState *fb, int *count_out) {
    if (!fb->selected_roles_mask) {
        if (count_out) *count_out = 0;
        return NULL;
    }
    int n = __builtin_popcount(fb->selected_roles_mask);
    const char **gids = g_new(const char *, n + 1);
    int idx = 0;
    for (int i = 0; i < NUM_ROLE_FILTERS; i++) {
        if (fb->selected_roles_mask & (1u << i))
            gids[idx++] = ROLE_FILTERS[i].gid;
    }
    gids[idx] = NULL;
    if (count_out) *count_out = idx;
    return gids;
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
    if (fb->filter_search_row)
        gtk_widget_set_visible(fb->filter_search_row, FALSE);
}
