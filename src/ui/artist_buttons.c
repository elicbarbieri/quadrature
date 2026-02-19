/**
 * Quadrature Artist Button Helpers
 *
 * Create clickable artist buttons for navigation. Supports:
 * - Single artist buttons (direct click navigation)
 * - Overflow menu buttons (popover with all artists alphabetically)
 * - Combined primary + featuring layouts for album detail rows
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Widget Measurement
 * ═══════════════════════════════════════════════════════════════════════════ */

static int measure_widget_width(GtkWidget* widget) {
    if (!widget) return 0;

    int min_width = 0, natural_width = 0;
    gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1,
                      &min_width, &natural_width, NULL, NULL);
    return natural_width;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Click Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

void on_artist_button_clicked(GtkButton *button, gpointer user_data) {
    RowCallbacks *cbs = user_data;
    gpointer artist_id_ptr = g_object_get_data(G_OBJECT(button), "artist-id");
    int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(artist_id_ptr);

    if (artist_id > 0 && cbs && cbs->on_activate) {
        cbs->on_activate(artist_id, cbs->user_data);
    }
}

void on_credit_mbid_navigate(GtkButton *button, gpointer user_data) {
    RowCallbacks *cbs = user_data;
    const char *mbid = g_object_get_data(G_OBJECT(button), "artist-mbid");
    const char *name = g_object_get_data(G_OBJECT(button), "artist-name");
    const char *type = g_object_get_data(G_OBJECT(button), "artist-type");
    if (mbid && cbs && cbs->on_mbid_navigate) {
        cbs->on_mbid_navigate(mbid, name, type, cbs->user_data);
    }
}

void on_album_button_clicked(GtkButton *button, gpointer user_data) {
    RowCallbacks *cbs = user_data;
    gpointer album_id_ptr = g_object_get_data(G_OBJECT(button), "album-id");
    int64_t album_id = (int64_t)GPOINTER_TO_SIZE(album_id_ptr);

    if (album_id > 0 && cbs && cbs->on_activate) {
        cbs->on_activate(album_id, cbs->user_data);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Popover Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Unparent popover when the overflow button leaves the widget tree.
 * Fires before dispose/finalize, so both widgets are still alive.
 * Prevents "Finalizing GtkButton with children" and floating-ref warnings. */
static void on_overflow_btn_unroot(GObject *obj, GParamSpec *pspec, gpointer popover) {
    (void)pspec;
    if (gtk_widget_get_root(GTK_WIDGET(obj)) == NULL)
        gtk_widget_unparent(GTK_WIDGET(popover));
}

static void on_overflow_btn_clicked(GtkButton *button, gpointer popover) {
    (void)button;
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_popover_artist_btn_clicked(GtkButton *button, gpointer popover) {
    RowCallbacks *cbs = g_object_get_data(G_OBJECT(button), "artist-callbacks-ref");
    gpointer artist_id_ptr = g_object_get_data(G_OBJECT(button), "artist-id");
    int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(artist_id_ptr);

    if (artist_id > 0 && cbs && cbs->on_activate) {
        cbs->on_activate(artist_id, cbs->user_data);
    }
    if (popover) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Button Creation
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget* create_artist_button(int64_t artist_id, const char* name, RowCallbacks* callbacks) {
    GtkWidget *btn = gtk_button_new_with_label(name);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "artist-btn");

    g_object_set_data(G_OBJECT(btn), "artist-id", GSIZE_TO_POINTER((gsize)artist_id));

    if (callbacks) {
        RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
        *cbs_copy = *callbacks;
        g_object_set_data_full(G_OBJECT(btn), "artist-callbacks", cbs_copy, g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_artist_button_clicked), cbs_copy);
    }

    return btn;
}

GtkWidget* create_artist_overflow_button(const GPtrArray* artists, RowCallbacks* callbacks) {
    GtkWidget *btn = gtk_button_new_with_label("…");
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "artist-btn");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(vbox, "popover-artist-list");

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), vbox);
    gtk_widget_set_parent(popover, btn);

    g_signal_connect(btn, "notify::root",
                     G_CALLBACK(on_overflow_btn_unroot), popover);
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_overflow_btn_clicked), popover);

    RowCallbacks *cbs_copy = NULL;
    if (callbacks) {
        cbs_copy = g_new0(RowCallbacks, 1);
        *cbs_copy = *callbacks;
        g_object_set_data_full(G_OBJECT(popover), "popover-callbacks", cbs_copy, g_free);
    }

    for (guint i = 0; i < artists->len; i++) {
        const library_track_artist_t *artist = g_ptr_array_index(artists, i);
        GtkWidget *artist_btn = gtk_button_new_with_label(artist->name);
        gtk_button_set_has_frame(GTK_BUTTON(artist_btn), FALSE);
        gtk_widget_add_css_class(artist_btn, "artist-btn");
        gtk_widget_set_halign(artist_btn, GTK_ALIGN_START);

        g_object_set_data(G_OBJECT(artist_btn), "artist-id",
                         GSIZE_TO_POINTER((gsize)artist->artist_id));
        g_object_set_data(G_OBJECT(artist_btn), "artist-callbacks-ref", cbs_copy);
        g_signal_connect(artist_btn, "clicked",
                        G_CALLBACK(on_popover_artist_btn_clicked), popover);

        gtk_box_append(GTK_BOX(vbox), artist_btn);
    }

    return btn;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ArtistBoxData — Resize-Aware Artist Button Layout
 * ═══════════════════════════════════════════════════════════════════════════ */

