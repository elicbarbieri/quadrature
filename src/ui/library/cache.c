/**
 * Quadrature Library Cache
 *
 * LRU cache for library data using integer keys with O(1) operations.
 * Uses composite keys (namespace + id) with intrusive linked list for LRU.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <stdatomic.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Composite key: top 8 bits = namespace, bottom 56 bits = id/offset */
typedef uint64_t cache_key_t;

#define MAKE_KEY(ns, id) (((cache_key_t)(ns) << 56) | ((cache_key_t)(id) & 0x00FFFFFFFFFFFFFF))

typedef struct _CacheEntry {
    cache_key_t key;        /* For reverse lookup during eviction */
    GPtrArray *items;       /* LibraryItem* array (ref held) */
    size_t total;           /* Total count for pagination */
    GList link;             /* Intrusive list node for O(1) LRU */
} CacheEntry;

struct _LibraryCache {
    GHashTable *entries;    /* cache_key_t -> CacheEntry* (g_direct_hash) */
    GQueue lru;             /* Doubly-linked for O(1) head removal */
    GMutex lock;
    size_t totals[3];       /* ARTIST, ALBUM, TRACK */
    gboolean has_total[3];

    /* Stats (atomic for thread-safe updates) */
    _Atomic size_t hits;
    _Atomic size_t misses;
    _Atomic size_t evictions;

    /* Stats reporting timer */
    guint stats_timer_id;
};

/* Get CacheEntry from embedded GList link */
#define ENTRY_FROM_LINK(l) ((CacheEntry *)((char *)(l) - offsetof(CacheEntry, link)))

/* Forward declaration for stats reporting */
void library_cache_enable_stats_reporting(LibraryCache *c, gboolean enable);

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cache_entry_free(CacheEntry *e) {
    if (!e) return;
    if (e->items) g_ptr_array_unref(e->items);
    g_free(e);
}

static void evict_oldest(LibraryCache *c) {
    if (g_hash_table_size(c->entries) < LIBRARY_CACHE_MAX_PAGES)
        return;

    /* Pop oldest from LRU head */
    GList *oldest = g_queue_pop_head_link(&c->lru);
    if (!oldest) return;

    CacheEntry *e = ENTRY_FROM_LINK(oldest);
    g_hash_table_remove(c->entries, GSIZE_TO_POINTER((gsize)e->key));
    cache_entry_free(e);
    atomic_fetch_add(&c->evictions, 1);
}

static void touch_entry(LibraryCache *c, CacheEntry *e) {
    /* Move to tail (most recently used) */
    g_queue_unlink(&c->lru, &e->link);
    g_queue_push_tail_link(&c->lru, &e->link);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

LibraryCache *library_cache_new(void) {
    LibraryCache *c = g_new0(LibraryCache, 1);
    g_mutex_init(&c->lock);
    /* Use direct hash for integer keys */
    c->entries = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_queue_init(&c->lru);

    /* Enable stats reporting by default */
    library_cache_enable_stats_reporting(c, TRUE);

    return c;
}

void library_cache_free(LibraryCache *c) {
    if (!c) return;

    /* Stop stats reporting */
    if (c->stats_timer_id != 0) {
        g_source_remove(c->stats_timer_id);
        c->stats_timer_id = 0;
    }

    /* Free all entries */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, c->entries);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cache_entry_free(value);
    }

    g_hash_table_destroy(c->entries);
    g_mutex_clear(&c->lock);
    g_free(c);
}

