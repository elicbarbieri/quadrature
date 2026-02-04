/**
 * Quadrature UI Row Helpers
 *
 * Creates template-based list rows from LibraryCache data.
 * Rows are stateless - click handlers attached separately via ui_row_attach_handlers().
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <string.h>

void ui_format_duration(uint32_t ms, char *buf, size_t len) {
    uint32_t sec = ms / 1000, min = sec / 60;
    snprintf(buf, len, "%u:%02u", min, sec % 60);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Button Helpers
 *
 * Create clickable artist buttons for navigation. Supports:
 * - Single artist buttons (direct click navigation)
 * - Overflow menu buttons (popover with all artists alphabetically)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Comparison function for sorting artists alphabetically */
static gint compare_artists_by_name(gconstpointer a, gconstpointer b) {
    const library_track_artist_t *artist_a = *(const library_track_artist_t **)a;
    const library_track_artist_t *artist_b = *(const library_track_artist_t **)b;
    return g_strcmp0(artist_a->name, artist_b->name);
}

/**
 * Measure the natural width of a widget.
 * Returns the natural width in pixels, or 0 if not yet allocated.
 */
static int measure_widget_width(GtkWidget* widget) {
    if (!widget) return 0;
    
    int min_width = 0, natural_width = 0;
    gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, 
                      &min_width, &natural_width, NULL, NULL);
    return natural_width;
}

/* Callback for single artist button click */
static void on_artist_button_clicked(GtkButton *button, gpointer user_data) {
    RowCallbacks *cbs = user_data;
    gpointer artist_id_ptr = g_object_get_data(G_OBJECT(button), "artist-id");
    int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(artist_id_ptr);
    
    if (artist_id > 0 && cbs && cbs->on_activate) {
        cbs->on_activate(artist_id, cbs->user_data);
    }
}

/* Callback for album button click */
static void on_album_button_clicked(GtkButton *button, gpointer user_data) {
    RowCallbacks *cbs = user_data;
    gpointer album_id_ptr = g_object_get_data(G_OBJECT(button), "album-id");
    int64_t album_id = (int64_t)GPOINTER_TO_SIZE(album_id_ptr);
    
    if (album_id > 0 && cbs && cbs->on_activate) {
        cbs->on_activate(album_id, cbs->user_data);
    }
}

/**
 * Create a single artist navigation button.
 * Clicking the button triggers on_activate callback with artist_id.
 */
static GtkWidget* create_artist_button(int64_t artist_id, const char* name, RowCallbacks* callbacks) {
    /* Create simple flat button with label */
    GtkWidget *btn = gtk_button_new_with_label(name);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "artist-btn");

    /* Store artist ID for handler access */
    g_object_set_data(G_OBJECT(btn), "artist-id", GSIZE_TO_POINTER((gsize)artist_id));

    /* Attach click handler to navigate to artist */
    if (callbacks) {
        RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
        *cbs_copy = *callbacks;
        g_object_set_data_full(G_OBJECT(btn), "artist-callbacks", cbs_copy, g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_artist_button_clicked), cbs_copy);
    }

    return btn;
}

