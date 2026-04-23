/**
 * Quadrature Artwork Manager
 *
 * Worker thread pool + per-library LRU cache + mmapped atlases for async thumbnail loading.
 *
 * Album IDs are global IDs (LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id)).
 * LIBRARY_GLOBAL_ID_LIB(id)   → which per-library atlas to search
 * LIBRARY_GLOBAL_ID_LOCAL(id) → binary search key within that atlas
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "../../core/internal.h"
#include "../../indexer/internal.h"
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>
#include <string.h>

#define ARTWORK_CACHE_DEFAULT_MAX_ENTRIES 1000
#define ARTIST_CACHE_DEFAULT_MAX_ENTRIES 500
#define ARTWORK_MANAGER_DEFAULT_WORKERS 4

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Task type tag for shared worker pool */
typedef enum {
    LOAD_ALBUM = 0,
    LOAD_ARTIST = 1,
    LOAD_FULLSIZE = 2,
} LoadTaskType;

/* Per-library atlas slot — bundles reader + paths, addressed via bitmap_map.
 * Mirrors library_cache's LibrarySlot pattern for consistent multi-library
 * indirection across the codebase. */
typedef struct {
    int bitmap_index;                   /* Stable library ID (matches library_cache) */
    artwork_atlas_reader_t *reader;     /* mmap'd atlas (or NULL if no atlas yet) */
    char *data_root;                    /* Library data root (for fanart fallback) */
    char *music_root;                   /* Music file root (for embedded art fallback) */
} ArtworkSlot;

typedef struct {
    int64_t id;          /* album_id or artist_id */
    GdkTexture *texture;
    GList *lru_link;
    uint32_t access_count;
} CacheEntry;

/* Shared frequency-weighted LRU texture cache (main-thread-only, no lock).
 * One instance per texture type (album, artist). */
typedef struct {
    GHashTable *map;         /* id → CacheEntry* */
    GQueue      lru;
    size_t      max_entries;
    _Atomic size_t hits;
    _Atomic size_t misses;
    _Atomic size_t evictions;
} TextureCache;

typedef struct {
    GtkWidget *image;
} CallbackReg;

typedef struct {
    int64_t id;
    GSList *callbacks;
} PendingLoad;

typedef struct {
    ArtworkManager *mgr;
    int64_t id;            /* Global album_id or artist_id */
    LoadTaskType type;     /* LOAD_ALBUM, LOAD_ARTIST, or LOAD_FULLSIZE */
} LoadTask;

struct _ArtworkManager {
    library_cache_t *library;
    int thumb_size;

    /* Per-library atlas slots, addressed by bitmap_index via bitmap_map.
     * Mirrors library_cache's slot + bitmap_map architecture:
     *   bitmap_map[bitmap_index] → &slots[position]
     * Reads are lock-free (g_atomic_pointer_get); writes under atlas_rwlock. */
    ArtworkSlot *slots;               /* Dense array of slots */
    int slot_count;
    ArtworkSlot **bitmap_map;         /* [bitmap_index] → &ArtworkSlot (atomic reads) */
    int bitmap_capacity;
    GRWLock atlas_rwlock;             /* Write: reload/add/remove. Read: worker lookups */

    /* Global artist atlas (UUID-keyed, shared across all libraries) */
    artist_atlas_reader_t *artist_atlas;
    char *artist_atlas_path;           /* Path for reload detection */

    /* Album + artist texture caches.
     *
     * NO LOCK NEEDED: every access (reads in bind callbacks, writes in
     * complete_on_main idle callbacks, evictions in reload/remove signal
     * handlers) runs on the GTK main thread.  Worker threads never touch
     * them — they post results via g_idle_add(). */
    TextureCache album_cache;
    TextureCache artist_cache;

    /* Pending loads — indexed by LoadTaskType (ALBUM / ARTIST / FULLSIZE). */
    GHashTable *pending[3];
    GMutex pending_locks[3];

    /* Worker pool (shared between album and artist loads) */
    GAsyncQueue *load_queue;
    GThread *workers[ARTWORK_MANAGER_DEFAULT_WORKERS];
    gboolean shutdown;

    /* Stats (atomic) — atlas_hits counts atlas-decode successes from workers */
    _Atomic size_t atlas_hits;

    /* Latency histograms (µs scale, lock-free recording) */
    perf_histogram_us_t texture_hit_hist;   /* lookup + LRU touch time */
    perf_histogram_us_t atlas_decode_hist;  /* atlas read + texture create time */

