/**
 * Quadrature Library Views
 *
 * GTK4 list views for browsing artists, albums, and songs.
 * Uses LibraryModel for main views (lazy loading) and
 * direct queries for detail views (small datasets).
 */

#include "../internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Create a disc header item for multi-disc albums */
static LibraryItem *library_item_new_disc_header(uint16_t disc_num) {
    LibraryItem *item = g_object_new(LIBRARY_TYPE_ITEM, NULL);
    item->kind = LIBRARY_ITEM_TRACK;  /* Same kind for list compatibility */
    item->is_disc_header = TRUE;
    item->disc_num = disc_num;
    char buf[32];
    snprintf(buf, sizeof(buf), "Disc %u", disc_num);
    item->name = g_strdup(buf);
    return item;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * View Data - Attached to container widget
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LibraryItemKind kind;
    gboolean is_detail;
    quadrature_db_t *db;
    LibraryCache *cache;
    ArtworkManager *art_mgr;
    LibraryCallbacks cbs;

    GtkWidget *container;
    GtkWidget *subtitle;
    GtkWidget *list_view;
    GtkWidget *scroll;

    /* Main views */
    LibraryModel *model;

    /* Sort state (for albums/songs views) */
    db_sort_t current_sort;
    GtkWidget *sort_buttons[4];

    /* Detail views */
    GListStore *store;
    int64_t detail_id;
    int64_t detail_artist_id;  /* For album detail: artist ID for navigation */
    GtkWidget *header_title;
    GtkWidget *header_info;
    GtkWidget *header_art;
    GtkWidget *header_artist_btn;  /* Artist link button */

    /* Scroll tracking for prefetching */
    double last_scroll_pos;
    int64_t last_scroll_time;
    int scroll_direction;  /* -1, 0, +1 */
} ViewData;

/* Row data attached to list items for async loading cancellation */
typedef struct {
    GCancellable *art_cancellable;
} RowData;

static void row_data_free(RowData *rd) {
    if (rd) {
        g_clear_object(&rd->art_cancellable);
        g_free(rd);
    }
}

static const char *VIEW_DATA_KEY = "library-view-data";

/* Info button click handler */
static void on_info_btn_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    ViewData *vd = g_object_get_data(G_OBJECT(button), "view-data");
    if (!vd || !vd->cbs.on_track_info) return;

    gint track_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "track-id"));
    if (track_id > 0) {
        vd->cbs.on_track_info((int64_t)track_id, vd->cbs.user_data);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Setup/Bind - Unified for all item types
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Load row template from resource
 * Note: For full optimization, consider converting to composite widgets which
 * parse templates ONCE at class registration. For now, we parse each time. */
static GtkWidget *load_row_template(const char *resource_path) {
    GtkBuilder *builder = gtk_builder_new_from_resource(resource_path);
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);
    g_object_unref(builder);
    return row;
}

static void row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;

    /* Create row data for cancellation tracking */
    RowData *rd = g_new0(RowData, 1);
    rd->art_cancellable = g_cancellable_new();
    g_object_set_data_full(G_OBJECT(li), "row-data", rd, (GDestroyNotify)row_data_free);

    /* Songs table view (main view only) - load from template */
    if (!vd->is_detail && vd->kind == LIBRARY_ITEM_TRACK) {
        GtkWidget *row = load_row_template("/org/quadrature/ui/song_list_view.ui");
        gtk_list_item_set_child(li, row);
        return;
    }

    /* Artist rows - load from template */
    if (vd->kind == LIBRARY_ITEM_ARTIST) {
        GtkWidget *row = load_row_template("/org/quadrature/ui/library_artist_row.ui");
        gtk_list_item_set_child(li, row);
        return;
    }

    /* Album rows (main view) - load from template */
    if (vd->kind == LIBRARY_ITEM_ALBUM && !vd->is_detail) {
        GtkWidget *row = load_row_template("/org/quadrature/ui/library_album_row.ui");
        gtk_list_item_set_child(li, row);
        return;
    }

    /* Detail track rows - load from template */
    if (vd->is_detail && vd->kind == LIBRARY_ITEM_TRACK) {
        GtkWidget *row = load_row_template("/org/quadrature/ui/library_track_row.ui");

        /* Find and connect the info button */
        GtkWidget *child = gtk_widget_get_first_child(row);
        while (child) {
            if (GTK_IS_BUTTON(child)) {
                g_signal_connect(child, "clicked",
                                 G_CALLBACK(on_info_btn_clicked), NULL);
                break;
            }
            child = gtk_widget_get_next_sibling(child);
        }

        gtk_list_item_set_child(li, row);
        return;
    }

    /* Fallback: album detail rows (albums shown in artist detail) - load from template */
    GtkWidget *row = load_row_template("/org/quadrature/ui/library_album_row.ui");
    gtk_list_item_set_child(li, row);
}

static void row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    (void)data;

    /* Clear the library-item reference to avoid stale pointers */
    GtkWidget *box = gtk_list_item_get_child(li);
    if (box) {
        g_object_set_data(G_OBJECT(box), "library-item", NULL);
    }

    RowData *rd = g_object_get_data(G_OBJECT(li), "row-data");
    if (rd && rd->art_cancellable) {
        g_cancellable_cancel(rd->art_cancellable);
        g_clear_object(&rd->art_cancellable);
        rd->art_cancellable = g_cancellable_new();
    }
}

