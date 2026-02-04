/**
 * Quadrature Artwork Manager
 *
 * Worker thread pool + LRU cache + mmapped atlas for async thumbnail loading.
 */

#define G_LOG_DOMAIN "quadrature"

#include "artwork_manager.h"
#include "../../indexer/internal.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>

#define ARTWORK_CACHE_DEFAULT_MAX_ENTRIES 1000
#define ARTWORK_MANAGER_DEFAULT_WORKERS 4

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int64_t album_id;
    GdkTexture *texture;
    GList *lru_link;
    uint32_t access_count;  /* Frequency counter for weighted eviction */
} CacheEntry;

typedef struct {
    GtkWidget *image;
} CallbackReg;

typedef struct {
    int64_t album_id;
    GSList *callbacks;      /* list of CallbackReg */
} PendingLoad;

typedef struct {
    ArtworkManager *mgr;
    int64_t album_id;
} LoadTask;

struct _ArtworkManager {
    library_cache_t *library;
    int thumb_size;

    /* Texture cache (thumbnails only) */
    GHashTable *cache;          /* album_id -> CacheEntry */
    GQueue lru;
    GMutex cache_lock;
    size_t max_entries;

    /* Pending loads */
    GHashTable *pending;        /* album_id -> PendingLoad */
    GMutex pending_lock;

    /* Worker pool */
    GAsyncQueue *load_queue;
    GThread *workers[ARTWORK_MANAGER_DEFAULT_WORKERS];
    gboolean shutdown;

    /* Atlas */
    char *atlas_path;
    int atlas_fd;
    void *atlas_map;
    size_t atlas_size;
    const artwork_atlas_header_t *atlas_header;
    const artwork_atlas_entry_t *atlas_index;
    const uint8_t *atlas_data;
    GMutex atlas_lock;