    _Atomic uint64_t hit_sample_counter;    /* mod-64 sampling for cache hit timing */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cache_entry_free(CacheEntry *e) {
    if (e) { g_clear_object(&e->texture); g_free(e); }
}

static void callback_reg_free(CallbackReg *reg) {
    if (!reg) return;
    if (reg->image)
        g_object_remove_weak_pointer(G_OBJECT(reg->image), (gpointer *)&reg->image);
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

/**
 * Scan {library_root}/artwork/ for "{N}px-artwork-*.atlas" and return the path
 * with the highest timestamp. Lexicographic order is correct because Unix timestamps
 * are fixed 10-digit numbers. Caller must g_free() the result.
 */
static char *find_latest_atlas(const char *library_root, int thumb_size) {
    char *artwork_dir = g_build_filename(library_root, "artwork", NULL);
    GDir *dir = g_dir_open(artwork_dir, 0, NULL);
    if (!dir) { g_free(artwork_dir); return NULL; }

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%dpx-artwork-", thumb_size);

    char *latest = NULL;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, prefix) || !g_str_has_suffix(name, ".atlas")) continue;
        char *full = g_build_filename(artwork_dir, name, NULL);
        if (!latest || strcmp(full, latest) > 0) { g_free(latest); latest = full; }
        else g_free(full);
    }
    g_dir_close(dir);
    g_free(artwork_dir);
    return latest;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Frequency-Weighted Entry-Count Cache
 *
 * Scans the bottom EVICTION_SCAN_PERCENT% of the LRU queue and evicts the
 * entry with the lowest access_count, keeping frequently browsed art cached
 * even if not recently viewed.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define EVICTION_SCAN_MIN 16
#define EVICTION_SCAN_PERCENT 10

static void texture_cache_init(TextureCache *tc, size_t max_entries) {
    tc->map = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, NULL);
    g_queue_init(&tc->lru);
    tc->max_entries = max_entries;
    atomic_store(&tc->hits, 0);
    atomic_store(&tc->misses, 0);
    atomic_store(&tc->evictions, 0);
}

static void texture_cache_clear(TextureCache *tc) {
    for (GList *l = tc->lru.head; l; l = l->next) cache_entry_free(l->data);
    g_queue_clear(&tc->lru);
    g_hash_table_remove_all(tc->map);
}

static void texture_cache_destroy(TextureCache *tc) {
    texture_cache_clear(tc);
    g_hash_table_destroy(tc->map);
}

/* Look up id (main-thread only) and bump hit/miss counters.
 * On hit, promotes entry to MRU and increments access_count. */
static GdkTexture *texture_cache_touch(TextureCache *tc, int64_t id) {
    CacheEntry *e = g_hash_table_lookup(tc->map, &id);
    if (!e) { atomic_fetch_add(&tc->misses, 1); return NULL; }
    atomic_fetch_add(&tc->hits, 1);
    e->access_count++;
    g_queue_unlink(&tc->lru, e->lru_link);
    g_queue_push_head_link(&tc->lru, e->lru_link);
    return e->texture;
}

/* Insert + evict down to max_entries via frequency-weighted LRU scan.
 * No-op if id already cached or texture is NULL. Refs the texture. */
static void texture_cache_insert(TextureCache *tc, int64_t id, GdkTexture *texture) {
    if (!texture || g_hash_table_contains(tc->map, &id)) return;

    CacheEntry *e = g_new0(CacheEntry, 1);
    e->id = id;
    e->texture = g_object_ref(texture);
    e->access_count = 1;
    g_hash_table_insert(tc->map, &e->id, e);
    g_queue_push_head(&tc->lru, e);
    e->lru_link = tc->lru.head;

    while (g_hash_table_size(tc->map) > tc->max_entries && !g_queue_is_empty(&tc->lru)) {
        guint qlen = g_queue_get_length(&tc->lru);
        guint scan = qlen * EVICTION_SCAN_PERCENT / 100;
        if (scan < EVICTION_SCAN_MIN) scan = EVICTION_SCAN_MIN;
        if (scan > qlen) scan = qlen;

        GList *victim = tc->lru.tail;
        uint32_t min_count = UINT32_MAX;
        GList *l = tc->lru.tail;
        for (guint i = 0; l && i < scan; i++, l = l->prev) {
            CacheEntry *c = l->data;
            if (c->access_count < min_count) { min_count = c->access_count; victim = l; }
        }
        CacheEntry *v = victim->data;
        g_hash_table_remove(tc->map, &v->id);
        g_queue_delete_link(&tc->lru, victim);
        cache_entry_free(v);
        atomic_fetch_add(&tc->evictions, 1);
    }
}

/* Drop every entry whose id decodes to the given library bitmap_index. */
static void texture_cache_evict_library(TextureCache *tc, int bitmap_index) {
    GList *l = tc->lru.head;
    while (l) {
        GList *next = l->next;
        CacheEntry *e = l->data;
        if (LIBRARY_GLOBAL_ID_LIB(e->id) == bitmap_index) {
            g_hash_table_remove(tc->map, &e->id);
            g_queue_delete_link(&tc->lru, l);
            cache_entry_free(e);
        }
        l = next;
    }
}