static void row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    GtkWidget *box = gtk_list_item_get_child(li);
    LibraryItem *item = gtk_list_item_get_item(li);
    if (!item) return;

    /* Store item reference on the row widget for right-click lookup */
    g_object_set_data(G_OBJECT(box), "library-item", item);

    /* Get row data for async artwork loading */
    RowData *rd = g_object_get_data(G_OBJECT(li), "row-data");

    /* Songs table view (main view only) */
    if (!vd->is_detail && vd->kind == LIBRARY_ITEM_TRACK) {
        GtkWidget *child = gtk_widget_get_first_child(box);

        /* Art (child 0) */
        GtkWidget *art_img = child;
        child = gtk_widget_get_next_sibling(child);

        /* Title (child 1) */
        GtkWidget *title_lbl = child;
        child = gtk_widget_get_next_sibling(child);

        /* Album (child 2) */
        GtkWidget *album_lbl = child;
        child = gtk_widget_get_next_sibling(child);

        /* Artist (child 3) */
        GtkWidget *artist_lbl = child;
        child = gtk_widget_get_next_sibling(child);

        /* Year (child 4) */
        GtkWidget *year_lbl = child;
        child = gtk_widget_get_next_sibling(child);

        /* Duration (child 5) */
        GtkWidget *dur_lbl = child;
        child = gtk_widget_get_next_sibling(child);

        /* Track# (child 6) */
        GtkWidget *track_lbl = child;
        child = gtk_widget_get_next_sibling(child);

        /* Disc# (child 7) */
        GtkWidget *disc_lbl = child;

        if (item->placeholder) {
            gtk_image_set_from_icon_name(GTK_IMAGE(art_img), "media-optical-symbolic");
            gtk_label_set_text(GTK_LABEL(title_lbl), "");
            gtk_label_set_text(GTK_LABEL(album_lbl), "");
            gtk_label_set_text(GTK_LABEL(artist_lbl), "");
            gtk_label_set_text(GTK_LABEL(year_lbl), "");
            gtk_label_set_text(GTK_LABEL(dur_lbl), "");
            gtk_label_set_text(GTK_LABEL(track_lbl), "");
            gtk_label_set_text(GTK_LABEL(disc_lbl), "");
            return;
        }

        /* Populate columns */
        gtk_label_set_text(GTK_LABEL(title_lbl), item->name ? item->name : "");
        gtk_label_set_text(GTK_LABEL(album_lbl), item->tertiary ? item->tertiary : "");
        gtk_label_set_text(GTK_LABEL(artist_lbl), item->secondary ? item->secondary : "");

        char buf[32];
        if (item->year > 0) {
            snprintf(buf, sizeof(buf), "%u", item->year);
            gtk_label_set_text(GTK_LABEL(year_lbl), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(year_lbl), "");
        }

        ui_format_duration(item->duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(dur_lbl), buf);

        snprintf(buf, sizeof(buf), "%u", item->track_num);
        gtk_label_set_text(GTK_LABEL(track_lbl), buf);

        snprintf(buf, sizeof(buf), "%u", item->disc_num);
        gtk_label_set_text(GTK_LABEL(disc_lbl), buf);

        /* Album art - async loading from atlas */
        if (vd->art_mgr) {
            artwork_manager_load_thumb_into(vd->art_mgr, item->parent_id,
                                             LOAD_PRIORITY_VISIBLE, art_img,
                                             rd ? rd->art_cancellable : NULL);
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(art_img), "media-optical-symbolic");
        }
        return;
    }

    GtkWidget *child = gtk_widget_get_first_child(box);
    GtkWidget *art = NULL, *track_num = NULL, *text, *title, *sub = NULL, *right = NULL;

    /* Navigate children based on view type */
    if (vd->kind == LIBRARY_ITEM_ALBUM) {
        art = child;
        child = gtk_widget_get_next_sibling(art);
    }
    if (vd->is_detail && vd->kind == LIBRARY_ITEM_TRACK) {
        track_num = child;
        child = gtk_widget_get_next_sibling(track_num);
    }

    text = child;
    title = gtk_widget_get_first_child(text);
    if (!(vd->is_detail && vd->kind == LIBRARY_ITEM_TRACK))
        sub = gtk_widget_get_next_sibling(title);

    right = gtk_widget_get_next_sibling(text);

    /* Handle disc headers */
    if (item->is_disc_header) {
        gtk_widget_add_css_class(box, "disc-header");
        gtk_label_set_text(GTK_LABEL(title), item->name);
        if (track_num) gtk_label_set_text(GTK_LABEL(track_num), "");
        if (sub) gtk_label_set_text(GTK_LABEL(sub), "");
        if (right && GTK_IS_LABEL(right)) gtk_label_set_text(GTK_LABEL(right), "");
        return;
    }

    /* Remove disc-header class if present from recycled row */
    gtk_widget_remove_css_class(box, "disc-header");

    /* Set content */
    gtk_label_set_text(GTK_LABEL(title), item->name);

    if (item->placeholder) {
        if (sub) gtk_label_set_text(GTK_LABEL(sub), "");
        if (right && GTK_IS_LABEL(right)) gtk_label_set_text(GTK_LABEL(right), "");
        if (art) gtk_image_set_from_icon_name(GTK_IMAGE(art), "media-optical-symbolic");
        return;
    }

    char buf[256];

    switch (item->kind) {
    case LIBRARY_ITEM_ARTIST: {
        snprintf(buf, sizeof(buf), "%zu album%s, %zu track%s",
                 item->count1, item->count1 == 1 ? "" : "s",
                 item->count2, item->count2 == 1 ? "" : "s");
        if (sub) gtk_label_set_text(GTK_LABEL(sub), buf);

        /* Populate art strip with album thumbnails */
        GtkWidget *art_strip = right;  /* art_strip is after text for artists */
        if (art_strip && GTK_IS_BOX(art_strip) && vd->db && vd->art_mgr) {
            /* Clear existing children */
            GtkWidget *c;
            while ((c = gtk_widget_get_first_child(art_strip)) != NULL)
                gtk_box_remove(GTK_BOX(art_strip), c);

            /* Try cache first to avoid DB query on every row bind */
            int64_t *cached_album_ids = NULL;
            size_t cached_count = 0;
            gboolean cache_hit = artwork_manager_get_artist_albums(vd->art_mgr, item->id,
                                                                    &cached_album_ids, &cached_count);

            if (cache_hit && cached_count > 0) {
                /* Use cached album IDs - just need to load artwork */
                for (size_t i = 0; i < cached_count; i++) {
                    GtkWidget *img = gtk_image_new_from_icon_name("media-optical-symbolic");
                    gtk_widget_add_css_class(img, "album-art-strip-thumb");
                    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
                    gtk_box_append(GTK_BOX(art_strip), img);

                    artwork_manager_load_thumb_into(vd->art_mgr, cached_album_ids[i],
                                                     LOAD_PRIORITY_VISIBLE, img,
                                                     rd ? rd->art_cancellable : NULL);
                }
                g_free(cached_album_ids);
            } else {
                /* Cache miss - query DB and populate cache */
                db_album_t *albums = NULL;
                size_t album_count = 0;
                if (db_get_albums_by_artist(vd->db, item->id, &albums, &album_count) == QUADRATURE_OK) {
                    size_t show_count = album_count > 6 ? 6 : album_count;

                    /* Cache the album IDs for future binds */
                    int64_t *album_ids = g_new(int64_t, show_count);
                    for (size_t i = 0; i < show_count; i++) {
                        album_ids[i] = albums[i].id;
                    }
                    artwork_manager_put_artist_albums(vd->art_mgr, item->id, album_ids, show_count);
                    g_free(album_ids);

                    /* Create image widgets and load artwork */
                    for (size_t i = 0; i < show_count; i++) {
                        GtkWidget *img = gtk_image_new_from_icon_name("media-optical-symbolic");
                        gtk_widget_add_css_class(img, "album-art-strip-thumb");
                        gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
                        gtk_box_append(GTK_BOX(art_strip), img);

                        artwork_manager_load_thumb_into(vd->art_mgr, albums[i].id,
                                                         LOAD_PRIORITY_VISIBLE, img,
                                                         rd ? rd->art_cancellable : NULL);
                    }
                    db_albums_free(albums, album_count);
                }
            }
        }
        break;
    }

    case LIBRARY_ITEM_ALBUM:
        if (item->year > 0)
            snprintf(buf, sizeof(buf), "%s \u2022 %u", item->secondary, item->year);
        else
            snprintf(buf, sizeof(buf), "%s", item->secondary);
        if (sub) gtk_label_set_text(GTK_LABEL(sub), buf);

        if (right && GTK_IS_LABEL(right)) {
            snprintf(buf, sizeof(buf), "%zu", item->count1);
            gtk_label_set_text(GTK_LABEL(right), buf);
        }

        if (art) {
            /* Async artwork loading from atlas */
            if (vd->art_mgr) {
                artwork_manager_load_thumb_into(vd->art_mgr, item->id,
                                                 LOAD_PRIORITY_VISIBLE, art,
                                                 rd ? rd->art_cancellable : NULL);
            } else {
                gtk_image_set_from_icon_name(GTK_IMAGE(art), "media-optical-symbolic");
            }
        }
        break;

    case LIBRARY_ITEM_TRACK:
        if (track_num) {
            snprintf(buf, sizeof(buf), "%u", item->track_num);
            gtk_label_set_text(GTK_LABEL(track_num), buf);
        }
        if (sub) {
            snprintf(buf, sizeof(buf), "%s \u2014 %s", item->secondary, item->tertiary);
            gtk_label_set_text(GTK_LABEL(sub), buf);
        }
        if (right && GTK_IS_LABEL(right)) {
            ui_format_duration(item->duration_ms, buf, sizeof(buf));
            gtk_label_set_text(GTK_LABEL(right), buf);
        }
        /* Info button (after duration in detail track rows) */
        if (vd->is_detail && right) {
            GtkWidget *info_btn = gtk_widget_get_next_sibling(right);
            if (info_btn && GTK_IS_BUTTON(info_btn)) {
                /* Store track ID on the button for the callback */
                g_object_set_data(G_OBJECT(info_btn), "track-id",
                                  GINT_TO_POINTER((gint)item->id));
                g_object_set_data(G_OBJECT(info_btn), "view-data", vd);
            }
        }
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Activation / Scroll Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_activated(GtkListView *lv, guint pos, gpointer data) {
    (void)lv;
    ViewData *vd = data;

    GListModel *m = vd->model ? G_LIST_MODEL(vd->model) : G_LIST_MODEL(vd->store);
    LibraryItem *item = g_list_model_get_item(m, pos);
    if (!item || item->placeholder || item->is_disc_header) {
        g_clear_object(&item);
        return;
    }

    /* Double-click navigates to albums/artists, but does NOT queue tracks.
     * Tracks are queued via right-click only. */
    if (item->kind != LIBRARY_ITEM_TRACK && vd->cbs.on_navigate) {
        vd->cbs.on_navigate(item->kind, item->id, vd->cbs.user_data);
    }

    g_object_unref(item);
}

/**
 * Right-click handler for queuing tracks to focused channel.
 * Finds the item under the cursor and queues it directly.
 */
static void on_list_right_click(GtkGestureClick *gesture, int n_press,
                                 double x, double y, gpointer data) {
    (void)n_press;
    ViewData *vd = data;

    /* Get the list view from the gesture */
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!GTK_IS_LIST_VIEW(widget)) return;

    /* Use pick to find the widget at the click position */
    GtkWidget *picked = gtk_widget_pick(widget, x, y, GTK_PICK_DEFAULT);
    if (!picked) return;

    /* Walk up the widget tree to find a widget with row-data containing a LibraryItem */
    GtkWidget *row = picked;
    LibraryItem *item = NULL;
    while (row) {
        /* Check if this widget has our row data */
        gpointer row_data = g_object_get_data(G_OBJECT(row), "library-item");
        if (row_data) {
            item = LIBRARY_ITEM(row_data);
            break;
        }
        row = gtk_widget_get_parent(row);
    }

    /* Fallback: try to get from selection if row lookup failed */
    if (!item) {
        GtkListView *lv = GTK_LIST_VIEW(widget);
        GtkSelectionModel *sel = gtk_list_view_get_model(lv);
        if (sel && GTK_IS_SINGLE_SELECTION(sel)) {
            guint pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(sel));
            if (pos != GTK_INVALID_LIST_POSITION) {
                GListModel *m = vd->model ? G_LIST_MODEL(vd->model) : G_LIST_MODEL(vd->store);
                item = g_list_model_get_item(m, pos);
                if (item) {
                    /* We got a ref, need to unref later */
                    if (item->placeholder || item->is_disc_header) {
                        g_object_unref(item);
                        return;
                    }
                    if (item->kind == LIBRARY_ITEM_TRACK && vd->cbs.on_play) {
                        vd->cbs.on_play(item->path, item->name, item->secondary, item->tertiary,
                                        item->id, vd->cbs.user_data);
                    }
                    g_object_unref(item);
                    return;
                }
            }
        }
        return;
    }

    if (item->placeholder || item->is_disc_header) return;

    /* Only queue tracks, not albums/artists */
    if (item->kind == LIBRARY_ITEM_TRACK && vd->cbs.on_play) {
        vd->cbs.on_play(item->path, item->name, item->secondary, item->tertiary,
                        item->id, vd->cbs.user_data);
    }
}

