/**
 * Quadrature Library Views
 *
 * GTK4 virtualized list views for browsing artists and albums.
 *
 * Model pipeline (GTK4 composable models):
 *   GListStore → GtkSortListModel → GtkFilterListModel → GtkSingleSelection → GtkListView
 *
 * The store is populated ONCE with all items for the enabled libraries.
 * Filter and sort changes invalidate their respective model wrappers —
 * GTK diffs old vs new results and emits minimal items-changed signals,
 * allowing efficient row recycling with zero allocation churn.
 *
 * Filtering uses a hybrid approach: the cache's SQL queries pre-compute
 * the set of matching entity IDs, and the GtkCustomFilter checks membership
 * in this set (O(1) per item). This reuses battle-tested SQL filtering
 * while gaining GTK's model diffing.
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

    /* GTK4 model pipeline objects (owned by model chain, not freed directly) */
    GtkCustomFilter *filter;
    GtkCustomSorter *sorter;

    /* Pre-computed filter state: set of entity IDs that pass current filters.
     * NULL = no filter active (all items pass). Rebuilt on each filter change. */
    GHashTable *filter_match_ids;

    /* Cancellable for in-flight async credit entity search */
    GCancellable *credit_cancel;

    /* Shared filter bar */
    FilterBarState filter_bar;

    /* Index scrubber overlay */
    GtkWidget *scrubber_overlay;    /* GtkOverlay wrapping scroll */
    GtkWidget *scrubber;            /* QuadScrubber widget */
    GtkWidget *scrubber_badge;      /* GtkLabel badge, sibling overlay child */
} ViewData;

static const char *VIEW_DATA_KEY = "library-view-data";

/* Forward declarations for functions used in async completion callbacks */
static void update_subtitle(ViewData *vd);
static void rebuild_scrubber_buckets(ViewData *vd);

