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

static void sync_sort_dropdown(FilterBarState *fb) {
    if (!fb->sort_dropdown || fb->sort_option_count == 0) return;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fb->sort_dropdown),
                                (guint)fb->current_sort_index);
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
 * Year Filter — GAction-driven
 *
 * Each decade is a boolean stateful GAction. GtkPopoverMenu renders them
 * as checkmark items automatically. The bitmask is derived from action state.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_year_action_toggle(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)param;
    FilterBarState *fb = data;
    gboolean active = g_variant_get_boolean(g_action_get_state(G_ACTION(action)));
    g_simple_action_set_state(action, g_variant_new_boolean(!active));

    /* Rebuild bitmask from action states */
    fb->selected_years_mask = 0;
    for (int i = 0; i < NUM_DECADES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "decade-%d", i);
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(fb->year_actions), name);
        if (a && g_variant_get_boolean(g_action_get_state(a)))
            fb->selected_years_mask |= (uint16_t)(1 << i);
    }
    update_year_label(fb);
    notify_changed(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Search Filter (debounced)
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Restart a debounce timer — cancel any pending fire and schedule a new one. */
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

    if (fb->filter_genre) {
        uncheck_popover(GTK_MENU_BUTTON(fb->filter_genre));
        update_genre_label(fb);
    }

    /* Reset year GActions to FALSE */
    if (fb->year_actions) {
        for (int i = 0; i < NUM_DECADES; i++) {
            char name[16];
            snprintf(name, sizeof(name), "decade-%d", i);
            GAction *a = g_action_map_lookup_action(G_ACTION_MAP(fb->year_actions), name);
            if (a) g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(FALSE));
        }
    }
    if (fb->filter_year) update_year_label(fb);
}

static void on_clear_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    FilterBarState *fb = data;
    filter_bar_clear(fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sort Dropdown Handler (GtkDropDown)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_sort_dropdown_selected(GtkDropDown *dd, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    FilterBarState *fb = data;
    guint idx = gtk_drop_down_get_selected(dd);
    if (idx == GTK_INVALID_LIST_POSITION) return;
    fb->current_sort_index = (int)idx;
    notify_changed(fb);
}

/* (build_checkbox_popover removed — year filter now uses GMenu + GAction) */

static void setup_sort_dropdown(FilterBarState *fb) {
    if (!fb->sort_dropdown || fb->sort_option_count == 0) return;

    const char **labels = g_new(const char *, fb->sort_option_count + 1);
    for (int i = 0; i < fb->sort_option_count; i++)
        labels[i] = fb->sort_options[i].label;
    labels[fb->sort_option_count] = NULL;

    GtkStringList *model = gtk_string_list_new(labels);
    g_free(labels);

    gtk_drop_down_set_model(GTK_DROP_DOWN(fb->sort_dropdown), G_LIST_MODEL(model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fb->sort_dropdown), 0);
    g_object_unref(model);

    g_signal_connect(fb->sort_dropdown, "notify::selected",
                     G_CALLBACK(on_sort_dropdown_selected), fb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Genre Popover Builder
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

/** Build year filter using GMenu + boolean stateful GActions.
 *  GtkPopoverMenu renders boolean-state actions as checkmark items. */
static void build_year_popover(FilterBarState *fb) {
    if (!fb->filter_year) return;

    /* Create action group with one boolean action per decade */
    fb->year_actions = g_simple_action_group_new();
    GMenu *menu = g_menu_new();

    for (int i = 0; i < NUM_DECADES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "decade-%d", i);

        GSimpleAction *action = g_simple_action_new_stateful(
            name, NULL, g_variant_new_boolean(FALSE));
        g_signal_connect(action, "activate",
                         G_CALLBACK(on_year_action_toggle), fb);
        g_action_map_add_action(G_ACTION_MAP(fb->year_actions), G_ACTION(action));
        g_object_unref(action);

        char detailed[32];
        snprintf(detailed, sizeof(detailed), "year.decade-%d", i);
        g_menu_append(menu, DECADE_NAMES[i], detailed);
    }

    /* Attach action group to the menu button and set the menu model */
    gtk_widget_insert_action_group(GTK_WIDGET(fb->filter_year),
                                   "year", G_ACTION_GROUP(fb->year_actions));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(fb->filter_year),
                                    G_MENU_MODEL(menu));
    g_object_unref(menu);
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
            gtk_drop_down_set_selected(GTK_DROP_DOWN(fb->filter_role), 0);
        notify_changed(fb);
    }
}

static gboolean on_credit_debounce_fire(gpointer data) {
    FilterBarState *fb = data;
    fb->credit_debounce_timer = 0;
    notify_changed(fb);
    return G_SOURCE_REMOVE;
}

static void on_credit_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    FilterBarState *fb = data;
    debounce_restart(&fb->credit_debounce_timer, on_credit_debounce_fire, fb);
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

static void on_role_dropdown_selected(GtkDropDown *dd, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    FilterBarState *fb = data;
    guint idx = gtk_drop_down_get_selected(dd);
    if (idx == GTK_INVALID_LIST_POSITION) return;
    fb->selected_role_index = (int)idx;
    notify_changed(fb);
}

static void setup_role_dropdown(FilterBarState *fb) {
    if (!fb->filter_role) return;

    const char *labels[NUM_ROLE_FILTERS + 1];
    for (int i = 0; i < NUM_ROLE_FILTERS; i++)
        labels[i] = ROLE_FILTERS[i].name;
    labels[NUM_ROLE_FILTERS] = NULL;

    GtkStringList *model = gtk_string_list_new(labels);
    gtk_drop_down_set_model(GTK_DROP_DOWN(fb->filter_role), G_LIST_MODEL(model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fb->filter_role), 0);
    g_object_unref(model);

    g_signal_connect(fb->filter_role, "notify::selected",
                     G_CALLBACK(on_role_dropdown_selected), fb);
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

    g_object_unref(builder);

    /* Initialize state */
    fb->selected_genres = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Build popovers */
    filter_bar_rebuild_genre_popover(fb);
    build_year_popover(fb);

    /* Sort dropdown (GtkDropDown with GtkStringList model) */
    if (sort_options && sort_count > 0) {
        gtk_widget_set_visible(fb->sort_dropdown, TRUE);
        setup_sort_dropdown(fb);
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

    /* Setup role dropdown (GtkDropDown with GtkStringList model) */
    setup_role_dropdown(fb);

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
    sync_sort_dropdown(fb);

    /* Clear advanced panel */
    if (fb->credit_search_entry)
        gtk_editable_set_text(GTK_EDITABLE(fb->credit_search_entry), "");
    if (fb->credit_debounce_timer) {
        g_source_remove(fb->credit_debounce_timer);
        fb->credit_debounce_timer = 0;
    }
    fb->selected_role_index = 0;
    if (fb->filter_role)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(fb->filter_role), 0);
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
    g_clear_object(&fb->year_actions);
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