/* Bitmap index → slot lookup (lock-free read, mirrors library_cache's bitmap_to_slot) */
static inline ArtworkSlot *bitmap_to_art_slot(ArtworkManager *mgr, int bitmap_index) {
    if (bitmap_index < 0 || bitmap_index >= mgr->bitmap_capacity) return NULL;
    return g_atomic_pointer_get(&mgr->bitmap_map[bitmap_index]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Worker Thread
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    ArtworkManager *mgr;
    int64_t id;
    GdkTexture *texture;
    GSList *callbacks;
    LoadTaskType type;
} LoadComplete;

/* P1 fix: GDestroyNotify for LoadComplete — ensures cleanup even if idle
 * source is removed during shutdown before the callback fires. */
static void load_complete_free(gpointer data) {
    LoadComplete *lc = data;
    if (!lc) return;
    g_clear_object(&lc->texture);
    for (GSList *l = lc->callbacks; l; l = l->next) {
        CallbackReg *reg = l->data;
        if (reg && reg->image)
            g_object_remove_weak_pointer(G_OBJECT(reg->image), (gpointer *)&reg->image);
        g_free(reg);
    }
    g_slist_free(lc->callbacks);
    lc->callbacks = NULL;
    g_free(lc);
}

/* Main-thread idle callback for completed artwork loads.
 * Paired with load_complete_free as GDestroyNotify (P1 fix):
 * on normal dispatch this callback processes widgets and NULLs fields so the
 * destroy notify only frees the struct.  On shutdown (source removed before
 * firing), the destroy notify cleans up unprocessed resources. */
static gboolean complete_on_main(gpointer data) {
    LoadComplete *lc = data;
    ArtworkManager *mgr = lc->mgr;

    if (lc->type == LOAD_FULLSIZE) {
        for (GSList *l = lc->callbacks; l; l = l->next) {
            CallbackReg *reg = l->data;
            if (reg->image) {
                GtkWidget *image = reg->image;
                g_object_remove_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
                reg->image = NULL;
                GtkWidget *parent = gtk_widget_get_parent(image);
                if (parent)
                    gtk_widget_remove_css_class(parent, "artwork-loading");
                if (lc->texture)
                    gtk_picture_set_paintable(GTK_PICTURE(image), GDK_PAINTABLE(lc->texture));
            }
        }
        return G_SOURCE_REMOVE;  /* load_complete_free handles struct cleanup */
    }

    TextureCache *tc = (lc->type == LOAD_ARTIST) ? &mgr->artist_cache : &mgr->album_cache;
    texture_cache_insert(tc, lc->id, lc->texture);

    for (GSList *l = lc->callbacks; l; l = l->next) {
        CallbackReg *reg = l->data;
        if (reg->image) {
            GtkWidget *image = reg->image;
            g_object_remove_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
            reg->image = NULL;
            if (lc->texture) {
                gtk_widget_remove_css_class(image, "artwork-loading");
                gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(lc->texture));
            }
        }
    }
    return G_SOURCE_REMOVE;  /* load_complete_free handles struct cleanup */
}

/**
 * Look up an artist in the global UUID-keyed atlas.
 * Must be called with atlas_rwlock reader-held.
 * Returns pixel data as GBytes (caller owns), or NULL if not found.
 * On success, sets *out_thumb_size and *out_channels for texture creation.
 */
static GBytes *artist_atlas_lookup_pixels(ArtworkManager *mgr, int64_t artist_id,
                                           uint32_t *out_thumb_size, uint8_t *out_channels) {
    if (!mgr->artist_atlas || !mgr->library) return NULL;

    /* Get MBID from library cache — need it for UUID-keyed lookup */
    const library_artist_info_t *info = library_cache_get_artist(mgr->library, artist_id, LIBRARY_MASK_ALL);
    if (!info || !info->musicbrainz_id) return NULL;

    uint8_t uuid_bin[ARTIST_ATLAS_UUID_SIZE];
    if (!mbid_parse(info->musicbrainz_id, uuid_bin)) return NULL;

    /* Binary search in the global artist atlas */
    int32_t idx = artist_atlas_reader_lookup(mgr->artist_atlas, uuid_bin);
    if (idx >= 0) {
        const uint8_t *pixels = artist_atlas_reader_get_pixels(mgr->artist_atlas, idx);
        if (!pixels) return NULL;
        *out_thumb_size = artist_atlas_reader_get_thumb_size(mgr->artist_atlas);
        *out_channels = artist_atlas_reader_get_channels(mgr->artist_atlas);
        return g_bytes_new(pixels, artist_atlas_reader_get_pixel_stride(mgr->artist_atlas));
    }

    return NULL;
}