static void on_scroll_changed(GtkAdjustment *adj, gpointer data) {
    ViewData *vd = data;
    if (!vd->model) return;

    double val = gtk_adjustment_get_value(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double page = gtk_adjustment_get_page_size(adj);

    if (upper > 0 && (upper - val - page) < (upper * 0.2)) {
        size_t visible_end = (size_t)((val + page) / 60.0);
        size_t next = ((visible_end / LIBRARY_PAGE_SIZE) + 1) * LIBRARY_PAGE_SIZE;
        library_model_prefetch(vd->model, next);
    }
}

static void on_items_changed(GListModel *m, guint pos, guint rm, guint add, gpointer data) {
    (void)pos; (void)rm; (void)add;
    ViewData *vd = data;
    if (!vd->subtitle) return;

    guint total = g_list_model_get_n_items(m);
    char buf[32];
    const char *unit = (vd->kind == LIBRARY_ITEM_ARTIST) ? "artist" :
                       (vd->kind == LIBRARY_ITEM_ALBUM) ? "album" : "song";
    snprintf(buf, sizeof(buf), "%u %s%s", total, unit, total == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sort Button Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_sort_button_states(ViewData *vd) {
    static const db_sort_t album_sorts[] = {
        DB_SORT_NAME_ASC, DB_SORT_YEAR_DESC, DB_SORT_ARTIST_ASC, DB_SORT_ADDED_DESC
    };
    static const db_sort_t track_sorts[] = {
        DB_SORT_NAME_ASC, DB_SORT_YEAR_DESC, DB_SORT_ARTIST_ASC, DB_SORT_ADDED_DESC
    };

    const db_sort_t *sorts = (vd->kind == LIBRARY_ITEM_ALBUM) ? album_sorts : track_sorts;

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

    static const db_sort_t album_sorts[] = {
        DB_SORT_NAME_ASC, DB_SORT_YEAR_DESC, DB_SORT_ARTIST_ASC, DB_SORT_ADDED_DESC
    };
    static const db_sort_t track_sorts[] = {
        DB_SORT_NAME_ASC, DB_SORT_YEAR_DESC, DB_SORT_ARTIST_ASC, DB_SORT_ADDED_DESC
    };

    const db_sort_t *sorts = (vd->kind == LIBRARY_ITEM_ALBUM) ? album_sorts : track_sorts;

    for (int i = 0; i < 4; i++) {
        if (GTK_WIDGET(btn) == vd->sort_buttons[i]) {
            vd->current_sort = sorts[i];
            break;
        }
    }

    update_sort_button_states(vd);

    /* Refresh the model with new sort */
    if (vd->model) {
        library_model_set_sort(vd->model, vd->current_sort);
    }
}

/* Setup sort buttons from template widgets */
static void setup_sort_buttons(ViewData *vd, GtkBuilder *builder) {
    const char *ids[] = {"sort_title", "sort_year", "sort_artist", "sort_added"};

    for (int i = 0; i < 4; i++) {
        vd->sort_buttons[i] = GTK_WIDGET(gtk_builder_get_object(builder, ids[i]));
        if (vd->sort_buttons[i]) {
            g_signal_connect(vd->sort_buttons[i], "clicked", G_CALLBACK(on_sort_clicked), vd);
        }
    }

    /* Default to Artist sort for albums */
    vd->current_sort = (vd->kind == LIBRARY_ITEM_ALBUM) ? DB_SORT_ARTIST_ASC : DB_SORT_NAME_ASC;
    update_sort_button_states(vd);
}

/* Column header definitions for songs table view */
typedef struct {
    const char *label;
    int width;       /* -1 for flexible */
    db_sort_t sort;  /* Sort to apply when clicked */
} SongColumnDef;

static const SongColumnDef SONG_COLUMNS[] = {
    {"",       40,  DB_SORT_NAME_ASC},     /* Art (not sortable) */
    {"TITLE",  -1,  DB_SORT_NAME_ASC},     /* flex */
    {"ALBUM",  -1,  DB_SORT_ALBUM_ASC},    /* flex */
    {"ARTIST", -1,  DB_SORT_ARTIST_ASC},   /* flex */
    {"YEAR",   48,  DB_SORT_YEAR_DESC},
    {"DUR",    48,  DB_SORT_DURATION_ASC},
    {"#",      32,  DB_SORT_TRACK_NUM_ASC},
    {"DISC",   32,  DB_SORT_DISC_NUM_ASC},
};
#define NUM_SONG_COLUMNS (sizeof(SONG_COLUMNS) / sizeof(SONG_COLUMNS[0]))

static void on_column_header_clicked(GtkButton *btn, gpointer data) {
    ViewData *vd = data;
    const char *label = gtk_button_get_label(btn);
    if (!label || label[0] == '\0') return;  /* Skip art column */

    for (size_t i = 0; i < NUM_SONG_COLUMNS; i++) {
        if (g_strcmp0(label, SONG_COLUMNS[i].label) == 0) {
            vd->current_sort = SONG_COLUMNS[i].sort;
            if (vd->model)
                library_model_set_sort(vd->model, SONG_COLUMNS[i].sort);
            break;
        }
    }
}

static GtkWidget *create_song_column_headers(ViewData *vd) {
    /* Load column headers from template - margin-8 class applied in template */
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/song_list_view.ui");
    GtkWidget *header_box = GTK_WIDGET(gtk_builder_get_object(builder, "column_headers"));
    g_object_ref(header_box);

    /* Connect click handlers for sortable columns */
    const char *col_ids[] = {"col_title", "col_album", "col_artist", "col_year",
                              "col_duration", "col_track", "col_disc"};
    for (size_t i = 0; i < sizeof(col_ids) / sizeof(col_ids[0]); i++) {
        GtkWidget *btn = GTK_WIDGET(gtk_builder_get_object(builder, col_ids[i]));
        if (btn) {
            g_signal_connect(btn, "clicked", G_CALLBACK(on_column_header_clicked), vd);
        }
    }

    g_object_unref(builder);
    return header_box;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main View Constructor
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *library_view_new(LibraryItemKind kind, quadrature_db_t *db,
                             LibraryCache *cache, ArtworkManager *art_mgr,
                             const LibraryCallbacks *cbs) {
    /* Load container from template */
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_list_view.ui");
    GtkWidget *box = GTK_WIDGET(gtk_builder_get_object(builder, "container"));
    g_object_ref(box);

    ViewData *vd = g_new0(ViewData, 1);
    vd->kind = kind;
    vd->db = db;
    vd->cache = cache;
    vd->art_mgr = art_mgr;
    if (cbs) vd->cbs = *cbs;
    vd->container = box;
    g_object_set_data_full(G_OBJECT(box), VIEW_DATA_KEY, vd, g_free);

    /* Get template widgets */
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    vd->subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    GtkWidget *sort_buttons = GTK_WIDGET(gtk_builder_get_object(builder, "sort_buttons"));
    vd->scroll = GTK_WIDGET(gtk_builder_get_object(builder, "scroll"));

    /* Set title based on view type */
    const char *titles[] = {"Artists", "Albums", "Songs"};
    gtk_label_set_text(GTK_LABEL(title), titles[kind]);

    /* Show sort buttons for albums view, setup click handlers */
    if (kind == LIBRARY_ITEM_ALBUM) {
        gtk_widget_set_visible(sort_buttons, TRUE);
        setup_sort_buttons(vd, builder);
    }

    /* Add column headers for songs table view (inserted before scroll) */
    if (kind == LIBRARY_ITEM_TRACK) {
        GtkWidget *headers = create_song_column_headers(vd);
        /* Insert before scroll window */
        gtk_box_insert_child_after(GTK_BOX(box), headers, sort_buttons);
    }

    g_object_unref(builder);

    /* Create model and list view */
    vd->model = library_model_new(kind, db, cache);
    g_signal_connect(vd->model, "items-changed", G_CALLBACK(on_items_changed), vd);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(row_setup), vd);
    g_signal_connect(factory, "bind", G_CALLBACK(row_bind), vd);
    g_signal_connect(factory, "unbind", G_CALLBACK(row_unbind), vd);

    GtkSelectionModel *sel = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(vd->model)));

    vd->list_view = gtk_list_view_new(sel, factory);
    gtk_widget_add_css_class(vd->list_view, "library-list");
    g_signal_connect(vd->list_view, "activate", G_CALLBACK(on_activated), vd);

    /* Right-click gesture for queuing tracks */
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_list_right_click), vd);
    gtk_widget_add_controller(vd->list_view, GTK_EVENT_CONTROLLER(right_click));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vd->scroll), vd->list_view);

    GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vd->list_view));
    g_signal_connect(vadj, "value-changed", G_CALLBACK(on_scroll_changed), vd);

    return box;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Detail View - Artist or Album
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_back_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    ViewData *vd = data;
    if (vd->cbs.on_back)
        vd->cbs.on_back(vd->cbs.user_data);
}