/* Callback for artist popover row activation */
static void on_popover_artist_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer popover) {
    RowCallbacks *cbs = g_object_get_data(G_OBJECT(list), "row-callbacks");
    gpointer artist_id_ptr = g_object_get_data(G_OBJECT(row), "artist-id");
    int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(artist_id_ptr);
    
    /* Navigate to artist */
    if (artist_id > 0 && cbs && cbs->on_activate) {
        cbs->on_activate(artist_id, cbs->user_data);
    }
    
    /* Close popover */
    if (popover) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

/**
 * Create overflow menu button showing "..." with popover of all artists alphabetically.
 */
static GtkWidget* create_artist_overflow_button(const GPtrArray* artists, RowCallbacks* callbacks) {
    GtkBuilder *btn_builder = gtk_builder_new_from_resource("/org/quadrature/ui/artist_menu_button.ui");
    GtkWidget *btn = GTK_WIDGET(gtk_builder_get_object(btn_builder, "artist_menu_btn"));
    GtkWidget *label = GTK_WIDGET(gtk_builder_get_object(btn_builder, "artist_label"));
    g_object_ref(btn);
    g_object_unref(btn_builder);

    /* Set label to "..." */
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), "…");
    }

    /* Create popover with artist list */
    GtkBuilder *pop_builder = gtk_builder_new_from_resource("/org/quadrature/ui/artist_popover.ui");
    GtkWidget *popover = GTK_WIDGET(gtk_builder_get_object(pop_builder, "artist_popover"));
    GtkWidget *artist_list = GTK_WIDGET(gtk_builder_get_object(pop_builder, "artist_list"));
    g_object_ref(popover);
    g_object_unref(pop_builder);

    /* Sort artists alphabetically */
    GPtrArray *sorted = g_ptr_array_new();
    for (guint i = 0; i < artists->len; i++) {
        g_ptr_array_add(sorted, g_ptr_array_index(artists, i));
    }
    g_ptr_array_sort(sorted, compare_artists_by_name);

    /* Populate list with all artists */
    for (guint i = 0; i < sorted->len; i++) {
        const library_track_artist_t *artist = g_ptr_array_index(sorted, i);
        
        GtkBuilder *row_builder = gtk_builder_new_from_resource("/org/quadrature/ui/artist_popover_row.ui");
        GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(row_builder, "artist_row"));
        GtkWidget *row_label = GTK_WIDGET(gtk_builder_get_object(row_builder, "artist_name"));
        g_object_ref(row);
        g_object_unref(row_builder);

        if (row_label) {
            gtk_label_set_text(GTK_LABEL(row_label), artist->name);
        }

        /* Store artist ID */
        g_object_set_data(G_OBJECT(row), "artist-id", GSIZE_TO_POINTER((gsize)artist->artist_id));

        gtk_list_box_append(GTK_LIST_BOX(artist_list), row);
    }

    g_ptr_array_free(sorted, TRUE);

    /* Store callbacks for row activation handler */
    if (callbacks) {
        RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
        *cbs_copy = *callbacks;
        g_object_set_data_full(G_OBJECT(artist_list), "row-callbacks", cbs_copy, g_free);
    }

    /* Handle row activation - navigate to artist and close popover */
    g_signal_connect(artist_list, "row-activated", 
                    G_CALLBACK(on_popover_artist_row_activated), popover);

    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), popover);

    return btn;
}

/* Data structure for artist box resize handling */
typedef struct {
    const GPtrArray *track_artists;  /* Weak reference - owned by library_cache */
    library_artist_role_t role;
    RowCallbacks *callbacks;         /* Owned copy */
    gboolean add_feat_prefix;
} ArtistBoxData;

static void artist_box_data_free(gpointer data) {
    ArtistBoxData *abd = data;
    if (abd->callbacks) {
        g_free(abd->callbacks);
    }
    g_free(abd);
}

/**
 * Populate artist buttons with width-aware overflow handling.
 * Called initially and on size-allocate when window resizes.
 */
