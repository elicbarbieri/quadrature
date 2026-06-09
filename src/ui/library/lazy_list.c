/**
 * Quadrature LazyList — Virtualized list with composable model pipeline
 *
 * Wraps GtkListView with a composable model chain:
 *   GListStore → GtkSortListModel → GtkFilterListModel → GtkSingleSelection
 *
 * Filter and sort changes invalidate their respective models without
 * rebuilding the store — GTK diffs the old vs new results and emits
 * minimal items-changed signals, allowing efficient row recycling.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject Item Types — thin wrappers around cache-owned data
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _QuadArtistItemClass {
    GObjectClass parent;
};
G_DEFINE_FINAL_TYPE(QuadArtistItem, quad_artist_item, G_TYPE_OBJECT)
static void
quad_artist_item_class_init(QuadArtistItemClass *klass)
{
    (void)klass;
}
static void
quad_artist_item_init(QuadArtistItem *self)
{
    (void)self;
}

QuadArtistItem *
quad_artist_item_new(const library_artist_info_t *info)
{
    QuadArtistItem *item = g_object_new(QUAD_TYPE_ARTIST_ITEM, NULL);
    item->info = info;
    return item;
}

struct _QuadAlbumItemClass {
    GObjectClass parent;
};
G_DEFINE_FINAL_TYPE(QuadAlbumItem, quad_album_item, G_TYPE_OBJECT)
static void
quad_album_item_class_init(QuadAlbumItemClass *klass)
{
    (void)klass;
}
static void
quad_album_item_init(QuadAlbumItem *self)
{
    (void)self;
}

QuadAlbumItem *
quad_album_item_new(const library_album_info_t *info)
{
    QuadAlbumItem *item = g_object_new(QUAD_TYPE_ALBUM_ITEM, NULL);
    item->info = info;
    return item;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LazyList Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _LazyList {
    GListStore *store;             /* Base data (all items, unfiltered) */
    GtkSortListModel *sorted;      /* Sorts store contents */
    GtkFilterListModel *filtered;  /* Filters sorted contents */
    GtkSingleSelection *selection; /* Selection over filtered view */
    GtkWidget *list_view;

    /* Scroll monitoring */
    GtkAdjustment *vadj;

    /* Callbacks */
    LazyListCallbacks cbs;
};

/* Forward declaration for activate handler */
static void on_list_activate(GtkListView *lv, guint position, gpointer data);

LazyList *
lazy_list_new(GType item_type, const LazyListCallbacks *cbs)
{
    g_assert(cbs != NULL);

    LazyList *ll = g_new0(LazyList, 1);
    ll->cbs = *cbs;

    /* Model pipeline: GListStore → Sort → Filter → Selection → ListView */
    ll->store = g_list_store_new(item_type);

    ll->sorted = gtk_sort_list_model_new(G_LIST_MODEL(ll->store), NULL);
    ll->filtered = gtk_filter_list_model_new(G_LIST_MODEL(ll->sorted), NULL);

    ll->selection = gtk_single_selection_new(G_LIST_MODEL(ll->filtered));
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

void
lazy_list_free(LazyList *ll)
{
    if (!ll)
        return;
    /* store, sorted, filtered, selection are owned by the list view */
    g_free(ll);
}

GtkWidget *
lazy_list_get_widget(LazyList *ll)
{
    g_assert(ll != NULL);
    return ll->list_view;
}

GListStore *
lazy_list_get_store(LazyList *ll)
{
    g_assert(ll != NULL);
    return ll->store;
}

GListModel *
lazy_list_get_filtered_model(LazyList *ll)
{
    g_assert(ll != NULL);
    return G_LIST_MODEL(ll->filtered);
}

GObject *
lazy_list_get_selected_item(LazyList *ll)
{
    g_assert(ll != NULL);
    return gtk_single_selection_get_selected_item(ll->selection);
}

void
lazy_list_set_filter(LazyList *ll, GtkFilter *filter)
{
    g_assert(ll != NULL);
    gtk_filter_list_model_set_filter(ll->filtered, filter);
}

void
lazy_list_set_sorter(LazyList *ll, GtkSorter *sorter)
{
    g_assert(ll != NULL);
    gtk_sort_list_model_set_sorter(ll->sorted, sorter);
}

void
lazy_list_connect_scroll(LazyList *ll, GtkScrolledWindow *scroll)
{
    g_assert(ll != NULL);
    g_assert(scroll != NULL);

    ll->vadj = gtk_scrolled_window_get_vadjustment(scroll);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Activate Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
on_list_activate(GtkListView *lv, guint position, gpointer data)
{
    (void)lv;
    LazyList *ll = data;
    if (ll->cbs.activate)
        ll->cbs.activate(position, ll->cbs.user_data);
}