static void on_artist_link_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    ViewData *vd = data;
    if (vd->detail_artist_id > 0 && vd->cbs.on_navigate)
        vd->cbs.on_navigate(LIBRARY_ITEM_ARTIST, vd->detail_artist_id, vd->cbs.user_data);
}

static void load_artist_detail(ViewData *vd) {
    int64_t start = g_get_monotonic_time();

    g_list_store_remove_all(vd->store);
    if (vd->detail_id <= 0) return;

    GPtrArray *items = NULL;
    db_album_t *albums = NULL;
    size_t count = 0;

    /* Try cache first */
    if (library_cache_get_detail(vd->cache, LIBRARY_ITEM_ALBUM, vd->detail_id, &items)) {
        if (items->len > 0) {
            LibraryItem *first = g_ptr_array_index(items, 0);
            gtk_label_set_text(GTK_LABEL(vd->header_title), first->secondary);

            size_t tracks = 0;
            for (guint i = 0; i < items->len; i++) {
                LibraryItem *it = g_ptr_array_index(items, i);
                tracks += it->count1;
                g_list_store_append(vd->store, it);
            }

            char buf[64];
            snprintf(buf, sizeof(buf), "%u album%s, %zu track%s",
                     items->len, items->len == 1 ? "" : "s",
                     tracks, tracks == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->header_info), buf);
        }

        double elapsed_ms = (g_get_monotonic_time() - start) / 1000.0;
        g_info("Artist detail: id=%" G_GINT64_FORMAT " albums=%u time=%.2fms (cached)",
               vd->detail_id, items->len, elapsed_ms);

        g_ptr_array_unref(items);
        return;
    }

    /* Fetch from DB */
    if (db_get_albums_by_artist(vd->db, vd->detail_id, &albums, &count) != QUADRATURE_OK || count == 0)
        return;

    items = g_ptr_array_new_with_free_func(g_object_unref);
    size_t tracks = 0;

    gtk_label_set_text(GTK_LABEL(vd->header_title), albums[0].artist_name);

    for (size_t i = 0; i < count; i++) {
        LibraryItem *it = library_item_new_album(&albums[i]);
        g_ptr_array_add(items, it);
        tracks += albums[i].track_count;
        g_list_store_append(vd->store, it);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%zu album%s, %zu track%s",
             count, count == 1 ? "" : "s", tracks, tracks == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(vd->header_info), buf);

    library_cache_put_detail(vd->cache, LIBRARY_ITEM_ALBUM, vd->detail_id, items);

    double elapsed_ms = (g_get_monotonic_time() - start) / 1000.0;
    g_info("Artist detail: id=%" G_GINT64_FORMAT " albums=%zu time=%.2fms",
           vd->detail_id, count, elapsed_ms);

    g_ptr_array_unref(items);
    db_albums_free(albums, count);
}