void library_cache_invalidate(LibraryCache *c) {
    if (!c) return;
    g_mutex_lock(&c->lock);

    /* Free all entries */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, c->entries);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cache_entry_free(value);
    }

    g_hash_table_remove_all(c->entries);
    g_queue_init(&c->lru);

    for (int i = 0; i < 3; i++)
        c->has_total[i] = FALSE;

    g_mutex_unlock(&c->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Unified Get/Put
 * ═══════════════════════════════════════════════════════════════════════════ */

gboolean library_cache_get(LibraryCache *c, CacheNamespace ns,
                           int64_t id, GPtrArray **out, size_t *total) {
    if (!c || !out) return FALSE;

    cache_key_t key = MAKE_KEY(ns, id);

    g_mutex_lock(&c->lock);
    CacheEntry *e = g_hash_table_lookup(c->entries, GSIZE_TO_POINTER((gsize)key));
    if (e) {
        touch_entry(c, e);
        *out = g_ptr_array_ref(e->items);
        if (total) *total = e->total;
        g_mutex_unlock(&c->lock);
        atomic_fetch_add(&c->hits, 1);
        return TRUE;
    }
    g_mutex_unlock(&c->lock);
    atomic_fetch_add(&c->misses, 1);
    return FALSE;
}

void library_cache_put(LibraryCache *c, CacheNamespace ns,
                       int64_t id, GPtrArray *items, size_t total) {
    if (!c || !items) return;

    cache_key_t key = MAKE_KEY(ns, id);

    g_mutex_lock(&c->lock);

    /* Check if entry exists - update in place */
    CacheEntry *existing = g_hash_table_lookup(c->entries, GSIZE_TO_POINTER((gsize)key));
    if (existing) {
        g_ptr_array_unref(existing->items);
        existing->items = g_ptr_array_ref(items);
        existing->total = total;
        touch_entry(c, existing);
        g_mutex_unlock(&c->lock);
        return;
    }

    /* Evict if at capacity */
    evict_oldest(c);

    /* Create new entry */
    CacheEntry *e = g_new0(CacheEntry, 1);
    e->key = key;
    e->items = g_ptr_array_ref(items);
    e->total = total;

    /* Add to hash table and LRU tail */
    g_hash_table_insert(c->entries, GSIZE_TO_POINTER((gsize)key), e);
    g_queue_push_tail_link(&c->lru, &e->link);

    g_mutex_unlock(&c->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Convenience Wrappers (keeping existing API for compatibility)
 * ═══════════════════════════════════════════════════════════════════════════ */

gboolean library_cache_get_page(LibraryCache *c, LibraryItemKind kind,
                                 size_t offset, GPtrArray **out, size_t *total) {
    CacheNamespace ns;
    switch (kind) {
        case LIBRARY_ITEM_ARTIST: ns = CACHE_PAGE_ARTIST; break;
        case LIBRARY_ITEM_ALBUM:  ns = CACHE_PAGE_ALBUM;  break;
        case LIBRARY_ITEM_TRACK:  ns = CACHE_PAGE_TRACK;  break;
        default: return FALSE;
    }
    return library_cache_get(c, ns, (int64_t)offset, out, total);
}

void library_cache_put_page(LibraryCache *c, LibraryItemKind kind,
                             size_t offset, GPtrArray *items, size_t total) {
    CacheNamespace ns;
    switch (kind) {
        case LIBRARY_ITEM_ARTIST: ns = CACHE_PAGE_ARTIST; break;
        case LIBRARY_ITEM_ALBUM:  ns = CACHE_PAGE_ALBUM;  break;
        case LIBRARY_ITEM_TRACK:  ns = CACHE_PAGE_TRACK;  break;
        default: return;
    }
    library_cache_put(c, ns, (int64_t)offset, items, total);
}

gboolean library_cache_get_detail(LibraryCache *c, LibraryItemKind kind,
                                   int64_t parent_id, GPtrArray **out) {
    CacheNamespace ns = (kind == LIBRARY_ITEM_ALBUM) ? CACHE_DETAIL_ALBUM : CACHE_DETAIL_TRACK;
    return library_cache_get(c, ns, parent_id, out, NULL);
}

void library_cache_put_detail(LibraryCache *c, LibraryItemKind kind,
                               int64_t parent_id, GPtrArray *items) {
    CacheNamespace ns = (kind == LIBRARY_ITEM_ALBUM) ? CACHE_DETAIL_ALBUM : CACHE_DETAIL_TRACK;
    library_cache_put(c, ns, parent_id, items, items ? items->len : 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Totals
 * ═══════════════════════════════════════════════════════════════════════════ */

gboolean library_cache_get_total(LibraryCache *c, LibraryItemKind kind, size_t *total) {
    if (!c || !total || kind < 0 || kind > 2) return FALSE;
    g_mutex_lock(&c->lock);
    gboolean has = c->has_total[kind];
    if (has) *total = c->totals[kind];
    g_mutex_unlock(&c->lock);
    return has;
}

void library_cache_set_total(LibraryCache *c, LibraryItemKind kind, size_t total) {
    if (!c || kind < 0 || kind > 2) return;
    g_mutex_lock(&c->lock);
    c->totals[kind] = total;
    c->has_total[kind] = TRUE;
    g_mutex_unlock(&c->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 *
 * Stats are interval-based: counters reset after each reporting cycle.
 * This gives meaningful per-interval metrics rather than cumulative values.
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean stats_report_timer(gpointer data) {
    LibraryCache *c = data;

    /* Read and reset interval counters atomically */
    size_t hits = atomic_exchange(&c->hits, 0);
    size_t misses = atomic_exchange(&c->misses, 0);
    size_t evictions = atomic_exchange(&c->evictions, 0);

    g_mutex_lock(&c->lock);
    size_t entry_count = g_hash_table_size(c->entries);
    g_mutex_unlock(&c->lock);

    size_t total = hits + misses;

    /* Suppress log when idle - no activity conveys no useful information */
    if (total == 0 && evictions == 0) {
        return G_SOURCE_CONTINUE;
    }

    /* Calculate hit rate for this interval */
    double hit_rate = total > 0 ? (100.0 * hits / total) : 0.0;

    g_info("LibraryCache: hit_rate=%.1f%% hits=%zu misses=%zu evictions=%zu "
           "entries=%zu/%d",
           hit_rate, hits, misses, evictions,
           entry_count, LIBRARY_CACHE_MAX_PAGES);

    return G_SOURCE_CONTINUE;
}

void library_cache_enable_stats_reporting(LibraryCache *c, gboolean enable) {
    if (!c) return;

    if (enable && c->stats_timer_id == 0) {
        /* Dump initial stats immediately so we see the starting state */
        stats_report_timer(c);
        c->stats_timer_id = g_timeout_add_seconds(15, stats_report_timer, c);
        g_info("LibraryCache: stats reporting enabled (every 15s)");
    } else if (!enable && c->stats_timer_id != 0) {
        g_source_remove(c->stats_timer_id);
        c->stats_timer_id = 0;
        g_info("LibraryCache: stats reporting disabled");
    }
}

void library_cache_get_stats(LibraryCache *c, size_t *hits, size_t *misses,
                              size_t *evictions) {
    if (!c) return;
    if (hits) *hits = atomic_load(&c->hits);
    if (misses) *misses = atomic_load(&c->misses);
    if (evictions) *evictions = atomic_load(&c->evictions);
}