    /* Stats (atomic) */
    _Atomic size_t hits;
    _Atomic size_t misses;
    _Atomic size_t evictions;
    _Atomic size_t atlas_hits;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cache_entry_free(CacheEntry *e) {
    if (e) { g_clear_object(&e->texture); g_free(e); }
}

static void callback_reg_free(CallbackReg *reg) {
    if (!reg) return;
    if (reg->image) {
        g_object_remove_weak_pointer(G_OBJECT(reg->image), (gpointer *)&reg->image);
    }
    g_free(reg);
}

static void pending_load_free(PendingLoad *p) {
    if (!p) return;
    g_slist_free_full(p->callbacks, (GDestroyNotify)callback_reg_free);
    g_free(p);
}

static void load_task_free(LoadTask *t) {
    g_free(t);
}

static char *compute_atlas_path(int thumb_size) {
    char size_dir[16];
    snprintf(size_dir, sizeof(size_dir), "%dpx", thumb_size);
    return g_build_filename(g_get_user_data_dir(), "quadrature", "art", size_dir, "artwork.atlas", NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Frequency-Weighted Entry-Count Cache
 *
 * Instead of pure LRU (evict oldest), we scan the bottom portion of the queue
 * and evict the entry with the lowest access_count. This keeps frequently
 * accessed album art cached longer, even if not recently viewed.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define EVICTION_SCAN_MIN 16
#define EVICTION_SCAN_PERCENT 10

static void evict_if_needed(ArtworkManager *mgr) {
    while (g_hash_table_size(mgr->cache) > mgr->max_entries && !g_queue_is_empty(&mgr->lru)) {
        guint queue_len = g_queue_get_length(&mgr->lru);
        guint scan_count = queue_len * EVICTION_SCAN_PERCENT / 100;
        if (scan_count < EVICTION_SCAN_MIN) scan_count = EVICTION_SCAN_MIN;
        if (scan_count > queue_len) scan_count = queue_len;

        GList *best_to_evict = NULL;
        uint32_t min_count = UINT32_MAX;

        GList *l = mgr->lru.tail;
        for (guint i = 0; l && i < scan_count; i++, l = l->prev) {
            CacheEntry *e = l->data;
            if (e->access_count < min_count) {
                min_count = e->access_count;
                best_to_evict = l;
            }
        }

        if (!best_to_evict) {
            best_to_evict = mgr->lru.tail;
        }

        CacheEntry *e = best_to_evict->data;
        atomic_fetch_add(&mgr->evictions, 1);
        g_hash_table_remove(mgr->cache, &e->album_id);
        g_queue_delete_link(&mgr->lru, best_to_evict);
        cache_entry_free(e);
    }
}

static void touch_entry(ArtworkManager *mgr, CacheEntry *e) {
    e->access_count++;
    if (e->lru_link) {
        g_queue_unlink(&mgr->lru, e->lru_link);
        g_queue_push_head_link(&mgr->lru, e->lru_link);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Atlas
 * ═══════════════════════════════════════════════════════════════════════════ */

static void atlas_unmap(ArtworkManager *mgr) {
    if (mgr->atlas_map && mgr->atlas_map != MAP_FAILED)
        munmap(mgr->atlas_map, mgr->atlas_size);
    if (mgr->atlas_fd >= 0) close(mgr->atlas_fd);
    mgr->atlas_map = NULL;
    mgr->atlas_fd = -1;
    mgr->atlas_header = NULL;
    mgr->atlas_index = NULL;
    mgr->atlas_data = NULL;
}

static void atlas_load(ArtworkManager *mgr, const char *path) {
    atlas_unmap(mgr);
    if (!path) return;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(artwork_atlas_header_t)) {
        close(fd); return;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }

    const artwork_atlas_header_t *h = map;
    if (memcmp(h->magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE) != 0 ||
        h->version != ARTWORK_ATLAS_VERSION) {
        munmap(map, st.st_size); close(fd); return;
    }

    mgr->atlas_fd = fd;
    mgr->atlas_map = map;
    mgr->atlas_size = st.st_size;
    mgr->atlas_header = h;
    mgr->atlas_index = (const artwork_atlas_entry_t *)((uint8_t *)map + sizeof(*h));
    mgr->atlas_data = (const uint8_t *)mgr->atlas_index + h->count * sizeof(artwork_atlas_entry_t);
    g_free(mgr->atlas_path);
    mgr->atlas_path = g_strdup(path);
    g_info("Atlas loaded: %s (%u entries)", path, h->count);
}

static const artwork_atlas_entry_t *atlas_lookup(ArtworkManager *mgr, int64_t album_id) {
    if (!mgr->atlas_header) return NULL;
    uint32_t lo = 0, hi = mgr->atlas_header->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int64_t mid_id = mgr->atlas_index[mid].album_id;
        if (mid_id == album_id) return &mgr->atlas_index[mid];
        if (mid_id < album_id) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static GdkTexture *atlas_load_texture(ArtworkManager *mgr, const artwork_atlas_entry_t *entry) {
    if (!mgr->atlas_data || !entry) return NULL;

    const uint8_t *png = mgr->atlas_data + entry->offset;
    GBytes *bytes = g_bytes_new(png, entry->size);
    GdkTexture *tex = gdk_texture_new_from_bytes(bytes, NULL);
    g_bytes_unref(bytes);
    return tex;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Worker Thread
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    ArtworkManager *mgr;
    int64_t album_id;
    GdkTexture *texture;
    GSList *callbacks;
} LoadComplete;

static gboolean complete_on_main(gpointer data) {
    LoadComplete *lc = data;
    ArtworkManager *mgr = lc->mgr;

    /* Cache the texture */
    if (lc->texture) {
        g_mutex_lock(&mgr->cache_lock);
        if (!g_hash_table_contains(mgr->cache, &lc->album_id)) {
            CacheEntry *e = g_new0(CacheEntry, 1);
            e->album_id = lc->album_id;
            e->texture = g_object_ref(lc->texture);
            e->access_count = 1;
            g_hash_table_insert(mgr->cache, &e->album_id, e);
            g_queue_push_head(&mgr->lru, e);
            e->lru_link = mgr->lru.head;
            evict_if_needed(mgr);
        }
        g_mutex_unlock(&mgr->cache_lock);
    }

    /* Set texture on all waiting images */
    for (GSList *l = lc->callbacks; l; l = l->next) {
        CallbackReg *reg = l->data;
        if (reg->image) {
            g_object_remove_weak_pointer(G_OBJECT(reg->image), (gpointer *)&reg->image);
            if (lc->texture) {
                gtk_widget_remove_css_class(reg->image, "artwork-loading");
                gtk_image_set_from_paintable(GTK_IMAGE(reg->image), GDK_PAINTABLE(lc->texture));
            }
        }
        g_free(reg);
    }
    g_slist_free(lc->callbacks);
    g_clear_object(&lc->texture);
    g_free(lc);
    return G_SOURCE_REMOVE;
}

static gpointer worker_func(gpointer data) {
    ArtworkManager *mgr = data;

    while (!g_atomic_int_get(&mgr->shutdown)) {
        LoadTask *task = g_async_queue_timeout_pop(mgr->load_queue, 100000);
        if (!task) continue;
        if (g_atomic_int_get(&mgr->shutdown)) { load_task_free(task); break; }

        /* Check if still pending */
        g_mutex_lock(&mgr->pending_lock);
        PendingLoad *p = g_hash_table_lookup(mgr->pending, &task->album_id);
        gboolean cancelled = !p;
        g_mutex_unlock(&mgr->pending_lock);
        if (cancelled) { load_task_free(task); continue; }

        /* Atlas lookup */
        GdkTexture *tex = NULL;
        g_mutex_lock(&mgr->atlas_lock);
        const artwork_atlas_entry_t *entry = atlas_lookup(mgr, task->album_id);
        if (entry) {
            tex = atlas_load_texture(mgr, entry);
        }
        g_mutex_unlock(&mgr->atlas_lock);

        if (tex) {
            atomic_fetch_add(&mgr->atlas_hits, 1);
        } else {
            g_debug("ArtworkManager: album_id=%" G_GINT64_FORMAT " not found in atlas",
                    task->album_id);
        }

        /* Complete on main thread */
        g_mutex_lock(&mgr->pending_lock);
        p = g_hash_table_lookup(mgr->pending, &task->album_id);
        GSList *callbacks = p ? p->callbacks : NULL;
        if (p) { p->callbacks = NULL; g_hash_table_remove(mgr->pending, &task->album_id); }
        g_mutex_unlock(&mgr->pending_lock);

        if (callbacks) {
            LoadComplete *lc = g_new0(LoadComplete, 1);
            lc->mgr = mgr;
            lc->album_id = task->album_id;
            lc->texture = tex ? g_object_ref(tex) : NULL;
            lc->callbacks = callbacks;
            g_idle_add(complete_on_main, lc);
        }

        g_clear_object(&tex);
        load_task_free(task);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

ArtworkManager *artwork_manager_new(library_cache_t *library, int cache_size, size_t cache_count) {
    ArtworkManager *mgr = g_new0(ArtworkManager, 1);
    mgr->library = library;
    mgr->thumb_size = cache_size > 0 ? cache_size : 48;
    mgr->max_entries = cache_count > 0 ? cache_count : ARTWORK_CACHE_DEFAULT_MAX_ENTRIES;
    mgr->atlas_fd = -1;

    g_mutex_init(&mgr->cache_lock);
    g_mutex_init(&mgr->pending_lock);
    g_mutex_init(&mgr->atlas_lock);

    mgr->cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, NULL);
    g_queue_init(&mgr->lru);
    mgr->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, (GDestroyNotify)pending_load_free);
    mgr->load_queue = g_async_queue_new_full((GDestroyNotify)load_task_free);

    /* Load atlas from computed path */
    char *path = compute_atlas_path(mgr->thumb_size);
    g_mutex_lock(&mgr->atlas_lock);
    atlas_load(mgr, path);
    g_mutex_unlock(&mgr->atlas_lock);
    g_free(path);

    /* Start workers */
    for (int i = 0; i < ARTWORK_MANAGER_DEFAULT_WORKERS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "art-%d", i);
        mgr->workers[i] = g_thread_new(name, worker_func, mgr);
    }

    return mgr;
}

void artwork_manager_free(ArtworkManager *mgr) {
    if (!mgr) return;

    g_atomic_int_set(&mgr->shutdown, TRUE);
    for (int i = 0; i < ARTWORK_MANAGER_DEFAULT_WORKERS; i++)
        if (mgr->workers[i]) g_thread_join(mgr->workers[i]);

    g_mutex_lock(&mgr->pending_lock);
    g_hash_table_destroy(mgr->pending);
    g_mutex_unlock(&mgr->pending_lock);

    g_mutex_lock(&mgr->cache_lock);
    GList *l = mgr->lru.head;
    while (l) { cache_entry_free(l->data); l = l->next; }
    g_queue_clear(&mgr->lru);
    g_hash_table_destroy(mgr->cache);
    g_mutex_unlock(&mgr->cache_lock);

    g_mutex_lock(&mgr->atlas_lock);
    atlas_unmap(mgr);
    g_free(mgr->atlas_path);
    g_mutex_unlock(&mgr->atlas_lock);

    g_async_queue_unref(mgr->load_queue);
    g_mutex_clear(&mgr->cache_lock);
    g_mutex_clear(&mgr->pending_lock);
    g_mutex_clear(&mgr->atlas_lock);
    g_free(mgr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Thumbnails
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_thumbnail(ArtworkManager *mgr, int64_t album_id, GtkWidget *image) {
    g_assert(mgr != NULL);
    g_assert(image != NULL);

    /* Cache hit? */
    g_mutex_lock(&mgr->cache_lock);
    CacheEntry *e = g_hash_table_lookup(mgr->cache, &album_id);
    if (e) {
        touch_entry(mgr, e);
        atomic_fetch_add(&mgr->hits, 1);
        gtk_widget_remove_css_class(image, "artwork-loading");
        gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(e->texture));
        g_mutex_unlock(&mgr->cache_lock);
        return;
    }
    g_mutex_unlock(&mgr->cache_lock);

    /* Set placeholder */
    gtk_image_set_from_icon_name(GTK_IMAGE(image), "media-optical-symbolic");
    gtk_widget_add_css_class(image, "artwork-loading");
    atomic_fetch_add(&mgr->misses, 1);

    /* Already pending? Coalesce. */
    g_mutex_lock(&mgr->pending_lock);
    PendingLoad *p = g_hash_table_lookup(mgr->pending, &album_id);
    if (p) {
        CallbackReg *reg = g_new0(CallbackReg, 1);
        reg->image = image;
        g_object_add_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
        p->callbacks = g_slist_prepend(p->callbacks, reg);
        g_mutex_unlock(&mgr->pending_lock);
        return;
    }

    /* New request */
    p = g_new0(PendingLoad, 1);
    p->album_id = album_id;
    CallbackReg *reg = g_new0(CallbackReg, 1);
    reg->image = image;
    g_object_add_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
    p->callbacks = g_slist_prepend(NULL, reg);
    g_hash_table_insert(mgr->pending, &p->album_id, p);
    g_mutex_unlock(&mgr->pending_lock);

    LoadTask *task = g_new0(LoadTask, 1);
    task->mgr = mgr;
    task->album_id = album_id;
    g_async_queue_push(mgr->load_queue, task);
}

int artwork_manager_get_thumb_size(ArtworkManager *mgr) {
    g_assert(mgr != NULL);
    return mgr->thumb_size;
}

void artwork_manager_reload_atlas(ArtworkManager *mgr) {
    g_assert(mgr != NULL);

    /* Clear texture cache */
    g_mutex_lock(&mgr->cache_lock);
    GList *l = mgr->lru.head;
    while (l) { cache_entry_free(l->data); l = l->next; }
    g_queue_clear(&mgr->lru);
    g_hash_table_remove_all(mgr->cache);
    g_mutex_unlock(&mgr->cache_lock);

    /* Reload atlas from computed path */
    char *path = compute_atlas_path(mgr->thumb_size);
    g_mutex_lock(&mgr->atlas_lock);
    atlas_load(mgr, path);
    g_mutex_unlock(&mgr->atlas_lock);
    g_free(path);
}

void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, int64_t album_id) {
    if (!mgr || !mgr->library || album_id <= 0) return;
    library_cache_prefetch_fullsize_artwork(mgr->library, album_id);
}
