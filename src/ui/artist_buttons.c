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
 * Artist Buttons — Width-Aware Layout via Overflow Box
 *
 * Two modes:
 *   single-role  — one artist role per box (track rows)
 *   combined     — primary buttons pinned, featuring overflows (album detail)
 *
 * Both delegate to ui_overflow_box_setup() for measurement, backoff,
 * signal wiring, and hysteresis.
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

static const library_track_artist_t *nth_artist_by_role(
    const GPtrArray *track_artists, library_artist_role_t role, guint n) {
    guint count = 0;
    for (guint i = 0; i < track_artists->len; i++) {
        const library_track_artist_t *a = g_ptr_array_index(track_artists, i);
        if (a->role == role) {
            if (count == n) return a;
            count++;
        }
    }
    return NULL;
}

/* Shared callback data for both single-role and combined modes */
typedef struct {
    library_cache_t       *cache;
    int64_t                track_id;
    library_artist_role_t  role;            /* Item role (FEATURING for combined) */
    RowCallbacks          *callbacks;       /* Owned copy */
    gboolean               overflow_all;   /* Overflow popover shows all roles */
} ArtistOverflowData;

static void artist_overflow_data_free(gpointer data) {
    ArtistOverflowData *d = data;
    g_free(d->callbacks);
    g_free(d);
}

static GtkWidget *artist_create_item(guint index, gpointer user_data) {
    ArtistOverflowData *d = user_data;
    const GPtrArray *ta = library_cache_get_track_artists(d->cache, d->track_id);
    if (!ta) return gtk_label_new("…");
    const library_track_artist_t *a = nth_artist_by_role(ta, d->role, index);
    if (!a) return gtk_label_new("…");
    return create_artist_button(a->artist_id, a->name, d->callbacks);
}

static GtkWidget *artist_create_overflow(guint first_hidden, guint total,
                                          gpointer user_data) {
    (void)first_hidden; (void)total;
    ArtistOverflowData *d = user_data;
    const GPtrArray *ta = library_cache_get_track_artists(d->cache, d->track_id);
    if (!ta) {
        GtkWidget *btn = gtk_button_new_with_label("…");
        gtk_widget_add_css_class(btn, "artist-btn");
        return btn;
    }

    GPtrArray *for_popover;
    if (d->overflow_all) {
        /* Combined mode: popover shows all artists (primary + featuring) */
        for_popover = g_ptr_array_new();
        for (guint i = 0; i < ta->len; i++)
            g_ptr_array_add(for_popover, g_ptr_array_index(ta, i));
    } else {
        for_popover = filter_artists_by_role(ta, d->role);
    }

    GtkWidget *btn = create_artist_overflow_button(for_popover, d->callbacks);
    g_ptr_array_free(for_popover, TRUE);
    return btn;
}

void populate_artist_buttons(GtkWidget *box,
                              GtkWidget *constraint_widget,
                              double constraint_fraction,
                              library_cache_t *cache,
                              int64_t track_id,
                              library_artist_role_t role,
                              RowCallbacks *callbacks,
                              gboolean add_feat_prefix) {
    ui_box_clear(GTK_BOX(box));

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
    int pinned = 0;

    if (add_feat_prefix) {
        GtkWidget *ft = gtk_label_new("ft ");
        gtk_widget_add_css_class(ft, "library-row-subtitle");
        gtk_box_append(GTK_BOX(box), ft);
        pinned = 1;
    }

    ArtistOverflowData *d = g_new0(ArtistOverflowData, 1);
    d->cache        = cache;
    d->track_id     = track_id;
    d->role         = role;
    d->callbacks    = callbacks ? g_memdup2(callbacks, sizeof(RowCallbacks)) : NULL;
    d->overflow_all = FALSE;

    ui_overflow_box_setup(&(UiOverflowBoxParams){
        .box                = box,
        .constraint_widget  = constraint_widget,
        .constraint_fraction = constraint_fraction,
        .default_max_width  = 200,
        .pinned_children    = pinned,
        .create_item        = artist_create_item,
        .create_overflow    = artist_create_overflow,
        .item_count         = count,
        .user_data          = d,
        .user_data_destroy  = artist_overflow_data_free,
    });
}