static void populate_artist_buttons_internal(GtkWidget* box, ArtistBoxData *abd) {
    /* Clear existing children */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box))) {
        gtk_box_remove(GTK_BOX(box), child);
    }

    if (!abd->track_artists || abd->track_artists->len == 0) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    /* Filter artists by role */
    GPtrArray *filtered = g_ptr_array_new();
    for (guint i = 0; i < abd->track_artists->len; i++) {
        const library_track_artist_t *artist = g_ptr_array_index(abd->track_artists, i);
        if (artist->role == abd->role) {
            g_ptr_array_add(filtered, (gpointer)artist);
        }
    }

    if (filtered->len == 0) {
        g_ptr_array_free(filtered, TRUE);
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    /* Make box visible */
    gtk_widget_set_visible(box, TRUE);

    /* Use the box's own allocated width (set by layout manager / constraint solver) */
    int max_width = gtk_widget_get_width(box);
    if (max_width <= 0) max_width = 400;  /* Default before first allocation */

    int accumulated_width = 0;

    /* Add "feat. " label prefix for featuring artists */
    GtkWidget *feat_label = NULL;
    if (abd->add_feat_prefix && filtered->len > 0) {
        feat_label = gtk_label_new("feat. ");
        gtk_widget_add_css_class(feat_label, "library-row-subtitle");
        gtk_box_append(GTK_BOX(box), feat_label);
        accumulated_width += measure_widget_width(feat_label);
    }

    /* Measure comma width once */
    GtkWidget *comma_measure = gtk_label_new(", ");
    gtk_widget_add_css_class(comma_measure, "library-row-subtitle");
    int comma_width = measure_widget_width(comma_measure);
    g_object_ref_sink(comma_measure);
    g_object_unref(comma_measure);

    /* Track added widgets for potential overflow removal */
    GPtrArray *added_widgets = g_ptr_array_new();
    guint artists_added = 0;

    /* Add artist buttons with width tracking */
    for (guint i = 0; i < filtered->len; i++) {
        const library_track_artist_t *artist = g_ptr_array_index(filtered, i);
        GtkWidget *btn = create_artist_button(artist->artist_id, artist->name, abd->callbacks);
        
        int btn_width = measure_widget_width(btn);
        int comma_cost = (i < filtered->len - 1) ? comma_width : 0;
        int total_cost = btn_width + comma_cost;

        /* Check if adding this button would exceed available width */
        if (accumulated_width + total_cost > max_width && artists_added > 0) {
            /* Would overflow - need to create overflow button */
            
            /* Create overflow button first to measure its width */
            GPtrArray *remaining = g_ptr_array_new();
            for (guint j = i; j < filtered->len; j++) {
                g_ptr_array_add(remaining, g_ptr_array_index(filtered, j));
            }
            GtkWidget *overflow_btn = create_artist_overflow_button(remaining, abd->callbacks);
            g_ptr_array_free(remaining, TRUE);
            
            int overflow_width = measure_widget_width(overflow_btn);
            
            /* Remove previously added widgets until overflow button fits */
            while (added_widgets->len > 0 && accumulated_width + overflow_width > max_width) {
                /* Remove last widget (button or comma) */
                GtkWidget *last = g_ptr_array_index(added_widgets, added_widgets->len - 1);
                int last_width = measure_widget_width(last);
                gtk_box_remove(GTK_BOX(box), last);
                accumulated_width -= last_width;
                g_ptr_array_remove_index(added_widgets, added_widgets->len - 1);
                
                /* If we removed a button, decrement artists_added */
                if (GTK_IS_BUTTON(last) && !GTK_IS_MENU_BUTTON(last)) {
                    artists_added--;
                }
            }
            
            /* Add overflow button */
            gtk_box_append(GTK_BOX(box), overflow_btn);
            g_object_unref(btn);  /* Clean up the button we didn't add */
            break;
        }

        /* Add button */
        gtk_box_append(GTK_BOX(box), btn);
        g_ptr_array_add(added_widgets, btn);
        accumulated_width += btn_width;
        artists_added++;

        /* Add comma separator (except after last) */
        if (i < filtered->len - 1) {
            GtkWidget *comma = gtk_label_new(", ");
            gtk_widget_add_css_class(comma, "library-row-subtitle");
            gtk_box_append(GTK_BOX(box), comma);
            g_ptr_array_add(added_widgets, comma);
            accumulated_width += comma_width;
        }
    }

    g_ptr_array_free(added_widgets, TRUE);
    g_ptr_array_free(filtered, TRUE);
}

/**
 * Map callback: recalculate artist buttons when box is realized with actual dimensions.
 * This ensures we have accurate width measurements after GTK layout.
 */
static void on_artist_box_map(GtkWidget *box, gpointer user_data) {
    ArtistBoxData *abd = user_data;
    
    /* Recalculate with actual allocated dimensions */
    populate_artist_buttons_internal(box, abd);
}

/**
 * Resize callback for window size changes.
 * Monitors the box's own allocated width (set by layout manager / constraint solver).
 */
static gboolean on_artist_box_tick(GtkWidget *box, GdkFrameClock *clock, gpointer user_data) {
    (void)clock;
    ArtistBoxData *abd = user_data;

    int box_width = gtk_widget_get_width(box);
    if (box_width <= 0) return G_SOURCE_CONTINUE;

    /* Track width changes to avoid redundant recalculation */
    int *last_width_ptr = g_object_get_data(G_OBJECT(box), "last-box-width");
    if (!last_width_ptr) {
        last_width_ptr = g_new(int, 1);
        *last_width_ptr = 0;
        g_object_set_data_full(G_OBJECT(box), "last-box-width", last_width_ptr, g_free);
    }

    if (abs(box_width - *last_width_ptr) < 10) return G_SOURCE_CONTINUE;
    *last_width_ptr = box_width;

    populate_artist_buttons_internal(box, abd);

    return G_SOURCE_CONTINUE;
}

/**
 * Populate a GtkBox with artist buttons, filtering by role.
 * For featuring artists, prepends "feat. " to the first button.
 *
 * Width-aware: uses the box's own allocated width (from layout manager /
 * constraint solver) as the overflow threshold. Recalculates on resize.
 */