static void view_data_free(gpointer data) {
    ViewData *vd = data;
    if (vd->credit_cancel) {
        g_cancellable_cancel(vd->credit_cancel);
        g_clear_object(&vd->credit_cancel);
    }
    filter_bar_destroy(&vd->filter_bar);
    g_clear_pointer(&vd->filter_match_ids, g_hash_table_unref);
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
/* ── Async Credit Entity Search (GTask-based) ─────────────────────────── */

/* Input for background credit entity search */
typedef struct {
    library_cache_t *cache;
    LibraryItemKind kind;
    char *credit_text;
    char *role_gid;
} CreditEntityInput;

static void credit_entity_input_free(gpointer data) {
    CreditEntityInput *in = data;
    if (!in) return;
    g_free(in->credit_text);
    g_free(in->role_gid);
    g_free(in);
}

/**
 * Build a set of global entity IDs matched by credit search.
 * Thread-safe: accesses only library_cache and DB handles (no GTK widgets).
 *
 * @param kind LIBRARY_ITEM_ALBUM → return album IDs, LIBRARY_ITEM_ARTIST → artist IDs
 */
static GHashTable *build_credit_entity_set_impl(library_cache_t *cache,
                                                  LibraryItemKind kind,
                                                  const char *credit_text,
                                                  const char *role_gid) {
    GHashTable *entity_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

    int lib_count = library_cache_get_library_count(cache);
    for (int li = 0; li < lib_count; li++) {
        int bi = library_cache_get_bitmap_index(cache, li);
        if (!library_cache_get_available(cache, bi)) continue;
        library_cache_dbs_t dbs = library_cache_get_dbs(cache, bi);
        if (!dbs.meta) continue;

        /* Search metadata artists matching credit text */
        db_meta_artist_search_result_t *artists = NULL;
        size_t artist_count = 0;
        quadrature_result_t res = db_meta_search_artists(
            dbs.meta, credit_text, 50, &artists, &artist_count);

        if (res != QUADRATURE_OK || artist_count == 0) {
            if (artists) db_meta_artist_search_results_free(artists, artist_count);
            continue;
        }

        gboolean have_lib_db = (dbs.db != NULL);

        for (size_t ai = 0; ai < artist_count; ai++) {
            const char *artist_mbid = artists[ai].artist_mbid;
            if (!artist_mbid) continue;

            db_meta_artist_credit_t *credits = NULL;
            size_t credit_count = 0;
            res = db_meta_get_credits_by_artist(
                dbs.meta, artist_mbid, role_gid, &credits, &credit_count);

            if (res == QUADRATURE_OK && credit_count > 0 && have_lib_db) {
                /* Batch-resolve all credit positions at once */
                db_track_position_t *positions = g_new0(db_track_position_t, credit_count);
                int64_t *track_ids = g_new0(int64_t, credit_count);

                for (size_t ci = 0; ci < credit_count; ci++) {
                    positions[ci].release_mbid = credits[ci].release_mbid;
                    positions[ci].disc_num = credits[ci].disc_num;
                    positions[ci].track_num = credits[ci].track_num;
                }

                db_resolve_track_positions_batch(dbs.db, positions, credit_count, track_ids);

                for (size_t ci = 0; ci < credit_count; ci++) {
                    if (track_ids[ci] == 0) continue;

                    int64_t global_track_id = LIBRARY_MAKE_GLOBAL_ID(li, track_ids[ci]);
                    const library_track_info_t *track =
                        library_cache_get_track(cache, global_track_id);
                    if (track) {
                        int64_t eid = (kind == LIBRARY_ITEM_ALBUM)
                                      ? track->album_id : track->artist_id;
                        if (!g_hash_table_contains(entity_ids, &eid)) {
                            int64_t *key = g_new(int64_t, 1);
                            *key = eid;
                            g_hash_table_add(entity_ids, key);
                        }
                    }
                }

                g_free(positions);
                g_free(track_ids);
            }
            db_meta_artist_credits_free(credits, credit_count);
        }

        db_meta_artist_search_results_free(artists, artist_count);
    }

    return entity_ids;
}

/* GTask worker thread for credit entity search */
static void credit_entity_search_thread(GTask *task, gpointer src,
                                          gpointer data, GCancellable *cancel) {
    (void)src;
    CreditEntityInput *in = data;

    if (g_cancellable_is_cancelled(cancel)) return;

    GHashTable *result = build_credit_entity_set_impl(
        in->cache, in->kind, in->credit_text, in->role_gid);

    if (g_cancellable_is_cancelled(cancel)) {
        g_hash_table_unref(result);
        return;
    }

    g_task_return_pointer(task, result, (GDestroyNotify)g_hash_table_unref);
}

/* Completion: apply credit set intersection and notify GTK filter */
static void on_credit_entity_search_done(GObject *src, GAsyncResult *res,
                                           gpointer data) {
    (void)src;
    ViewData *vd = data;
    GError *error = NULL;
    GHashTable *credit_set = g_task_propagate_pointer(G_TASK(res), &error);

    if (!credit_set) {
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("credit entity search failed: %s", error->message);
        g_clear_error(&error);
        return;
    }

    /* Intersect: remove items from filter_match_ids not in credit_set */
    if (vd->filter_match_ids) {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, vd->filter_match_ids);
        while (g_hash_table_iter_next(&iter, &key, NULL)) {
            if (!g_hash_table_contains(credit_set, key))
                g_hash_table_iter_remove(&iter);
        }
    }
    g_hash_table_unref(credit_set);

    /* Notify GTK that the filter has changed */
    gtk_filter_changed(GTK_FILTER(vd->filter), GTK_FILTER_CHANGE_DIFFERENT);

    /* Update dependent UI */
    update_subtitle(vd);
    rebuild_scrubber_buckets(vd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GtkCustomFilter — ID-set membership check
 *
 * When filters are active, filter_match_ids holds the set of entity IDs
 * that pass. The filter function is O(1) per item (hash lookup).
 * When no filters are active, filter_match_ids is NULL and all items pass.
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean view_filter_func(gpointer item, gpointer user_data) {
    ViewData *vd = user_data;

    /* "Show Featuring" filter — hide artists with no albums of their own */
    if (vd->kind == LIBRARY_ITEM_ARTIST && QUAD_IS_ARTIST_ITEM(item) &&
        !filter_bar_get_show_featuring(&vd->filter_bar)) {
        if (QUAD_ARTIST_ITEM(item)->info->album_count < 1)
            return FALSE;
    }

    if (!vd->filter_match_ids) return TRUE;

    int64_t eid;
    if (QUAD_IS_ARTIST_ITEM(item))
        eid = QUAD_ARTIST_ITEM(item)->info->artist_id;
    else
        eid = QUAD_ALBUM_ITEM(item)->info->album_id;

    return g_hash_table_contains(vd->filter_match_ids, &eid);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GtkCustomSorter — in-memory sort comparisons
 * ═══════════════════════════════════════════════════════════════════════════ */

static int safe_utf8_collate(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return g_utf8_collate(a, b);
}

static int artist_sort_func(gconstpointer a, gconstpointer b, gpointer user_data) {
    ViewData *vd = user_data;
    const library_artist_info_t *aa = QUAD_ARTIST_ITEM((gpointer)a)->info;
    const library_artist_info_t *bb = QUAD_ARTIST_ITEM((gpointer)b)->info;
    library_sort_t sort = filter_bar_get_sort(&vd->filter_bar);

    switch (sort) {
        case LIBRARY_SORT_NAME_ASC:
            return safe_utf8_collate(aa->name, bb->name);
        case LIBRARY_SORT_NAME_DESC:
            return safe_utf8_collate(bb->name, aa->name);
        case LIBRARY_SORT_RECENT:
            /* Higher ID = more recently added */
            return (bb->artist_id > aa->artist_id) - (bb->artist_id < aa->artist_id);
        default:
            return 0;
    }
}

static int album_sort_func(gconstpointer a, gconstpointer b, gpointer user_data) {
    ViewData *vd = user_data;
    const library_album_info_t *aa = QUAD_ALBUM_ITEM((gpointer)a)->info;
    const library_album_info_t *bb = QUAD_ALBUM_ITEM((gpointer)b)->info;
    library_sort_t sort = filter_bar_get_sort(&vd->filter_bar);

    switch (sort) {
        case LIBRARY_SORT_NAME_ASC:
            return safe_utf8_collate(aa->title, bb->title);
        case LIBRARY_SORT_NAME_DESC:
            return safe_utf8_collate(bb->title, aa->title);
        case LIBRARY_SORT_YEAR_ASC: {
            int cmp = (int)aa->year - (int)bb->year;
            return cmp != 0 ? cmp : safe_utf8_collate(aa->title, bb->title);
        }
        case LIBRARY_SORT_YEAR_DESC: {
            int cmp = (int)bb->year - (int)aa->year;
            return cmp != 0 ? cmp : safe_utf8_collate(aa->title, bb->title);
        }
        case LIBRARY_SORT_ARTIST_ASC: {
            int cmp = safe_utf8_collate(aa->artist_name, bb->artist_name);
            return cmp != 0 ? cmp : safe_utf8_collate(aa->title, bb->title);
        }
        case LIBRARY_SORT_RECENT:
            return (bb->album_id > aa->album_id) - (bb->album_id < aa->album_id);
        default:
            return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Factory Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void artist_row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GtkWidget *row = ui_create_artist_row_shell();
    gtk_list_item_set_child(li, row);
}

/**
 * Apply library-row-first / library-row-last classes based on position.
 * Handles both adding AND removing so unbind doesn't need to touch CSS at all.
 * GTK add/remove are no-ops when the class is already present/absent.
 */
static void sync_section_position_classes(GtkWidget *child, guint position, guint n_items) {
    if (position == 0)
        gtk_widget_add_css_class(child, "library-row-first");
    else
        gtk_widget_remove_css_class(child, "library-row-first");

    if (position == n_items - 1)
        gtk_widget_add_css_class(child, "library-row-last");
    else
        gtk_widget_remove_css_class(child, "library-row-last");
}

static void artist_row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    ViewData *vd = data;
    QuadArtistItem *item = QUAD_ARTIST_ITEM(gtk_list_item_get_item(li));
    GtkWidget *row = gtk_list_item_get_child(li);
    UiWindow *w = UI_WINDOW(vd->cbs.user_data);
    uint32_t mask = w ? w->library_mask : LIBRARY_MASK_ALL;
    ui_rebind_artist_row(row, item->info, vd->cache, vd->art_mgr, mask);
    guint pos = gtk_list_item_get_position(li);
    guint n = g_list_model_get_n_items(lazy_list_get_filtered_model(vd->lazy_list));
    sync_section_position_classes(row, pos, n);
}

static void artist_row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)li; (void)data;
    /* CSS classes are now managed entirely in bind via sync_section_position_classes */
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
    guint n = g_list_model_get_n_items(lazy_list_get_filtered_model(vd->lazy_list));
    sync_section_position_classes(row, pos, n);
}

static void album_row_unbind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)li; (void)data;
    /* CSS classes are now managed entirely in bind via sync_section_position_classes */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Activation Handlers
 *
 * Navigation is deferred to g_idle_add so the gesture/signal emission chain
 * completes before we tear down widget trees (which can invalidate objects
 * still referenced on the call stack, causing SIGSEGV).
 *
 * Position is relative to the filtered model (what the list view shows).
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
    GListModel *model = lazy_list_get_filtered_model(vd->lazy_list);
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
    GListModel *model = lazy_list_get_filtered_model(vd->lazy_list);
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
 * Store Population — load all items once (no filtering/sorting)
 *
 * Called on: initial load, library re-index, library mask change.
 * NOT called on filter/sort changes — those invalidate the model wrappers.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t view_library_mask(ViewData *vd) {
    UiWindow *w = UI_WINDOW(vd->cbs.user_data);
    return w ? w->library_mask : LIBRARY_MASK_ALL;
}

