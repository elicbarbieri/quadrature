/**
 * Quadrature Artwork Manager
 *
 * Two-tier system:
 * - Thumbnails: 4-worker thread pool, LRU cache, mmapped atlas
 * - Full-size: posix_fadvise() prefetch, zero userspace overhead
 */

#define G_LOG_DOMAIN "quadrature"

#include "artwork_manager.h"
#include "quadrature/artwork_atlas.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>

#define ARTWORK_CACHE_DEFAULT_MAX_ENTRIES 1000
#define ARTWORK_MANAGER_DEFAULT_WORKERS 4
#define ARTWORK_LOAD_TIMEOUT_MS 5000  /* 5 second timeout for artwork loads */
#define LOAD_TIME_SAMPLE_COUNT 1000   /* Track last N load times for percentiles */

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
    ArtThumbCallback callback;
    gpointer user_data;
    GtkWidget *image;
} CallbackReg;

typedef struct {
    int64_t album_id;
    LoadPriority priority;
    GCancellable *cancellable;
    GSList *callbacks;
    gboolean is_prefetch;
} PendingLoad;

typedef struct {
    int64_t *album_ids;
    size_t count;
} ArtistAlbums;

typedef struct {
    ArtworkManager *mgr;
    int64_t album_id;
    gint64 start_time_us;  /* Monotonic time when task was queued */
} LoadTask;

struct _ArtworkManager {
    quadrature_db_t *db;

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

    /* Artist albums cache */
    GHashTable *artist_albums;  /* artist_id -> ArtistAlbums */
    GMutex artist_lock;

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
    _Atomic size_t load_failures;
    _Atomic size_t load_timeouts;

    /* Load time tracking for percentiles */
    GMutex load_times_lock;
    double load_times_ms[LOAD_TIME_SAMPLE_COUNT];  /* Circular buffer */
    size_t load_times_index;                        /* Next write position */
    size_t load_times_count;                        /* Total samples (up to LOAD_TIME_SAMPLE_COUNT) */

    /* Stats reporting timer */
    guint stats_timer_id;
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
    g_clear_object(&p->cancellable);
    g_slist_free_full(p->callbacks, (GDestroyNotify)callback_reg_free);
    g_free(p);
}

static void artist_albums_free(ArtistAlbums *a) {
    if (a) { g_free(a->album_ids); g_free(a); }
}

static void load_task_free(LoadTask *t) {
    if (t) g_free(t);
}

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void record_load_time(ArtworkManager *mgr, double load_time_ms) {
    g_mutex_lock(&mgr->load_times_lock);
    mgr->load_times_ms[mgr->load_times_index] = load_time_ms;
    mgr->load_times_index = (mgr->load_times_index + 1) % LOAD_TIME_SAMPLE_COUNT;
    if (mgr->load_times_count < LOAD_TIME_SAMPLE_COUNT) {
        mgr->load_times_count++;
    }
    g_mutex_unlock(&mgr->load_times_lock);
}

