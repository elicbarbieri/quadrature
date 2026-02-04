/**
 * Quadrature Library Views
 *
 * GTK4 virtualized list views for browsing artists and albums.
 * Uses LazyList (GtkListView + GListStore) for efficient rendering.
 * Factory callbacks create/bind/unbind row widgets on demand.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * View Data - Attached to container widget
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LibraryItemKind kind;
    library_cache_t *cache;
    ArtworkManager *art_mgr;
    LibraryCallbacks cbs;

    GtkWidget *container;
    GtkWidget *subtitle;
    GtkWidget *scroll;

    LazyList *lazy_list;

    /* Sort state */
    library_sort_t current_sort;
    GtkWidget *sort_buttons[4];

    /* Filter panel */
    GtkWidget *filter_panel;
    GtkWidget *filter_genre;     /* GtkMenuButton */
    GtkWidget *filter_year;      /* GtkMenuButton */
    GtkWidget *filter_search;
    GtkWidget *filter_clear;
    guint filter_debounce_timer;

    /* Multi-select filter state */
    GPtrArray *genre_list;          /* Sorted unique genre strings (owned) */
    GHashTable *selected_genres;    /* Set of selected genre strings */
    uint16_t selected_years_mask;   /* Bitmask: bit 0=2020s .. bit 7=Pre-1960 */
} ViewData;

static const char *VIEW_DATA_KEY = "library-view-data";

static void view_data_free(gpointer data) {
    ViewData *vd = data;
    if (vd->filter_debounce_timer) {
        g_source_remove(vd->filter_debounce_timer);
        vd->filter_debounce_timer = 0;
    }
    if (vd->selected_genres) g_hash_table_unref(vd->selected_genres);
    if (vd->genre_list) g_ptr_array_unref(vd->genre_list);
    lazy_list_free(vd->lazy_list);
    g_free(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Factory Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void artist_row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)li; (void)data;
}

static void artist_row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    QuadArtistItem *item = QUAD_ARTIST_ITEM(gtk_list_item_get_item(li));
    GtkWidget *row = ui_create_artist_row(item->info, vd->cache, vd->art_mgr, TRUE, NULL);
    gtk_list_item_set_child(li, row);
}

static void artist_row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    gtk_list_item_set_child(li, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Album Factory Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void album_row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)li; (void)data;
}

static void album_row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    QuadAlbumItem *item = QUAD_ALBUM_ITEM(gtk_list_item_get_item(li));
    GtkWidget *row = ui_create_album_row(item->info, vd->cache, vd->art_mgr, TRUE,
                                          &vd->cbs.artist_cbs, NULL);
    ui_row_attach_handlers(row, &vd->cbs.album_cbs);
    gtk_list_item_set_child(li, row);
}

