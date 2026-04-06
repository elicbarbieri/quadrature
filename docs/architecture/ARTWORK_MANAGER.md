# ArtworkManager

Two-tier artwork system: managed thumbnails with full caching infrastructure, full-size via kernel page cache.

**Dependency:** ArtworkManager depends on [LibraryCache](LIBRARY_CACHE.md) for full-size artwork prefetch (album-id → path resolution).

## Thumbnail API

For list views. Uses frequency-weighted LRU texture cache, per-library mmapped atlases, worker pool, and latency metrics.

Thumbnails are generated during library scan (Phase 4), resized to Npx (default 48), and packed into a per-library atlas file. Any albums without locatable artwork get a grey placeholder thumbnail in the UI.

```c
ArtworkManager *artwork_manager_new(library_cache_t *library,
                                    const char **data_roots,
                                    const char **music_roots,
                                    int lib_count,
                                    int cache_size, size_t cache_count);
void artwork_manager_free(ArtworkManager *mgr);

// Async load thumbnail into widget (cache hit = synchronous, miss = deferred worker)
void artwork_manager_get_thumbnail(ArtworkManager *mgr, int64_t album_id, GtkWidget *image);

// Reload a specific library's atlas after indexing completes
void artwork_manager_reload_library_atlas(ArtworkManager *mgr, int lib_idx,
                                          const char *atlas_path);
```

`data_roots` are used for atlas file lookups; `music_roots` are used as fallback paths for embedded artwork extraction.

### Global IDs

All `album_id` values passed to the ArtworkManager must be **global IDs** as defined in `library/library_id.h`:

```c
// Encode: upper 16 bits = library index, lower 48 bits = local SQLite ID
int64_t global_id = LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id);

// Decode (done internally by ArtworkManager):
int     lib_idx  = LIBRARY_GLOBAL_ID_LIB(global_id);   // → which per-library atlas
int64_t local_id = LIBRARY_GLOBAL_ID_LOCAL(global_id); // → binary search key
```

Library 0 global IDs are identical to their local SQLite IDs (upper 16 bits = 0), providing full backward compatibility for single-library use.

The library_cache already returns global IDs from all entity accessors, so callsites pass `album->album_id` directly without any encoding step.

### Atlas Format

Atlas files live alongside the library database:

```
{data_root}/artwork/{N}px-artwork-{unix_timestamp}.atlas
```

Header (32 bytes, packed):

```c
typedef struct __attribute__((packed)) {
    char magic[4];          // "QDRA"
    uint32_t version;       // 2
    uint32_t count;         // Number of entries
    uint32_t flags;         // Reserved
    uint32_t thumb_size;    // Thumbnail size in pixels (default 48)
    uint8_t channels;       // Color channels (3 = RGB)
    uint8_t reserved[11];
} artwork_atlas_header_t;
```

Body: `[sorted int64 album_ids][pixel_data: count × stride]` where `stride = thumb_size × thumb_size × channels`.

**Trailing checksum:** a CRC32 of the entire file (header + body) is appended as the final
4 bytes. On load, the reader verifies the checksum before mmapping. A mismatch (partial
write during power loss) causes a fallback to the previous atlas file — the 3-file rotation
guarantees at least one valid atlas exists. The artist atlas uses the same trailing CRC32.

- **One atlas per library root** — no shared global atlas
- **Timestamped filenames** — each indexer run produces a new file, enabling atomic swap without disturbing live readers
- **3-file rotation** — the indexer keeps only the 3 most recent atlas files per library per size; older files are deleted after a successful write
- **Startup load** — `artwork_manager_new()` calls `find_latest_atlas()` for each library, which scans `{root}/artwork/` and returns the path with the highest timestamp (lexicographic order is correct for fixed-width Unix timestamps)
- **Binary search** — entries sorted by `local_id`, O(log n) lookup

### Atlas Lifecycle

```
Indexer Phase 3 (ARTWORK):
  1. find_latest_existing_atlas() → preserve unchanged entries
  2. Generate new path: {N}px-artwork-{timestamp}.atlas
  3. Build new atlas (parallel image processing)
  4. artwork_atlas_builder_finish() → atomic rename from temp
  5. rotate_atlas_files() → delete all but 3 newest
  6. Store path in idx->atlas_path

INDEXER_COMPLETED callback fires:
  progress.atlas_path = new atlas path

IndexerController emits "completed" signal on main thread:
  on_indexer_done() in window.c:
    → artwork_manager_reload_library_atlas(mgr, lib_idx, progress.atlas_path)
      → clear texture cache (all entries)
      → lib_atlas_load() for the affected library slot
```

### Stats:

`artwork_manager_get_stats()` for hits, misses, evictions, atlas hits, latency percentiles (p50/p90/p99).

## Artist Thumbnails

For artist list views. Uses a **global** UUID-keyed atlas (shared across all libraries), separate texture cache, and shared workers.

```c
// Async load artist thumbnail into widget (cache hit = synchronous, miss = deferred worker)
void artwork_manager_get_artist_thumbnail(ArtworkManager *mgr, int64_t artist_id, GtkWidget *image);

// Reload the global artist atlas (no lib_idx — single shared atlas)
void artwork_manager_reload_artist_atlas(ArtworkManager *mgr);
```

### Global Artist Atlas

Unlike album atlases (per-library, int64 keys), the artist atlas is **global** and **UUID-keyed**:

```
~/.local/share/quadrature/atlas/artists.atlas
~/.local/share/quadrature/atlas/artists.atlas.lock   ← flock() write serialization
```

Header (32 bytes, packed):

```c
typedef struct __attribute__((packed)) {
    char magic[4];          // "QDAR"
    uint32_t version;       // 1
    uint32_t art_count;     // Number of entries with artwork
    uint32_t no_art_count;  // Number of known-no-artwork entries
    uint32_t thumb_size;    // Thumbnail size in pixels
    uint8_t channels;       // Color channels (3 = RGB)
    uint8_t reserved[11];
} artist_atlas_header_t;
```

Body layout:

```
[uuid_keys: uint8_t[art_count][16]]          sorted binary MusicBrainz UUIDs
[pixels: uint8_t[art_count][pixel_stride]]   dense RGB pixel data
[no_art_count: uint32_t]                     trailing count
[no_art_uuids: uint8_t[no_art_count][16]]   sorted binary UUIDs (skip on future runs)
```

- **Built during Phase 7** (fanart.tv artist art fetch)
- **UUID-keyed** — 16-byte binary MusicBrainz UUIDs, sorted for binary search
- **Write serialization** via `flock()` on lock file — safe across concurrent indexer runs
- **No timestamp rotation** — single file, atomically rewritten each run

### MBID Dedup (Multi-Library)

When an artist appears in multiple libraries with the same MusicBrainz ID, the library cache merges them into a single entity with `merged_source_ids[]`. The texture cache keys by the merged artist's global ID, so:

1. First request → resolve artist_id → MBID (via library cache) → binary search in global atlas
1. Subsequent requests → O(1) cache hit on the merged global ID
1. No separate MBID hashmap needed — dedup is implicit via cache merge

### Artist Cache

Separate from the album texture cache to avoid ID collisions. Same frequency-weighted LRU eviction strategy. Default capacity: 500 entries (~4.5 MB at 48x48 RGBA).

## Full-Size Artwork

For detail views. No application-level caching; `posix_fadvise(WILLNEED)` prefetches into kernel page cache, then GTK loads directly.

```c
// Prefetch full-size artwork by global album ID.
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
