/**
 * Quadrature Library Views
 *
 * GTK4 virtualized list views for browsing artists and albums.
 * Uses LazyList (GtkListView + GListStore) for efficient rendering.
 * Factory callbacks create/bind/unbind row widgets on demand.
 * Filter/sort state managed by the shared FilterBarState component.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Sort Option Definitions
 * ═══════════════════════════════════════════════════════════════════════════ */

static const FilterBarSortOption ARTIST_SORT_OPTIONS[] = {
    { "Name (A-Z)", LIBRARY_SORT_NAME_ASC },
    { "Name (Z-A)", LIBRARY_SORT_NAME_DESC },
    { "Recent",     LIBRARY_SORT_RECENT },
};
#define NUM_ARTIST_SORTS 3

static const FilterBarSortOption ALBUM_SORT_OPTIONS[] = {
    { "Name (A-Z)",      LIBRARY_SORT_NAME_ASC },
    { "Date (newest)",   LIBRARY_SORT_YEAR_DESC },
    { "Artist (A-Z)",    LIBRARY_SORT_ARTIST_ASC },
    { "Recent",          LIBRARY_SORT_RECENT },
};
#define NUM_ALBUM_SORTS 4

/* ═══════════════════════════════════════════════════════════════════════════
 * View Data - Attached to container widget
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LibraryItemKind kind;
    library_cache_t *cache;
    ArtworkManager *art_mgr;
    LibraryCallbacks cbs;
    app_settings_t *settings;       /* For credit search (library paths) */

    GtkWidget *container;
    GtkWidget *subtitle;
    GtkWidget *scroll;

    LazyList *lazy_list;

    /* Shared filter bar */
    FilterBarState filter_bar;

    /* Index scrubber overlay */
    GtkWidget *scrubber_overlay;    /* GtkOverlay wrapping scroll */
    GtkWidget *scrubber;            /* QuadScrubber widget */
    GtkWidget *scrubber_badge;      /* GtkLabel badge, sibling overlay child */
} ViewData;

static const char *VIEW_DATA_KEY = "library-view-data";