static void populate_artist_buttons(GtkWidget* box,
                                     const GPtrArray* track_artists,
                                     library_artist_role_t role,
                                     RowCallbacks* callbacks,
                                     gboolean add_feat_prefix) {
    ArtistBoxData *abd = g_new0(ArtistBoxData, 1);
    abd->track_artists = track_artists;  /* Weak reference */
    abd->role = role;
    abd->callbacks = callbacks ? g_memdup2(callbacks, sizeof(RowCallbacks)) : NULL;
    abd->add_feat_prefix = add_feat_prefix;

    g_object_set_data_full(G_OBJECT(box), "artist-box-data", abd, artist_box_data_free);
    g_signal_connect(box, "map", G_CALLBACK(on_artist_box_map), abd);
    gtk_widget_add_tick_callback(box, on_artist_box_tick, abd, NULL);

    populate_artist_buttons_internal(box, abd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Track Row Proportional Column Sizing
 *
 * GtkBox distributes extra space equally among hexpand children.
 * This tick callback enforces a 60/40 split of flexible space between
 * col_left (title/artists) and col_right (album/featuring) by setting
 * explicit width requests after measuring fixed-width siblings.
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean track_row_proportional_tick(GtkWidget *row, GdkFrameClock *clock, gpointer data) {
    (void)clock; (void)data;

    int row_width = gtk_widget_get_width(row);
    if (row_width <= 0) return G_SOURCE_CONTINUE;

    /* Skip if width hasn't changed */
    int *last = g_object_get_data(G_OBJECT(row), "last-prop-width");
    if (!last) {
        last = g_new(int, 1);
        *last = 0;
        g_object_set_data_full(G_OBJECT(row), "last-prop-width", last, g_free);
    }
    if (abs(row_width - *last) < 5) return G_SOURCE_CONTINUE;
    *last = row_width;

    GtkWidget *col_left = g_object_get_data(G_OBJECT(row), "prop-col-left");
    GtkWidget *col_right = g_object_get_data(G_OBJECT(row), "prop-col-right");
    GtkWidget *art = g_object_get_data(G_OBJECT(row), "prop-art");
    GtkWidget *col_meta = g_object_get_data(G_OBJECT(row), "prop-col-meta");
    if (!col_left || !col_right) return G_SOURCE_CONTINUE;

    int art_w = art ? gtk_widget_get_width(art) : 0;
    int meta_w = col_meta ? gtk_widget_get_width(col_meta) : 0;
    int spacing = 12 * 3;  /* 3 gaps between 4 children */
    int flexible = row_width - art_w - meta_w - spacing;
    if (flexible <= 0) return G_SOURCE_CONTINUE;

    int left_w = (int)(flexible * 0.6);
    int right_w = flexible - left_w;

    gtk_widget_set_size_request(col_left, left_w, -1);
    gtk_widget_set_size_request(col_right, right_w, -1);

    return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Creation Functions
 *
 * Create rows from library_cache types. Each row stores entity data via
 * g_object_set_data() for handler access:
 *   - Artist rows: "artist-id"
 *   - Album rows: "album-id", "first-track-id", "first-track-path"
 *   - Track rows: "track-id", "track-path"
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *ui_create_artist_row(const library_artist_info_t *artist,
                                 library_cache_t *cache,
                                 ArtworkManager *art_mgr,
                                 gboolean show_art_strip,
                                 UiRowSizeGroups *size_groups) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_artist_row.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    GtkWidget *art_strip = GTK_WIDGET(gtk_builder_get_object(builder, "art_strip"));

    g_object_unref(builder);

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), artist->name);
    }

    if (subtitle) {
        char buf[64];
        /* Format: "N albums · Appears on N tracks" */
        if (artist->album_count > 0 && artist->track_count > 0) {
            snprintf(buf, sizeof(buf), "%u album%s \u00b7 Appears on %u track%s",
                     artist->album_count, artist->album_count == 1 ? "" : "s",
                     artist->track_count, artist->track_count == 1 ? "" : "s");
        } else if (artist->album_count > 0) {
            snprintf(buf, sizeof(buf), "%u album%s",
                     artist->album_count, artist->album_count == 1 ? "" : "s");
        } else if (artist->track_count > 0) {
            snprintf(buf, sizeof(buf), "Appears on %u track%s",
                     artist->track_count, artist->track_count == 1 ? "" : "s");
        } else {
            snprintf(buf, sizeof(buf), "No albums");
        }
        gtk_label_set_text(GTK_LABEL(subtitle), buf);
    }

    if (art_strip) {
        if (show_art_strip && cache && art_mgr) {
            /* Populate art strip with album thumbnails (up to 6 most recent) */
            const GPtrArray *albums = library_cache_get_albums_by_artist(cache, artist->artist_id);
            if (albums && albums->len > 0) {
                guint count = albums->len > 6 ? 6 : albums->len;
                guint start_idx = albums->len > 6 ? albums->len - 6 : 0;  /* Start from last 6 albums */
                int thumb_px = artwork_manager_get_thumb_size(art_mgr);
                for (guint i = start_idx; i < albums->len; i++) {
                    const library_album_info_t *album = g_ptr_array_index(albums, i);
                    GtkWidget *img = gtk_image_new();
                    gtk_image_set_pixel_size(GTK_IMAGE(img), thumb_px);
                    gtk_widget_add_css_class(img, "album-art-strip-thumb");
                    artwork_manager_get_thumbnail(art_mgr, album->album_id, img);
                    gtk_box_append(GTK_BOX(art_strip), img);
                }
                gtk_widget_set_visible(art_strip, TRUE);
            } else {
                gtk_widget_set_visible(art_strip, FALSE);
            }
        } else {
            gtk_widget_set_visible(art_strip, FALSE);
        }
    }

    /* Add to size groups for column alignment */
    if (size_groups) {
        if (size_groups->col1 && title && subtitle) {
            gtk_size_group_add_widget(size_groups->col1, title);
            gtk_size_group_add_widget(size_groups->col1, subtitle);
        }
        if (size_groups->col2 && art_strip) {
            gtk_size_group_add_widget(size_groups->col2, art_strip);
        }
    }

    /* Store artist ID for handler access */
    g_object_set_data(G_OBJECT(row), "artist-id", GSIZE_TO_POINTER((gsize)artist->artist_id));

    return row;
}