static void album_row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    gtk_list_item_set_child(li, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Activation Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_artist_activate(guint position, gpointer data) {
    ViewData *vd = data;
    GListModel *model = G_LIST_MODEL(lazy_list_get_store(vd->lazy_list));
    QuadArtistItem *item = QUAD_ARTIST_ITEM(g_list_model_get_item(model, position));
    if (!item) return;

    if (vd->cbs.on_navigate)
        vd->cbs.on_navigate(LIBRARY_ITEM_ARTIST, item->info->artist_id, vd->cbs.user_data);

    g_object_unref(item);
}

static void on_album_activate(guint position, gpointer data) {
    ViewData *vd = data;
    GListModel *model = G_LIST_MODEL(lazy_list_get_store(vd->lazy_list));
    QuadAlbumItem *item = QUAD_ALBUM_ITEM(g_list_model_get_item(model, position));
    if (!item) return;

    if (vd->cbs.on_navigate)
        vd->cbs.on_navigate(LIBRARY_ITEM_ALBUM, item->info->album_id, vd->cbs.user_data);

    g_object_unref(item);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Filter Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *DECADE_NAMES[] = {
    "2020s", "2010s", "2000s", "1990s", "1980s", "1970s", "1960s", "Pre-1960",
};
#define NUM_DECADES 8

/* Check if any filter is active */
static gboolean filters_active(ViewData *vd) {
    if (!vd->filter_panel) return FALSE;
    if (vd->selected_genres && g_hash_table_size(vd->selected_genres) > 0)
        return TRUE;
    if (vd->selected_years_mask != 0)
        return TRUE;
    if (vd->filter_search) {
        const char *text = gtk_editable_get_text(GTK_EDITABLE(vd->filter_search));
        if (text && *text) return TRUE;
    }
    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * List Population
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Marshal GHashTable of selected genres into a const char** array for db_search_opts_t */
static const char **genres_to_array(GHashTable *selected, size_t *count_out) {
    if (!selected || g_hash_table_size(selected) == 0) {
        *count_out = 0;
        return NULL;
    }
    size_t n = g_hash_table_size(selected);
    const char **arr = g_new(const char *, n);
    GHashTableIter iter;
    gpointer key;
    size_t i = 0;
    g_hash_table_iter_init(&iter, selected);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        arr[i++] = (const char *)key;
    *count_out = n;
    return arr;
}

static void populate_artists(ViewData *vd) {
    GListStore *store = lazy_list_get_store(vd->lazy_list);
    g_list_store_remove_all(store);

    if (!vd->cache) return;

    gboolean filtering = filters_active(vd);

    if (filtering) {
        /* Get total count from unfiltered DB query */
        GPtrArray *all = library_cache_get_artists_filtered(vd->cache, vd->current_sort, NULL, NULL);
        guint total = all ? all->len : 0;
        if (all) g_ptr_array_unref(all);

        /* Build filter opts */
        const char *search_text = vd->filter_search ?
            gtk_editable_get_text(GTK_EDITABLE(vd->filter_search)) : NULL;
        if (search_text && !*search_text) search_text = NULL;

        size_t genre_count = 0;
        const char **genre_arr = genres_to_array(vd->selected_genres, &genre_count);
        db_search_opts_t filter_opts = {
            .genres = genre_arr,
            .genre_count = genre_count,
            .year_mask = vd->selected_years_mask
        };
        const db_search_opts_t *opts = (genre_count > 0 || vd->selected_years_mask) ? &filter_opts : NULL;

        GPtrArray *filtered = library_cache_get_artists_filtered(
            vd->cache, vd->current_sort, search_text, opts);

        for (guint i = 0; i < filtered->len; i++) {
            const library_artist_info_t *artist = g_ptr_array_index(filtered, i);
            QuadArtistItem *item = quad_artist_item_new(artist);
            g_list_store_append(store, item);
            g_object_unref(item);
        }

        if (vd->subtitle) {
            char buf[96];
            if (filtered->len != total)
                snprintf(buf, sizeof(buf), "%u artist%s (%u shown)",
                         total, total == 1 ? "" : "s", filtered->len);
            else
                snprintf(buf, sizeof(buf), "%u artist%s",
                         total, total == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(filtered);
        g_free(genre_arr);
    } else {
        /* No filters - get all artists via filtered API with NULL params */
        GPtrArray *artists = library_cache_get_artists_filtered(vd->cache, vd->current_sort, NULL, NULL);
        if (!artists) return;

        for (guint i = 0; i < artists->len; i++) {
            const library_artist_info_t *artist = g_ptr_array_index(artists, i);
            QuadArtistItem *item = quad_artist_item_new(artist);
            g_list_store_append(store, item);
            g_object_unref(item);
        }

        if (vd->subtitle) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%u artist%s",
                     artists->len, artists->len == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(artists);
    }
}

static void populate_albums(ViewData *vd) {
    GListStore *store = lazy_list_get_store(vd->lazy_list);
    g_list_store_remove_all(store);

    if (!vd->cache) return;

    gboolean filtering = filters_active(vd);

    if (filtering) {
        /* Get total count from unfiltered DB query */
        GPtrArray *all = library_cache_get_albums_filtered(vd->cache, vd->current_sort, NULL, NULL);
        guint total = all ? all->len : 0;
        if (all) g_ptr_array_unref(all);

        /* Build filter opts */
        const char *search_text = vd->filter_search ?
            gtk_editable_get_text(GTK_EDITABLE(vd->filter_search)) : NULL;
        if (search_text && !*search_text) search_text = NULL;

        size_t genre_count = 0;
        const char **genre_arr = genres_to_array(vd->selected_genres, &genre_count);
        db_search_opts_t filter_opts = {
            .genres = genre_arr,
            .genre_count = genre_count,
            .year_mask = vd->selected_years_mask
        };
        const db_search_opts_t *opts = (genre_count > 0 || vd->selected_years_mask) ? &filter_opts : NULL;

        GPtrArray *filtered = library_cache_get_albums_filtered(
            vd->cache, vd->current_sort, search_text, opts);

        for (guint i = 0; i < filtered->len; i++) {
            const library_album_info_t *album = g_ptr_array_index(filtered, i);
            QuadAlbumItem *item = quad_album_item_new(album);
            g_list_store_append(store, item);
            g_object_unref(item);
        }

        if (vd->subtitle) {
            char buf[96];
            if (filtered->len != total)
                snprintf(buf, sizeof(buf), "%u album%s (%u shown)",
                         total, total == 1 ? "" : "s", filtered->len);
            else
                snprintf(buf, sizeof(buf), "%u album%s",
                         total, total == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(filtered);
        g_free(genre_arr);
    } else {
        /* No filters - get all albums via filtered API with NULL params */
        GPtrArray *albums = library_cache_get_albums_filtered(vd->cache, vd->current_sort, NULL, NULL);
        if (!albums) return;

        for (guint i = 0; i < albums->len; i++) {
            const library_album_info_t *album = g_ptr_array_index(albums, i);
            QuadAlbumItem *item = quad_album_item_new(album);
            g_list_store_append(store, item);
            g_object_unref(item);
        }

        if (vd->subtitle) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%u album%s",
                     albums->len, albums->len == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(albums);
    }
}

static void populate_view(ViewData *vd) {
    switch (vd->kind) {
        case LIBRARY_ITEM_ARTIST:
            populate_artists(vd);
            break;
        case LIBRARY_ITEM_ALBUM:
            populate_albums(vd);
            break;
        case LIBRARY_ITEM_TRACK:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sort Button Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_sort_button_states(ViewData *vd) {
    static const library_sort_t sorts[] = {
        LIBRARY_SORT_NAME_ASC, LIBRARY_SORT_YEAR_DESC,
        LIBRARY_SORT_ARTIST_ASC, LIBRARY_SORT_RECENT
    };

    for (int i = 0; i < 4; i++) {
        if (!vd->sort_buttons[i]) continue;

        gboolean active = (vd->current_sort == sorts[i]);
        gtk_widget_remove_css_class(vd->sort_buttons[i], "sort-button-active");
        if (active)
            gtk_widget_add_css_class(vd->sort_buttons[i], "sort-button-active");
    }
}

static void on_sort_clicked(GtkButton *btn, gpointer data) {
    ViewData *vd = data;

    static const library_sort_t sorts[] = {
        LIBRARY_SORT_NAME_ASC, LIBRARY_SORT_YEAR_DESC,
        LIBRARY_SORT_ARTIST_ASC, LIBRARY_SORT_RECENT
    };

    for (int i = 0; i < 4; i++) {
        if (GTK_WIDGET(btn) == vd->sort_buttons[i]) {
            vd->current_sort = sorts[i];
            break;
        }
    }

    update_sort_button_states(vd);
    populate_view(vd);
}

static void setup_sort_buttons(ViewData *vd, GtkBuilder *builder) {
    const char *ids[] = {"sort_title", "sort_year", "sort_artist", "sort_added"};

    for (int i = 0; i < 4; i++) {
        vd->sort_buttons[i] = GTK_WIDGET(gtk_builder_get_object(builder, ids[i]));
        if (vd->sort_buttons[i]) {
            g_signal_connect(vd->sort_buttons[i], "clicked", G_CALLBACK(on_sort_clicked), vd);
        }
    }

    vd->current_sort = (vd->kind == LIBRARY_ITEM_ALBUM) ?
                       LIBRARY_SORT_ARTIST_ASC : LIBRARY_SORT_NAME_ASC;
    update_sort_button_states(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Filter Panel Setup
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Multi-select button label updates ── */

static void update_genre_button_label(ViewData *vd) {
    if (!vd->filter_genre) return;
    guint count = vd->selected_genres ? g_hash_table_size(vd->selected_genres) : 0;
    if (count == 0) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(vd->filter_genre), "All");
    } else if (count == 1) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, vd->selected_genres);
        g_hash_table_iter_next(&iter, &key, NULL);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(vd->filter_genre), (const char *)key);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u selected", count);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(vd->filter_genre), buf);
    }
}

static void update_year_button_label(ViewData *vd) {
    if (!vd->filter_year) return;
    int count = __builtin_popcount(vd->selected_years_mask);
    if (count == 0) {
        gtk_menu_button_set_label(GTK_MENU_BUTTON(vd->filter_year), "All");
    } else if (count == 1) {
        for (int i = 0; i < NUM_DECADES; i++) {
            if (vd->selected_years_mask & (1 << i)) {
                gtk_menu_button_set_label(GTK_MENU_BUTTON(vd->filter_year), DECADE_NAMES[i]);
                break;
            }
        }
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d selected", count);
        gtk_menu_button_set_label(GTK_MENU_BUTTON(vd->filter_year), buf);
    }
}

/* ── Checkbox toggle handlers ── */

static void on_genre_check_toggled(GtkCheckButton *check, gpointer data) {
    ViewData *vd = data;
    const char *genre = gtk_check_button_get_label(check);
    if (gtk_check_button_get_active(check))
        g_hash_table_add(vd->selected_genres, g_strdup(genre));
    else
        g_hash_table_remove(vd->selected_genres, genre);
    update_genre_button_label(vd);
    populate_view(vd);
}

static void on_year_check_toggled(GtkCheckButton *check, gpointer data) {
    ViewData *vd = data;
    int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "bit-index"));
    if (gtk_check_button_get_active(check))
        vd->selected_years_mask |= (uint16_t)(1 << bit);
    else
        vd->selected_years_mask &= (uint16_t)~(1 << bit);
    update_year_button_label(vd);
    populate_view(vd);
}

/* ── Search filter (unchanged logic) ── */

static gboolean on_view_filter_search_debounce(gpointer data) {
    ViewData *vd = data;
    vd->filter_debounce_timer = 0;
    populate_view(vd);
    return G_SOURCE_REMOVE;
}

static void on_view_filter_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    ViewData *vd = data;
    if (vd->filter_debounce_timer)
        g_source_remove(vd->filter_debounce_timer);
    vd->filter_debounce_timer = g_timeout_add(200, on_view_filter_search_debounce, vd);
}

/* ── Clear all filters ── */

static void clear_multiselect_state(ViewData *vd);

static void on_view_filter_clear_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    ViewData *vd = data;
    clear_multiselect_state(vd);
    if (vd->filter_search)
        gtk_editable_set_text(GTK_EDITABLE(vd->filter_search), "");
    if (vd->filter_debounce_timer) {
        g_source_remove(vd->filter_debounce_timer);
        vd->filter_debounce_timer = 0;
    }
    populate_view(vd);
}

/* Uncheck all checkboxes in a popover's content box */
static void uncheck_popover_checks(GtkMenuButton *mb) {
    GtkPopover *popover = gtk_menu_button_get_popover(mb);
    if (!popover) return;
    GtkWidget *scroll = gtk_popover_get_child(GTK_POPOVER(popover));
    if (!scroll) return;
    GtkWidget *box = NULL;
    if (GTK_IS_SCROLLED_WINDOW(scroll))
        box = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    else
        box = scroll;
    if (!box) return;
    for (GtkWidget *child = gtk_widget_get_first_child(box);
         child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_CHECK_BUTTON(child))
            gtk_check_button_set_active(GTK_CHECK_BUTTON(child), FALSE);
    }
}

static void clear_multiselect_state(ViewData *vd) {
    if (vd->selected_genres) g_hash_table_remove_all(vd->selected_genres);
    vd->selected_years_mask = 0;
    if (vd->filter_genre) {
        uncheck_popover_checks(GTK_MENU_BUTTON(vd->filter_genre));
        update_genre_button_label(vd);
    }
    if (vd->filter_year) {
        uncheck_popover_checks(GTK_MENU_BUTTON(vd->filter_year));
        update_year_button_label(vd);
    }
}

/* ── Popover builders ── */

/* Collect unique genres from all albums and build the genre popover */
static void build_genre_popover(ViewData *vd) {
    if (!vd->filter_genre || !vd->cache) return;

    GPtrArray *albums = library_cache_get_albums_filtered(vd->cache, LIBRARY_SORT_NAME_ASC, NULL, NULL);
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

    if (vd->genre_list) g_ptr_array_unref(vd->genre_list);
    vd->genre_list = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = sorted; l; l = l->next)
        g_ptr_array_add(vd->genre_list, g_strdup((const char *)l->data));
    g_list_free(sorted);
    g_hash_table_unref(genre_set);
    g_ptr_array_unref(albums);

    /* Build popover with checkboxes */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (guint i = 0; i < vd->genre_list->len; i++) {
        const char *genre = g_ptr_array_index(vd->genre_list, i);
        GtkWidget *check = gtk_check_button_new_with_label(genre);
        g_signal_connect(check, "toggled", G_CALLBACK(on_genre_check_toggled), vd);
        gtk_box_append(GTK_BOX(box), check);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(vd->filter_genre), popover);
}