static void load_album_detail(ViewData *vd) {
    int64_t start = g_get_monotonic_time();

    g_info("load_album_detail: ENTER detail_id=%" G_GINT64_FORMAT " db=%p",
           vd->detail_id, (void*)vd->db);

    g_list_store_remove_all(vd->store);
    if (vd->detail_id <= 0) {
        g_warning("load_album_detail: invalid detail_id=%" G_GINT64_FORMAT, vd->detail_id);
        return;
    }

    GPtrArray *items = NULL;
    db_track_t *tracks = NULL;
    size_t count = 0;

    /* Try cache */
    if (library_cache_get_detail(vd->cache, LIBRARY_ITEM_TRACK, vd->detail_id, &items)) {
        if (items->len > 0) {
            LibraryItem *first = g_ptr_array_index(items, 0);
            /* Find first non-header item for metadata */
            for (guint i = 0; i < items->len; i++) {
                LibraryItem *it = g_ptr_array_index(items, i);
                if (!it->is_disc_header) {
                    gtk_label_set_text(GTK_LABEL(vd->header_title), it->tertiary);
                    vd->detail_artist_id = it->artist_id;
                    break;
                }
            }
            /* Update artist link button if present */
            if (vd->header_artist_btn && !first->is_disc_header) {
                gtk_button_set_label(GTK_BUTTON(vd->header_artist_btn), first->secondary);
            }

            /* Header art loaded via atlas thumbnail for album detail */
            if (vd->header_art && vd->art_mgr) {
                artwork_manager_load_thumb_into(vd->art_mgr, vd->detail_id,
                                                 LOAD_PRIORITY_VISIBLE,
                                                 vd->header_art, NULL);
            }

            for (guint i = 0; i < items->len; i++)
                g_list_store_append(vd->store, g_ptr_array_index(items, i));

            /* Notify for audio preloading (cached) */
            if (vd->cbs.on_album_loaded) {
                size_t track_count = 0;
                for (guint i = 0; i < items->len; i++) {
                    LibraryItem *it = g_ptr_array_index(items, i);
                    if (!it->is_disc_header) track_count++;
                }
                if (track_count > 0) {
                    LibraryTrackInfo *track_info = g_new(LibraryTrackInfo, track_count);
                    size_t j = 0;
                    for (guint i = 0; i < items->len && j < track_count; i++) {
                        LibraryItem *it = g_ptr_array_index(items, i);
                        if (!it->is_disc_header) {
                            track_info[j].track_id = it->id;
                            track_info[j].path = it->path;
                            j++;
                        }
                    }
                    vd->cbs.on_album_loaded(vd->detail_id, track_info, track_count, vd->cbs.user_data);
                    g_free(track_info);
                }
            }
        }

        double elapsed_ms = (g_get_monotonic_time() - start) / 1000.0;
        g_info("Album detail: id=%" G_GINT64_FORMAT " tracks=%u time=%.2fms (cached)",
               vd->detail_id, items->len, elapsed_ms);

        g_ptr_array_unref(items);
        return;
    }

    /* Fetch from DB */
    quadrature_result_t res = db_get_tracks_by_album(vd->db, vd->detail_id, &tracks, &count);
    g_info("load_album_detail: db_get_tracks_by_album returned res=%d count=%zu", res, count);
    if (res != QUADRATURE_OK) {
        g_warning("load_album_detail: query FAILED res=%d for album_id=%" G_GINT64_FORMAT,
                  res, vd->detail_id);
        return;
    }
    if (count == 0) {
        g_warning("load_album_detail: NO TRACKS for album_id=%" G_GINT64_FORMAT, vd->detail_id);
        return;
    }

    items = g_ptr_array_new_with_free_func(g_object_unref);

    gtk_label_set_text(GTK_LABEL(vd->header_title), tracks[0].album);

    /* Store artist_id for navigation and set artist link */
    vd->detail_artist_id = tracks[0].artist_id;
    if (vd->header_artist_btn) {
        gtk_button_set_label(GTK_BUTTON(vd->header_artist_btn), tracks[0].artist);
    } else {
        gtk_label_set_text(GTK_LABEL(vd->header_info), tracks[0].artist);
    }

    /* Header art loaded via atlas thumbnail */
    if (vd->header_art && vd->art_mgr) {
        artwork_manager_load_thumb_into(vd->art_mgr, vd->detail_id,
                                         LOAD_PRIORITY_VISIBLE,
                                         vd->header_art, NULL);
    }

    /* Detect if album has multiple discs */
    uint16_t max_disc = 1;
    for (size_t i = 0; i < count; i++) {
        if (tracks[i].disc_num > max_disc)
            max_disc = tracks[i].disc_num;
    }
    gboolean multi_disc = (max_disc > 1);

    uint16_t current_disc = 0;
    for (size_t i = 0; i < count; i++) {
        /* Insert disc header when disc changes (multi-disc only) */
        if (multi_disc && tracks[i].disc_num != current_disc) {
            current_disc = tracks[i].disc_num;
            LibraryItem *header = library_item_new_disc_header(current_disc);
            g_ptr_array_add(items, header);
            g_list_store_append(vd->store, header);
        }

        LibraryItem *it = library_item_new_track(&tracks[i]);
        g_ptr_array_add(items, it);
        g_list_store_append(vd->store, it);
    }

    library_cache_put_detail(vd->cache, LIBRARY_ITEM_TRACK, vd->detail_id, items);

    double elapsed_ms = (g_get_monotonic_time() - start) / 1000.0;
    g_info("Album detail: id=%" G_GINT64_FORMAT " tracks=%zu time=%.2fms",
           vd->detail_id, count, elapsed_ms);

    /* Notify for audio preloading */
    if (vd->cbs.on_album_loaded && count > 0) {
        LibraryTrackInfo *track_info = g_new(LibraryTrackInfo, count);
        for (size_t i = 0; i < count; i++) {
            track_info[i].track_id = tracks[i].id;
            track_info[i].path = tracks[i].path;
        }
        vd->cbs.on_album_loaded(vd->detail_id, track_info, count, vd->cbs.user_data);
        g_free(track_info);
    }

    g_ptr_array_unref(items);
    db_tracks_free(tracks, count);
}

GtkWidget *library_detail_view_new(LibraryItemKind kind, quadrature_db_t *db,
                                    LibraryCache *cache, ArtworkManager *art_mgr,
                                    const LibraryCallbacks *cbs) {
    /* kind determines what we show: ARTIST shows albums, ALBUM shows tracks */
    LibraryItemKind list_kind = (kind == LIBRARY_ITEM_ARTIST) ? LIBRARY_ITEM_ALBUM : LIBRARY_ITEM_TRACK;

    /* Load container from template */
    GtkBuilder *container_builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_detail_container.ui");
    GtkWidget *box = GTK_WIDGET(gtk_builder_get_object(container_builder, "container"));
    g_object_ref(box);

    ViewData *vd = g_new0(ViewData, 1);
    vd->kind = list_kind;
    vd->is_detail = TRUE;
    vd->db = db;
    vd->cache = cache;
    vd->art_mgr = art_mgr;
    if (cbs) vd->cbs = *cbs;
    vd->container = box;
    g_object_set_data_full(G_OBJECT(box), VIEW_DATA_KEY, vd, g_free);

    /* Get container widgets */
    GtkWidget *section_label = GTK_WIDGET(gtk_builder_get_object(container_builder, "section_label"));
    vd->scroll = GTK_WIDGET(gtk_builder_get_object(container_builder, "scroll"));

    /* Set section label */
    gtk_label_set_text(GTK_LABEL(section_label), list_kind == LIBRARY_ITEM_ALBUM ? "Albums" : "Tracks");

    /* Load header from template */
    GtkBuilder *header_builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_detail_header.ui");
    GtkWidget *header = GTK_WIDGET(gtk_builder_get_object(header_builder, "detail_header"));
    g_object_ref(header);

    /* Get header widgets */
    GtkWidget *back_btn = GTK_WIDGET(gtk_builder_get_object(header_builder, "back_button"));
    vd->header_art = GTK_WIDGET(gtk_builder_get_object(header_builder, "album_art"));
    vd->header_title = GTK_WIDGET(gtk_builder_get_object(header_builder, "title"));
    vd->header_artist_btn = GTK_WIDGET(gtk_builder_get_object(header_builder, "artist_link"));
    vd->header_info = GTK_WIDGET(gtk_builder_get_object(header_builder, "info_label"));

    /* Connect back button */
    g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back_clicked), vd);

    /* Configure for album vs artist detail */
    if (kind == LIBRARY_ITEM_ALBUM) {
        gtk_widget_set_visible(vd->header_art, TRUE);
        gtk_widget_set_visible(vd->header_artist_btn, TRUE);
        g_signal_connect(vd->header_artist_btn, "clicked", G_CALLBACK(on_artist_link_clicked), vd);
    } else {
        gtk_widget_set_visible(vd->header_info, TRUE);
    }

    /* Insert header at the beginning of container */
    gtk_box_prepend(GTK_BOX(box), header);

    g_object_unref(header_builder);
    g_object_unref(container_builder);

    /* Create list */
    vd->store = g_list_store_new(LIBRARY_TYPE_ITEM);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(row_setup), vd);
    g_signal_connect(factory, "bind", G_CALLBACK(row_bind), vd);
    g_signal_connect(factory, "unbind", G_CALLBACK(row_unbind), vd);

    GtkSelectionModel *sel = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(vd->store)));

    vd->list_view = gtk_list_view_new(sel, factory);
    gtk_widget_add_css_class(vd->list_view, "library-list");
    g_signal_connect(vd->list_view, "activate", G_CALLBACK(on_activated), vd);

    /* Right-click gesture for queuing tracks */
    GtkGesture *right_click_detail = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_detail), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click_detail, "pressed", G_CALLBACK(on_list_right_click), vd);
    gtk_widget_add_controller(vd->list_view, GTK_EVENT_CONTROLLER(right_click_detail));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vd->scroll), vd->list_view);

    return box;
}