GtkWidget *ui_create_album_row(const library_album_info_t *album,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_count,
                                RowCallbacks *artist_cbs,
                                UiRowSizeGroups *size_groups) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_album_row.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *primary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "primary_artists_box"));
    GtkWidget *count = GTK_WIDGET(gtk_builder_get_object(builder, "count"));
    GtkWidget *year = GTK_WIDGET(gtk_builder_get_object(builder, "year"));
    GtkWidget *genres_box = GTK_WIDGET(gtk_builder_get_object(builder, "genres_box"));

    g_object_unref(builder);

    /* Load album art */
    if (art && art_mgr) {
        gtk_image_set_pixel_size(GTK_IMAGE(art), artwork_manager_get_thumb_size(art_mgr));
        artwork_manager_get_thumbnail(art_mgr, album->album_id, art);
    }

    /* Album title (top row) */
    if (title) {
        gtk_label_set_text(GTK_LABEL(title), album->title);
    }

    /* Top-right metadata: track count and year */
    if (count) {
        if (show_count && album->track_count > 0) {
            char buf[16];
            if (album->track_count == 1)
                snprintf(buf, sizeof(buf), "   Single");
            else {
                uint32_t display_count = album->track_count >= 100 ? 99 : album->track_count;
                snprintf(buf, sizeof(buf), "%2u Tracks", display_count);
            }
            gtk_label_set_text(GTK_LABEL(count), buf);
        } else {
            gtk_widget_set_visible(count, FALSE);
        }
    }

    if (year) {
        if (album->year > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", album->year);
            gtk_label_set_text(GTK_LABEL(year), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(year), "");
        }
    }

    /* Bottom-left: primary artist - clickable button or dimmed label */
    if (primary_artists_box && album->artist_name && album->artist_name[0]) {
        if (artist_cbs && !ui_is_various_artists(album->artist_name)) {
            /* Clickable artist button for navigation */
            GtkWidget *btn = create_artist_button(album->artist_id, album->artist_name, artist_cbs);
            gtk_box_append(GTK_BOX(primary_artists_box), btn);
        } else {
            /* Plain label for Various Artists or when no callbacks */
            GtkWidget *artist_label = gtk_label_new(album->artist_name);
            gtk_label_set_xalign(GTK_LABEL(artist_label), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(artist_label), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(artist_label, "library-row-subtitle");
            if (ui_is_various_artists(album->artist_name))
                gtk_widget_add_css_class(artist_label, "text-dim");
            gtk_box_append(GTK_BOX(primary_artists_box), artist_label);
        }
    }

    /* Bottom-right: genre pills (max 3) */
    if (genres_box)
        ui_populate_genre_pills(GTK_BOX(genres_box), album->genres, 3);

    /* Add to size groups for column alignment */
    if (size_groups) {
        if (size_groups->col1 && title)
            gtk_size_group_add_widget(size_groups->col1, title);
    }

    /* Store album ID for handler access */
    g_object_set_data(G_OBJECT(row), "album-id", GSIZE_TO_POINTER((gsize)album->album_id));

    /* Store first track ID for keyboard shortcuts */
    if (cache) {
        const GPtrArray *tracks = library_cache_get_tracks_by_album(cache, album->album_id);
        if (tracks && tracks->len > 0) {
            const library_track_info_t *first_track = g_ptr_array_index(tracks, 0);
            g_object_set_data(G_OBJECT(row), "first-track-id",
                            GSIZE_TO_POINTER((gsize)first_track->track_id));
        }
    }

    return row;
}