static void artist_box_data_free(gpointer data) {
    ArtistBoxData *abd = data;
    if (abd->callbacks) {
        g_free(abd->callbacks);
    }
    g_free(abd);
}

static GPtrArray *filter_artists_by_role(const GPtrArray *track_artists,
                                          library_artist_role_t role) {
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < track_artists->len; i++) {
        const library_track_artist_t *a = g_ptr_array_index(track_artists, i);
        if (a->role == role)
            g_ptr_array_add(out, (gpointer)a);
    }
    return out;
}

/**
 * Populate artist buttons with width-aware overflow handling.
 *
 * Two modes:
 *   single-role  (combined == FALSE)  — one role per box (track rows).
 *   combined     (combined == TRUE)   — primary + featuring in one box
 *                                        (album detail).  Primary buttons
 *                                        are always shown; featuring buttons
 *                                        overflow first.
 */
static void populate_artist_buttons_internal(GtkWidget* box, ArtistBoxData *abd) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box))) {
        gtk_box_remove(GTK_BOX(box), child);
    }

    const GPtrArray *track_artists = (abd->cache && abd->track_id > 0)
        ? library_cache_get_track_artists(abd->cache, abd->track_id)
        : NULL;

    if (!track_artists || track_artists->len == 0) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    int raw_width = abd->constraint_widget
        ? gtk_widget_get_width(abd->constraint_widget) : 0;
    int max_width = (raw_width > 0)
        ? (int)(raw_width * abd->constraint_fraction)
        : 200;

    int accumulated_width = 0;

    /* ── Combined mode: primary buttons (unconditional) then featuring (overflow) ── */
    if (abd->combined) {
        GPtrArray *primary = filter_artists_by_role(track_artists, LIBRARY_ARTIST_ROLE_PRIMARY);
        GPtrArray *featuring = filter_artists_by_role(track_artists, LIBRARY_ARTIST_ROLE_FEATURING);

        if (primary->len == 0 && featuring->len == 0) {
            g_ptr_array_free(primary, TRUE);
            g_ptr_array_free(featuring, TRUE);
            gtk_widget_set_visible(box, FALSE);
            return;
        }

        gtk_widget_set_visible(box, TRUE);

        if (abd->show_primary) {
            for (guint i = 0; i < primary->len; i++) {
                const library_track_artist_t *a = g_ptr_array_index(primary, i);
                GtkWidget *btn = create_artist_button(a->artist_id, a->name, abd->callbacks);
                gtk_box_append(GTK_BOX(box), btn);
                accumulated_width += measure_widget_width(btn);
            }
        }

        if (featuring->len > 0) {
            GtkWidget *ft = gtk_label_new("ft ");
            gtk_widget_add_css_class(ft, "library-row-subtitle");
            gtk_box_append(GTK_BOX(box), ft);
            accumulated_width += measure_widget_width(ft);

            GPtrArray *added = g_ptr_array_new();
            guint feat_added = 0;

            for (guint i = 0; i < featuring->len; i++) {
                const library_track_artist_t *a = g_ptr_array_index(featuring, i);
                GtkWidget *btn = create_artist_button(a->artist_id, a->name, abd->callbacks);
                int btn_w = measure_widget_width(btn);

                if (accumulated_width + btn_w > max_width && feat_added > 0) {
                    GPtrArray *all = g_ptr_array_new();
                    for (guint k = 0; k < primary->len; k++)
                        g_ptr_array_add(all, g_ptr_array_index(primary, k));
                    for (guint k = 0; k < featuring->len; k++)
                        g_ptr_array_add(all, g_ptr_array_index(featuring, k));
                    GtkWidget *overflow = create_artist_overflow_button(all, abd->callbacks);
                    g_ptr_array_free(all, TRUE);
                    int ow = measure_widget_width(overflow);

                    while (added->len > 0 && accumulated_width + ow > max_width) {
                        GtkWidget *last = g_ptr_array_index(added, added->len - 1);
                        accumulated_width -= measure_widget_width(last);
                        gtk_box_remove(GTK_BOX(box), last);
                        g_ptr_array_remove_index(added, added->len - 1);
                    }

                    gtk_box_append(GTK_BOX(box), overflow);
                    g_object_ref_sink(btn);
                    g_object_unref(btn);
                    break;
                }

                gtk_box_append(GTK_BOX(box), btn);
                g_ptr_array_add(added, btn);
                accumulated_width += btn_w;
                feat_added++;
            }

            g_ptr_array_free(added, TRUE);
        }

        g_ptr_array_free(primary, TRUE);
        g_ptr_array_free(featuring, TRUE);
        return;
    }

    /* ── Single-role mode (track rows) ────────────────────────────────── */
    GPtrArray *filtered = filter_artists_by_role(track_artists, abd->role);

    if (filtered->len == 0) {
        g_ptr_array_free(filtered, TRUE);
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    gtk_widget_set_visible(box, TRUE);

    if (abd->add_feat_prefix) {
        GtkWidget *ft = gtk_label_new("ft ");
        gtk_widget_add_css_class(ft, "library-row-subtitle");
        gtk_box_append(GTK_BOX(box), ft);
        accumulated_width += measure_widget_width(ft);
    }

    GPtrArray *added_widgets = g_ptr_array_new();
    guint artists_added = 0;

    for (guint i = 0; i < filtered->len; i++) {
        const library_track_artist_t *artist = g_ptr_array_index(filtered, i);
        GtkWidget *btn = create_artist_button(artist->artist_id, artist->name, abd->callbacks);
        int btn_width = measure_widget_width(btn);

        if (accumulated_width + btn_width > max_width && artists_added > 0) {
            GtkWidget *overflow_btn = create_artist_overflow_button(filtered, abd->callbacks);
            int overflow_width = measure_widget_width(overflow_btn);

            while (added_widgets->len > 0 && accumulated_width + overflow_width > max_width) {
                GtkWidget *last = g_ptr_array_index(added_widgets, added_widgets->len - 1);
                accumulated_width -= measure_widget_width(last);
                gtk_box_remove(GTK_BOX(box), last);
                g_ptr_array_remove_index(added_widgets, added_widgets->len - 1);
                artists_added--;
            }

            gtk_box_append(GTK_BOX(box), overflow_btn);
            g_object_ref_sink(btn);
            g_object_unref(btn);
            break;
        }

        gtk_box_append(GTK_BOX(box), btn);
        g_ptr_array_add(added_widgets, btn);
        accumulated_width += btn_width;
        artists_added++;
    }

    g_ptr_array_free(added_widgets, TRUE);
    g_ptr_array_free(filtered, TRUE);
}