static void view_data_free(gpointer data) {
    ViewData *vd = data;
    filter_bar_destroy(&vd->filter_bar);
    lazy_list_free(vd->lazy_list);
    g_free(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Credit Search Post-Filter
 *
 * Builds a set of entity IDs (album or artist) matched by credit search.
 * Searches the metadata DB for artist names matching credit text, fetches
 * their credits (optionally role-filtered), resolves to main DB track IDs,
 * and derives the album/artist IDs from those tracks.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Build a set of global entity IDs matched by credit search.
 * Returns NULL if credit search is not active. Caller must g_hash_table_unref.
 *
 * @param kind LIBRARY_ITEM_ALBUM → return album IDs, LIBRARY_ITEM_ARTIST → artist IDs
 */
static GHashTable *build_credit_entity_set(ViewData *vd) {
    const char *credit_text = filter_bar_get_credit_text(&vd->filter_bar);
    if (!credit_text) return NULL;  /* No credit text → no credit filter */

    const char *role_gid = filter_bar_get_role_gid(&vd->filter_bar);

    GHashTable *entity_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

    for (int li = 0; li < vd->settings->library_path_count; li++) {
        const char *library_root = app_settings_get_library_data_path(vd->settings, li);

        quadrature_meta_db_t *meta_db = NULL;
        if (db_meta_open_readonly(library_root, &meta_db) != QUADRATURE_OK || !meta_db)
            continue;

        /* Search metadata artists matching credit text */
        db_meta_artist_search_result_t *artists = NULL;
        size_t artist_count = 0;
        quadrature_result_t res = db_meta_search_artists(
            meta_db, credit_text, 50, &artists, &artist_count);

        if (res != QUADRATURE_OK || artist_count == 0) {
            if (artists) db_meta_artist_search_results_free(artists, artist_count);
            db_meta_close(meta_db);
            continue;
        }

        /* Open main DB for positional bridge */
        char *db_path = g_build_filename(library_root, "quadrature.sqlite", NULL);
        quadrature_db_t *lib_db = NULL;
        gboolean have_lib_db = (db_open_readonly(db_path, &lib_db) == QUADRATURE_OK && lib_db);
        g_free(db_path);

        /* For each matched artist, get credits and resolve to entity IDs */
        for (size_t ai = 0; ai < artist_count; ai++) {
            const char *artist_mbid = artists[ai].artist_mbid;
            if (!artist_mbid) continue;

            db_meta_artist_credit_t *credits = NULL;
            size_t credit_count = 0;
            res = db_meta_get_credits_by_artist(
                meta_db, artist_mbid, role_gid, &credits, &credit_count);

            if (res == QUADRATURE_OK && credit_count > 0 && have_lib_db) {
                for (size_t ci = 0; ci < credit_count; ci++) {
                    db_meta_artist_credit_t *c = &credits[ci];
                    if (!c->release_mbid) continue;

                    int64_t local_track_id = 0;
                    if (db_get_track_by_position(lib_db, c->release_mbid,
                            c->disc_num, c->track_num, &local_track_id) == QUADRATURE_OK) {
                        int64_t global_track_id = LIBRARY_MAKE_GLOBAL_ID(li, local_track_id);
                        const library_track_info_t *track =
                            library_cache_get_track(vd->cache, global_track_id);
                        if (track) {
                            int64_t eid = (vd->kind == LIBRARY_ITEM_ALBUM)
                                          ? track->album_id : track->artist_id;
                            if (!g_hash_table_contains(entity_ids, &eid)) {
                                int64_t *key = g_new(int64_t, 1);
                                *key = eid;
                                g_hash_table_add(entity_ids, key);
                            }
                        }
                    }
                }
            }
            db_meta_artist_credits_free(credits, credit_count);
        }

        if (have_lib_db) db_close(lib_db);
        db_meta_artist_search_results_free(artists, artist_count);
        db_meta_close(meta_db);
    }

    return entity_ids;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Factory Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void artist_row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GtkWidget *row = ui_create_artist_row_shell();
    gtk_list_item_set_child(li, row);
}

/** Apply library-row-first / library-row-last classes based on position. */
static void apply_section_position_classes(GtkWidget *child, guint position, guint n_items) {
    if (position == 0)
        gtk_widget_add_css_class(child, "library-row-first");
    if (position == n_items - 1)
        gtk_widget_add_css_class(child, "library-row-last");
}

static void clear_section_position_classes(GtkWidget *child) {
    gtk_widget_remove_css_class(child, "library-row-first");
    gtk_widget_remove_css_class(child, "library-row-last");
}

static void artist_row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    QuadArtistItem *item = QUAD_ARTIST_ITEM(gtk_list_item_get_item(li));
    GtkWidget *row = gtk_list_item_get_child(li);
    ui_rebind_artist_row(row, item->info, vd->cache, vd->art_mgr);
    guint pos = gtk_list_item_get_position(li);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(lazy_list_get_store(vd->lazy_list)));
    apply_section_position_classes(row, pos, n);
}

static void artist_row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GtkWidget *child = gtk_list_item_get_child(li);
    if (child) clear_section_position_classes(child);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Album Factory Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void album_row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    GtkWidget *row = ui_create_album_row_shell();
    ui_row_attach_handlers(row, &vd->cbs.album_cbs);
    gtk_list_item_set_child(li, row);
}

static void album_row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    QuadAlbumItem *item = QUAD_ALBUM_ITEM(gtk_list_item_get_item(li));
    GtkWidget *row = gtk_list_item_get_child(li);
    ui_rebind_album_row(row, item->info, vd->cache, vd->art_mgr, TRUE, &vd->cbs.artist_cbs);
    guint pos = gtk_list_item_get_position(li);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(lazy_list_get_store(vd->lazy_list)));
    apply_section_position_classes(row, pos, n);
}

static void album_row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GtkWidget *child = gtk_list_item_get_child(li);
    if (child) clear_section_position_classes(child);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Activation Handlers
 *
 * Navigation is deferred to g_idle_add so the gesture/signal emission chain
 * completes before we tear down widget trees (which can invalidate objects
 * still referenced on the call stack, causing SIGSEGV).
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LibraryItemKind kind;
    int64_t id;
    LibraryCallbacks cbs;
} NavigateIdleData;

static gboolean navigate_idle(gpointer data) {
    NavigateIdleData *nd = data;
    if (nd->cbs.on_navigate)
        nd->cbs.on_navigate(nd->kind, nd->id, nd->cbs.user_data);
    g_free(nd);
    return G_SOURCE_REMOVE;
}