static gpointer worker_func(gpointer data) {
    ArtworkManager *mgr = data;

    while (!g_atomic_int_get(&mgr->shutdown)) {
        LoadTask *task = g_async_queue_timeout_pop(mgr->load_queue, 100000);
        if (!task) continue;
        if (g_atomic_int_get(&mgr->shutdown)) { load_task_free(task); break; }

        GMutex *pending_lock   = &mgr->pending_locks[task->type];
        GHashTable *pending_table = mgr->pending[task->type];

        g_mutex_lock(pending_lock);
        PendingLoad *p = g_hash_table_lookup(pending_table, &task->id);
        gboolean cancelled = !p;
        g_mutex_unlock(pending_lock);
        if (cancelled) { load_task_free(task); continue; }

        /* Fullsize loads: read album art from disk (heavy I/O, may invoke FFmpeg).
         * Derive lib_idx from the global album_id. For merged albums, try the
         * representative's library first, then each merged source in order. */
        if (task->type == LOAD_FULLSIZE) {
            GdkTexture *tex = NULL;
            const library_album_info_t *album =
                library_cache_get_album(mgr->library, task->id, LIBRARY_MASK_ALL);
            if (album && album->path) {
                /* Resolve library slot via bitmap_map */
                int rep_lib = LIBRARY_GLOBAL_ID_LIB(task->id);
                ArtworkSlot *slot = bitmap_to_art_slot(mgr, rep_lib);
                if (slot) {
                    char *album_dir = g_build_filename(slot->music_root, album->path, NULL);
                    uint8_t *art_data = NULL;
                    size_t art_size = 0;
                    if (artwork_find_bytes(album_dir, &art_data, &art_size) == QUADRATURE_OK) {
                        GBytes *bytes = g_bytes_new_take(art_data, art_size);
                        GError *error = NULL;
                        tex = gdk_texture_new_from_bytes(bytes, &error);
                        g_bytes_unref(bytes);
                        if (!tex) {
                            g_warning("Fullsize art decode failed for %s: %s",
                                      album_dir, error->message);
                            g_error_free(error);
                        }
                    }
                    g_free(album_dir);

                    /* Fallback: fanart.tv cached album cover (keyed by release group UUID) */
                    if (!tex && album->musicbrainz_release_group_id && slot->data_root) {
                        char *fanart_path = g_strdup_printf(
                            "%s/artwork/fanart_album_covers/%s.jpg",
                            slot->data_root,
                            album->musicbrainz_release_group_id);
                        if (g_file_test(fanart_path, G_FILE_TEST_EXISTS)) {
                            gchar *contents = NULL;
                            gsize length = 0;
                            if (g_file_get_contents(fanart_path, &contents, &length, NULL)
                                && length > 0) {
                                GBytes *bytes = g_bytes_new_take(contents, length);
                                GError *error = NULL;
                                tex = gdk_texture_new_from_bytes(bytes, &error);
                                g_bytes_unref(bytes);
                                if (!tex) g_clear_error(&error);
                            }
                        }
                        g_free(fanart_path);
                    }
                }
            }

            g_mutex_lock(pending_lock);
            p = g_hash_table_lookup(pending_table, &task->id);
            GSList *callbacks = p ? p->callbacks : NULL;
            if (p) { p->callbacks = NULL; g_hash_table_remove(pending_table, &task->id); }
            g_mutex_unlock(pending_lock);

            if (callbacks) {
                LoadComplete *lc = g_new0(LoadComplete, 1);
                lc->mgr = mgr;
                lc->id = task->id;
                lc->texture = tex ? g_object_ref(tex) : NULL;
                lc->callbacks = callbacks;
                lc->type = LOAD_FULLSIZE;
                g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, complete_on_main,
                                lc, (GDestroyNotify)load_complete_free);
            }
            g_clear_object(&tex);
            load_task_free(task);
            continue;
        }

        /* Decode global ID → library index + local ID, then look up in correct atlas.
         * P2 fix: copy pixel data under atlas_rwlock, create texture OUTSIDE lock
         * to reduce contention across all 4 worker threads. */
        struct timespec _at0, _at1;
        clock_gettime(CLOCK_MONOTONIC, &_at0);

        GdkTexture *tex = NULL;

        /* Under atlas_rwlock: binary search + pixel copy to GBytes.
         * Texture creation (allocation-heavy) happens after unlock. */
        GBytes *pixel_bytes = NULL;
        uint32_t tex_thumb_size = 0;
        uint8_t tex_channels = 0;

        g_rw_lock_reader_lock(&mgr->atlas_rwlock);
        if (task->type == LOAD_ARTIST) {
            pixel_bytes = artist_atlas_lookup_pixels(mgr, task->id,
                                                      &tex_thumb_size, &tex_channels);
        } else {
            int lib_idx = LIBRARY_GLOBAL_ID_LIB(task->id);
            int64_t local_id = LIBRARY_GLOBAL_ID_LOCAL(task->id);
            ArtworkSlot *slot = bitmap_to_art_slot(mgr, lib_idx);
            if (slot && slot->reader) {
                int32_t idx = artwork_atlas_reader_lookup(slot->reader, local_id);
                if (idx >= 0) {
                    const uint8_t *px = artwork_atlas_reader_get_pixels_at(slot->reader, idx);
                    if (px) {
                        pixel_bytes = g_bytes_new(px,
                            artwork_atlas_reader_get_pixel_stride(slot->reader));
                        tex_thumb_size = artwork_atlas_reader_get_thumb_size(slot->reader);
                        tex_channels = artwork_atlas_reader_get_channels(slot->reader);
                    }
                }
            }
        }
        g_rw_lock_reader_unlock(&mgr->atlas_rwlock);

        /* Create texture outside lock (P2: narrows critical section) */
        if (pixel_bytes) {
            tex = gdk_memory_texture_new(
                tex_thumb_size, tex_thumb_size, GDK_MEMORY_R8G8B8,
                pixel_bytes, tex_thumb_size * tex_channels);
            g_bytes_unref(pixel_bytes);
        }

        clock_gettime(CLOCK_MONOTONIC, &_at1);
        uint64_t atlas_us = (uint64_t)(_at1.tv_sec - _at0.tv_sec) * 1000000 +
                            (uint64_t)(_at1.tv_nsec - _at0.tv_nsec) / 1000;
        perf_histogram_record_us(&mgr->atlas_decode_hist, atlas_us);

        if (tex)
            atomic_fetch_add(&mgr->atlas_hits, 1);

        g_mutex_lock(pending_lock);
        p = g_hash_table_lookup(pending_table, &task->id);
        GSList *callbacks = p ? p->callbacks : NULL;
        if (p) { p->callbacks = NULL; g_hash_table_remove(pending_table, &task->id); }
        g_mutex_unlock(pending_lock);

        if (callbacks) {
            LoadComplete *lc = g_new0(LoadComplete, 1);
            lc->mgr = mgr;
            lc->id = task->id;
            lc->texture = tex ? g_object_ref(tex) : NULL;
            lc->callbacks = callbacks;
            lc->type = task->type;
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, complete_on_main,
                            lc, (GDestroyNotify)load_complete_free);
        }

        g_clear_object(&tex);
        load_task_free(task);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