void library_detail_view_set_id(GtkWidget *view, int64_t id) {
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd || !vd->is_detail) return;

    vd->detail_id = id;

    if (vd->kind == LIBRARY_ITEM_ALBUM)
        load_artist_detail(vd);
    else
        load_album_detail(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Refresh
 * ═══════════════════════════════════════════════════════════════════════════ */

void library_view_refresh(GtkWidget *view) {
    if (!view) return;
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd) return;

    if (vd->model)
        library_model_refresh(vd->model);
    else if (vd->is_detail && vd->kind == LIBRARY_ITEM_ALBUM)
        load_artist_detail(vd);
    else if (vd->is_detail)
        load_album_detail(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Unified Detail View - Template-based three-state view
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_NAV_STACK 16
#define PREVIEW_TRACKS 5

typedef struct {
    quadrature_db_t *db;
    LibraryCache *cache;
    ArtworkManager *art_mgr;
    LibraryCallbacks cbs;

    DetailState state;
    int64_t current_id;        /* Current artist/album ID */
    int64_t album_artist_id;   /* For album state: artist to navigate to */

    /* Navigation stack */
    NavEntry nav_stack[MAX_NAV_STACK];
    int nav_depth;

    /* Template widgets (from detail_view.ui) */
    GtkWidget *container;
    GtkWidget *back_header;
    GtkWidget *back_button;
    GtkWidget *back_label;
    GtkWidget *content_stack;

    /* Album state widgets */
    GtkWidget *album_art;
    GtkWidget *album_title;
    GtkWidget *album_artist_link;
    GtkWidget *album_year;
    GtkWidget *album_stats;
    GtkWidget *album_featuring_box;
    GtkWidget *album_featuring_artists;
    GtkWidget *album_tracks_container;

    /* Artist state widgets */
    GtkWidget *artist_name;
    GtkWidget *artist_stats;
    GtkWidget *artist_albums_container;
    GtkWidget *appears_on_section;
    GtkWidget *appears_on_stack;
    GtkWidget *appears_on_albums;
    GtkWidget *appears_on_tracks;
    GtkWidget *toggle_albums_btn;
    GtkWidget *toggle_tracks_btn;
} UnifiedDetailData;

static const char *UNIFIED_DATA_KEY = "unified-detail-data";

static void format_duration_str(uint32_t ms, char *buf, size_t len) {
    uint32_t sec = ms / 1000, min = sec / 60, hr = min / 60;
    if (hr > 0)
        snprintf(buf, len, "%uh %02um", hr, min % 60);
    else
        snprintf(buf, len, "%u:%02u", min, sec % 60);
}

/* Clear all children from a container */
static void clear_container(GtkWidget *container) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)))
        gtk_box_remove(GTK_BOX(container), child);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Navigation Stack
 * ───────────────────────────────────────────────────────────────────────────── */

static void nav_push(UnifiedDetailData *ud, NavEntryType type, int64_t id, const char *view_name) {
    if (ud->nav_depth >= MAX_NAV_STACK) return;
    NavEntry *e = &ud->nav_stack[ud->nav_depth++];
    e->type = type;
    e->id = id;
    e->view_name = view_name ? g_strdup(view_name) : NULL;
    e->scroll_pos = 0;
}

static NavEntry *nav_pop(UnifiedDetailData *ud) {
    if (ud->nav_depth <= 0) return NULL;
    return &ud->nav_stack[--ud->nav_depth];
}

static void nav_clear(UnifiedDetailData *ud) {
    for (int i = 0; i < ud->nav_depth; i++)
        g_free(ud->nav_stack[i].view_name);
    ud->nav_depth = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Album Card Creation (from template)
 * ───────────────────────────────────────────────────────────────────────────── */

static void on_album_card_navigate(GtkButton *btn, gpointer data);
static void on_album_card_see_all(GtkButton *btn, gpointer data);

static GtkWidget *create_album_card(UnifiedDetailData *ud, const db_album_t *album,
                                     const db_track_t *tracks, size_t track_count) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/album_card.ui");

    GtkWidget *card = GTK_WIDGET(gtk_builder_get_object(builder, "album_card"));
    g_object_ref(card);

    /* Get widgets */
    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "card_art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "card_title"));
    GtkWidget *year = GTK_WIDGET(gtk_builder_get_object(builder, "card_year"));
    GtkWidget *stats = GTK_WIDGET(gtk_builder_get_object(builder, "card_stats"));
    GtkWidget *track_list = GTK_WIDGET(gtk_builder_get_object(builder, "track_list"));
    GtkWidget *nav_btn = GTK_WIDGET(gtk_builder_get_object(builder, "card_navigate_btn"));
    GtkWidget *see_all_btn = GTK_WIDGET(gtk_builder_get_object(builder, "card_see_all_btn"));
    GtkWidget *see_all_label = GTK_WIDGET(gtk_builder_get_object(builder, "card_see_all_label"));

    /* Populate metadata */
    gtk_label_set_text(GTK_LABEL(title), album->title);

    char buf[64];
    if (album->year > 0) {
        snprintf(buf, sizeof(buf), "%u", album->year);
        gtk_label_set_text(GTK_LABEL(year), buf);
    } else {
        gtk_widget_set_visible(year, FALSE);
    }

    uint32_t total_ms = 0;
    for (size_t i = 0; i < track_count; i++)
        total_ms += tracks[i].duration_ms;

    char dur_buf[16];
    format_duration_str(total_ms, dur_buf, sizeof(dur_buf));
    snprintf(buf, sizeof(buf), "%zu track%s - %s",
             track_count, track_count == 1 ? "" : "s", dur_buf);
    gtk_label_set_text(GTK_LABEL(stats), buf);

    /* Load album art */
    if (ud->art_mgr) {
        artwork_manager_load_thumb_into(ud->art_mgr, album->id,
                                         LOAD_PRIORITY_VISIBLE, art, NULL);
    }

    /* Add preview tracks (up to PREVIEW_TRACKS) using template */
    size_t preview_count = track_count > PREVIEW_TRACKS ? PREVIEW_TRACKS : track_count;
    for (size_t i = 0; i < preview_count; i++) {
        GtkBuilder *row_builder = gtk_builder_new_from_resource("/org/quadrature/ui/preview_track_row.ui");
        GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(row_builder, "row"));
        GtkWidget *num = GTK_WIDGET(gtk_builder_get_object(row_builder, "track_num"));
        GtkWidget *ttl = GTK_WIDGET(gtk_builder_get_object(row_builder, "track_title"));
        GtkWidget *dur = GTK_WIDGET(gtk_builder_get_object(row_builder, "track_duration"));
        g_object_ref(row);

        /* Populate data */
        snprintf(buf, sizeof(buf), "%u", tracks[i].track_num);
        gtk_label_set_text(GTK_LABEL(num), buf);
        gtk_label_set_text(GTK_LABEL(ttl), tracks[i].title);
        format_duration_str(tracks[i].duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(dur), buf);

        gtk_box_append(GTK_BOX(track_list), row);
        g_object_unref(row_builder);
    }

    /* Show "see all" if more tracks */
    if (track_count > PREVIEW_TRACKS) {
        snprintf(buf, sizeof(buf), "...see all %zu tracks", track_count);
        gtk_label_set_text(GTK_LABEL(see_all_label), buf);
        gtk_widget_set_visible(see_all_btn, TRUE);
        g_object_set_data(G_OBJECT(see_all_btn), "album-id",
                          GSIZE_TO_POINTER((gsize)album->id));
        g_object_set_data(G_OBJECT(see_all_btn), "unified-data", ud);
        g_signal_connect(see_all_btn, "clicked", G_CALLBACK(on_album_card_see_all), ud);
    }

    /* Connect navigate button */
    g_object_set_data(G_OBJECT(nav_btn), "album-id", GSIZE_TO_POINTER((gsize)album->id));
    g_object_set_data(G_OBJECT(nav_btn), "unified-data", ud);
    g_signal_connect(nav_btn, "clicked", G_CALLBACK(on_album_card_navigate), ud);

    g_object_unref(builder);
    return card;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * State Loading
 * ───────────────────────────────────────────────────────────────────────────── */

