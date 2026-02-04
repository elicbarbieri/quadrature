/**
 * Quadrature LazyList — Virtualized list with scroll monitoring
 *
 * Wraps GtkListView + GListStore + factory + scroll velocity tracking.
 * Provides reusable infrastructure for virtualized artist/album lists.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject Item Types — thin wrappers around cache-owned data
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _QuadArtistItemClass { GObjectClass parent; };
G_DEFINE_FINAL_TYPE(QuadArtistItem, quad_artist_item, G_TYPE_OBJECT)
static void quad_artist_item_class_init(QuadArtistItemClass *klass) { (void)klass; }
static void quad_artist_item_init(QuadArtistItem *self) { (void)self; }

QuadArtistItem *quad_artist_item_new(const library_artist_info_t *info) {
    QuadArtistItem *item = g_object_new(QUAD_TYPE_ARTIST_ITEM, NULL);
    item->info = info;
    return item;
}

struct _QuadAlbumItemClass { GObjectClass parent; };
G_DEFINE_FINAL_TYPE(QuadAlbumItem, quad_album_item, G_TYPE_OBJECT)
static void quad_album_item_class_init(QuadAlbumItemClass *klass) { (void)klass; }
static void quad_album_item_init(QuadAlbumItem *self) { (void)self; }

QuadAlbumItem *quad_album_item_new(const library_album_info_t *info) {
    QuadAlbumItem *item = g_object_new(QUAD_TYPE_ALBUM_ITEM, NULL);
    item->info = info;
    return item;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LazyList Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _LazyList {
    GListStore *store;
    GtkSingleSelection *selection;
    GtkWidget *list_view;

    /* Scroll monitoring */
    GtkAdjustment *vadj;
    gulong vadj_signal_id;
    double last_scroll_value;
    int64_t last_scroll_time_us;

    /* Callbacks */
    LazyListCallbacks cbs;
};

/* Forward declaration for activate handler */
static void on_list_activate(GtkListView *lv, guint position, gpointer data);

LazyList *lazy_list_new(GType item_type, const LazyListCallbacks *cbs) {
    g_assert(cbs != NULL);

    LazyList *ll = g_new0(LazyList, 1);
    ll->cbs = *cbs;

    /* GListStore → GtkSingleSelection → GtkListView */
    ll->store = g_list_store_new(item_type);
    ll->selection = gtk_single_selection_new(G_LIST_MODEL(ll->store));
    gtk_single_selection_set_autoselect(ll->selection, FALSE);
    gtk_single_selection_set_can_unselect(ll->selection, TRUE);

    /* Factory */
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    if (cbs->setup)
        g_signal_connect(factory, "setup", G_CALLBACK(cbs->setup), cbs->user_data);
    if (cbs->bind)
        g_signal_connect(factory, "bind", G_CALLBACK(cbs->bind), cbs->user_data);
    if (cbs->unbind)
        g_signal_connect(factory, "unbind", G_CALLBACK(cbs->unbind), cbs->user_data);

    /* ListView */
    ll->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(ll->selection), factory);
    gtk_widget_add_css_class(ll->list_view, "library-list");

    if (cbs->activate)
        g_signal_connect(ll->list_view, "activate", G_CALLBACK(on_list_activate), ll);

    return ll;
}

void lazy_list_free(LazyList *ll) {
    if (!ll) return;

    if (ll->vadj && ll->vadj_signal_id) {
        g_signal_handler_disconnect(ll->vadj, ll->vadj_signal_id);
        ll->vadj_signal_id = 0;
    }

    /* store and selection are owned by the list view */
    g_free(ll);
}

GtkWidget *lazy_list_get_widget(LazyList *ll) {
    g_assert(ll != NULL);
    return ll->list_view;
}

GListStore *lazy_list_get_store(LazyList *ll) {
    g_assert(ll != NULL);
    return ll->store;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scroll Velocity Monitoring
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_vadj_value_changed(GtkAdjustment *adj, gpointer data) {
    LazyList *ll = data;
    double value = gtk_adjustment_get_value(adj);
    int64_t now = g_get_monotonic_time();

    if (ll->last_scroll_time_us > 0) {
        /* Track velocity for future adaptive prefetch */
        (void)(value - ll->last_scroll_value);
        (void)(now - ll->last_scroll_time_us);
    }

    ll->last_scroll_value = value;
    ll->last_scroll_time_us = now;
}

void lazy_list_connect_scroll(LazyList *ll, GtkScrolledWindow *scroll) {
    g_assert(ll != NULL);
    g_assert(scroll != NULL);

    ll->vadj = gtk_scrolled_window_get_vadjustment(scroll);
    ll->vadj_signal_id = g_signal_connect(ll->vadj, "value-changed",
                                           G_CALLBACK(on_vadj_value_changed), ll);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Activate Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_list_activate(GtkListView *lv, guint position, gpointer data) {
    (void)lv;
    LazyList *ll = data;
    if (ll->cbs.activate)
        ll->cbs.activate(position, ll->cbs.user_data);
}