static void get_load_time_percentiles(ArtworkManager *mgr, double *p50, double *p90, double *p99) {
    g_mutex_lock(&mgr->load_times_lock);

    if (mgr->load_times_count == 0) {
        g_mutex_unlock(&mgr->load_times_lock);
        if (p50) *p50 = 0.0;
        if (p90) *p90 = 0.0;
        if (p99) *p99 = 0.0;
        return;
    }

    /* Copy and sort */
    double *sorted = g_new(double, mgr->load_times_count);
    memcpy(sorted, mgr->load_times_ms, mgr->load_times_count * sizeof(double));
    size_t count = mgr->load_times_count;
    g_mutex_unlock(&mgr->load_times_lock);

    qsort(sorted, count, sizeof(double), compare_double);

    if (p50) *p50 = sorted[(size_t)(count * 0.50)];
    if (p90) *p90 = sorted[(size_t)(count * 0.90)];
    if (p99) *p99 = sorted[count > 1 ? (size_t)(count * 0.99) : count - 1];

    g_free(sorted);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Frequency-Weighted Entry-Count Cache
 *
 * Instead of pure LRU (evict oldest), we scan the bottom portion of the queue
 * and evict the entry with the lowest access_count. This keeps frequently
 * accessed album art cached longer, even if not recently viewed.
 *
 * Uses entry count limit rather than memory - textures live in GPU VRAM and
 * the atlas is mmapped, so tracking "memory" was meaningless.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define EVICTION_SCAN_MIN 16      /* Minimum entries to scan for eviction */
#define EVICTION_SCAN_PERCENT 10  /* Scan bottom 10% of queue */

static void evict_if_needed(ArtworkManager *mgr) {
    while (g_hash_table_size(mgr->cache) > mgr->max_entries && !g_queue_is_empty(&mgr->lru)) {
        /* Scan bottom portion of LRU for lowest access_count */
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

        /* Fallback to pure LRU tail if scan found nothing */
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
    e->access_count++;  /* Increment frequency on each access */
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
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
    g_object_unref(stream);
    g_bytes_unref(bytes);
    if (!pixbuf) return NULL;

    GdkTexture *tex = gdk_texture_new_for_pixbuf(pixbuf);
    g_object_unref(pixbuf);
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
            e->access_count = 1;  /* First access */
            g_hash_table_insert(mgr->cache, &e->album_id, e);
            g_queue_push_head(&mgr->lru, e);
            e->lru_link = mgr->lru.head;
            evict_if_needed(mgr);
        }
        g_mutex_unlock(&mgr->cache_lock);
    }

    /* Invoke callbacks */
    for (GSList *l = lc->callbacks; l; l = l->next) {
        CallbackReg *reg = l->data;
        if (reg->image) {
            /* Weak pointer is still valid - widget exists, remove it */
            g_object_remove_weak_pointer(G_OBJECT(reg->image), (gpointer *)&reg->image);
            if (lc->texture) {
                gtk_widget_remove_css_class(reg->image, "artwork-loading");
                gtk_image_set_from_paintable(GTK_IMAGE(reg->image), GDK_PAINTABLE(lc->texture));
            }
        } else if (reg->callback) {
            reg->callback(mgr, lc->album_id, lc->texture, reg->user_data);
        }
        /* Note: if reg->image was NULLed by weak ref, no cleanup needed */
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

        /* Check for timeout before processing */
        gint64 now_us = g_get_monotonic_time();
        gint64 elapsed_ms = (now_us - task->start_time_us) / 1000;
        if (elapsed_ms > ARTWORK_LOAD_TIMEOUT_MS) {
            g_warning("ArtworkManager: load timeout for album_id=%" G_GINT64_FORMAT
                      " (waited %ldms, timeout=%dms)",
                      task->album_id, (long)elapsed_ms, ARTWORK_LOAD_TIMEOUT_MS);
            atomic_fetch_add(&mgr->load_timeouts, 1);

            /* Remove from pending and notify callbacks with NULL texture */
            g_mutex_lock(&mgr->pending_lock);
            PendingLoad *p = g_hash_table_lookup(mgr->pending, &task->album_id);
            GSList *callbacks = p ? p->callbacks : NULL;
            if (p) { p->callbacks = NULL; g_hash_table_remove(mgr->pending, &task->album_id); }
            g_mutex_unlock(&mgr->pending_lock);

            if (callbacks) {
                LoadComplete *lc = g_new0(LoadComplete, 1);
                lc->mgr = mgr;
                lc->album_id = task->album_id;
                lc->texture = NULL;
                lc->callbacks = callbacks;
                g_idle_add(complete_on_main, lc);
            }
            load_task_free(task);
            continue;
        }

        /* Check cancellation */
        g_mutex_lock(&mgr->pending_lock);
        PendingLoad *p = g_hash_table_lookup(mgr->pending, &task->album_id);
        gboolean cancelled = !p || (p->cancellable && g_cancellable_is_cancelled(p->cancellable));
        g_mutex_unlock(&mgr->pending_lock);
        if (cancelled) { load_task_free(task); continue; }

        GdkTexture *tex = NULL;

        /* Atlas lookup */
        gint64 load_start_us = g_get_monotonic_time();
        g_mutex_lock(&mgr->atlas_lock);
        const artwork_atlas_entry_t *entry = atlas_lookup(mgr, task->album_id);
        if (entry) {
            tex = atlas_load_texture(mgr, entry);
        }
        g_mutex_unlock(&mgr->atlas_lock);
        gint64 load_end_us = g_get_monotonic_time();
        double load_time_ms = (load_end_us - load_start_us) / 1000.0;

        if (tex) {
            atomic_fetch_add(&mgr->atlas_hits, 1);
            record_load_time(mgr, load_time_ms);
        } else {
            /* Log the failure - artwork not found in atlas */
            atomic_fetch_add(&mgr->load_failures, 1);
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

ArtworkManager *artwork_manager_new(quadrature_db_t *db, size_t max_entries) {
    ArtworkManager *mgr = g_new0(ArtworkManager, 1);
    mgr->db = db;
    mgr->max_entries = max_entries > 0 ? max_entries : ARTWORK_CACHE_DEFAULT_MAX_ENTRIES;
    mgr->atlas_fd = -1;

    g_mutex_init(&mgr->cache_lock);
    g_mutex_init(&mgr->pending_lock);
    g_mutex_init(&mgr->artist_lock);
    g_mutex_init(&mgr->atlas_lock);
    g_mutex_init(&mgr->load_times_lock);

    mgr->cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, NULL);
    g_queue_init(&mgr->lru);
    mgr->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, (GDestroyNotify)pending_load_free);
    mgr->artist_albums = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)artist_albums_free);
    mgr->load_queue = g_async_queue_new_full((GDestroyNotify)load_task_free);

    /* Load atlas */
    char *path = g_build_filename(g_get_user_data_dir(), "quadrature", "artwork.atlas", NULL);
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

    /* Enable stats reporting by default */
    artwork_manager_enable_stats_reporting(mgr, TRUE);

    return mgr;
}