static void on_artist_box_map(GtkWidget *box, gpointer user_data) {
    ArtistBoxData *abd = user_data;
    populate_artist_buttons_internal(box, abd);
}

static gboolean on_artist_box_tick(GtkWidget *box, GdkFrameClock *clock, gpointer user_data) {
    (void)clock;
    ArtistBoxData *abd = user_data;

    int raw_width = abd->constraint_widget
        ? gtk_widget_get_width(abd->constraint_widget) : gtk_widget_get_width(box);
    int box_width = (int)(raw_width * abd->constraint_fraction);
    if (box_width <= 0) return G_SOURCE_CONTINUE;

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

void populate_artist_buttons(GtkWidget* box,
                              GtkWidget* constraint_widget,
                              double constraint_fraction,
                              library_cache_t *cache,
                              int64_t track_id,
                              library_artist_role_t role,
                              RowCallbacks* callbacks,
                              gboolean add_feat_prefix) {
    ArtistBoxData *abd = g_new0(ArtistBoxData, 1);
    abd->cache    = cache;
    abd->track_id = track_id;
    abd->role = role;
    abd->callbacks = callbacks ? g_memdup2(callbacks, sizeof(RowCallbacks)) : NULL;
    abd->add_feat_prefix = add_feat_prefix;
    abd->constraint_widget = constraint_widget;
    abd->constraint_fraction = constraint_fraction;

    g_object_set_data_full(G_OBJECT(box), "artist-box-data", abd, artist_box_data_free);
    g_signal_connect(box, "map", G_CALLBACK(on_artist_box_map), abd);
    gtk_widget_add_tick_callback(box, on_artist_box_tick, abd, NULL);

    populate_artist_buttons_internal(box, abd);
}

void populate_artist_buttons_combined(GtkWidget *box,
                                       GtkWidget *constraint_widget,
                                       double constraint_fraction,
                                       library_cache_t *cache,
                                       int64_t track_id,
                                       RowCallbacks *callbacks,
                                       gboolean show_primary) {
    ArtistBoxData *abd = g_new0(ArtistBoxData, 1);
    abd->cache = cache;
    abd->track_id = track_id;
    abd->callbacks = callbacks ? g_memdup2(callbacks, sizeof(RowCallbacks)) : NULL;
    abd->constraint_widget = constraint_widget;
    abd->constraint_fraction = constraint_fraction;
    abd->combined = TRUE;
    abd->show_primary = show_primary;

    g_object_set_data_full(G_OBJECT(box), "artist-box-data", abd, artist_box_data_free);
    g_signal_connect(box, "map", G_CALLBACK(on_artist_box_map), abd);
    gtk_widget_add_tick_callback(box, on_artist_box_tick, abd, NULL);

    populate_artist_buttons_internal(box, abd);
}
