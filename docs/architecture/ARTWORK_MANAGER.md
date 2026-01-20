# ArtworkManager

Two-tier artwork system: managed thumbnails with full caching infrastructure, zero-copy full-size via kernel page cache.

**Dependency:** ArtworkManager depends on [LibraryCache](LIBRARY_CACHE.md) for full-size artwork prefetch (ID → path resolution).

## Thumbnail API

For list views. Uses LRU texture cache, mmapped atlas, worker pool, and latency metrics.

```c
ArtworkManager *artwork_manager_new(library_cache_t *library, const char *atlas_path, size_t memory_limit);
void artwork_manager_free(ArtworkManager *mgr);

// Sync lookup (NULL on miss, no load triggered)
GdkTexture *artwork_manager_get_thumb(ArtworkManager *mgr, int64_t album_id);

// Async load with callback
void artwork_manager_load_thumb(ArtworkManager *mgr, int64_t album_id,
                                const char *fallback_path, LoadPriority priority,
                                GCancellable *cancel, ArtLoadCallback cb, gpointer data);

// Direct widget binding
void artwork_manager_load_thumb_into(ArtworkManager *mgr, int64_t album_id,
                                     const char *fallback_path, LoadPriority priority,
                                     GtkWidget *image, GCancellable *cancel);

// Scroll prefetch
void artwork_manager_prefetch_thumbs(ArtworkManager *mgr, const int64_t *album_ids, size_t count);
void artwork_manager_cancel_prefetches(ArtworkManager *mgr);

// Cache control
void artwork_manager_clear(ArtworkManager *mgr);
void artwork_manager_invalidate_album(ArtworkManager *mgr, int64_t album_id);
void artwork_manager_reload_atlas(ArtworkManager *mgr);
```

**Load path:** cache lookup → atlas binary search → file fallback

**Stats:** `artwork_manager_get_stats()` / `artwork_manager_get_full_stats()` for hits, misses, evictions, atlas hits, latency percentiles (p50/p90/p99).

## Full-Size Artwork

For detail views. No caching—uses `posix_fadvise(WILLNEED)` to prefetch into kernel page cache, then GTK loads directly.

```c
// Prefetch full-size artwork by album ID.
// Calls LibraryCache to resolve album_id → path and do the prefetch syscall.
void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, int64_t album_id);
```

### Implementation

ArtworkManager delegates to LibraryCache for path resolution and prefetch:

```c
void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, int64_t album_id) {
    // LibraryCache handles:
    // 1. Resolve album_id → album path
    // 2. Try artwork files in discovery order (art.jpg, cover.jpg, etc.)
    // 3. Call posix_fadvise(WILLNEED) on found file
    library_cache_prefetch_fullsize_artwork(mgr->library, album_id);
}
```

### Usage

```c
// On album row click (before navigation)
static void on_album_row_clicked(GtkWidget *row, gpointer data) {
    int64_t album_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "album-id"));

    // Prefetch artwork while navigation animation plays
    artwork_manager_prefetch_fullsize(art_mgr, album_id);

    // Navigate to detail view
    detail_view_show_album(detail, album_id);
}
```

**Rationale:** Only one full-size image visible at a time. Kernel page cache already optimal; userspace caching adds overhead with no benefit.

## Artist Albums Cache

Reduces DB queries for artist art strips.

```c
gboolean artwork_manager_get_artist_albums(ArtworkManager *mgr, int64_t artist_id,
                                           int64_t **album_ids, size_t *count);
void artwork_manager_put_artist_albums(ArtworkManager *mgr, int64_t artist_id,
                                       const int64_t *album_ids, size_t count);
void artwork_manager_invalidate_artist_cache(ArtworkManager *mgr);
```

## Thread Safety

| Component      | Protection           |
| -------------- | -------------------- |
| Texture cache  | `cache_lock` mutex   |
| Pending loads  | `pending_lock` mutex |
| Artist cache   | `artist_lock` mutex  |
| Atlas          | `atlas_lock` mutex   |
| Stats counters | Atomics              |

## Memory

- Texture cache: 128 MB default (~14k thumbnails)
- Artist cache: ~100 KB

## Targets

| Metric                       | Target |
| ---------------------------- | ------ |
| Thumb hit rate (browsing)    | >90%   |
| Thumb hit rate (fast scroll) | >70%   |
| Atlas load p99               | \<1ms  |
