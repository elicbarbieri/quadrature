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

    /* Suppress: dim + non-interactive when viewing this artist's own page */
    if (callbacks && callbacks->suppress_id > 0 && callbacks->suppress_id == artist_id) {
        gtk_widget_set_sensitive(btn, FALSE);
        return btn;
    }

    if (callbacks) {
        RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
        *cbs_copy = *callbacks;
        g_object_set_data_full(G_OBJECT(btn), "artist-callbacks", cbs_copy, g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_artist_button_clicked), cbs_copy);
    }

    return btn;
}

static void on_overflow_btn_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    gtk_popover_popup(GTK_POPOVER(user_data));
}

static void on_overflow_btn_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    gtk_widget_unparent(GTK_WIDGET(user_data));
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
    g_signal_connect(btn, "clicked", G_CALLBACK(on_overflow_btn_clicked), popover);
    g_signal_connect(btn, "destroy", G_CALLBACK(on_overflow_btn_destroy), popover);

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
 * Artist Buttons — Width-Aware Layout via QuadratureOverflowBox
 *
 * Two modes:
 *   single-role  — one artist role per box (track rows)
 *   combined     — primary buttons pinned, featuring overflows (album detail)
 *
 * Both populate a QuadratureOverflowBox directly; planning happens inside the
 * widget's own size_allocate so there is no stale-width race.
 * ═══════════════════════════════════════════════════════════════════════════ */

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

static guint count_by_role(const GPtrArray *track_artists, library_artist_role_t role) {
    guint n = 0;
    for (guint i = 0; i < track_artists->len; i++) {
        const library_track_artist_t *a = g_ptr_array_index(track_artists, i);
        if (a->role == role) n++;
    }
    return n;
}

void populate_artist_buttons(GtkWidget *box,
                              library_cache_t *cache,
                              int64_t track_id,
                              library_artist_role_t role,
                              RowCallbacks *callbacks,
                              gboolean add_feat_prefix) {
    QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(box);
    quadrature_overflow_box_clear_all(ofb);

    const GPtrArray *ta = (cache && track_id > 0)
        ? library_cache_get_track_artists(cache, track_id) : NULL;
    if (!ta || ta->len == 0) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    guint count = count_by_role(ta, role);
    if (count == 0) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    gtk_widget_set_visible(box, TRUE);
    guint pinned = 0;

    if (add_feat_prefix) {
        GtkWidget *ft = gtk_label_new("ft ");
        gtk_widget_add_css_class(ft, "library-row-subtitle");
        quadrature_overflow_box_append(ofb, ft);
        pinned = 1;
    }

    /* Items: one artist button per artist in the role */
    GPtrArray *role_artists = filter_artists_by_role(ta, role);
    for (guint i = 0; i < role_artists->len; i++) {
        const library_track_artist_t *a = g_ptr_array_index(role_artists, i);
        GtkWidget *btn = create_artist_button(a->artist_id, a->name, callbacks);
        quadrature_overflow_box_append(ofb, btn);
    }

    /* Overflow indicator: "…" button opening a popover with all role artists */
    GtkWidget *overflow_btn = create_artist_overflow_button(role_artists, callbacks);
    quadrature_overflow_box_append(ofb, overflow_btn);
    g_ptr_array_free(role_artists, TRUE);

    quadrature_overflow_box_set_pinned_count(ofb, pinned);
    quadrature_overflow_box_set_item_count(ofb, count);
}

void populate_artist_buttons_combined(GtkWidget *box,
                                       library_cache_t *cache,
                                       int64_t track_id,
                                       RowCallbacks *callbacks,
                                       gboolean show_primary) {
    QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(box);
    quadrature_overflow_box_clear_all(ofb);

    const GPtrArray *ta = (cache && track_id > 0)
        ? library_cache_get_track_artists(cache, track_id) : NULL;
    if (!ta || ta->len == 0) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    guint primary_count = count_by_role(ta, LIBRARY_ARTIST_ROLE_PRIMARY);
    guint feat_count    = count_by_role(ta, LIBRARY_ARTIST_ROLE_FEATURING);

    if (primary_count == 0 && feat_count == 0) {
        gtk_widget_set_visible(box, FALSE);
        return;
    }

    gtk_widget_set_visible(box, TRUE);
    guint pinned = 0;

    /* Primary buttons — pinned (always visible, never hidden) */
    if (show_primary && primary_count > 0) {
        GPtrArray *primaries = filter_artists_by_role(ta, LIBRARY_ARTIST_ROLE_PRIMARY);
        for (guint i = 0; i < primaries->len; i++) {
            const library_track_artist_t *a = g_ptr_array_index(primaries, i);
            GtkWidget *btn = create_artist_button(a->artist_id, a->name, callbacks);
            quadrature_overflow_box_append(ofb, btn);
            pinned++;
        }
        g_ptr_array_free(primaries, TRUE);
    }

    if (feat_count == 0) {
        quadrature_overflow_box_set_pinned_count(ofb, pinned);
        quadrature_overflow_box_set_item_count(ofb, 0);
        return;
    }

    /* "ft " separator: also pinned, between primaries and features */
    GtkWidget *ft = gtk_label_new("ft ");
    gtk_widget_add_css_class(ft, "library-row-subtitle");
    quadrature_overflow_box_append(ofb, ft);
    pinned++;

    /* Featuring artists — items (may overflow into "…" popover) */
    GPtrArray *feats = filter_artists_by_role(ta, LIBRARY_ARTIST_ROLE_FEATURING);
    for (guint i = 0; i < feats->len; i++) {
        const library_track_artist_t *a = g_ptr_array_index(feats, i);
        GtkWidget *btn = create_artist_button(a->artist_id, a->name, callbacks);
        quadrature_overflow_box_append(ofb, btn);
    }

    /* Overflow popover shows ALL artists (primary + featuring) */
    GPtrArray *all_artists = g_ptr_array_new();
    for (guint i = 0; i < ta->len; i++)
        g_ptr_array_add(all_artists, g_ptr_array_index(ta, i));
    GtkWidget *overflow_btn = create_artist_overflow_button(all_artists, callbacks);
    quadrature_overflow_box_append(ofb, overflow_btn);
    g_ptr_array_free(all_artists, TRUE);
    g_ptr_array_free(feats, TRUE);

    quadrature_overflow_box_set_pinned_count(ofb, pinned);
    quadrature_overflow_box_set_item_count(ofb, feat_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Credit Role Pill Layout — via QuadratureOverflowBox
 *
 * The credit_annotation QuadratureOverflowBox carries the artist button as its
 * pinned[0] child (either declared in the .ui template for track rows, or
 * appended by C for album rows before this function is called). We append
 * the role pills as items and a "…" overflow indicator last.
 * ═══════════════════════════════════════════════════════════════════════════ */

void populate_credit_pills(GtkWidget *credit_annotation,
                            const char *const *roles,
                            guint role_count) {
    QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(credit_annotation);

    /* Clear any items/indicator from a previous bind, preserve pinned[0] */
    quadrature_overflow_box_clear_items(ofb);

    if (role_count == 0) return;

    for (guint i = 0; i < role_count; i++) {
        GtkWidget *pill = gtk_label_new(roles[i]);
        gtk_widget_add_css_class(pill, "credit-role-label");
        quadrature_overflow_box_append(ofb, pill);
    }

    /* "…" overflow indicator — shown only if items don't fit */
    GtkWidget *more = gtk_label_new("…");
    gtk_widget_add_css_class(more, "credit-role-label");
    quadrature_overflow_box_append(ofb, more);

    quadrature_overflow_box_set_item_count(ofb, role_count);
}