ArtworkManager *artwork_manager_new(library_cache_t *library,
                                    const artwork_manager_source_t *sources,
                                    int source_count,
                                    int thumb_size, size_t cache_count) {
    ArtworkManager *mgr = g_new0(ArtworkManager, 1);
    mgr->library = library;
    mgr->thumb_size = thumb_size > 0 ? thumb_size : 96;
    texture_cache_init(&mgr->album_cache,
                   cache_count > 0 ? cache_count : ARTWORK_CACHE_DEFAULT_MAX_ENTRIES);
    texture_cache_init(&mgr->artist_cache, ARTIST_CACHE_DEFAULT_MAX_ENTRIES);

    /* Allocate per-library slots + bitmap_map (mirrors library_cache init) */
    int n = source_count > 0 ? source_count : 0;
    mgr->slots = n > 0 ? g_new0(ArtworkSlot, n) : NULL;
    mgr->slot_count = n;

    /* Determine bitmap_capacity: max bitmap_index + 1 */
    int max_bi = -1;
    for (int i = 0; i < n; i++) {
        if (sources[i].bitmap_index > max_bi)
            max_bi = sources[i].bitmap_index;
    }
    mgr->bitmap_capacity = max_bi + 1;
    mgr->bitmap_map = g_new0(ArtworkSlot *, mgr->bitmap_capacity > 0 ? mgr->bitmap_capacity : 1);

    for (int i = 0; i < n; i++) {
        ArtworkSlot *s = &mgr->slots[i];
        s->bitmap_index = sources[i].bitmap_index;
        s->data_root = g_strdup(sources[i].data_root);
        s->music_root = g_strdup(sources[i].music_root ? sources[i].music_root
                                                        : sources[i].data_root);
        s->reader = NULL;  /* Loaded below under rwlock */
        g_atomic_pointer_set(&mgr->bitmap_map[s->bitmap_index], s);
    }

    g_rw_lock_init(&mgr->atlas_rwlock);
    for (int i = 0; i < 3; i++) {
        g_mutex_init(&mgr->pending_locks[i]);
        mgr->pending[i] = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
                                                 (GDestroyNotify)pending_load_free);
    }

    mgr->load_queue = g_async_queue_new_full((GDestroyNotify)load_task_free);

    /* Load latest album atlases for each library slot */
    g_rw_lock_writer_lock(&mgr->atlas_rwlock);
    for (int i = 0; i < mgr->slot_count; i++) {
        char *path = find_latest_atlas(mgr->slots[i].data_root, mgr->thumb_size);
        mgr->slots[i].reader = path ? artwork_atlas_reader_open(path) : NULL;
        g_free(path);
    }

    /* Load global artist atlas from ~/.local/share/quadrature/atlas/ */
    {
        char *atlas_dir = g_build_filename(g_get_user_data_dir(), "quadrature", "atlas", NULL);
        char *artist_path = g_build_filename(atlas_dir, "artists.atlas", NULL);
        mgr->artist_atlas = artist_atlas_reader_open(artist_path);
        mgr->artist_atlas_path = artist_path;
        g_free(atlas_dir);
    }
    g_rw_lock_writer_unlock(&mgr->atlas_rwlock);

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

    /* P0 fix: drain any pending LoadComplete idle callbacks while mgr is
     * still valid.  Workers are dead (joined above), so no new callbacks
     * will be posted.  This ensures complete_on_main runs before we free
     * caches/atlases.  Remaining sources get their load_complete_free
     * destroy notify called automatically. */
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);

    for (int i = 0; i < 3; i++) {
        g_mutex_lock(&mgr->pending_locks[i]);
        g_hash_table_destroy(mgr->pending[i]);
        g_mutex_unlock(&mgr->pending_locks[i]);
    }

    texture_cache_destroy(&mgr->album_cache);
    texture_cache_destroy(&mgr->artist_cache);

    g_rw_lock_writer_lock(&mgr->atlas_rwlock);
    for (int i = 0; i < mgr->slot_count; i++) {
        artwork_atlas_reader_close(mgr->slots[i].reader);
        g_free(mgr->slots[i].data_root);
        g_free(mgr->slots[i].music_root);
    }
    g_free(mgr->slots);
    g_free(mgr->bitmap_map);
    artist_atlas_reader_close(mgr->artist_atlas);
    g_free(mgr->artist_atlas_path);
    g_rw_lock_writer_unlock(&mgr->atlas_rwlock);

    g_async_queue_unref(mgr->load_queue);
    for (int i = 0; i < 3; i++) g_mutex_clear(&mgr->pending_locks[i]);
    g_rw_lock_clear(&mgr->atlas_rwlock);
    g_free(mgr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Thumbnails
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Register `widget` as a listener for id (coalesce with existing pending) and
 * queue a load task if this is the first request. Thread-safe: takes the
 * per-type pending lock internally.
 */
static void queue_load(ArtworkManager *mgr, int64_t id, GtkWidget *widget,
                       LoadTaskType type) {
    GMutex *lock = &mgr->pending_locks[type];
    GHashTable *table = mgr->pending[type];

    g_mutex_lock(lock);
    PendingLoad *p = g_hash_table_lookup(table, &id);
    bool first = (p == NULL);
    if (first) {
        p = g_new0(PendingLoad, 1);
        p->id = id;
        g_hash_table_insert(table, &p->id, p);
    }
    CallbackReg *reg = g_new0(CallbackReg, 1);
    reg->image = widget;
    g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)&reg->image);
    p->callbacks = g_slist_prepend(p->callbacks, reg);
    g_mutex_unlock(lock);

    if (first) {
        LoadTask *task = g_new0(LoadTask, 1);
        task->mgr = mgr;
        task->id = id;
        task->type = type;
        g_async_queue_push(mgr->load_queue, task);
    }
}

