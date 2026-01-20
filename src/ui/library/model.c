/**
 * Quadrature Library Model
 *
 * Unified GObject item type and lazy-loading GListModel.
 * Fetches pages on demand via GTask, returns placeholders while loading.
 */

#include "internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * LibraryItem Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

G_DEFINE_FINAL_TYPE(LibraryItem, library_item, G_TYPE_OBJECT)

static void library_item_finalize(GObject *obj) {
    LibraryItem *item = LIBRARY_ITEM(obj);
    g_free(item->name);
    g_free(item->secondary);
    g_free(item->tertiary);
    g_free(item->path);
    G_OBJECT_CLASS(library_item_parent_class)->finalize(obj);
}

static void library_item_class_init(LibraryItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = library_item_finalize;
}

static void library_item_init(LibraryItem *item) {
    memset(&item->kind, 0, sizeof(*item) - offsetof(LibraryItem, kind));
}

LibraryItem *library_item_new_placeholder(LibraryItemKind kind) {
    LibraryItem *item = g_object_new(LIBRARY_TYPE_ITEM, NULL);
    item->kind = kind;
    item->placeholder = TRUE;
    item->name = g_strdup("Loading...");
    item->secondary = g_strdup("");
    item->tertiary = g_strdup("");
    item->path = g_strdup("");
    return item;
}

LibraryItem *library_item_new_artist(const db_artist_t *a) {
    LibraryItem *item = g_object_new(LIBRARY_TYPE_ITEM, NULL);
    item->kind = LIBRARY_ITEM_ARTIST;
    item->id = a->id;
    item->name = g_strdup(a->name);
    item->count1 = a->album_count;
    item->count2 = a->track_count;
    return item;
}

LibraryItem *library_item_new_album(const db_album_t *a) {
    LibraryItem *item = g_object_new(LIBRARY_TYPE_ITEM, NULL);
    item->kind = LIBRARY_ITEM_ALBUM;
    item->id = a->id;
    item->name = g_strdup(a->title);
    item->secondary = g_strdup(a->artist_name);
    item->parent_id = a->artist_id;
    item->year = a->year;
    item->count1 = a->track_count;
    return item;
}