/* Build year popover with fixed decade checkboxes */
static void build_year_popover(ViewData *vd) {
    if (!vd->filter_year) return;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (int i = 0; i < NUM_DECADES; i++) {
        GtkWidget *check = gtk_check_button_new_with_label(DECADE_NAMES[i]);
        g_object_set_data(G_OBJECT(check), "bit-index", GINT_TO_POINTER(i));
        g_signal_connect(check, "toggled", G_CALLBACK(on_year_check_toggled), vd);
        gtk_box_append(GTK_BOX(box), check);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "filter-popover");
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(vd->filter_year), popover);
}

/* ── Filter panel setup ── */

static void setup_filter_panel(ViewData *vd, GtkBuilder *builder) {
    vd->filter_panel = GTK_WIDGET(gtk_builder_get_object(builder, "filter_panel"));
    vd->filter_genre = GTK_WIDGET(gtk_builder_get_object(builder, "filter_genre"));
    vd->filter_year = GTK_WIDGET(gtk_builder_get_object(builder, "filter_year"));
    vd->filter_search = GTK_WIDGET(gtk_builder_get_object(builder, "filter_search"));
    vd->filter_clear = GTK_WIDGET(gtk_builder_get_object(builder, "filter_clear"));

    if (!vd->filter_panel) return;
    gtk_widget_set_visible(vd->filter_panel, TRUE);

    /* Initialize multi-select state */
    vd->selected_genres = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    vd->selected_years_mask = 0;

    /* Build popovers */
    build_genre_popover(vd);
    build_year_popover(vd);

    /* Connect search and clear signals */
    if (vd->filter_search)
        g_signal_connect(vd->filter_search, "search-changed",
                         G_CALLBACK(on_view_filter_search_changed), vd);
    if (vd->filter_clear)
        g_signal_connect(vd->filter_clear, "clicked",
                         G_CALLBACK(on_view_filter_clear_clicked), vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main View Constructor
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *library_view_new(LibraryItemKind kind,
                             library_cache_t *cache,
                             ArtworkManager *art_mgr,
                             const LibraryCallbacks *cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_list_view.ui");
    GtkWidget *box = GTK_WIDGET(gtk_builder_get_object(builder, "container"));
    g_object_ref(box);

    ViewData *vd = g_new0(ViewData, 1);
    vd->kind = kind;
    vd->cache = cache;
    vd->art_mgr = art_mgr;
    if (cbs) vd->cbs = *cbs;
    vd->container = box;
    g_object_set_data_full(G_OBJECT(box), VIEW_DATA_KEY, vd, view_data_free);

    /* Get template widgets */
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    vd->subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    GtkWidget *sort_buttons = GTK_WIDGET(gtk_builder_get_object(builder, "sort_buttons"));
    vd->scroll = GTK_WIDGET(gtk_builder_get_object(builder, "scroll"));

    /* Set title */
    const char *titles[] = {"Artists", "Albums", "Songs"};
    gtk_label_set_text(GTK_LABEL(title), titles[kind]);

    /* Setup sort buttons for albums */
    if (kind == LIBRARY_ITEM_ALBUM) {
        gtk_widget_set_visible(sort_buttons, TRUE);
        setup_sort_buttons(vd, builder);
    }

    /* Create LazyList with appropriate factory callbacks (before filter setup
     * since filter signal handlers trigger populate_view which needs the list) */
    LazyListCallbacks ll_cbs = {0};

    if (kind == LIBRARY_ITEM_ARTIST) {
        ll_cbs.setup = artist_row_setup;
        ll_cbs.bind = artist_row_bind;
        ll_cbs.unbind = artist_row_unbind;
        ll_cbs.activate = on_artist_activate;
        ll_cbs.user_data = vd;
        vd->lazy_list = lazy_list_new(QUAD_TYPE_ARTIST_ITEM, &ll_cbs);
    } else {
        ll_cbs.setup = album_row_setup;
        ll_cbs.bind = album_row_bind;
        ll_cbs.unbind = album_row_unbind;
        ll_cbs.activate = on_album_activate;
        ll_cbs.user_data = vd;
        vd->lazy_list = lazy_list_new(QUAD_TYPE_ALBUM_ITEM, &ll_cbs);
    }

    /* Set list view as child of scroll window */
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vd->scroll),
                                  lazy_list_get_widget(vd->lazy_list));

    /* Connect scroll monitoring */
    lazy_list_connect_scroll(vd->lazy_list, GTK_SCROLLED_WINDOW(vd->scroll));

    /* Setup filter panel (artists and albums views) */
    if (kind == LIBRARY_ITEM_ARTIST || kind == LIBRARY_ITEM_ALBUM)
        setup_filter_panel(vd, builder);

    g_object_unref(builder);

    /* Populate */
    populate_view(vd);

    return box;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Refresh
 * ═══════════════════════════════════════════════════════════════════════════ */

void library_view_refresh(GtkWidget *view) {
    g_assert(view != NULL);
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd) return;

    populate_view(vd);
}

void library_view_clear_filters(GtkWidget *view) {
    g_assert(view != NULL);
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd) return;

    clear_multiselect_state(vd);
    if (vd->filter_search)
        gtk_editable_set_text(GTK_EDITABLE(vd->filter_search), "");
    if (vd->filter_debounce_timer) {
        g_source_remove(vd->filter_debounce_timer);
        vd->filter_debounce_timer = 0;
    }
    populate_view(vd);
}