void artwork_manager_get_thumbnail(ArtworkManager *mgr, int64_t album_id, GtkWidget *image) {
    g_assert(mgr != NULL);
    g_assert(image != NULL);

    /* Cache hit? Sample 1-in-64 for timing histogram (avoids 80ns clock_gettime
     * overhead on every hit — at 1000fps × 100 rows that's 8µs/frame saved). */
    bool do_sample = (atomic_fetch_add(&mgr->hit_sample_counter, 1) & 63) == 0;
    struct timespec _t0, _t1;
    if (do_sample) clock_gettime(CLOCK_MONOTONIC, &_t0);

    /* Main-thread only — no lock needed (see struct comment). */
    GdkTexture *tex = texture_cache_touch(&mgr->album_cache, album_id);
    if (tex) {
        gtk_widget_remove_css_class(image, "artwork-loading");
        gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(tex));

        if (do_sample) {
            clock_gettime(CLOCK_MONOTONIC, &_t1);
            uint64_t us = (uint64_t)(_t1.tv_sec - _t0.tv_sec) * 1000000 +
                          (uint64_t)(_t1.tv_nsec - _t0.tv_nsec) / 1000;
            perf_histogram_record_us(&mgr->texture_hit_hist, us);
        }
        return;
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(image), "media-optical-symbolic");
    gtk_widget_add_css_class(image, "artwork-loading");
    queue_load(mgr, album_id, image, LOAD_ALBUM);
}

