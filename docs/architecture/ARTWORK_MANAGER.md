# ArtworkManager

Two-tier artwork system: managed thumbnails with full caching infrastructure, full-size via kernel page cache.

**Dependency:** ArtworkManager depends on [LibraryCache](LIBRARY_CACHE.md) for full-size artwork prefetch (album-id → path resolution).

## Thumbnail API

For list views. Uses LRU texture cache, mmapped atlas, worker pool, and latency metrics.

Thumbnails are generated during library scan, which will load all the artwork, resize it to 48x48, and write it to the atlas.
Any albums that cannot be resized and written to the atlas will be skipped and have a grey placeholder thumbnail in the UI.

```c
ArtworkManager *artwork_manager_new(library_cache_t *library, const char *atlas_path, size_t memory_limit);
void artwork_manager_free(ArtworkManager *mgr);

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

```

### Stats:

Each time the UI requests a thumbnail, the manager will be tracking the latency, cache hit/miss rates, etc... Evictions are also tracked, and the
internal functions to scan/seek album-ids from the atlas have latency hooks to track the time spent in the scan/seek.

`artwork_manager_get_stats()` for hits, misses, evictions, atlas hits, latency percentiles (p50/p90/p99).

### Atlas Format

The atlas is stored in .local/share/quadrature/unix-timestamp.atlas... When this library manager seeks for an album-id in the atlas, it will use the
latest atlas file. Atlas files will be re-generated during library indexing/scan to ensure that the album-ids are in sequential order so the file seek/mmap
offsets can be pre-computed. If albums get deleted from the library, those old album-ids will still be reserved until someone deletes both the sqlite db
and the atlas file for a full nuke/from-scratch library reset

## Full-Size Artwork

For detail views. There is no application-level caching, and `posix_fadvise(WILLNEED)` to prefetch into kernel page cache, then GTK loads directly.

```c
// Prefetch full-size artwork by album ID.
// Calls LibraryCache to resolve album_id → path and do the prefetch syscall.
void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, int64_t album_id);
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

## Targets

| Metric                       | Target |
| ---------------------------- | ------ |
| Thumb hit rate (browsing)    | >90%   |
| Thumb hit rate (fast scroll) | >70%   |
| Atlas load p99               | \<1ms  |