GtkWidget *ui_create_track_row(const library_track_info_t *track,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_album_info,
                                RowCallbacks *artist_cbs,
                                RowCallbacks *album_cbs,
                                UiRowSizeGroups *size_groups) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_track_row.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *album_box = GTK_WIDGET(gtk_builder_get_object(builder, "album_box"));
    GtkWidget *primary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "primary_artists_box"));
    GtkWidget *secondary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "secondary_artists_box"));
    GtkWidget *year = GTK_WIDGET(gtk_builder_get_object(builder, "year"));
    GtkWidget *duration = GTK_WIDGET(gtk_builder_get_object(builder, "duration"));
    GtkWidget *col_left = GTK_WIDGET(gtk_builder_get_object(builder, "col_left"));
    GtkWidget *col_right = GTK_WIDGET(gtk_builder_get_object(builder, "col_right"));
    GtkWidget *col_meta = GTK_WIDGET(gtk_builder_get_object(builder, "col_meta"));

    g_object_unref(builder);

    /* Proportional column sizing: store refs and attach tick callback */
    g_object_set_data(G_OBJECT(row), "prop-art", art);
    g_object_set_data(G_OBJECT(row), "prop-col-left", col_left);
    g_object_set_data(G_OBJECT(row), "prop-col-right", col_right);
    g_object_set_data(G_OBJECT(row), "prop-col-meta", col_meta);
    gtk_widget_add_tick_callback(row, track_row_proportional_tick, NULL, NULL);

    /* Load album art */
    if (art && art_mgr) {
        gtk_image_set_pixel_size(GTK_IMAGE(art), artwork_manager_get_thumb_size(art_mgr));
        artwork_manager_get_thumbnail(art_mgr, track->album_id, art);
    }

    /* Title */
    if (title) {
        gtk_label_set_text(GTK_LABEL(title), track->title);
    }

    /* Album button (if show_album_info enabled) */
    if (album_box) {
        if (show_album_info && track->album_title && track->album_title[0]) {
            GtkBuilder *album_builder = gtk_builder_new_from_resource("/org/quadrature/ui/album_button.ui");
            GtkWidget *album_btn = GTK_WIDGET(gtk_builder_get_object(album_builder, "album_btn"));
            GtkWidget *album_label = GTK_WIDGET(gtk_builder_get_object(album_builder, "album_label"));
            g_object_ref(album_btn);
            g_object_unref(album_builder);

            if (album_label) {
                gtk_label_set_text(GTK_LABEL(album_label), track->album_title);
            }

            /* Store album ID */
            g_object_set_data(G_OBJECT(album_btn), "album-id", GSIZE_TO_POINTER((gsize)track->album_id));

            /* Attach click handler */
            if (album_cbs) {
                RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
                *cbs_copy = *album_cbs;
                g_object_set_data_full(G_OBJECT(album_btn), "album-callbacks", cbs_copy, g_free);
                g_signal_connect(album_btn, "clicked", G_CALLBACK(on_album_button_clicked), cbs_copy);
            }

            gtk_box_append(GTK_BOX(album_box), album_btn);
            gtk_widget_set_visible(album_box, TRUE);
        } else {
            gtk_widget_set_visible(album_box, FALSE);
        }
    }

    /* Fetch and populate artist buttons */
    const GPtrArray *track_artists = NULL;
    if (cache) {
        track_artists = library_cache_get_track_artists(cache, track->track_id);
    }

    if (track_artists && track_artists->len > 0) {
        /* Store track artists for later access */
        g_object_set_data(G_OBJECT(row), "track-artists", (gpointer)track_artists);

        if (primary_artists_box) {
            populate_artist_buttons(primary_artists_box, track_artists,
                                   LIBRARY_ARTIST_ROLE_PRIMARY, artist_cbs, FALSE);
        }

        if (secondary_artists_box) {
            populate_artist_buttons(secondary_artists_box, track_artists,
                                   LIBRARY_ARTIST_ROLE_FEATURING, artist_cbs, TRUE);
        }
    } else {
        /* Fallback: no artist data, hide boxes */
        if (primary_artists_box) gtk_widget_set_visible(primary_artists_box, FALSE);
        if (secondary_artists_box) gtk_widget_set_visible(secondary_artists_box, FALSE);
    }

    /* Year */
    if (year) {
        if (track->year > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", track->year);
            gtk_label_set_text(GTK_LABEL(year), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(year), "");
        }
    }

    /* Duration */
    if (duration) {
        char buf[16];
        ui_format_duration(track->duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(duration), buf);
    }

    /* Add to size groups for column alignment */
    if (size_groups) {
        if (size_groups->col1 && title && primary_artists_box) {
            gtk_size_group_add_widget(size_groups->col1, title);
            gtk_size_group_add_widget(size_groups->col1, primary_artists_box);
        }
        if (size_groups->col2) {
            if (album_box)
                gtk_size_group_add_widget(size_groups->col2, album_box);
            if (secondary_artists_box)
                gtk_size_group_add_widget(size_groups->col2, secondary_artists_box);
        }
    }

    /* Store track ID and path for handler access */
    g_object_set_data(G_OBJECT(row), "track-id", GSIZE_TO_POINTER((gsize)track->track_id));
    g_object_set_data_full(G_OBJECT(row), "track-path", g_strdup(track->path), g_free);

    return row;
}