static void load_album_state(UnifiedDetailData *ud, int64_t album_id);
static void load_artist_state(UnifiedDetailData *ud, int64_t artist_id);

static void load_album_state(UnifiedDetailData *ud, int64_t album_id) {
    ud->state = DETAIL_STATE_ALBUM;
    ud->current_id = album_id;

    /* Fetch tracks */
    db_track_t *tracks = NULL;
    size_t count = 0;
    if (db_get_tracks_by_album(ud->db, album_id, &tracks, &count) != QUADRATURE_OK || count == 0)
        return;

    /* Set album metadata */
    gtk_label_set_text(GTK_LABEL(ud->album_title), tracks[0].album);
    gtk_button_set_label(GTK_BUTTON(ud->album_artist_link), tracks[0].artist);
    ud->album_artist_id = tracks[0].artist_id;

    char buf[64];
    if (tracks[0].year > 0) {
        snprintf(buf, sizeof(buf), "%u", tracks[0].year);
        gtk_label_set_text(GTK_LABEL(ud->album_year), buf);
        gtk_widget_set_visible(ud->album_year, TRUE);
    } else {
        gtk_widget_set_visible(ud->album_year, FALSE);
    }

    uint32_t total_ms = 0;
    for (size_t i = 0; i < count; i++)
        total_ms += tracks[i].duration_ms;

    char dur_buf[16];
    format_duration_str(total_ms, dur_buf, sizeof(dur_buf));
    snprintf(buf, sizeof(buf), "%zu track%s - %s", count, count == 1 ? "" : "s", dur_buf);
    gtk_label_set_text(GTK_LABEL(ud->album_stats), buf);

    /* Load album art */
    if (ud->art_mgr) {
        artwork_manager_load_thumb_into(ud->art_mgr, album_id,
                                         LOAD_PRIORITY_VISIBLE, ud->album_art, NULL);
    }

    /* Hide featuring section for now (could be populated with unique track artists) */
    gtk_widget_set_visible(ud->album_featuring_box, FALSE);

    /* Populate track list */
    clear_container(ud->album_tracks_container);

    uint16_t max_disc = 1;
    for (size_t i = 0; i < count; i++)
        if (tracks[i].disc_num > max_disc) max_disc = tracks[i].disc_num;

    gboolean multi_disc = (max_disc > 1);
    uint16_t current_disc = 0;

    for (size_t i = 0; i < count; i++) {
        /* Add disc header if needed - load from template */
        if (multi_disc && tracks[i].disc_num != current_disc) {
            current_disc = tracks[i].disc_num;
            GtkBuilder *hdr_builder = gtk_builder_new_from_resource("/org/quadrature/ui/disc_header.ui");
            GtkWidget *hdr = GTK_WIDGET(gtk_builder_get_object(hdr_builder, "disc_header"));
            GtkWidget *lbl = GTK_WIDGET(gtk_builder_get_object(hdr_builder, "disc_label"));
            g_object_ref(hdr);

            snprintf(buf, sizeof(buf), "DISC %u", current_disc);
            gtk_label_set_text(GTK_LABEL(lbl), buf);

            gtk_box_append(GTK_BOX(ud->album_tracks_container), hdr);
            g_object_unref(hdr_builder);
        }

        /* Add track row - load from template */
        GtkBuilder *row_builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_track_row.ui");
        GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(row_builder, "row"));
        g_object_ref(row);

        /* Navigate children to find widgets */
        GtkWidget *child = gtk_widget_get_first_child(row);
        GtkWidget *num = child;  /* track_num */
        child = gtk_widget_get_next_sibling(child);
        GtkWidget *text = child;  /* text box */
        GtkWidget *ttl = gtk_widget_get_first_child(text);  /* title */
        child = gtk_widget_get_next_sibling(text);
        GtkWidget *dur = child;  /* duration */

        snprintf(buf, sizeof(buf), "%u", tracks[i].track_num);
        gtk_label_set_text(GTK_LABEL(num), buf);
        gtk_label_set_text(GTK_LABEL(ttl), tracks[i].title);
        format_duration_str(tracks[i].duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(dur), buf);

        gtk_box_append(GTK_BOX(ud->album_tracks_container), row);
        g_object_unref(row_builder);
    }

    /* Switch to album state */
    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "album");
    gtk_widget_set_visible(ud->back_header, TRUE);

    db_tracks_free(tracks, count);
}

static void load_artist_state(UnifiedDetailData *ud, int64_t artist_id) {
    ud->state = DETAIL_STATE_ARTIST;
    ud->current_id = artist_id;

    /* Fetch albums */
    db_album_t *albums = NULL;
    size_t album_count = 0;
    if (db_get_albums_by_artist(ud->db, artist_id, &albums, &album_count) != QUADRATURE_OK || album_count == 0)
        return;

    /* Set artist metadata */
    gtk_label_set_text(GTK_LABEL(ud->artist_name), albums[0].artist_name);

    size_t total_tracks = 0;
    uint32_t total_ms = 0;
    for (size_t i = 0; i < album_count; i++)
        total_tracks += albums[i].track_count;

    /* Get total duration by summing album durations (need to fetch tracks) */
    for (size_t i = 0; i < album_count; i++) {
        db_track_t *tracks = NULL;
        size_t tc = 0;
        if (db_get_tracks_by_album(ud->db, albums[i].id, &tracks, &tc) == QUADRATURE_OK) {
            for (size_t j = 0; j < tc; j++)
                total_ms += tracks[j].duration_ms;
            db_tracks_free(tracks, tc);
        }
    }

    char buf[64], dur_buf[16];
    format_duration_str(total_ms, dur_buf, sizeof(dur_buf));
    snprintf(buf, sizeof(buf), "%zu album%s - %zu track%s - %s",
             album_count, album_count == 1 ? "" : "s",
             total_tracks, total_tracks == 1 ? "" : "s", dur_buf);
    gtk_label_set_text(GTK_LABEL(ud->artist_stats), buf);

    /* Populate album cards */
    clear_container(ud->artist_albums_container);

    for (size_t i = 0; i < album_count; i++) {
        db_track_t *tracks = NULL;
        size_t tc = 0;
        if (db_get_tracks_by_album(ud->db, albums[i].id, &tracks, &tc) == QUADRATURE_OK && tc > 0) {
            GtkWidget *card = create_album_card(ud, &albums[i], tracks, tc);
            gtk_box_append(GTK_BOX(ud->artist_albums_container), card);
            db_tracks_free(tracks, tc);
        }
    }

    /* Hide "Appears On" section for now (requires additional DB queries) */
    gtk_widget_set_visible(ud->appears_on_section, FALSE);

    /* Switch to artist state */
    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "artist");
    gtk_widget_set_visible(ud->back_header, TRUE);

    db_albums_free(albums, album_count);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Click Handlers
 * ───────────────────────────────────────────────────────────────────────────── */

static void on_unified_back_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UnifiedDetailData *ud = data;
    library_unified_detail_go_back(ud->container);
}

static void on_unified_artist_link_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UnifiedDetailData *ud = data;
    if (ud->album_artist_id > 0) {
        library_unified_detail_navigate_to_artist(ud->container, ud->album_artist_id, NULL);
    }
}