void artwork_manager_free(ArtworkManager *mgr) {
    if (!mgr) return;

    /* Stop stats reporting */
    if (mgr->stats_timer_id != 0) {
        g_source_remove(mgr->stats_timer_id);
        mgr->stats_timer_id = 0;
    }

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

    g_mutex_lock(&mgr->artist_lock);
    g_hash_table_destroy(mgr->artist_albums);
    g_mutex_unlock(&mgr->artist_lock);

    g_mutex_lock(&mgr->atlas_lock);
    atlas_unmap(mgr);
    g_free(mgr->atlas_path);
    g_mutex_unlock(&mgr->atlas_lock);

    g_async_queue_unref(mgr->load_queue);
    g_mutex_clear(&mgr->cache_lock);
    g_mutex_clear(&mgr->pending_lock);
    g_mutex_clear(&mgr->artist_lock);
    g_mutex_clear(&mgr->atlas_lock);
    g_mutex_clear(&mgr->load_times_lock);
    g_free(mgr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Thumbnails
 * ═══════════════════════════════════════════════════════════════════════════ */

GdkTexture *artwork_manager_get_thumb(ArtworkManager *mgr, int64_t album_id) {
    if (!mgr) return NULL;

    g_mutex_lock(&mgr->cache_lock);
    CacheEntry *e = g_hash_table_lookup(mgr->cache, &album_id);
    if (e) {
        touch_entry(mgr, e);
        atomic_fetch_add(&mgr->hits, 1);
    } else {
        atomic_fetch_add(&mgr->misses, 1);
    }
    GdkTexture *tex = e ? e->texture : NULL;
    g_mutex_unlock(&mgr->cache_lock);
    return tex;
}

void artwork_manager_load_thumb(ArtworkManager *mgr, int64_t album_id,
                                 LoadPriority priority, GCancellable *cancel,
                                 ArtThumbCallback cb, gpointer data) {
    if (!mgr) return;

    /* Cache hit? */
    g_mutex_lock(&mgr->cache_lock);
    CacheEntry *e = g_hash_table_lookup(mgr->cache, &album_id);
    if (e) {
        touch_entry(mgr, e);
        atomic_fetch_add(&mgr->hits, 1);
        GdkTexture *tex = e->texture;
        g_mutex_unlock(&mgr->cache_lock);
        if (cb) cb(mgr, album_id, tex, data);
        return;
    }
    g_mutex_unlock(&mgr->cache_lock);
    atomic_fetch_add(&mgr->misses, 1);

    /* Already pending? */
    g_mutex_lock(&mgr->pending_lock);
    PendingLoad *p = g_hash_table_lookup(mgr->pending, &album_id);
    if (p) {
        CallbackReg *reg = g_new0(CallbackReg, 1);
        reg->callback = cb;
        reg->user_data = data;
        p->callbacks = g_slist_prepend(p->callbacks, reg);
        if (priority < p->priority) p->priority = priority;
        g_mutex_unlock(&mgr->pending_lock);
        return;
    }

    /* New request */
    p = g_new0(PendingLoad, 1);
    p->album_id = album_id;
    p->priority = priority;
    p->cancellable = cancel ? g_object_ref(cancel) : g_cancellable_new();
    CallbackReg *reg = g_new0(CallbackReg, 1);
    reg->callback = cb;
    reg->user_data = data;
    p->callbacks = g_slist_prepend(NULL, reg);
    g_hash_table_insert(mgr->pending, &p->album_id, p);
    g_mutex_unlock(&mgr->pending_lock);

    LoadTask *task = g_new0(LoadTask, 1);
    task->mgr = mgr;
    task->album_id = album_id;
    task->start_time_us = g_get_monotonic_time();
    g_async_queue_push(mgr->load_queue, task);
}

void artwork_manager_load_thumb_into(ArtworkManager *mgr, int64_t album_id,
                                      LoadPriority priority, GtkWidget *image,
                                      GCancellable *cancel) {
    if (!mgr || !image) return;

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

    /* Already pending? */
    g_mutex_lock(&mgr->pending_lock);
    PendingLoad *p = g_hash_table_lookup(mgr->pending, &album_id);
    if (p) {
        CallbackReg *reg = g_new0(CallbackReg, 1);
        reg->image = image;
        g_object_add_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
        p->callbacks = g_slist_prepend(p->callbacks, reg);
        if (priority < p->priority) p->priority = priority;
        g_mutex_unlock(&mgr->pending_lock);
        return;
    }

    /* New request */
    p = g_new0(PendingLoad, 1);
    p->album_id = album_id;
    p->priority = priority;
    p->cancellable = cancel ? g_object_ref(cancel) : g_cancellable_new();
    CallbackReg *reg = g_new0(CallbackReg, 1);
    reg->image = image;
    g_object_add_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
    p->callbacks = g_slist_prepend(NULL, reg);
    g_hash_table_insert(mgr->pending, &p->album_id, p);
    g_mutex_unlock(&mgr->pending_lock);

    LoadTask *task = g_new0(LoadTask, 1);
    task->mgr = mgr;
    task->album_id = album_id;
    task->start_time_us = g_get_monotonic_time();
    g_async_queue_push(mgr->load_queue, task);
}

void artwork_manager_prefetch_thumbs(ArtworkManager *mgr, const int64_t *album_ids, size_t count) {
    if (!mgr || !album_ids) return;

    for (size_t i = 0; i < count; i++) {
        g_mutex_lock(&mgr->cache_lock);
        gboolean cached = g_hash_table_contains(mgr->cache, &album_ids[i]);
        g_mutex_unlock(&mgr->cache_lock);
        if (cached) continue;

        g_mutex_lock(&mgr->pending_lock);
        gboolean pending = g_hash_table_contains(mgr->pending, &album_ids[i]);
        if (!pending) {
            PendingLoad *p = g_new0(PendingLoad, 1);
            p->album_id = album_ids[i];
            p->priority = LOAD_PRIORITY_PREFETCH;
            p->is_prefetch = TRUE;
            p->cancellable = g_cancellable_new();
            g_hash_table_insert(mgr->pending, &p->album_id, p);

            LoadTask *task = g_new0(LoadTask, 1);
            task->mgr = mgr;
            task->album_id = album_ids[i];
            task->start_time_us = g_get_monotonic_time();
            g_async_queue_push(mgr->load_queue, task);
        }
        g_mutex_unlock(&mgr->pending_lock);
    }
}

void artwork_manager_cancel_prefetches(ArtworkManager *mgr) {
    if (!mgr) return;

    g_mutex_lock(&mgr->pending_lock);
    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init(&iter, mgr->pending);
    while (g_hash_table_iter_next(&iter, NULL, &val)) {
        PendingLoad *p = val;
        if (p->is_prefetch && p->cancellable) {
            g_cancellable_cancel(p->cancellable);
            g_hash_table_iter_remove(&iter);
        }
    }
    g_mutex_unlock(&mgr->pending_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Full-Size (kernel page cache)
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, const char *art_path) {
    (void)mgr;  /* Not needed, but keeps API consistent */
    if (!art_path || !art_path[0]) return;

    int fd = open(art_path, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        posix_fadvise(fd, 0, st.st_size, POSIX_FADV_WILLNEED);
    }
    close(fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Artist Albums Cache
 * ═══════════════════════════════════════════════════════════════════════════ */

gboolean artwork_manager_get_artist_albums(ArtworkManager *mgr, int64_t artist_id,
                                            int64_t **album_ids, size_t *count) {
    if (!mgr || !album_ids || !count) return FALSE;

    g_mutex_lock(&mgr->artist_lock);
    ArtistAlbums *a = g_hash_table_lookup(mgr->artist_albums, &artist_id);
    if (!a) {
        g_mutex_unlock(&mgr->artist_lock);
        return FALSE;
    }
    *count = a->count;
    *album_ids = g_memdup2(a->album_ids, a->count * sizeof(int64_t));
    g_mutex_unlock(&mgr->artist_lock);
    return TRUE;
}

void artwork_manager_put_artist_albums(ArtworkManager *mgr, int64_t artist_id,
                                        const int64_t *album_ids, size_t count) {
    if (!mgr || !album_ids || count == 0) return;

    size_t n = count > 6 ? 6 : count;
    ArtistAlbums *a = g_new0(ArtistAlbums, 1);
    a->album_ids = g_memdup2(album_ids, n * sizeof(int64_t));
    a->count = n;

    g_mutex_lock(&mgr->artist_lock);
    int64_t *key = g_new(int64_t, 1);
    *key = artist_id;
    g_hash_table_insert(mgr->artist_albums, key, a);
    g_mutex_unlock(&mgr->artist_lock);
}

void artwork_manager_invalidate_artist_cache(ArtworkManager *mgr) {
    if (!mgr) return;
    g_mutex_lock(&mgr->artist_lock);
    g_hash_table_remove_all(mgr->artist_albums);
    g_mutex_unlock(&mgr->artist_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Cache Management
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_clear(ArtworkManager *mgr) {
    if (!mgr) return;

    g_mutex_lock(&mgr->pending_lock);
    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init(&iter, mgr->pending);
    while (g_hash_table_iter_next(&iter, NULL, &val)) {
        PendingLoad *p = val;
        if (p->cancellable) g_cancellable_cancel(p->cancellable);
    }
    g_hash_table_remove_all(mgr->pending);
    g_mutex_unlock(&mgr->pending_lock);

    g_mutex_lock(&mgr->cache_lock);
    GList *l = mgr->lru.head;
    while (l) { cache_entry_free(l->data); l = l->next; }
    g_queue_clear(&mgr->lru);
    g_hash_table_remove_all(mgr->cache);
    g_mutex_unlock(&mgr->cache_lock);
}

void artwork_manager_invalidate_album(ArtworkManager *mgr, int64_t album_id) {
    if (!mgr) return;

    g_mutex_lock(&mgr->cache_lock);
    CacheEntry *e = g_hash_table_lookup(mgr->cache, &album_id);
    if (e) {
        g_queue_delete_link(&mgr->lru, e->lru_link);
        g_hash_table_remove(mgr->cache, &album_id);
        cache_entry_free(e);
    }
    g_mutex_unlock(&mgr->cache_lock);

    g_mutex_lock(&mgr->pending_lock);
    PendingLoad *p = g_hash_table_lookup(mgr->pending, &album_id);
    if (p && p->cancellable) {
        g_cancellable_cancel(p->cancellable);
        g_hash_table_remove(mgr->pending, &album_id);
    }
    g_mutex_unlock(&mgr->pending_lock);
}

void artwork_manager_reload_atlas(ArtworkManager *mgr) {
    if (!mgr) return;

    g_mutex_lock(&mgr->atlas_lock);
    char *path = mgr->atlas_path ? g_strdup(mgr->atlas_path) :
                 g_build_filename(g_get_user_data_dir(), "quadrature", "artwork.atlas", NULL);
    atlas_load(mgr, path);
    g_free(path);
    g_mutex_unlock(&mgr->atlas_lock);

    /* Clear cache - atlas may have new data */
    artwork_manager_clear(mgr);
    artwork_manager_invalidate_artist_cache(mgr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Statistics
 *
 * Stats are interval-based: counters reset after each stats_report_timer cycle.
 * This gives meaningful per-interval metrics rather than cumulative all-time values.
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_stats(ArtworkManager *mgr, size_t *hits, size_t *misses,
                                size_t *evictions, size_t *atlas_hits,
                                size_t *load_failures, size_t *load_timeouts) {
    if (!mgr) return;
    if (hits) *hits = atomic_load(&mgr->hits);
    if (misses) *misses = atomic_load(&mgr->misses);
    if (evictions) *evictions = atomic_load(&mgr->evictions);
    if (atlas_hits) *atlas_hits = atomic_load(&mgr->atlas_hits);
    if (load_failures) *load_failures = atomic_load(&mgr->load_failures);
    if (load_timeouts) *load_timeouts = atomic_load(&mgr->load_timeouts);
}

void artwork_manager_get_load_time_stats(ArtworkManager *mgr,
                                          double *p50_ms, double *p90_ms, double *p99_ms) {
    if (!mgr) return;
    get_load_time_percentiles(mgr, p50_ms, p90_ms, p99_ms);
}

static gboolean stats_report_timer(gpointer data) {
    ArtworkManager *mgr = data;

    /* Read and reset interval counters atomically */
    size_t hits = atomic_exchange(&mgr->hits, 0);
    size_t misses = atomic_exchange(&mgr->misses, 0);
    size_t evictions = atomic_exchange(&mgr->evictions, 0);
    size_t atlas_hits = atomic_exchange(&mgr->atlas_hits, 0);
    size_t load_failures = atomic_exchange(&mgr->load_failures, 0);
    size_t load_timeouts = atomic_exchange(&mgr->load_timeouts, 0);

    g_mutex_lock(&mgr->cache_lock);
    size_t entry_count = g_hash_table_size(mgr->cache);
    g_mutex_unlock(&mgr->cache_lock);

    size_t total = hits + misses;

    /* Suppress log when idle - no activity conveys no useful information */
    if (total == 0 && evictions == 0 && load_failures == 0 && load_timeouts == 0) {
        return G_SOURCE_CONTINUE;
    }

    /* Calculate hit rate for this interval */
    double hit_rate = total > 0 ? (100.0 * hits / total) : 0.0;

    /* Get load time percentiles */
    double p50 = 0.0, p90 = 0.0, p99 = 0.0;
    get_load_time_percentiles(mgr, &p50, &p90, &p99);

    g_info("ArtworkCache: hit_rate=%.1f%% hits=%zu misses=%zu evictions=%zu "
           "atlas_hits=%zu failures=%zu timeouts=%zu entries=%zu/%zu "
           "load_time_ms(p50=%.1f p90=%.1f p99=%.1f)",
           hit_rate, hits, misses, evictions, atlas_hits,
           load_failures, load_timeouts,
           entry_count, mgr->max_entries,
           p50, p90, p99);

    /* Log warnings for concerning metrics */
    if (load_timeouts > 0) {
        g_warning("ArtworkCache: %zu artwork load(s) timed out (>%dms)",
                  load_timeouts, ARTWORK_LOAD_TIMEOUT_MS);
    }
    if (load_failures > 10) {
        g_warning("ArtworkCache: high failure rate - %zu album(s) not found in atlas",
                  load_failures);
    }

    return G_SOURCE_CONTINUE;
}

void artwork_manager_enable_stats_reporting(ArtworkManager *mgr, gboolean enable) {
    if (!mgr) return;

    if (enable && mgr->stats_timer_id == 0) {
        /* Dump initial stats immediately so we see the starting state */
        stats_report_timer(mgr);
        mgr->stats_timer_id = g_timeout_add_seconds(15, stats_report_timer, mgr);
        g_info("ArtworkCache: stats reporting enabled (every 15s)");
    } else if (!enable && mgr->stats_timer_id != 0) {
        g_source_remove(mgr->stats_timer_id);
        mgr->stats_timer_id = 0;
        g_info("ArtworkCache: stats reporting disabled");
    }
}