typedef GObject *(*ItemNewFn)(gconstpointer info);

static GObject *artist_item_new_wrap(gconstpointer info) {
    return G_OBJECT(quad_artist_item_new(info));
}
static GObject *album_item_new_wrap(gconstpointer info) {
    return G_OBJECT(quad_album_item_new(info));
}

typedef GPtrArray *(*CacheQueryFn)(library_cache_t *cache, library_sort_t sort,
                                    const char *search, const db_search_opts_t *opts,
                                    uint32_t mask);

/** Reload the base store with all items for the current library mask. */
static void reload_store(ViewData *vd) {
    GListStore *store = lazy_list_get_store(vd->lazy_list);
    guint old_count = g_list_model_get_n_items(G_LIST_MODEL(store));

    if (!vd->cache) {
        if (old_count > 0) g_list_store_remove_all(store);
        return;
    }

    uint32_t mask = view_library_mask(vd);
    CacheQueryFn query_fn;
    ItemNewFn item_new;

    if (vd->kind == LIBRARY_ITEM_ARTIST) {
        query_fn = (CacheQueryFn)library_cache_get_artists_filtered;
        item_new = artist_item_new_wrap;
    } else {
        query_fn = (CacheQueryFn)library_cache_get_albums_filtered;
        item_new = album_item_new_wrap;
    }

    /* Fetch ALL items (no search, no genre/year filters) */
    GPtrArray *all = query_fn(vd->cache, LIBRARY_SORT_NAME_ASC, NULL, NULL, mask);
    if (!all) {
        if (old_count > 0) g_list_store_remove_all(store);
        return;
    }

    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < all->len; i++)
        g_ptr_array_add(items, item_new(g_ptr_array_index(all, i)));

    g_list_store_splice(store, 0, old_count, (gpointer *)items->pdata, items->len);
    g_ptr_array_unref(items);
    g_ptr_array_unref(all);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Filter Invalidation — re-compute matching ID set
 *
 * Uses the cache's SQL queries to determine which items pass the current
 * filters, then stores the result as a GHashTable for O(1) lookup in
 * the GtkCustomFilter function.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void invalidate_filter(ViewData *vd) {
    /* Clear previous filter state */
    g_clear_pointer(&vd->filter_match_ids, g_hash_table_unref);

    /* Cancel any in-flight credit search */
    if (vd->credit_cancel) {
        g_cancellable_cancel(vd->credit_cancel);
        g_clear_object(&vd->credit_cancel);
    }

    if (!vd->cache || !filter_bar_is_active(&vd->filter_bar)) {
        /* No filter active — all items pass */
        gtk_filter_changed(GTK_FILTER(vd->filter), GTK_FILTER_CHANGE_LESS_STRICT);
        return;
    }

    gboolean metadata_mode = (filter_bar_get_search_mode(&vd->filter_bar) == FILTER_SEARCH_METADATA);
    const char *credit_text = metadata_mode ? filter_bar_get_search_text(&vd->filter_bar) : NULL;

    if (metadata_mode && credit_text) {
        /* ── Metadata mode: credit search is the sole text filter ──
         * Don't pass search text to the entity query — the credit set
         * determines which entities pass. Genre/year filters still apply. */

        const char **genre_arr = NULL;
        size_t genre_count = 0;
        db_search_opts_t filter_opts = filter_bar_build_search_opts(
            &vd->filter_bar, &genre_arr, &genre_count);
        const db_search_opts_t *opts = (genre_count > 0 || filter_opts.year_mask) ? &filter_opts : NULL;
        uint32_t mask = view_library_mask(vd);

        /* Start with all entities (no text filter), optionally narrowed by genre/year */
        CacheQueryFn query_fn = (vd->kind == LIBRARY_ITEM_ARTIST)
            ? (CacheQueryFn)library_cache_get_artists_filtered
            : (CacheQueryFn)library_cache_get_albums_filtered;

        GPtrArray *filtered = query_fn(vd->cache, LIBRARY_SORT_NAME_ASC, NULL, opts, mask);

        vd->filter_match_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
        if (filtered) {
            for (guint i = 0; i < filtered->len; i++) {
                int64_t eid;
                if (vd->kind == LIBRARY_ITEM_ARTIST)
                    eid = ((const library_artist_info_t *)g_ptr_array_index(filtered, i))->artist_id;
                else
                    eid = ((const library_album_info_t *)g_ptr_array_index(filtered, i))->album_id;

                if (!g_hash_table_contains(vd->filter_match_ids, &eid)) {
                    int64_t *key = g_new(int64_t, 1);
                    *key = eid;
                    g_hash_table_add(vd->filter_match_ids, key);
                }
            }
            g_ptr_array_unref(filtered);
        }
        g_free(genre_arr);

        /* Dispatch async credit search — will intersect with filter_match_ids on completion */
        int role_count = 0;
        const char **role_gids = filter_bar_get_selected_role_gids(&vd->filter_bar, &role_count);

        CreditEntityInput *input = g_new0(CreditEntityInput, 1);
        input->cache = vd->cache;
        input->kind = vd->kind;
        input->credit_text = g_strdup(credit_text);
        input->role_gid = (role_gids && role_count > 0) ? g_strdup(role_gids[0]) : NULL;
        g_free(role_gids);

        vd->credit_cancel = g_cancellable_new();
        GTask *task = g_task_new(NULL, vd->credit_cancel,
                                  on_credit_entity_search_done, vd);
        g_task_set_task_data(task, input, credit_entity_input_free);
        g_task_run_in_thread(task, credit_entity_search_thread);
        g_object_unref(task);

        /* Apply genre/year filter immediately; credit narrowing arrives async */
        gtk_filter_changed(GTK_FILTER(vd->filter), GTK_FILTER_CHANGE_DIFFERENT);
    } else {
        /* ── Default mode: text search + genre/year filters applied synchronously ── */

        CacheQueryFn query_fn = (vd->kind == LIBRARY_ITEM_ARTIST)
            ? (CacheQueryFn)library_cache_get_artists_filtered
            : (CacheQueryFn)library_cache_get_albums_filtered;

        const char *search_text = filter_bar_get_search_text(&vd->filter_bar);
        const char **genre_arr = NULL;
        size_t genre_count = 0;
        db_search_opts_t filter_opts = filter_bar_build_search_opts(
            &vd->filter_bar, &genre_arr, &genre_count);
        const db_search_opts_t *opts = (genre_count > 0 || filter_opts.year_mask) ? &filter_opts : NULL;
        uint32_t mask = view_library_mask(vd);

        GPtrArray *filtered = query_fn(vd->cache, LIBRARY_SORT_NAME_ASC, search_text, opts, mask);

        vd->filter_match_ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
        if (filtered) {
            for (guint i = 0; i < filtered->len; i++) {
                int64_t eid;
                if (vd->kind == LIBRARY_ITEM_ARTIST)
                    eid = ((const library_artist_info_t *)g_ptr_array_index(filtered, i))->artist_id;
                else
                    eid = ((const library_album_info_t *)g_ptr_array_index(filtered, i))->album_id;

                if (!g_hash_table_contains(vd->filter_match_ids, &eid)) {
                    int64_t *key = g_new(int64_t, 1);
                    *key = eid;
                    g_hash_table_add(vd->filter_match_ids, key);
                }
            }
            g_ptr_array_unref(filtered);
        }
        g_free(genre_arr);

        gtk_filter_changed(GTK_FILTER(vd->filter), GTK_FILTER_CHANGE_DIFFERENT);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Subtitle + Scrubber Updates (after filter/sort changes)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_subtitle(ViewData *vd) {
    if (!vd->subtitle) return;
    const char *noun = (vd->kind == LIBRARY_ITEM_ARTIST) ? "artist" : "album";
    guint total = g_list_model_get_n_items(G_LIST_MODEL(lazy_list_get_store(vd->lazy_list)));
    guint shown = g_list_model_get_n_items(lazy_list_get_filtered_model(vd->lazy_list));

    char buf[96];
    if (shown != total)
        snprintf(buf, sizeof(buf), "%u %s%s (%u shown)",
                 total, noun, total == 1 ? "" : "s", shown);
    else
        snprintf(buf, sizeof(buf), "%u %s%s",
                 total, noun, total == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(vd->subtitle), buf);
}

static void rebuild_scrubber_buckets(ViewData *vd) {
    if (!vd->scrubber) return;

    GListModel *model = lazy_list_get_filtered_model(vd->lazy_list);
    guint n = g_list_model_get_n_items(model);

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
        GObject *obj = g_list_model_get_item(model, i);
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Filter/Sort Change Callback
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_filter_changed(gpointer data) {
    ViewData *vd = data;

    /* Re-compute matching ID set and invalidate the GTK filter */
    invalidate_filter(vd);

    /* Invalidate the sorter (sort option may have changed too) */
    gtk_sorter_changed(GTK_SORTER(vd->sorter), GTK_SORTER_CHANGE_DIFFERENT);

    /* Update dependent UI */
    update_subtitle(vd);
    rebuild_scrubber_buckets(vd);
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
     * since filter signal handlers trigger on_filter_changed which needs the list) */
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

    /* ── Set up model pipeline: Sort → Filter ── */
    GCompareDataFunc sort_fn = (kind == LIBRARY_ITEM_ARTIST)
        ? artist_sort_func : album_sort_func;
    vd->sorter = gtk_custom_sorter_new(sort_fn, vd, NULL);
    lazy_list_set_sorter(vd->lazy_list, GTK_SORTER(vd->sorter));

    vd->filter = gtk_custom_filter_new((GtkCustomFilterFunc)view_filter_func, vd, NULL);
    lazy_list_set_filter(vd->lazy_list, GTK_FILTER(vd->filter));

    /* Set list view as child of scroll window */
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vd->scroll),
                                  lazy_list_get_widget(vd->lazy_list));

    /* Connect scroll monitoring */
    lazy_list_connect_scroll(vd->lazy_list, GTK_SCROLLED_WINDOW(vd->scroll));

    /* Smooth scroll — animate discrete wheel events instead of instant jumps */
    ui_smooth_scroll_attach(GTK_SCROLLED_WINDOW(vd->scroll));

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

        /* Show "Show Feat." toggle for artists view */
        if (kind == LIBRARY_ITEM_ARTIST && vd->filter_bar.show_featuring_toggle)
            gtk_widget_set_visible(vd->filter_bar.show_featuring_toggle, TRUE);

        /* (Role filter is built into the filter bar, shown via search mode dropdown) */
    }

    g_object_unref(builder);

    /* Populate store with all items, then update UI */
    reload_store(vd);
    update_subtitle(vd);
    rebuild_scrubber_buckets(vd);

    return box;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Refresh
 * ═══════════════════════════════════════════════════════════════════════════ */