LibraryItem *library_item_new_track(const db_track_t *t) {
    LibraryItem *item = g_object_new(LIBRARY_TYPE_ITEM, NULL);
    item->kind = LIBRARY_ITEM_TRACK;
    item->id = t->id;
    item->name = g_strdup(t->title);
    item->secondary = g_strdup(t->artist);
    item->tertiary = g_strdup(t->album);
    item->path = g_strdup(t->path);
    item->parent_id = t->album_id;
    item->artist_id = t->artist_id;
    item->duration_ms = t->duration_ms;
    item->track_num = t->track_num;
    item->disc_num = t->disc_num > 0 ? t->disc_num : 1;
    item->year = t->year;
    return item;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LibraryModel Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _LibraryModel {
    GObject parent;
    LibraryItemKind kind;
    quadrature_db_t *db;
    LibraryCache *cache;
    db_sort_t sort;
    size_t total;
    gboolean total_known;
    GHashTable *pending;  /* page_num -> TRUE */
};

static void library_model_iface_init(GListModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(LibraryModel, library_model, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, library_model_iface_init))

/* Async fetch data */
typedef struct {
    LibraryModel *model;
    size_t offset;
    GPtrArray *items;
    size_t total;
} FetchData;

static GType library_model_get_item_type(GListModel *m) {
    (void)m;
    return LIBRARY_TYPE_ITEM;
}

static guint library_model_get_n_items(GListModel *m) {
    return (guint)LIBRARY_MODEL(m)->total;
}

static void trigger_fetch(LibraryModel *model, size_t page_offset);

static gpointer library_model_get_item(GListModel *m, guint pos) {
    LibraryModel *self = LIBRARY_MODEL(m);
    if (pos >= self->total) return NULL;

    size_t page_offset = (pos / LIBRARY_PAGE_SIZE) * LIBRARY_PAGE_SIZE;
    size_t idx = pos - page_offset;

    GPtrArray *page = NULL;
    size_t total = 0;

    if (library_cache_get_page(self->cache, self->kind, page_offset, &page, &total)) {
        if (idx < page->len) {
            LibraryItem *item = g_object_ref(g_ptr_array_index(page, idx));
            g_ptr_array_unref(page);
            return item;
        }
        g_ptr_array_unref(page);
    }

    /* Cache miss - trigger fetch */
    size_t page_num = pos / LIBRARY_PAGE_SIZE;
    if (!g_hash_table_contains(self->pending, GUINT_TO_POINTER(page_num)))
        trigger_fetch(self, page_offset);

    /* Return placeholder */
    return library_item_new_placeholder(self->kind);
}

static void library_model_iface_init(GListModelInterface *iface) {
    iface->get_item_type = library_model_get_item_type;
    iface->get_n_items = library_model_get_n_items;
    iface->get_item = library_model_get_item;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Async Fetch
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fetch_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    FetchData *fd = data;
    LibraryModel *model = fd->model;

    db_page_opts_t opts = {
        .offset = fd->offset,
        .limit = LIBRARY_PAGE_SIZE,
        .sort = model->sort
    };

    fd->items = g_ptr_array_new_with_free_func(g_object_unref);

    switch (model->kind) {
    case LIBRARY_ITEM_ARTIST: {
        db_artist_t *artists = NULL;
        size_t count = 0;
        if (db_get_artists_page(model->db, &opts, &artists, &count, &fd->total) == QUADRATURE_OK) {
            for (size_t i = 0; i < count; i++)
                g_ptr_array_add(fd->items, library_item_new_artist(&artists[i]));
            db_artists_free(artists, count);
        }
        break;
    }
    case LIBRARY_ITEM_ALBUM: {
        db_album_t *albums = NULL;
        size_t count = 0;
        if (db_get_albums_page(model->db, &opts, &albums, &count, &fd->total) == QUADRATURE_OK) {
            for (size_t i = 0; i < count; i++)
                g_ptr_array_add(fd->items, library_item_new_album(&albums[i]));
            db_albums_free(albums, count);
        }
        break;
    }
    case LIBRARY_ITEM_TRACK: {
        db_track_t *tracks = NULL;
        size_t count = 0;
        if (db_get_tracks_page(model->db, &opts, &tracks, &count, &fd->total) == QUADRATURE_OK) {
            for (size_t i = 0; i < count; i++)
                g_ptr_array_add(fd->items, library_item_new_track(&tracks[i]));
            db_tracks_free(tracks, count);
        }
        break;
    }
    }

    g_info("fetch_thread: kind=%d offset=%zu items=%u total=%zu",
           model->kind, fd->offset, fd->items ? fd->items->len : 0, fd->total);

    g_task_return_pointer(task, fd, NULL);
}

static void fetch_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src; (void)data;
    GError *err = NULL;
    FetchData *fd = g_task_propagate_pointer(G_TASK(res), &err);
    if (err) { g_error_free(err); return; }

    LibraryModel *model = fd->model;
    size_t page = fd->offset / LIBRARY_PAGE_SIZE;

    g_hash_table_remove(model->pending, GUINT_TO_POINTER(page));

    if (!fd->items || fd->items->len == 0) {
        g_warning("fetch_done: EMPTY result for kind=%d offset=%zu total=%zu",
                  model->kind, fd->offset, fd->total);
        if (fd->items) g_ptr_array_unref(fd->items);
        g_free(fd);
        return;
    }

    g_info("fetch_done: kind=%d offset=%zu items=%u total=%zu",
           model->kind, fd->offset, fd->items->len, fd->total);

    /* Store in cache */
    library_cache_put_page(model->cache, model->kind, fd->offset, fd->items, fd->total);
    library_cache_set_total(model->cache, model->kind, fd->total);

    /* Update total */
    gboolean total_changed = FALSE;
    if (fd->total != model->total) {
        size_t old = model->total;
        model->total = fd->total;
        model->total_known = TRUE;
        total_changed = TRUE;

        if (old < fd->total)
            g_list_model_items_changed(G_LIST_MODEL(model), old, 0, fd->total - old);
        else if (old > fd->total)
            g_list_model_items_changed(G_LIST_MODEL(model), fd->total, old - fd->total, 0);
    }

    /* Notify views to rebind items in fetched range */
    if (!total_changed) {
        g_list_model_items_changed(G_LIST_MODEL(model), fd->offset, fd->items->len, fd->items->len);
    }

    g_ptr_array_unref(fd->items);
    g_free(fd);
}