int artwork_manager_get_thumb_size(ArtworkManager *mgr) {
    g_assert(mgr != NULL);
    return mgr->thumb_size;
}

void artwork_manager_reload_library_atlas(ArtworkManager *mgr, int bitmap_index,
                                           const char *atlas_path) {
    g_assert(mgr != NULL);
    g_assert(atlas_path != NULL && atlas_path[0] != '\0');

    /* Evict only texture cache entries belonging to this library —
     * preserves warm textures for other libraries during multi-library indexing */
    texture_cache_evict_library(&mgr->album_cache, bitmap_index);

    /* Swap in the new atlas for this library */
    g_rw_lock_writer_lock(&mgr->atlas_rwlock);
    ArtworkSlot *slot = bitmap_to_art_slot(mgr, bitmap_index);
    if (slot) {
        artwork_atlas_reader_close(slot->reader);
        slot->reader = artwork_atlas_reader_open(atlas_path);
    } else {
        g_warning("artwork_manager_reload_library_atlas: bitmap_index %d not found",
                  bitmap_index);
    }
    g_rw_lock_writer_unlock(&mgr->atlas_rwlock);
}

void artwork_manager_add_library(ArtworkManager *mgr, int bitmap_index,
                                  const char *data_root, const char *music_root) {
    g_assert(mgr != NULL);
    g_assert(data_root != NULL);
    g_assert(bitmap_index >= 0);

    g_rw_lock_writer_lock(&mgr->atlas_rwlock);

    /* Grow slots array (realloc may move it → must re-register all bitmap_map ptrs) */
    int new_pos = mgr->slot_count;
    mgr->slots = g_realloc(mgr->slots, sizeof(ArtworkSlot) * (size_t)(new_pos + 1));
    ArtworkSlot *s = &mgr->slots[new_pos];
    s->bitmap_index = bitmap_index;
    s->data_root = g_strdup(data_root);
    s->music_root = g_strdup(music_root ? music_root : data_root);
    char *path = find_latest_atlas(data_root, mgr->thumb_size);
    s->reader = path ? artwork_atlas_reader_open(path) : NULL;
    g_free(path);
    mgr->slot_count = new_pos + 1;

    /* Grow bitmap_map if needed */
    if (bitmap_index >= mgr->bitmap_capacity) {
        int new_cap = bitmap_index + 1;
        mgr->bitmap_map = g_realloc(mgr->bitmap_map, sizeof(ArtworkSlot *) * (size_t)new_cap);
        memset(&mgr->bitmap_map[mgr->bitmap_capacity], 0,
               sizeof(ArtworkSlot *) * (size_t)(new_cap - mgr->bitmap_capacity));
        mgr->bitmap_capacity = new_cap;
    }

    /* Re-register ALL slots (realloc may have moved the array) */
    for (int i = 0; i < mgr->slot_count; i++)
        g_atomic_pointer_set(&mgr->bitmap_map[mgr->slots[i].bitmap_index], &mgr->slots[i]);

    g_rw_lock_writer_unlock(&mgr->atlas_rwlock);
}