GtkWidget *ui_create_album_detail_track_item(const library_track_info_t *track,
                                               library_cache_t *cache,
                                               RowCallbacks *artist_cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/album_detail_track_item.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *track_num = GTK_WIDGET(gtk_builder_get_object(builder, "track_num"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *secondary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "secondary_artists_box"));
    GtkWidget *duration = GTK_WIDGET(gtk_builder_get_object(builder, "duration"));

    g_object_unref(builder);

    if (track_num) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", track->track_num);
        gtk_label_set_text(GTK_LABEL(track_num), buf);
    }

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), track->title);
    }

    /* Fetch and populate featuring artists only (primary artists are in album header) */
    const GPtrArray *track_artists = NULL;
    if (cache) {
        track_artists = library_cache_get_track_artists(cache, track->track_id);
    }

    if (track_artists && track_artists->len > 0 && secondary_artists_box) {
        populate_artist_buttons(secondary_artists_box, track_artists,
                               LIBRARY_ARTIST_ROLE_FEATURING, artist_cbs, TRUE);
    } else if (secondary_artists_box) {
        gtk_widget_set_visible(secondary_artists_box, FALSE);
    }

    if (duration) {
        char buf[16];
        ui_format_duration(track->duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(duration), buf);
    }

    /* Store track data for handlers */
    g_object_set_data(G_OBJECT(row), "track-id", GSIZE_TO_POINTER((gsize)track->track_id));
    g_object_set_data_full(G_OBJECT(row), "track-path", g_strdup(track->path), g_free);

    return row;
}

GtkWidget *ui_create_disc_header(uint16_t disc_num) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/album_detail_disc_header.ui");
    GtkWidget *header = GTK_WIDGET(gtk_builder_get_object(builder, "disc_header"));
    GtkWidget *label = GTK_WIDGET(gtk_builder_get_object(builder, "disc_label"));
    g_object_ref(header);
    g_object_unref(builder);

    if (label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "DISC %u", disc_num);
        gtk_label_set_text(GTK_LABEL(label), buf);
    }

    return header;
}