static void trigger_fetch(LibraryModel *model, size_t page_offset) {
    size_t page = page_offset / LIBRARY_PAGE_SIZE;
    if (g_hash_table_contains(model->pending, GUINT_TO_POINTER(page)))
        return;

    g_hash_table_add(model->pending, GUINT_TO_POINTER(page));

    FetchData *fd = g_new0(FetchData, 1);
    fd->model = model;
    fd->offset = page_offset;

    GTask *task = g_task_new(NULL, NULL, fetch_done, NULL);
    g_task_set_task_data(task, fd, NULL);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject
 * ═══════════════════════════════════════════════════════════════════════════ */

static void library_model_dispose(GObject *obj) {
    LibraryModel *self = LIBRARY_MODEL(obj);
    g_clear_pointer(&self->pending, g_hash_table_destroy);
    G_OBJECT_CLASS(library_model_parent_class)->dispose(obj);
}

static void library_model_class_init(LibraryModelClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = library_model_dispose;
}

static void library_model_init(LibraryModel *self) {
    self->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

LibraryModel *library_model_new(LibraryItemKind kind, quadrature_db_t *db, LibraryCache *cache) {
    g_info("library_model_new: kind=%d db=%p cache=%p", kind, (void*)db, (void*)cache);

    LibraryModel *m = g_object_new(LIBRARY_TYPE_MODEL, NULL);
    m->kind = kind;
    m->db = db;
    m->cache = cache;

    size_t total = 0;
    if (library_cache_get_total(cache, kind, &total)) {
        m->total = total;
        m->total_known = TRUE;
        g_info("library_model_new: kind=%d using cached total=%zu", kind, total);
    } else {
        g_info("library_model_new: kind=%d no cached total, triggering fetch", kind);
        trigger_fetch(m, 0);
    }

    return m;
}

void library_model_refresh(LibraryModel *m) {
    g_return_if_fail(LIBRARY_IS_MODEL(m));

    g_hash_table_remove_all(m->pending);

    size_t old = m->total;
    m->total = 0;
    m->total_known = FALSE;

    if (old > 0)
        g_list_model_items_changed(G_LIST_MODEL(m), 0, old, 0);

    trigger_fetch(m, 0);
}

void library_model_prefetch(LibraryModel *m, size_t offset) {
    g_return_if_fail(LIBRARY_IS_MODEL(m));
    size_t page = offset / LIBRARY_PAGE_SIZE;
    if (!g_hash_table_contains(m->pending, GUINT_TO_POINTER(page)))
        trigger_fetch(m, page * LIBRARY_PAGE_SIZE);
}

LibraryItemKind library_model_get_kind(LibraryModel *m) {
    g_return_val_if_fail(LIBRARY_IS_MODEL(m), LIBRARY_ITEM_ARTIST);
    return m->kind;
}

void library_model_set_sort(LibraryModel *m, db_sort_t sort) {
    g_return_if_fail(LIBRARY_IS_MODEL(m));
    if (m->sort == sort) return;
    m->sort = sort;
    library_model_refresh(m);
}

db_sort_t library_model_get_sort(LibraryModel *m) {
    g_return_val_if_fail(LIBRARY_IS_MODEL(m), DB_SORT_NAME_ASC);
    return m->sort;
}