void artwork_manager_remove_library(ArtworkManager *mgr, int bitmap_index) {
    g_assert(mgr != NULL);

    /* Evict only this library's texture cache entries.
     * bitmap_index is stable — no global ID shift, so other libraries' entries stay valid. */
    texture_cache_evict_library(&mgr->album_cache, bitmap_index);
    texture_cache_evict_library(&mgr->artist_cache, bitmap_index);

    /* Remove the slot, compact, and re-register bitmap_map pointers */
    g_rw_lock_writer_lock(&mgr->atlas_rwlock);

    /* Find slot position by bitmap_index */
    int pos = -1;
    for (int i = 0; i < mgr->slot_count; i++) {
        if (mgr->slots[i].bitmap_index == bitmap_index) { pos = i; break; }
    }
    if (pos < 0) {
        g_rw_lock_writer_unlock(&mgr->atlas_rwlock);
        return;
    }

    /* Destroy slot internals */
    artwork_atlas_reader_close(mgr->slots[pos].reader);
    g_free(mgr->slots[pos].data_root);
    g_free(mgr->slots[pos].music_root);

    /* Clear bitmap_map entry */
    g_atomic_pointer_set(&mgr->bitmap_map[bitmap_index], NULL);

    /* Shift remaining slots down */
    int remaining = mgr->slot_count - pos - 1;
    if (remaining > 0)
        memmove(&mgr->slots[pos], &mgr->slots[pos + 1], sizeof(ArtworkSlot) * (size_t)remaining);
    mgr->slot_count--;

    /* Re-register shifted slots in bitmap_map (addresses changed after memmove) */
    for (int i = pos; i < mgr->slot_count; i++)
        g_atomic_pointer_set(&mgr->bitmap_map[mgr->slots[i].bitmap_index], &mgr->slots[i]);

    g_rw_lock_writer_unlock(&mgr->atlas_rwlock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Artist Thumbnails
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_artist_thumbnail(ArtworkManager *mgr, int64_t artist_id, GtkWidget *image) {
    g_assert(mgr != NULL);
    g_assert(image != NULL);

    /* Main-thread only — no lock needed (see struct comment). */
    GdkTexture *tex = texture_cache_touch(&mgr->artist_cache, artist_id);
    if (tex) {
        gtk_widget_remove_css_class(image, "artwork-loading");
        gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(tex));
        return;
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(image), "avatar-default-symbolic");
    gtk_widget_add_css_class(image, "artwork-loading");
    queue_load(mgr, artist_id, image, LOAD_ARTIST);
}

void artwork_manager_reload_artist_atlas(ArtworkManager *mgr) {
    g_assert(mgr != NULL);

    /* Evict ALL artist texture cache entries (global atlas changed) */
    texture_cache_clear(&mgr->artist_cache);

    /* Re-open the global artist atlas */
    g_rw_lock_writer_lock(&mgr->atlas_rwlock);
    artist_atlas_reader_close(mgr->artist_atlas);
    mgr->artist_atlas = artist_atlas_reader_open(mgr->artist_atlas_path);
    g_rw_lock_writer_unlock(&mgr->atlas_rwlock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Full-Resolution Album Art (Detail Views)
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_fullsize_album_art(ArtworkManager *mgr,
                                             int64_t album_id, GtkWidget *picture) {
    g_assert(mgr != NULL);
    g_assert(picture != NULL);

    /* Add pulsing loading class to the art container (parent of the GtkPicture) */
    GtkWidget *parent = gtk_widget_get_parent(picture);
    if (parent)
        gtk_widget_add_css_class(parent, "artwork-loading");

    queue_load(mgr, album_id, picture, LOAD_FULLSIZE);
}

void artwork_manager_get_stats(ArtworkManager *mgr, artwork_manager_stats_t *out) {
    g_assert(mgr != NULL && out != NULL);
    memset(out, 0, sizeof(*out));

    /* Main-thread only — no lock needed */
    out->texture_cache_count = g_hash_table_size(mgr->album_cache.map)
                             + g_hash_table_size(mgr->artist_cache.map);
    size_t px = (size_t)mgr->thumb_size * mgr->thumb_size * 4;  /* RGBA */
    out->texture_cache_bytes = out->texture_cache_count * px;

    /* Per-library atlas mmap sizes */
    int n = mgr->slot_count;
    if (n > ARTWORK_MANAGER_MAX_LIBRARIES) n = ARTWORK_MANAGER_MAX_LIBRARIES;
    out->lib_count = n;
    for (int i = 0; i < n; i++)
        out->atlas_mmap_bytes[i] = artwork_atlas_reader_get_file_size(mgr->slots[i].reader);

    /* Atomic counters — relaxed reads are fine for dashboard display */
    out->total_hits   = atomic_load(&mgr->album_cache.hits)
                      + atomic_load(&mgr->artist_cache.hits);
    out->total_misses = atomic_load(&mgr->album_cache.misses)
                      + atomic_load(&mgr->artist_cache.misses);
    out->atlas_hits   = atomic_load(&mgr->atlas_hits);
    out->evictions    = atomic_load(&mgr->album_cache.evictions)
                      + atomic_load(&mgr->artist_cache.evictions);

    /* Pending load queue depth — sum across all load types */
    out->pending_load_count = 0;
    for (int i = 0; i < 3; i++) {
        g_mutex_lock(&mgr->pending_locks[i]);
        out->pending_load_count += g_hash_table_size(mgr->pending[i]);
        g_mutex_unlock(&mgr->pending_locks[i]);
    }
}

const void *artwork_manager_get_texture_hit_hist(ArtworkManager *mgr) {
    return mgr ? &mgr->texture_hit_hist : NULL;
}

const void *artwork_manager_get_atlas_decode_hist(ArtworkManager *mgr) {
    return mgr ? &mgr->atlas_decode_hist : NULL;
}