static void on_album_card_navigate(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    int64_t album_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "album-id"));
    if (album_id > 0) {
        library_unified_detail_navigate_to_album(ud->container, album_id, NULL);
    }
}

static void on_album_card_see_all(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    int64_t album_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "album-id"));
    if (album_id > 0) {
        library_unified_detail_navigate_to_album(ud->container, album_id, NULL);
    }
}

static void on_toggle_albums(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_tracks_btn), FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(ud->appears_on_stack), "albums");
    }
}

static void on_toggle_tracks(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_albums_btn), FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(ud->appears_on_stack), "tracks");
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────────── */

GtkWidget *library_unified_detail_view_new(quadrature_db_t *db,
                                            LibraryCache *cache,
                                            ArtworkManager *art_mgr,
                                            const LibraryCallbacks *cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/detail_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "detail_view"));
    g_object_ref(view);

    UnifiedDetailData *ud = g_new0(UnifiedDetailData, 1);
    ud->db = db;
    ud->cache = cache;
    ud->art_mgr = art_mgr;
    if (cbs) ud->cbs = *cbs;
    ud->state = DETAIL_STATE_EMPTY;
    ud->container = view;

    /* Get widget references */
    ud->back_header = GTK_WIDGET(gtk_builder_get_object(builder, "back_header"));
    ud->back_button = GTK_WIDGET(gtk_builder_get_object(builder, "back_button"));
    ud->back_label = GTK_WIDGET(gtk_builder_get_object(builder, "back_label"));
    ud->content_stack = GTK_WIDGET(gtk_builder_get_object(builder, "content_stack"));

    /* Album state widgets */
    ud->album_art = GTK_WIDGET(gtk_builder_get_object(builder, "album_art"));
    ud->album_title = GTK_WIDGET(gtk_builder_get_object(builder, "album_title"));
    ud->album_artist_link = GTK_WIDGET(gtk_builder_get_object(builder, "album_artist_link"));
    ud->album_year = GTK_WIDGET(gtk_builder_get_object(builder, "album_year"));
    ud->album_stats = GTK_WIDGET(gtk_builder_get_object(builder, "album_stats"));
    ud->album_featuring_box = GTK_WIDGET(gtk_builder_get_object(builder, "album_featuring_box"));
    ud->album_featuring_artists = GTK_WIDGET(gtk_builder_get_object(builder, "album_featuring_artists"));
    ud->album_tracks_container = GTK_WIDGET(gtk_builder_get_object(builder, "album_tracks_container"));

    /* Artist state widgets */
    ud->artist_name = GTK_WIDGET(gtk_builder_get_object(builder, "artist_name"));
    ud->artist_stats = GTK_WIDGET(gtk_builder_get_object(builder, "artist_stats"));
    ud->artist_albums_container = GTK_WIDGET(gtk_builder_get_object(builder, "artist_albums_container"));
    ud->appears_on_section = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_section"));
    ud->appears_on_stack = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_stack"));
    ud->appears_on_albums = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_albums"));
    ud->appears_on_tracks = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_tracks"));
    ud->toggle_albums_btn = GTK_WIDGET(gtk_builder_get_object(builder, "toggle_albums_btn"));
    ud->toggle_tracks_btn = GTK_WIDGET(gtk_builder_get_object(builder, "toggle_tracks_btn"));

    /* Connect signals */
    g_signal_connect(ud->back_button, "clicked", G_CALLBACK(on_unified_back_clicked), ud);
    g_signal_connect(ud->album_artist_link, "clicked", G_CALLBACK(on_unified_artist_link_clicked), ud);
    g_signal_connect(ud->toggle_albums_btn, "toggled", G_CALLBACK(on_toggle_albums), ud);
    g_signal_connect(ud->toggle_tracks_btn, "toggled", G_CALLBACK(on_toggle_tracks), ud);

    /* Store data and start in empty state */
    g_object_set_data_full(G_OBJECT(view), UNIFIED_DATA_KEY, ud, g_free);
    gtk_widget_set_visible(ud->back_header, FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "empty");

    g_object_unref(builder);
    return view;
}

void library_unified_detail_navigate_to_artist(GtkWidget *view, int64_t artist_id,
                                                const char *source_view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (!ud) return;

    /* Push current state to nav stack if not empty */
    if (ud->state != DETAIL_STATE_EMPTY) {
        NavEntryType type = (ud->state == DETAIL_STATE_ARTIST) ? NAV_ENTRY_ARTIST : NAV_ENTRY_ALBUM;
        nav_push(ud, type, ud->current_id, NULL);
    } else if (source_view) {
        nav_push(ud, NAV_ENTRY_VIEW, 0, source_view);
    }

    /* Update back label */
    if (source_view) {
        char label[64];
        snprintf(label, sizeof(label), "Back to %s", source_view);
        gtk_label_set_text(GTK_LABEL(ud->back_label), label);
    } else {
        gtk_label_set_text(GTK_LABEL(ud->back_label), "Back");
    }

    load_artist_state(ud, artist_id);
}

void library_unified_detail_navigate_to_album(GtkWidget *view, int64_t album_id,
                                               const char *source_view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (!ud) return;

    /* Push current state to nav stack if not empty */
    if (ud->state != DETAIL_STATE_EMPTY) {
        NavEntryType type = (ud->state == DETAIL_STATE_ARTIST) ? NAV_ENTRY_ARTIST : NAV_ENTRY_ALBUM;
        nav_push(ud, type, ud->current_id, NULL);
    } else if (source_view) {
        nav_push(ud, NAV_ENTRY_VIEW, 0, source_view);
    }

    /* Update back label based on previous state */
    if (ud->state == DETAIL_STATE_ARTIST) {
        gtk_label_set_text(GTK_LABEL(ud->back_label), "Back to Artist");
    } else if (source_view) {
        char label[64];
        snprintf(label, sizeof(label), "Back to %s", source_view);
        gtk_label_set_text(GTK_LABEL(ud->back_label), label);
    } else {
        gtk_label_set_text(GTK_LABEL(ud->back_label), "Back");
    }

    load_album_state(ud, album_id);
}

void library_unified_detail_show_empty(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (!ud) return;

    nav_clear(ud);
    ud->state = DETAIL_STATE_EMPTY;
    ud->current_id = 0;
    gtk_widget_set_visible(ud->back_header, FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "empty");
}

gboolean library_unified_detail_go_back(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (!ud || ud->nav_depth == 0) {
        /* No navigation history - callback to parent */
        if (ud && ud->cbs.on_back)
            ud->cbs.on_back(ud->cbs.user_data);
        return FALSE;
    }

    NavEntry *prev = nav_pop(ud);
    if (!prev) return FALSE;

    switch (prev->type) {
    case NAV_ENTRY_VIEW:
        /* Return to main view via callback */
        if (ud->cbs.on_back)
            ud->cbs.on_back(ud->cbs.user_data);
        library_unified_detail_show_empty(view);
        break;
    case NAV_ENTRY_ARTIST:
        load_artist_state(ud, prev->id);
        break;
    case NAV_ENTRY_ALBUM:
        load_album_state(ud, prev->id);
        break;
    }

    /* Update back label for new state */
    if (ud->nav_depth > 0) {
        NavEntry *top = &ud->nav_stack[ud->nav_depth - 1];
        if (top->view_name) {
            char label[64];
            snprintf(label, sizeof(label), "Back to %s", top->view_name);
            gtk_label_set_text(GTK_LABEL(ud->back_label), label);
        } else if (top->type == NAV_ENTRY_ARTIST) {
            gtk_label_set_text(GTK_LABEL(ud->back_label), "Back to Artist");
        } else {
            gtk_label_set_text(GTK_LABEL(ud->back_label), "Back");
        }
    }

    g_free(prev->view_name);
    return TRUE;
}

DetailState library_unified_detail_get_state(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    return ud ? ud->state : DETAIL_STATE_EMPTY;
}