GtkWidget *ui_create_album_detail_card(const library_album_info_t *album,
                                        const GPtrArray *tracks,
                                        library_cache_t *cache,
                                        ArtworkManager *art_mgr,
                                        guint max_preview_tracks,
                                        RowCallbacks *track_cbs,
                                        RowCallbacks *artist_cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/album_card.ui");
    GtkWidget *card = GTK_WIDGET(gtk_builder_get_object(builder, "album_card"));
    g_object_ref(card);

    /* Get widget references */
    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "card_art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "card_title"));
    GtkWidget *artist_link = GTK_WIDGET(gtk_builder_get_object(builder, "card_artist_link"));
    GtkWidget *year = GTK_WIDGET(gtk_builder_get_object(builder, "card_year"));
    GtkWidget *stats = GTK_WIDGET(gtk_builder_get_object(builder, "card_stats"));
    GtkWidget *track_list = GTK_WIDGET(gtk_builder_get_object(builder, "track_list"));

    g_object_unref(builder);

    /* Populate album metadata */
    if (title) {
        gtk_label_set_text(GTK_LABEL(title), album->title);
    }

    /* Populate artist link button */
    if (artist_link) {
        gtk_button_set_label(GTK_BUTTON(artist_link), album->artist_name);
        gtk_widget_set_tooltip_text(artist_link, "View artist");
        /* Store artist ID for caller to access */
        g_object_set_data(G_OBJECT(artist_link), "artist-id",
                         GSIZE_TO_POINTER((gsize)album->artist_id));
    }

    if (year) {
        if (album->year > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", album->year);
            gtk_label_set_text(GTK_LABEL(year), buf);
            gtk_widget_set_visible(year, TRUE);
        } else {
            gtk_widget_set_visible(year, FALSE);
        }
    }

    if (stats) {
        char buf[64], dur_buf[16];
        uint32_t sec = album->total_duration_ms / 1000;
        uint32_t min = sec / 60;
        uint32_t hr = min / 60;
        if (hr > 0)
            snprintf(dur_buf, sizeof(dur_buf), "%uh %02um", hr, min % 60);
        else
            snprintf(dur_buf, sizeof(dur_buf), "%u:%02u", min, sec % 60);

        snprintf(buf, sizeof(buf), "%u track%s - %s",
                 album->track_count, album->track_count == 1 ? "" : "s", dur_buf);
        gtk_label_set_text(GTK_LABEL(stats), buf);
    }

    /* Load album art */
    if (art && art_mgr) {
        artwork_manager_get_thumbnail(art_mgr, album->album_id, art);
    }

    /* Populate track list with automatic disc headers */
    if (track_list && tracks && tracks->len > 0) {
        guint track_count = tracks->len;
        guint preview_count = (max_preview_tracks > 0 && track_count > max_preview_tracks)
                              ? max_preview_tracks : track_count;

        /* Connect row-activated for Enter key / double-click */
        g_signal_connect(track_list, "row-activated", G_CALLBACK(ui_list_box_row_activated), NULL);

        /* Detect if multi-disc */
        uint16_t max_disc = 1;
        for (guint i = 0; i < track_count; i++) {
            const library_track_info_t *t = g_ptr_array_index(tracks, i);
            if (t->disc_num > max_disc) max_disc = t->disc_num;
        }
        gboolean multi_disc = (max_disc > 1);

        uint16_t current_disc = 0;
        for (guint i = 0; i < preview_count; i++) {
            const library_track_info_t *track = g_ptr_array_index(tracks, i);

            /* Insert disc header when disc changes (multi-disc only) */
            if (multi_disc && track->disc_num != current_disc) {
                current_disc = track->disc_num;
                GtkWidget *disc_hdr = ui_create_disc_header(current_disc);
                GtkWidget *hdr_row = gtk_list_box_row_new();
                gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(hdr_row), disc_hdr);
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(hdr_row), FALSE);
                gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(hdr_row), FALSE);
                gtk_list_box_append(GTK_LIST_BOX(track_list), hdr_row);
            }

            /* Create track row using helper (stores track-id/track-path) */
            GtkWidget *content = ui_create_album_detail_track_item(track, cache, artist_cbs);
            if (track_cbs) {
                ui_row_attach_handlers(content, track_cbs);
            }

            /* Wrap in GtkListBoxRow from template - CANNOT avoid C wrapping
             * Reason: Dynamic number of tracks, must be created in loop */
            GtkBuilder *row_builder = gtk_builder_new_from_resource("/org/quadrature/ui/album_detail_track_row_wrapper.ui");
            GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(row_builder, "track_row"));
            g_object_ref(row);
            g_object_unref(row_builder);
            
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
            gtk_list_box_append(GTK_LIST_BOX(track_list), row);
        }
    }

    /* Store album ID for handler access */
    g_object_set_data(G_OBJECT(card), "album-id", GSIZE_TO_POINTER((gsize)album->album_id));

    return card;
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

void ui_list_box_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data) {
    (void)list; (void)user_data;

    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child) return;

    RowHandlerData *data = g_object_get_data(G_OBJECT(child), "row-handler-data");
    if (!data || !data->cbs.on_activate) return;

    int64_t id = get_row_entity_id(child);
    if (id > 0)
        data->cbs.on_activate(id, data->cbs.user_data);
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
