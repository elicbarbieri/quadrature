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
#define ARTWORK_ATLAS_KEEP_COUNT 3

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Task type tag for shared worker pool */
typedef enum {
    LOAD_ALBUM = 0,
    LOAD_ARTIST = 1,
    LOAD_FULLSIZE = 2,
} LoadTaskType;

/* Per-library atlas state (v2: fixed-stride raw RGB pixels) */
typedef struct {
    char *root;                              /* Library root path (owned) */
    int atlas_fd;
    void *atlas_map;
    size_t atlas_size;
    const artwork_atlas_header_t *atlas_header;
    const int64_t *album_ids;               /* Sorted album ID array (into mmap) */
    const uint8_t *pixel_data;              /* Fixed-stride pixel arrays (into mmap) */
    uint32_t pixel_stride;                  /* thumb_size * thumb_size * channels */
} LibraryAtlas;

typedef struct {
    int64_t album_id;
    GdkTexture *texture;
    GList *lru_link;
    uint32_t access_count;
} CacheEntry;

typedef struct {
    GtkWidget *image;
} CallbackReg;

typedef struct {
    int64_t album_id;
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

    /* Per-library atlas slots, indexed by library index */
    LibraryAtlas *libraries;           /* Album atlases (root = data path) */
    char **music_roots;                /* Music file roots (for embedded art fallback) */
    int lib_count;
    GMutex atlas_lock;   /* Covers all LibraryAtlas + global artist atlas */

    /* Global artist atlas (UUID-keyed, shared across all libraries) */
    artist_atlas_reader_t *artist_atlas;
    char *artist_atlas_path;           /* Path for reload detection */

    /* Frequency-weighted LRU texture cache (keyed by global album_id) */
    GHashTable *cache;
    GQueue lru;
    GMutex cache_lock;
    size_t max_entries;

    /* Artist texture cache (keyed by global artist_id, separate from album cache) */
    GHashTable *artist_cache;
    GQueue artist_lru;
    GMutex artist_cache_lock;
    size_t artist_max_entries;

    /* Pending loads (album) */
    GHashTable *pending;
    GMutex pending_lock;

    /* Pending loads (artist) */
    GHashTable *artist_pending;
    GMutex artist_pending_lock;

    /* Pending loads (fullsize — detail views, no cache) */
    GHashTable *fullsize_pending;
    GMutex fullsize_pending_lock;

    /* Worker pool (shared between album and artist loads) */
    GAsyncQueue *load_queue;
    GThread *workers[ARTWORK_MANAGER_DEFAULT_WORKERS];
    gboolean shutdown;

    /* Stats (atomic) */
    _Atomic size_t hits;
    _Atomic size_t misses;
    _Atomic size_t evictions;
    _Atomic size_t atlas_hits;
    _Atomic size_t artist_hits;
    _Atomic size_t artist_misses;

    /* Latency histograms (µs scale, lock-free recording) */
    perf_histogram_us_t texture_hit_hist;   /* cache_lock + lookup + unlock time */
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

        if (!best_to_evict) best_to_evict = mgr->lru.tail;

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
 * Per-Library Atlas Operations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void lib_atlas_unmap(LibraryAtlas *la) {
    if (la->atlas_map && la->atlas_map != MAP_FAILED)
        munmap(la->atlas_map, la->atlas_size);
    if (la->atlas_fd >= 0) close(la->atlas_fd);
    la->atlas_map = NULL;
    la->atlas_fd = -1;
    la->atlas_size = 0;
    la->atlas_header = NULL;
    la->album_ids = NULL;
    la->pixel_data = NULL;
    la->pixel_stride = 0;
}

static void lib_atlas_load(LibraryAtlas *la, const char *path) {
    lib_atlas_unmap(la);
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

    la->atlas_fd = fd;
    la->atlas_map = map;
    la->atlas_size = st.st_size;
    la->atlas_header = h;
    la->pixel_stride = h->thumb_size * h->thumb_size * h->channels;
    la->album_ids = (const int64_t *)((uint8_t *)map + sizeof(*h));
    la->pixel_data = (const uint8_t *)la->album_ids + h->count * sizeof(int64_t);
    g_info("Atlas v2 loaded for lib '%s': %s (%u entries, stride=%u)",
           la->root ? la->root : "?", path, h->count, la->pixel_stride);
}

/* Binary search for local_id within a library's atlas. Returns index or -1. */
static int32_t lib_atlas_lookup(const LibraryAtlas *la, int64_t local_id) {
    if (!la->atlas_header || !la->album_ids) return -1;
    uint32_t lo = 0, hi = la->atlas_header->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int64_t mid_id = la->album_ids[mid];
        if (mid_id == local_id) return (int32_t)mid;
        if (mid_id < local_id) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* Create texture directly from raw mmap'd pixel data (zero decode). */
static GdkTexture *lib_atlas_load_texture(const LibraryAtlas *la, int32_t index) {
    if (!la->pixel_data || index < 0) return NULL;
    const uint8_t *pixels = la->pixel_data + (uint32_t)index * la->pixel_stride;
    GBytes *bytes = g_bytes_new(pixels, la->pixel_stride);
    GdkTexture *tex = gdk_memory_texture_new(
        la->atlas_header->thumb_size,
        la->atlas_header->thumb_size,
        GDK_MEMORY_R8G8B8,
        bytes,
        la->atlas_header->thumb_size * la->atlas_header->channels);
    g_bytes_unref(bytes);
    return tex;
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

/* Artist eviction — identical to album eviction but on artist cache */
static void artist_evict_if_needed(ArtworkManager *mgr) {
    while (g_hash_table_size(mgr->artist_cache) > mgr->artist_max_entries &&
           !g_queue_is_empty(&mgr->artist_lru)) {
        guint queue_len = g_queue_get_length(&mgr->artist_lru);
        guint scan_count = queue_len * EVICTION_SCAN_PERCENT / 100;
        if (scan_count < EVICTION_SCAN_MIN) scan_count = EVICTION_SCAN_MIN;
        if (scan_count > queue_len) scan_count = queue_len;

        GList *best_to_evict = NULL;
        uint32_t min_count = UINT32_MAX;

        GList *l = mgr->artist_lru.tail;
        for (guint i = 0; l && i < scan_count; i++, l = l->prev) {
            CacheEntry *e = l->data;
            if (e->access_count < min_count) {
                min_count = e->access_count;
                best_to_evict = l;
            }
        }
        if (!best_to_evict) best_to_evict = mgr->artist_lru.tail;

        CacheEntry *e = best_to_evict->data;
        g_hash_table_remove(mgr->artist_cache, &e->album_id);
        g_queue_delete_link(&mgr->artist_lru, best_to_evict);
        cache_entry_free(e);
    }
}

static gboolean complete_on_main(gpointer data) {
    LoadComplete *lc = data;
    ArtworkManager *mgr = lc->mgr;

    /* Fullsize loads: no cache, just set the texture on the GtkPicture */
    if (lc->type == LOAD_FULLSIZE) {
        for (GSList *l = lc->callbacks; l; l = l->next) {
            CallbackReg *reg = l->data;
            if (reg->image) {
                g_object_remove_weak_pointer(G_OBJECT(reg->image), (gpointer *)&reg->image);
                GtkWidget *parent = gtk_widget_get_parent(reg->image);
                if (parent)
                    gtk_widget_remove_css_class(parent, "artwork-loading");
                if (lc->texture)
                    gtk_picture_set_paintable(GTK_PICTURE(reg->image), GDK_PAINTABLE(lc->texture));
            }
            g_free(reg);
        }
        g_slist_free(lc->callbacks);
        g_clear_object(&lc->texture);
        g_free(lc);
        return G_SOURCE_REMOVE;
    }

    /* Route to the correct cache based on task type */
    GHashTable *cache;
    GQueue *lru;
    GMutex *lock;
    if (lc->type == LOAD_ARTIST) {
        cache = mgr->artist_cache;
        lru = &mgr->artist_lru;
        lock = &mgr->artist_cache_lock;
    } else {
        cache = mgr->cache;
        lru = &mgr->lru;
        lock = &mgr->cache_lock;
    }

    if (lc->texture) {
        g_mutex_lock(lock);
        if (!g_hash_table_contains(cache, &lc->id)) {
            CacheEntry *e = g_new0(CacheEntry, 1);
            e->album_id = lc->id;  /* album_id field used for both album/artist IDs */
            e->texture = g_object_ref(lc->texture);
            e->access_count = 1;
            g_hash_table_insert(cache, &e->album_id, e);
            g_queue_push_head(lru, e);
            e->lru_link = lru->head;
            if (lc->type == LOAD_ARTIST)
                artist_evict_if_needed(mgr);
            else
                evict_if_needed(mgr);
        }
        g_mutex_unlock(lock);
    }

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

typedef enum {
    ARTIST_LOOKUP_HIT,        /* Found artwork in atlas */
    ARTIST_LOOKUP_NO_ART,     /* Known to have no artwork (silent) */
    ARTIST_LOOKUP_UNKNOWN,    /* Not in atlas at all */
} ArtistLookupResult;

/**
 * Look up an artist in the global UUID-keyed atlas.
 * Must be called with atlas_lock held.
 * Returns texture (caller owns) or NULL, and sets *result accordingly.
 */
static GdkTexture *artist_atlas_lookup(ArtworkManager *mgr, int64_t artist_id,
                                        ArtistLookupResult *result) {
    *result = ARTIST_LOOKUP_UNKNOWN;

    if (!mgr->artist_atlas || !mgr->library) return NULL;

    /* Get MBID from library cache — need it for UUID-keyed lookup */
    const library_artist_info_t *info = library_cache_get_artist(mgr->library, artist_id);
    if (!info || !info->musicbrainz_id) return NULL;

    uint8_t uuid_bin[ARTIST_ATLAS_UUID_SIZE];
    if (!mbid_parse(info->musicbrainz_id, uuid_bin)) return NULL;

    /* Binary search in the global artist atlas */
    int32_t idx = artist_atlas_reader_lookup(mgr->artist_atlas, uuid_bin);
    if (idx >= 0) {
        *result = ARTIST_LOOKUP_HIT;
        const uint8_t *pixels = artist_atlas_reader_get_pixels(mgr->artist_atlas, idx);
        if (!pixels) return NULL;
        uint32_t thumb_size = artist_atlas_reader_get_thumb_size(mgr->artist_atlas);
        uint32_t stride = artist_atlas_reader_get_pixel_stride(mgr->artist_atlas);
        GBytes *bytes = g_bytes_new(pixels, stride);
        GdkTexture *tex = gdk_memory_texture_new(
            thumb_size, thumb_size, GDK_MEMORY_R8G8B8,
            bytes, thumb_size * artist_atlas_reader_get_channels(mgr->artist_atlas));
        g_bytes_unref(bytes);
        return tex;
    }

    /* Check no-art index */
    if (artist_atlas_reader_is_no_art(mgr->artist_atlas, uuid_bin)) {
        *result = ARTIST_LOOKUP_NO_ART;
        return NULL;
    }

    return NULL;
}

static gpointer worker_func(gpointer data) {
    ArtworkManager *mgr = data;

    while (!g_atomic_int_get(&mgr->shutdown)) {
        LoadTask *task = g_async_queue_timeout_pop(mgr->load_queue, 100000);
        if (!task) continue;
        if (g_atomic_int_get(&mgr->shutdown)) { load_task_free(task); break; }

        /* Select the correct pending table based on task type */
        GMutex *pending_lock;
        GHashTable *pending_table;
        if (task->type == LOAD_FULLSIZE) {
            pending_lock = &mgr->fullsize_pending_lock;
            pending_table = mgr->fullsize_pending;
        } else if (task->type == LOAD_ARTIST) {
            pending_lock = &mgr->artist_pending_lock;
            pending_table = mgr->artist_pending;
        } else {
            pending_lock = &mgr->pending_lock;
            pending_table = mgr->pending;
        }

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
                library_cache_get_album(mgr->library, task->id);
            if (album && album->path) {
                /* Build candidate library indices: representative first, then sources */
                int candidates[16];
                int n_candidates = 0;
                int rep_lib = LIBRARY_GLOBAL_ID_LIB(task->id);
                if (rep_lib >= 0 && rep_lib < mgr->lib_count)
                    candidates[n_candidates++] = rep_lib;
                for (int m = 0; m < album->merged_source_count && n_candidates < 16; m++) {
                    int src_lib = LIBRARY_GLOBAL_ID_LIB(album->merged_source_ids[m]);
                    if (src_lib >= 0 && src_lib < mgr->lib_count)
                        candidates[n_candidates++] = src_lib;
                }

                for (int c = 0; c < n_candidates && !tex; c++) {
                    char *album_dir = g_build_filename(
                        mgr->music_roots[candidates[c]], album->path, NULL);
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
                g_idle_add(complete_on_main, lc);
            }
            g_clear_object(&tex);
            load_task_free(task);
            continue;
        }

        /* Decode global ID → library index + local ID, then look up in correct atlas.
         * Instrumented: time the atlas lock+lookup+decode for perf dashboard. */
        struct timespec _at0, _at1;
        clock_gettime(CLOCK_MONOTONIC, &_at0);

        GdkTexture *tex = NULL;
        ArtistLookupResult artist_result = ARTIST_LOOKUP_UNKNOWN;

        /* Pre-fetch merged source IDs BEFORE acquiring atlas_lock to avoid
         * nested locking (atlas_lock → cache->lock). Stack-local copy of
         * source global IDs is safe — they're immutable integers. */
        int64_t merge_sources[16];
        int merge_count = 0;
        if (task->type == LOAD_ALBUM && mgr->library) {
            const library_album_info_t *album =
                library_cache_get_album(mgr->library, task->id);
            if (album) {
                merge_count = album->merged_source_count < 16
                            ? album->merged_source_count : 16;
                for (int m = 0; m < merge_count; m++)
                    merge_sources[m] = album->merged_source_ids[m];
            }
        }

        g_mutex_lock(&mgr->atlas_lock);
        if (task->type == LOAD_ARTIST) {
            tex = artist_atlas_lookup(mgr, task->id, &artist_result);
        } else {
            /* Try representative's atlas first */
            int lib_idx = LIBRARY_GLOBAL_ID_LIB(task->id);
            int64_t local_id = LIBRARY_GLOBAL_ID_LOCAL(task->id);
            if (lib_idx >= 0 && lib_idx < mgr->lib_count) {
                int32_t idx = lib_atlas_lookup(&mgr->libraries[lib_idx], local_id);
                if (idx >= 0)
                    tex = lib_atlas_load_texture(&mgr->libraries[lib_idx], idx);
            }
            /* Fallback: try merged source libraries' atlases (no lock nesting) */
            for (int m = 0; m < merge_count && !tex; m++) {
                int src_lib = LIBRARY_GLOBAL_ID_LIB(merge_sources[m]);
                int64_t src_local = LIBRARY_GLOBAL_ID_LOCAL(merge_sources[m]);
                if (src_lib >= 0 && src_lib < mgr->lib_count) {
                    int32_t idx = lib_atlas_lookup(&mgr->libraries[src_lib], src_local);
                    if (idx >= 0)
                        tex = lib_atlas_load_texture(&mgr->libraries[src_lib], idx);
                }
            }
        }
        g_mutex_unlock(&mgr->atlas_lock);

        clock_gettime(CLOCK_MONOTONIC, &_at1);
        uint64_t atlas_us = (uint64_t)(_at1.tv_sec - _at0.tv_sec) * 1000000 +
                            (uint64_t)(_at1.tv_nsec - _at0.tv_nsec) / 1000;
        perf_histogram_record_us(&mgr->atlas_decode_hist, atlas_us);

        if (tex) {
            atomic_fetch_add(&mgr->atlas_hits, 1);
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
            lc->type = task->type;
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

ArtworkManager *artwork_manager_new(library_cache_t *library,
                                    const char **data_roots,
                                    const char **music_roots,
                                    int lib_count,
                                    int cache_size, size_t cache_count) {
    ArtworkManager *mgr = g_new0(ArtworkManager, 1);
    mgr->library = library;
    mgr->thumb_size = cache_size > 0 ? cache_size : 96;
    mgr->max_entries = cache_count > 0 ? cache_count : ARTWORK_CACHE_DEFAULT_MAX_ENTRIES;
    mgr->artist_max_entries = ARTIST_CACHE_DEFAULT_MAX_ENTRIES;

    /* Allocate per-library atlas slots (album only) */
    mgr->lib_count = lib_count > 0 ? lib_count : 0;
    int alloc = mgr->lib_count > 0 ? mgr->lib_count : 1;
    mgr->libraries = g_new0(LibraryAtlas, alloc);
    mgr->music_roots = mgr->lib_count > 0 ? g_new0(char *, mgr->lib_count) : NULL;
    for (int i = 0; i < mgr->lib_count; i++) {
        mgr->libraries[i].root = g_strdup(data_roots[i]);
        mgr->libraries[i].atlas_fd = -1;
        mgr->music_roots[i] = g_strdup(music_roots ? music_roots[i] : data_roots[i]);
    }

    g_mutex_init(&mgr->cache_lock);
    g_mutex_init(&mgr->pending_lock);
    g_mutex_init(&mgr->atlas_lock);
    g_mutex_init(&mgr->artist_cache_lock);
    g_mutex_init(&mgr->artist_pending_lock);

    mgr->cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, NULL);
    g_queue_init(&mgr->lru);
    mgr->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
                                          (GDestroyNotify)pending_load_free);

    mgr->artist_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, NULL);
    g_queue_init(&mgr->artist_lru);
    mgr->artist_pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
                                                  (GDestroyNotify)pending_load_free);

    g_mutex_init(&mgr->fullsize_pending_lock);
    mgr->fullsize_pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
                                                    (GDestroyNotify)pending_load_free);

    mgr->load_queue = g_async_queue_new_full((GDestroyNotify)load_task_free);

    /* Load latest album atlases for each library */
    g_mutex_lock(&mgr->atlas_lock);
    for (int i = 0; i < mgr->lib_count; i++) {
        char *path = find_latest_atlas(mgr->libraries[i].root, mgr->thumb_size);
        lib_atlas_load(&mgr->libraries[i], path);
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
    g_mutex_unlock(&mgr->atlas_lock);

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

    g_mutex_lock(&mgr->artist_pending_lock);
    g_hash_table_destroy(mgr->artist_pending);
    g_mutex_unlock(&mgr->artist_pending_lock);

    g_mutex_lock(&mgr->fullsize_pending_lock);
    g_hash_table_destroy(mgr->fullsize_pending);
    g_mutex_unlock(&mgr->fullsize_pending_lock);

    g_mutex_lock(&mgr->cache_lock);
    GList *l = mgr->lru.head;
    while (l) { cache_entry_free(l->data); l = l->next; }
    g_queue_clear(&mgr->lru);
    g_hash_table_destroy(mgr->cache);
    g_mutex_unlock(&mgr->cache_lock);

    g_mutex_lock(&mgr->artist_cache_lock);
    l = mgr->artist_lru.head;
    while (l) { cache_entry_free(l->data); l = l->next; }
    g_queue_clear(&mgr->artist_lru);
    g_hash_table_destroy(mgr->artist_cache);
    g_mutex_unlock(&mgr->artist_cache_lock);

    g_mutex_lock(&mgr->atlas_lock);
    for (int i = 0; i < mgr->lib_count; i++) {
        lib_atlas_unmap(&mgr->libraries[i]);
        g_free(mgr->libraries[i].root);
    }
    g_free(mgr->libraries);
    artist_atlas_reader_close(mgr->artist_atlas);
    g_free(mgr->artist_atlas_path);
    for (int i = 0; i < mgr->lib_count; i++)
        g_free(mgr->music_roots[i]);
    g_free(mgr->music_roots);
    g_mutex_unlock(&mgr->atlas_lock);

    g_async_queue_unref(mgr->load_queue);
    g_mutex_clear(&mgr->cache_lock);
    g_mutex_clear(&mgr->pending_lock);
    g_mutex_clear(&mgr->atlas_lock);
    g_mutex_clear(&mgr->artist_cache_lock);
    g_mutex_clear(&mgr->artist_pending_lock);
    g_mutex_clear(&mgr->fullsize_pending_lock);
    g_free(mgr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Thumbnails
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_thumbnail(ArtworkManager *mgr, int64_t album_id, GtkWidget *image) {
    g_assert(mgr != NULL);
    g_assert(image != NULL);

    /* Cache hit? Sample 1-in-64 for timing histogram (avoids 80ns clock_gettime
     * overhead on every hit — at 1000fps × 100 rows that's 8µs/frame saved). */
    bool do_sample = (atomic_fetch_add(&mgr->hit_sample_counter, 1) & 63) == 0;
    struct timespec _t0, _t1;
    if (do_sample) clock_gettime(CLOCK_MONOTONIC, &_t0);

    g_mutex_lock(&mgr->cache_lock);
    CacheEntry *e = g_hash_table_lookup(mgr->cache, &album_id);
    if (e) {
        touch_entry(mgr, e);
        atomic_fetch_add(&mgr->hits, 1);
        gtk_widget_remove_css_class(image, "artwork-loading");
        gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(e->texture));
        g_mutex_unlock(&mgr->cache_lock);

        if (do_sample) {
            clock_gettime(CLOCK_MONOTONIC, &_t1);
            uint64_t us = (uint64_t)(_t1.tv_sec - _t0.tv_sec) * 1000000 +
                          (uint64_t)(_t1.tv_nsec - _t0.tv_nsec) / 1000;
            perf_histogram_record_us(&mgr->texture_hit_hist, us);
        }
        return;
    }
    g_mutex_unlock(&mgr->cache_lock);

    gtk_image_set_from_icon_name(GTK_IMAGE(image), "media-optical-symbolic");
    gtk_widget_add_css_class(image, "artwork-loading");
    atomic_fetch_add(&mgr->misses, 1);

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
    task->id = album_id;
    task->type = LOAD_ALBUM;
    g_async_queue_push(mgr->load_queue, task);
}

int artwork_manager_get_thumb_size(ArtworkManager *mgr) {
    g_assert(mgr != NULL);
    return mgr->thumb_size;
}

void artwork_manager_reload_library_atlas(ArtworkManager *mgr, int lib_idx,
                                           const char *atlas_path) {
    g_assert(mgr != NULL);
    if (lib_idx < 0 || lib_idx >= mgr->lib_count) {
        g_warning("artwork_manager_reload_library_atlas: lib_idx %d out of range [0, %d)",
                  lib_idx, mgr->lib_count);
        return;
    }
    g_assert(atlas_path != NULL && atlas_path[0] != '\0');

    /* Evict only texture cache entries belonging to this library —
     * preserves warm textures for other libraries during multi-library indexing */
    g_mutex_lock(&mgr->cache_lock);
    GList *l = mgr->lru.head;
    while (l) {
        GList *next = l->next;
        CacheEntry *e = l->data;
        if (LIBRARY_GLOBAL_ID_LIB(e->album_id) == lib_idx) {
            g_hash_table_remove(mgr->cache, &e->album_id);
            g_queue_delete_link(&mgr->lru, l);
            cache_entry_free(e);
        }
        l = next;
    }
    g_mutex_unlock(&mgr->cache_lock);

    /* Swap in the new atlas for this library */
    g_mutex_lock(&mgr->atlas_lock);
    lib_atlas_load(&mgr->libraries[lib_idx], atlas_path);
    g_mutex_unlock(&mgr->atlas_lock);
}

void artwork_manager_add_library(ArtworkManager *mgr, const char *data_root,
                                  const char *music_root) {
    g_assert(mgr != NULL);
    g_assert(data_root != NULL);

    g_mutex_lock(&mgr->atlas_lock);
    int new_idx = mgr->lib_count;
    mgr->libraries = g_realloc(mgr->libraries,
                               sizeof(LibraryAtlas) * (size_t)(new_idx + 1));
    memset(&mgr->libraries[new_idx], 0, sizeof(LibraryAtlas));
    mgr->libraries[new_idx].root = g_strdup(data_root);
    mgr->libraries[new_idx].atlas_fd = -1;

    mgr->music_roots = g_realloc(mgr->music_roots,
                                  sizeof(char *) * (size_t)(new_idx + 1));
    mgr->music_roots[new_idx] = g_strdup(music_root ? music_root : data_root);

    /* Try loading existing atlas (may not exist yet for new library) */
    char *path = find_latest_atlas(data_root, mgr->thumb_size);
    lib_atlas_load(&mgr->libraries[new_idx], path);
    g_free(path);

    mgr->lib_count = new_idx + 1;
    g_mutex_unlock(&mgr->atlas_lock);
}

void artwork_manager_remove_library(ArtworkManager *mgr, int lib_idx) {
    g_assert(mgr != NULL);
    if (lib_idx < 0 || lib_idx >= mgr->lib_count) return;

    /* Evict ALL album texture cache entries — global IDs shift after removal,
     * so entries from shifted libraries have stale keys. */
    g_mutex_lock(&mgr->cache_lock);
    GList *l = mgr->lru.head;
    while (l) {
        GList *next = l->next;
        CacheEntry *e = l->data;
        g_hash_table_remove(mgr->cache, &e->album_id);
        g_queue_delete_link(&mgr->lru, l);
        cache_entry_free(e);
        l = next;
    }
    g_mutex_unlock(&mgr->cache_lock);

    /* Evict ALL artist texture cache entries (artist IDs may also shift) */
    g_mutex_lock(&mgr->artist_cache_lock);
    l = mgr->artist_lru.head;
    while (l) {
        GList *next = l->next;
        CacheEntry *e = l->data;
        g_hash_table_remove(mgr->artist_cache, &e->album_id);
        g_queue_delete_link(&mgr->artist_lru, l);
        cache_entry_free(e);
        l = next;
    }
    g_mutex_unlock(&mgr->artist_cache_lock);

    /* Remove the atlas slot and shift remaining down */
    g_mutex_lock(&mgr->atlas_lock);
    lib_atlas_unmap(&mgr->libraries[lib_idx]);
    g_free(mgr->libraries[lib_idx].root);
    g_free(mgr->music_roots[lib_idx]);

    int remaining = mgr->lib_count - lib_idx - 1;
    if (remaining > 0) {
        memmove(&mgr->libraries[lib_idx], &mgr->libraries[lib_idx + 1],
                sizeof(LibraryAtlas) * (size_t)remaining);
        memmove(&mgr->music_roots[lib_idx], &mgr->music_roots[lib_idx + 1],
                sizeof(char *) * (size_t)remaining);
    }
    mgr->lib_count--;
    g_mutex_unlock(&mgr->atlas_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Artist Thumbnails
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_artist_thumbnail(ArtworkManager *mgr, int64_t artist_id, GtkWidget *image) {
    g_assert(mgr != NULL);
    g_assert(image != NULL);

    /* Cache hit? */
    g_mutex_lock(&mgr->artist_cache_lock);
    CacheEntry *e = g_hash_table_lookup(mgr->artist_cache, &artist_id);
    if (e) {
        e->access_count++;
        if (e->lru_link) {
            g_queue_unlink(&mgr->artist_lru, e->lru_link);
            g_queue_push_head_link(&mgr->artist_lru, e->lru_link);
        }
        atomic_fetch_add(&mgr->artist_hits, 1);
        gtk_widget_remove_css_class(image, "artwork-loading");
        gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(e->texture));
        g_mutex_unlock(&mgr->artist_cache_lock);
        return;
    }
    g_mutex_unlock(&mgr->artist_cache_lock);

    gtk_image_set_from_icon_name(GTK_IMAGE(image), "avatar-default-symbolic");
    gtk_widget_add_css_class(image, "artwork-loading");
    atomic_fetch_add(&mgr->artist_misses, 1);

    g_mutex_lock(&mgr->artist_pending_lock);
    PendingLoad *p = g_hash_table_lookup(mgr->artist_pending, &artist_id);
    if (p) {
        CallbackReg *reg = g_new0(CallbackReg, 1);
        reg->image = image;
        g_object_add_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
        p->callbacks = g_slist_prepend(p->callbacks, reg);
        g_mutex_unlock(&mgr->artist_pending_lock);
        return;
    }

    p = g_new0(PendingLoad, 1);
    p->album_id = artist_id;  /* album_id field reused for artist_id */
    CallbackReg *reg = g_new0(CallbackReg, 1);
    reg->image = image;
    g_object_add_weak_pointer(G_OBJECT(image), (gpointer *)&reg->image);
    p->callbacks = g_slist_prepend(NULL, reg);
    g_hash_table_insert(mgr->artist_pending, &p->album_id, p);
    g_mutex_unlock(&mgr->artist_pending_lock);

    LoadTask *task = g_new0(LoadTask, 1);
    task->mgr = mgr;
    task->id = artist_id;
    task->type = LOAD_ARTIST;
    g_async_queue_push(mgr->load_queue, task);
}

void artwork_manager_reload_artist_atlas(ArtworkManager *mgr) {
    g_assert(mgr != NULL);

    /* Evict ALL artist texture cache entries (global atlas changed) */
    g_mutex_lock(&mgr->artist_cache_lock);
    GList *l = mgr->artist_lru.head;
    while (l) {
        GList *next = l->next;
        cache_entry_free(l->data);
        l = next;
    }
    g_queue_clear(&mgr->artist_lru);
    g_hash_table_remove_all(mgr->artist_cache);
    g_mutex_unlock(&mgr->artist_cache_lock);

    /* Re-open the global artist atlas */
    g_mutex_lock(&mgr->atlas_lock);
    artist_atlas_reader_close(mgr->artist_atlas);
    mgr->artist_atlas = artist_atlas_reader_open(mgr->artist_atlas_path);
    g_mutex_unlock(&mgr->atlas_lock);
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

    /* Coalesce: if already pending for this album, just add the callback */
    g_mutex_lock(&mgr->fullsize_pending_lock);
    PendingLoad *p = g_hash_table_lookup(mgr->fullsize_pending, &album_id);
    if (p) {
        CallbackReg *reg = g_new0(CallbackReg, 1);
        reg->image = picture;
        g_object_add_weak_pointer(G_OBJECT(picture), (gpointer *)&reg->image);
        p->callbacks = g_slist_prepend(p->callbacks, reg);
        g_mutex_unlock(&mgr->fullsize_pending_lock);
        return;
    }

    p = g_new0(PendingLoad, 1);
    p->album_id = album_id;
    CallbackReg *reg = g_new0(CallbackReg, 1);
    reg->image = picture;
    g_object_add_weak_pointer(G_OBJECT(picture), (gpointer *)&reg->image);
    p->callbacks = g_slist_prepend(NULL, reg);
    g_hash_table_insert(mgr->fullsize_pending, &p->album_id, p);
    g_mutex_unlock(&mgr->fullsize_pending_lock);

    LoadTask *task = g_new0(LoadTask, 1);
    task->mgr = mgr;
    task->id = album_id;
    task->type = LOAD_FULLSIZE;
    g_async_queue_push(mgr->load_queue, task);
}

void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, int64_t album_id) {
    if (!mgr || !mgr->library || album_id <= 0) return;
    library_cache_prefetch_fullsize_artwork(mgr->library, album_id);
}

void artwork_manager_get_stats(ArtworkManager *mgr, artwork_manager_stats_t *out) {
    g_assert(mgr != NULL && out != NULL);
    memset(out, 0, sizeof(*out));

    /* Texture cache — brief lock to read size */
    g_mutex_lock(&mgr->cache_lock);
    out->texture_cache_count = g_hash_table_size(mgr->cache);
    g_mutex_unlock(&mgr->cache_lock);
    size_t px = (size_t)mgr->thumb_size * mgr->thumb_size * 4;  /* RGBA */
    out->texture_cache_bytes = out->texture_cache_count * px;

    /* Per-library atlas mmap sizes (set once at load, safe without lock) */
    int n = mgr->lib_count;
    if (n > ARTWORK_MANAGER_MAX_LIBRARIES) n = ARTWORK_MANAGER_MAX_LIBRARIES;
    out->lib_count = n;
    for (int i = 0; i < n; i++)
        out->atlas_mmap_bytes[i] = mgr->libraries[i].atlas_size;

    /* Atomic counters — relaxed reads are fine for dashboard display */
    out->total_hits   = atomic_load(&mgr->hits);
    out->total_misses = atomic_load(&mgr->misses);
    out->atlas_hits   = atomic_load(&mgr->atlas_hits);
    out->evictions    = atomic_load(&mgr->evictions);

    /* Pending load queue depth */
    g_mutex_lock(&mgr->pending_lock);
    out->pending_load_count = g_hash_table_size(mgr->pending);
    g_mutex_unlock(&mgr->pending_lock);
}

const void *artwork_manager_get_texture_hit_hist(ArtworkManager *mgr) {
    return mgr ? &mgr->texture_hit_hist : NULL;
}

const void *artwork_manager_get_atlas_decode_hist(ArtworkManager *mgr) {
    return mgr ? &mgr->atlas_decode_hist : NULL;
}