static void on_artist_activate(guint position, gpointer data) {
    ViewData *vd = data;
    GListModel *model = G_LIST_MODEL(lazy_list_get_store(vd->lazy_list));
    QuadArtistItem *item = QUAD_ARTIST_ITEM(g_list_model_get_item(model, position));
    if (!item) return;

    NavigateIdleData *nd = g_new0(NavigateIdleData, 1);
    nd->kind = LIBRARY_ITEM_ARTIST;
    nd->id = item->info->artist_id;
    nd->cbs = vd->cbs;
    g_idle_add(navigate_idle, nd);

    g_object_unref(item);
}

static void on_album_activate(guint position, gpointer data) {
    ViewData *vd = data;
    GListModel *model = G_LIST_MODEL(lazy_list_get_store(vd->lazy_list));
    QuadAlbumItem *item = QUAD_ALBUM_ITEM(g_list_model_get_item(model, position));
    if (!item) return;

    NavigateIdleData *nd = g_new0(NavigateIdleData, 1);
    nd->kind = LIBRARY_ITEM_ALBUM;
    nd->id = item->info->album_id;
    nd->cbs = vd->cbs;
    g_idle_add(navigate_idle, nd);

    g_object_unref(item);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * List Population
 * ═══════════════════════════════════════════════════════════════════════════ */

static void populate_artists(ViewData *vd) {
    GListStore *store = lazy_list_get_store(vd->lazy_list);
    g_list_store_remove_all(store);

    if (!vd->cache) return;

    library_sort_t sort = filter_bar_get_sort(&vd->filter_bar);
    gboolean filtering = filter_bar_is_active(&vd->filter_bar);

    if (filtering) {
        /* Get total count from unfiltered query */
        GPtrArray *all = library_cache_get_artists_filtered(vd->cache, sort, NULL, NULL);
        guint total = all ? all->len : 0;
        if (all) g_ptr_array_unref(all);

        /* Build filter opts */
        const char *search_text = filter_bar_get_search_text(&vd->filter_bar);

        const char **genre_arr = NULL;
        size_t genre_count = 0;
        db_search_opts_t filter_opts = filter_bar_build_search_opts(
            &vd->filter_bar, &genre_arr, &genre_count);
        const db_search_opts_t *opts = (genre_count > 0 || filter_opts.year_mask) ? &filter_opts : NULL;

        GPtrArray *filtered = library_cache_get_artists_filtered(
            vd->cache, sort, search_text, opts);

        /* Credit post-filter: intersect with credit-matched artist IDs */
        GHashTable *credit_set = build_credit_entity_set(vd);

        GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < filtered->len; i++) {
            const library_artist_info_t *artist = g_ptr_array_index(filtered, i);
            if (credit_set && !g_hash_table_contains(credit_set, &artist->artist_id))
                continue;
            g_ptr_array_add(items, quad_artist_item_new(artist));
        }
        guint shown = items->len;
        g_list_store_splice(store, 0, 0, (gpointer *)items->pdata, items->len);
        g_ptr_array_unref(items);

        if (credit_set) g_hash_table_unref(credit_set);

        if (vd->subtitle) {
            char buf[96];
            if (shown != total)
                snprintf(buf, sizeof(buf), "%u artist%s (%u shown)",
                         total, total == 1 ? "" : "s", shown);
            else
                snprintf(buf, sizeof(buf), "%u artist%s",
                         total, total == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(filtered);
        g_free(genre_arr);
    } else {
        GPtrArray *artists = library_cache_get_artists_filtered(vd->cache, sort, NULL, NULL);
        if (!artists) return;

        GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < artists->len; i++) {
            const library_artist_info_t *artist = g_ptr_array_index(artists, i);
            g_ptr_array_add(items, quad_artist_item_new(artist));
        }
        g_list_store_splice(store, 0, 0, (gpointer *)items->pdata, items->len);
        g_ptr_array_unref(items);

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

    library_sort_t sort = filter_bar_get_sort(&vd->filter_bar);
    gboolean filtering = filter_bar_is_active(&vd->filter_bar);

    if (filtering) {
        GPtrArray *all = library_cache_get_albums_filtered(vd->cache, sort, NULL, NULL);
        guint total = all ? all->len : 0;
        if (all) g_ptr_array_unref(all);

        const char *search_text = filter_bar_get_search_text(&vd->filter_bar);

        const char **genre_arr = NULL;
        size_t genre_count = 0;
        db_search_opts_t filter_opts = filter_bar_build_search_opts(
            &vd->filter_bar, &genre_arr, &genre_count);
        const db_search_opts_t *opts = (genre_count > 0 || filter_opts.year_mask) ? &filter_opts : NULL;

        GPtrArray *filtered = library_cache_get_albums_filtered(
            vd->cache, sort, search_text, opts);

        /* Credit post-filter: intersect with credit-matched album IDs */
        GHashTable *credit_set = build_credit_entity_set(vd);

        GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < filtered->len; i++) {
            const library_album_info_t *album = g_ptr_array_index(filtered, i);
            if (credit_set && !g_hash_table_contains(credit_set, &album->album_id))
                continue;
            g_ptr_array_add(items, quad_album_item_new(album));
        }
        guint shown = items->len;
        g_list_store_splice(store, 0, 0, (gpointer *)items->pdata, items->len);
        g_ptr_array_unref(items);

        if (credit_set) g_hash_table_unref(credit_set);

        if (vd->subtitle) {
            char buf[96];
            if (shown != total)
                snprintf(buf, sizeof(buf), "%u album%s (%u shown)",
                         total, total == 1 ? "" : "s", shown);
            else
                snprintf(buf, sizeof(buf), "%u album%s",
                         total, total == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(filtered);
        g_free(genre_arr);
    } else {
        GPtrArray *albums = library_cache_get_albums_filtered(vd->cache, sort, NULL, NULL);
        if (!albums) return;

        GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = 0; i < albums->len; i++) {
            const library_album_info_t *album = g_ptr_array_index(albums, i);
            g_ptr_array_add(items, quad_album_item_new(album));
        }
        g_list_store_splice(store, 0, 0, (gpointer *)items->pdata, items->len);
        g_ptr_array_unref(items);

        if (vd->subtitle) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%u album%s",
                     albums->len, albums->len == 1 ? "" : "s");
            gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
        }

        g_ptr_array_unref(albums);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scrubber Bucket Rebuilding
 * ═══════════════════════════════════════════════════════════════════════════ */

static void rebuild_scrubber_buckets(ViewData *vd) {
    if (!vd->scrubber) return;

    GListStore *store = lazy_list_get_store(vd->lazy_list);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(store));

    library_sort_t current_sort = filter_bar_get_sort(&vd->filter_bar);
    gboolean is_year = (current_sort == LIBRARY_SORT_YEAR_ASC ||
                        current_sort == LIBRARY_SORT_YEAR_DESC);
    gboolean show    = (current_sort == LIBRARY_SORT_NAME_ASC  ||
                        current_sort == LIBRARY_SORT_NAME_DESC  ||
                        current_sort == LIBRARY_SORT_ARTIST_ASC ||
                        is_year);

    if (!show || n == 0) {
        quad_scrubber_clear(QUAD_SCRUBBER(vd->scrubber));
        return;
    }

    GPtrArray *buckets = g_ptr_array_new_with_free_func(
        (GDestroyNotify)scrubber_bucket_free);

    char cur_key[16] = {0};

    for (guint i = 0; i < n; i++) {
        GObject *obj = g_list_model_get_item(G_LIST_MODEL(store), i);
        char key[16];

        if (is_year) {
            uint16_t year = QUAD_IS_ALBUM_ITEM(obj)
                            ? QUAD_ALBUM_ITEM(obj)->info->year
                            : 0;
            g_snprintf(key, sizeof(key), "%u", year > 0 ? (guint)year : 0u);
            if (year == 0) g_strlcpy(key, "?", sizeof(key));
        } else {
            const char *name = NULL;

            if (QUAD_IS_ARTIST_ITEM(obj)) {
                name = QUAD_ARTIST_ITEM(obj)->info->name;
            } else if (QUAD_IS_ALBUM_ITEM(obj)) {
                name = (current_sort == LIBRARY_SORT_ARTIST_ASC)
                       ? QUAD_ALBUM_ITEM(obj)->info->artist_name
                       : QUAD_ALBUM_ITEM(obj)->info->title;
            }

            if (!name || !name[0]) {
                g_strlcpy(key, "#", sizeof(key));
            } else {
                gunichar ch = g_unichar_toupper(g_utf8_get_char(name));
                if (g_unichar_isalpha(ch)) {
                    int len = g_unichar_to_utf8(ch, key);
                    key[len] = '\0';
                } else {
                    g_strlcpy(key, "#", sizeof(key));
                }
            }
        }

        if (strcmp(key, cur_key) != 0) {
            ScrubberBucket *b = g_new0(ScrubberBucket, 1);
            b->label    = g_strdup(key);
            b->position = i;
            b->count    = 1;
            g_ptr_array_add(buckets, b);
            g_strlcpy(cur_key, key, sizeof(cur_key));
        } else {
            ScrubberBucket *last = g_ptr_array_index(buckets, buckets->len - 1);
            last->count++;
        }

        g_object_unref(obj);
    }

    quad_scrubber_set_total(QUAD_SCRUBBER(vd->scrubber), n);
    quad_scrubber_set_buckets(QUAD_SCRUBBER(vd->scrubber), buckets);
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
    rebuild_scrubber_buckets(vd);
}

/* Filter bar callback: repopulate when any filter/sort changes */
static void on_filter_changed(gpointer data) {
    ViewData *vd = data;
    populate_view(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main View Constructor
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *library_view_new(LibraryItemKind kind,
                             library_cache_t *cache,
                             ArtworkManager *art_mgr,
                             const LibraryCallbacks *cbs,
                             app_settings_t *settings) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_list_view.ui");
    GtkWidget *box = GTK_WIDGET(gtk_builder_get_object(builder, "container"));
    g_object_ref(box);

    ViewData *vd = g_new0(ViewData, 1);
    vd->kind = kind;
    vd->settings = settings;
    vd->cache = cache;
    vd->art_mgr = art_mgr;
    if (cbs) vd->cbs = *cbs;
    vd->container = box;
    g_object_set_data_full(G_OBJECT(box), VIEW_DATA_KEY, vd, view_data_free);

    /* Get template widgets */
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    vd->subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    vd->scroll = GTK_WIDGET(gtk_builder_get_object(builder, "scroll"));

    /* Set title */
    const char *titles[] = {"Artists", "Albums", "Songs"};
    gtk_label_set_text(GTK_LABEL(title), titles[kind]);

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

    /* ── Scrubber overlay ──
     * Wrap the scroll window in a GtkOverlay so QuadScrubber can slide in. */
    vd->scrubber_overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(vd->scrubber_overlay, TRUE);

    g_object_ref(vd->scroll);
    gtk_box_remove(GTK_BOX(vd->container), vd->scroll);
    gtk_overlay_set_child(GTK_OVERLAY(vd->scrubber_overlay), vd->scroll);
    g_object_unref(vd->scroll);

    vd->scrubber = quad_scrubber_new();
    gtk_widget_set_halign(vd->scrubber, GTK_ALIGN_END);
    gtk_widget_set_valign(vd->scrubber, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(vd->scrubber_overlay), vd->scrubber);

    vd->scrubber_badge = gtk_label_new("");
    gtk_widget_add_css_class(vd->scrubber_badge, "scrubber-badge");
    gtk_widget_set_halign(vd->scrubber_badge, GTK_ALIGN_END);
    gtk_widget_set_valign(vd->scrubber_badge, GTK_ALIGN_START);
    gtk_widget_set_visible(vd->scrubber_badge, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(vd->scrubber_overlay), vd->scrubber_badge);

    quad_scrubber_set_list_view(QUAD_SCRUBBER(vd->scrubber),
                                GTK_LIST_VIEW(lazy_list_get_widget(vd->lazy_list)));
    quad_scrubber_set_vadj(QUAD_SCRUBBER(vd->scrubber),
                           gtk_scrolled_window_get_vadjustment(
                               GTK_SCROLLED_WINDOW(vd->scroll)));
    quad_scrubber_set_badge(QUAD_SCRUBBER(vd->scrubber), vd->scrubber_badge);

    gtk_box_append(GTK_BOX(vd->container), vd->scrubber_overlay);

    /* ── Filter bar setup ──
     * Initialize shared filter bar with view-specific sort options. */
    if (kind == LIBRARY_ITEM_ARTIST || kind == LIBRARY_ITEM_ALBUM) {
        const FilterBarSortOption *sort_opts;
        int sort_count;
        if (kind == LIBRARY_ITEM_ARTIST) {
            sort_opts = ARTIST_SORT_OPTIONS;
            sort_count = NUM_ARTIST_SORTS;
        } else {
            sort_opts = ALBUM_SORT_OPTIONS;
            sort_count = NUM_ALBUM_SORTS;
        }

        GtkWidget *bar = filter_bar_init(&vd->filter_bar, cache,
                                          sort_opts, sort_count,
                                          on_filter_changed, vd);

        /* Insert filter bar between subtitle and scrubber overlay */
        gtk_box_insert_child_after(GTK_BOX(vd->container), bar, vd->subtitle);

        /* Attach advanced credit search panel */
        filter_bar_attach_advanced(&vd->filter_bar, vd->container);
    }

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

    filter_bar_clear(&vd->filter_bar);
}