void library_view_refresh(GtkWidget *view) {
    g_assert(view != NULL);
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd) return;

    /* Rebuild genre popover from (now-warm) cache */
    filter_bar_rebuild_genre_popover(&vd->filter_bar);

    /* Full reload: repopulate store, re-apply filter, update UI */
    reload_store(vd);
    invalidate_filter(vd);
    gtk_sorter_changed(GTK_SORTER(vd->sorter), GTK_SORTER_CHANGE_DIFFERENT);
    update_subtitle(vd);
    rebuild_scrubber_buckets(vd);
}

int64_t library_view_get_selected_track_id(GtkWidget *view) {
    g_assert(view != NULL);
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd) return 0;

    GObject *item = lazy_list_get_selected_item(vd->lazy_list);
    if (!item) return 0;

    if (vd->kind == LIBRARY_ITEM_ALBUM && QUAD_IS_ALBUM_ITEM(item)) {
        QuadAlbumItem *ai = QUAD_ALBUM_ITEM(item);
        return ai->info->first_track_id;
    }
    /* Artists don't have a single track to load */
    return 0;
}

void library_view_clear_filters(GtkWidget *view) {
    g_assert(view != NULL);
    ViewData *vd = g_object_get_data(G_OBJECT(view), VIEW_DATA_KEY);
    if (!vd) return;

    filter_bar_clear(&vd->filter_bar);
}