void populate_artist_buttons_combined(GtkWidget *box,
                                       GtkWidget *constraint_widget,
                                       double constraint_fraction,
                                       library_cache_t *cache,
                                       int64_t track_id,
                                       RowCallbacks *callbacks,
                                       gboolean show_primary) {
    ui_box_clear(GTK_BOX(box));

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
    int pinned = 0;

    /* Primary buttons: always shown, never overflowed */
    if (show_primary) {
        for (guint i = 0; i < primary_count; i++) {
            const library_track_artist_t *a =
                nth_artist_by_role(ta, LIBRARY_ARTIST_ROLE_PRIMARY, i);
            GtkWidget *btn = create_artist_button(a->artist_id, a->name, callbacks);
            gtk_box_append(GTK_BOX(box), btn);
            pinned++;
        }
    }

    if (feat_count == 0) return;

    /* "ft " separator: pinned between primary and featuring */
    GtkWidget *ft = gtk_label_new("ft ");
    gtk_widget_add_css_class(ft, "library-row-subtitle");
    gtk_box_append(GTK_BOX(box), ft);
    pinned++;

    ArtistOverflowData *d = g_new0(ArtistOverflowData, 1);
    d->cache        = cache;
    d->track_id     = track_id;
    d->role         = LIBRARY_ARTIST_ROLE_FEATURING;
    d->callbacks    = callbacks ? g_memdup2(callbacks, sizeof(RowCallbacks)) : NULL;
    d->overflow_all = TRUE;

    ui_overflow_box_setup(&(UiOverflowBoxParams){
        .box                = box,
        .constraint_widget  = constraint_widget,
        .constraint_fraction = constraint_fraction,
        .default_max_width  = 200,
        .pinned_children    = pinned,
        .create_item        = artist_create_item,
        .create_overflow    = artist_create_overflow,
        .item_count         = feat_count,
        .user_data          = d,
        .user_data_destroy  = artist_overflow_data_free,
    });
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Credit Role Pill Layout — via Overflow Box
 *
 * Fills credit_annotation box with role pills after the artist button.
 * Excess pills collapse into "+N more".
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  **roles;       /* Owned deep copies */
    guint   role_count;
} CreditPillData;

static void credit_pill_data_free(gpointer data) {
    CreditPillData *d = data;
    for (guint i = 0; i < d->role_count; i++)
        g_free(d->roles[i]);
    g_free(d->roles);
    g_free(d);
}

static GtkWidget *credit_create_item(guint index, gpointer user_data) {
    CreditPillData *d = user_data;
    GtkWidget *pill = gtk_label_new(d->roles[index]);
    gtk_widget_add_css_class(pill, "credit-role-label");
    return pill;
}

static GtkWidget *credit_create_overflow(guint first_hidden, guint total,
                                          gpointer user_data) {
    (void)user_data;
    guint remaining = total - first_hidden;
    char buf[32];
    snprintf(buf, sizeof(buf), "+%u more", remaining);
    GtkWidget *more = gtk_label_new(buf);
    gtk_widget_add_css_class(more, "credit-role-label");
    return more;
}

void populate_credit_pills(GtkWidget *credit_annotation,
                            GtkWidget *constraint_widget,
                            double constraint_fraction,
                            const char *const *roles,
                            guint role_count,
                            int first_child_width) {
    (void)first_child_width;  /* Pinned child width measured by overflow box */

    CreditPillData *d = g_new0(CreditPillData, 1);
    d->role_count = role_count;
    d->roles = g_new0(char *, role_count);
    for (guint i = 0; i < role_count; i++)
        d->roles[i] = g_strdup(roles[i]);

    ui_overflow_box_setup(&(UiOverflowBoxParams){
        .box                = credit_annotation,
        .constraint_widget  = constraint_widget,
        .constraint_fraction = constraint_fraction,
        .default_max_width  = 300,
        .pinned_children    = 1,  /* Artist button already in box */
        .create_item        = credit_create_item,
        .create_overflow    = credit_create_overflow,
        .item_count         = role_count,
        .user_data          = d,
        .user_data_destroy  = credit_pill_data_free,
    });
}
