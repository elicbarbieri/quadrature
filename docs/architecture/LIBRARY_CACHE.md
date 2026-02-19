# Library Cache

**Foundation layer** for all library data access. Uses flat arrays indexed by entity ID for O(1) lookups, with a background warming thread that pages all data into cache after startup. Two DB connections (`db_ui` for the main thread, `db_warm` for the warming thread) provide zero contention via SQLite WAL.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              CONSUMERS                                      │
│                                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ ArtworkMgr   │  │  AudioCache  │  │AudioPipeline │  │    UI Views  │    │
│  │              │  │              │  │              │  │              │    │
│  │ prefetch_    │  │ prefetch_    │  │ get_next_    │  │ search()     │    │
│  │ fullsize_    │  │ audio_files  │  │ track_id()   │  │ get_albums() │    │
│  │ artwork()    │  │ ()           │  │              │  │ get_tracks() │    │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘    │
│         │                 │                 │                 │            │
│         └─────────────────┴─────────────────┴─────────────────┘            │
│                                   │                                         │
│                                   v                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                        LIBRARY CACHE                                │   │
│   │                                                                     │   │
│   │  Query API:              Prefetch API:         Track Navigation:    │   │
│   │  - search()              - prefetch_fullsize_  - get_next_track_id()│   │
│   │  - get_artists()           artwork(album_id)   - get_prev_track_id()│   │
│   │  - get_albums()          - prefetch_audio_                          │   │
│   │  - get_tracks_by_album()   files(track_ids)                         │   │
│   │  - get_artist()                                                     │   │
│   │  - get_album()           (resolves IDs → paths, does posix_fadvise) │   │
│   │  - get_track()                                                      │   │
│   │                                                                     │   │
│   │  NO dependencies on ArtworkManager, AudioCache, or AudioPipeline    │   │
│   │                                                                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                   │                                         │
│                                   v                                         │
│                    ┌───────────────────────────────┐                        │
│                    │    SQLite (WAL mode)          │                        │
│                    │  db_ui      │   db_warm       │                        │
│                    │  (main thr) │   (warming thr) │                        │
│                    └───────────────────────────────┘                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Purpose

**Foundation layer.** LibraryCache is the core API that everything else depends on:

- **UI Views** → query for artists, albums, tracks, search
- **AudioPipeline** → query for next/previous track resolution
- **AudioCache** → call prefetch for audio files (ID → path resolution)
- **ArtworkManager** → call prefetch for artwork (ID → path resolution)

**Key principle:** LibraryCache has **no dependencies** on AudioCache, ArtworkManager, or AudioPipeline. The dependency arrows point inward.

## Data Structures

### Flat Array Storage

Entities are stored in pointer arrays indexed directly by SQLite auto-increment ID. Since IDs are dense sequential integers (1, 2, 3, ...), this gives O(1) lookup with minimal memory overhead.

```c
struct library_cache {
    // Entity arrays (indexed by ID — O(1) lookup)
    library_artist_info_t** artists;    // artists[artist_id] → info or NULL
    size_t artists_capacity;            // max_artist_id + 1
    library_album_info_t**  albums;     // albums[album_id] → info or NULL
    size_t albums_capacity;
    library_track_info_t**  tracks;     // tracks[track_id] → info or NULL
    size_t tracks_capacity;

    // Relationship arrays (indexed by entity ID)
    GArray**    album_tracks;           // album_tracks[album_id] → GArray<int64_t>
    GPtrArray** track_artists;          // track_artists[track_id] → GPtrArray<library_track_artist_t*>
    GPtrArray** artist_albums;          // artist_albums[artist_id] → GPtrArray<library_album_info_t*>
    GPtrArray** artist_appearances;     // artist_appearances[artist_id] → GPtrArray<library_album_info_t*>
    GPtrArray** artist_appearance_tracks; // indexed by artist_id

    // Sorted list caches (NULL until warming or sync fallback populates them)
    GPtrArray* all_artists;
    GPtrArray* all_albums;

    // Warming thread state
    quadrature_db_t* db_warm;           // Second readonly connection for warming
    GThread*         warm_thread;
    atomic_int       warm_cancel;
    atomic_int       warm_state;        // IDLE / WARMING / READY

    GMutex lock;
    quadrature_db_t* db;                // UI readonly connection (main thread only)
};
```

Arrays are sized at creation via `SELECT MAX(id) FROM table`. Gaps from deleted rows are just NULL slots.

### Entity Info Types

```c
typedef struct {
    int64_t artist_id;
    char* name;
    uint32_t album_count;
    uint32_t track_count;
    uint32_t total_duration_ms;
} library_artist_info_t;

typedef struct {
    int64_t album_id;
    int64_t artist_id;
    char* title;
    char* artist_name;
    char* path;              // Relative path to album directory
    char* genres;            // Semicolon-separated distinct genres, or NULL
    uint16_t year;
    uint16_t track_count;
    uint16_t disc_count;
    uint32_t total_duration_ms;
} library_album_info_t;

typedef struct {
    int64_t track_id;
    int64_t album_id;
    int64_t artist_id;       // Primary artist ID (position 0)
    char* path;              // Full file path for decoding
    char* title;
    char* artist_name;       // Primary artist name
    char* artist_display;    // Formatted: "Artist A feat. Artist B" (or NULL)
    char* album_title;
    char* genre;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
} library_track_info_t;

typedef struct {
    int64_t artist_id;
    char* name;
    library_artist_role_t role;  // PRIMARY or FEATURING
    int position;                // Display order
} library_track_artist_t;
```

### Search Results

```c
typedef struct {
    GPtrArray* artists;      // library_artist_info_t* (cache-owned pointers)
    GPtrArray* albums;       // library_album_info_t*  (cache-owned pointers)
    GPtrArray* tracks;       // library_track_info_t*  (cache-owned pointers)
    size_t total_artists;    // Total matches (may exceed array size)
    size_t total_albums;
    size_t total_tracks;
} library_search_results_t;
```

Search results contain pointers into the cache — the arrays are caller-owned but the items are cache-owned. Free with `library_search_results_free()`.

## Cache Warming

### Startup Sequence

```
library_cache_create()
  → opens db_ui and db_warm (two readonly connections)
  → SELECT MAX(id) for artists/albums/tracks → sizes flat arrays

build_ui()
  → library_view_new() calls get_artists() / get_albums()
  → all_artists == NULL → sync fallback loads via db_ui
  → views populate immediately

library_cache_start_warming()
  → spawns background thread using db_warm
  → Phase 1: Page artists (1000/page)    → cache->artists[]
  → Phase 2: Page albums  (1000/page)    → cache->albums[]
  → Phase 3: Load tracks per album       → cache->tracks[] + album_tracks[]
  → Phase 4: Populate artist_display + cache track_artists
  → Phase 5: Compute aggregates (duration, counts, "appears on")
  → g_idle_add(ready_cb) → UI refreshes with complete data
```

### Threading Model

```
Main Thread (GTK)                     Warming Thread (db_warm)
═══════════════                       ═══════════════════════

library_cache_create()
  → opens db_ui, db_warm
build_ui()
  → get_artists() sync fallback        (not started yet)
  → views populated
library_cache_start_warming()
  → spawns thread ─────────────────→  warming_thread_func()
                                        │
UI: get_artists_filtered()              │ page artists via db_warm
  → ID query via db_ui                  │   mutex_lock → insert → unlock
  → resolve IDs from cache              │ ...
                                        │ page albums via db_warm
                                        │ load tracks per album
                                        │ compute aggregates
                                        │ atomic_store(state, READY)
                                        │ g_idle_add(ready_cb)
                                        ▼
on_cache_ready() ◄── idle fires       [thread exits]
  → library_view_refresh()
  → views re-populate with full data
```

**Key invariants:**
- `db_ui` is only used from the main thread. `db_warm` is only used from the warming thread. SQLite WAL allows concurrent readers — zero DB contention.
- The cache `GMutex` is held briefly (pointer insertions only, no I/O). Neither thread blocks the other meaningfully.
- Warming thread only *adds* entries, never removes/replaces. Only `library_cache_clear()` removes entries, and it joins the warming thread first.
- If the sync fallback has already built `all_artists`/`all_albums`, the warming thread skips replacing them — prevents use-after-free races.

### Filtered Query Path

Filtered queries use ID-only SQL resolved against the in-memory cache:

```
UI keystroke → get_artists_filtered(search_text, genre/year filters)
  → SQL: SELECT a.id FROM artists a WHERE ... (no JOINs, no GROUP BY)
  → Returns int64_t[] IDs
  → Cache resolves: cache->artists[id] for each ID
  → Returns GPtrArray of cache-owned pointers
```

This is ~10x faster than the old path which executed full multi-JOIN GROUP BY queries per keystroke.

## API Reference

### Lifecycle

```c
quadrature_result_t library_cache_create(
    const char* db_path,
    const char* music_base,
    library_cache_t** out
);
void library_cache_destroy(library_cache_t* cache);
```

Creates two readonly DB connections internally (`db_ui` + `db_warm`). Allocates flat arrays sized by `MAX(id)`.

### Cache Warming

```c
typedef enum {
    LIBRARY_CACHE_IDLE = 0,
    LIBRARY_CACHE_WARMING = 1,
    LIBRARY_CACHE_READY = 2,
} library_cache_state_t;

typedef void (*library_cache_ready_cb)(void* user_data);

void library_cache_set_ready_callback(library_cache_t* cache,
                                       library_cache_ready_cb cb, void* user_data);
void library_cache_start_warming(library_cache_t* cache);
library_cache_state_t library_cache_get_state(library_cache_t* cache);
```

`start_warming()` spawns a background thread. `ready_cb` fires on the main thread via `g_idle_add` when warming completes. Safe to call multiple times (no-op if already warming or ready).

### Entity Getters (Single Item)

```c
const library_track_info_t* library_cache_get_track(library_cache_t* cache, int64_t track_id);
const library_album_info_t* library_cache_get_album(library_cache_t* cache, int64_t album_id);
const library_artist_info_t* library_cache_get_artist(library_cache_t* cache, int64_t artist_id);
const GPtrArray* library_cache_get_track_artists(library_cache_t* cache, int64_t track_id);
```

O(1) flat array lookup. On cache miss (before warming completes), falls back to `db_ui` query.

### List Queries

```c
// Sorted full lists (cache-owned, do not free)
const GPtrArray* library_cache_get_artists(library_cache_t* cache, library_sort_t sort);
const GPtrArray* library_cache_get_albums(library_cache_t* cache, library_sort_t sort);

// Relationship queries (cache-owned, do not free)
const GPtrArray* library_cache_get_tracks_by_album(library_cache_t* cache, int64_t album_id);
const GPtrArray* library_cache_get_albums_by_artist(library_cache_t* cache, int64_t artist_id);
const GPtrArray* library_cache_get_artist_appearances(library_cache_t* cache, int64_t artist_id);
const GPtrArray* library_cache_get_artist_appearance_tracks(library_cache_t* cache, int64_t artist_id);

// Filtered queries (caller owns array, cache owns items — g_ptr_array_unref when done)
GPtrArray* library_cache_get_artists_filtered(library_cache_t* cache,
    library_sort_t sort, const char* search_text, const db_search_opts_t* filters);
GPtrArray* library_cache_get_albums_filtered(library_cache_t* cache,
    library_sort_t sort, const char* search_text, const db_search_opts_t* filters);
```

If `all_artists`/`all_albums` is NULL (warming not done), a sync fallback loads from `db_ui`.

Filtered queries execute ID-only SQL via `db_ui`, then resolve IDs against the cache. Items not yet warmed are skipped — the view refreshes when warming completes.

### Search

```c
library_search_results_t* library_cache_search(
    library_cache_t* cache,
    const char* query,
    library_search_filter_t filter,
    size_t limit,
    const db_search_opts_t* opts
);
void library_search_results_free(library_search_results_t* results);
```

Uses ID-only queries for artists/albums and FTS for tracks, resolved against the cache. Results contain cache-owned pointers. Caller must free with `library_search_results_free()`.

### Track Navigation (Instant Resolution)

```c
int64_t library_cache_get_next_track_id(library_cache_t* cache, int64_t current_track_id);
int64_t library_cache_get_prev_track_id(library_cache_t* cache, int64_t current_track_id);
```

O(1) lookup in `album_tracks` array. Handles multi-disc albums correctly (disc 1 track N → disc 2 track 1). Returns 0 at album boundaries.

### Prefetch API

```c
void library_cache_prefetch_fullsize_artwork(library_cache_t* cache, int64_t album_id);
void library_cache_prefetch_audio_files(library_cache_t* cache, const int64_t* track_ids, size_t count);
```

Resolves IDs → paths from the cache, then calls `posix_fadvise(WILLNEED)` to hint the kernel page cache.

### Cache Management

```c
void library_cache_clear(library_cache_t* cache);
```

Joins the warming thread, frees all entity arrays, re-allocates fresh arrays, resets state to IDLE. Called after re-indexing, followed by `start_warming()`.

## Pointer Lifetimes & UI Safety

### Interior Pointers

Every pointer returned by a `library_cache_get_*` function is an **interior pointer** — a raw address into the cache's own heap-allocated slot arrays. These pointers are stable for as long as the cache is warm, but become dangling the moment `library_cache_clear()` or `library_cache_clear_slot()` runs.

```
cache->slots[lib_idx]->track_artists[local_id]  ← the actual GPtrArray on the heap
                                          ▲
library_cache_get_track_artists() ────────┘  returns a raw pointer to this
```

`library_cache_clear()` calls `free_slot_arrays()`, which calls `g_ptr_array_unref()` on every entry.  Any code still holding a pointer into those arrays is now reading freed memory.

### The Safety Rule

> **Never store a raw cache pointer in widget data that outlives the row-creation call.**

| Safe | Unsafe |
|---|---|
| Use a cache pointer to set a label's text, then discard it | Store a cache pointer in a tick-callback struct |
| Use a cache pointer for artwork lookup and then discard it | Store a cache pointer in a GObject data key for async access |
| Store `(cache, entity_id)` in long-lived widget data | Store `const GPtrArray *track_artists` in long-lived widget data |

The rule exists because `library_cache_clear()` is called from the main thread after re-indexing, and GTK tick callbacks, `"map"` signal handlers, and other long-lived widget callbacks also run on the main thread. They are interleaved, not concurrent — but they are not scoped to the same call. A tick callback created during row setup at time T may run after `library_cache_clear()` runs at time T+N.

### Correct Pattern: Lookup Key, Not Pointer

When widget data structures need ongoing access to cache relationship data (e.g. to re-populate artist buttons on window resize), store the lookup key and re-fetch:

```c
// BAD — raw interior pointer in long-lived widget data
typedef struct {
    const GPtrArray *track_artists;   // dangling after library_cache_clear()
    ...
} ArtistBoxData;

// GOOD — lookup key; re-fetch on every use
typedef struct {
    library_cache_t *cache;           // stable for app lifetime
    int64_t          track_id;        // stable entity ID
    ...
} ArtistBoxData;

// In the tick/map callback:
const GPtrArray *artists = library_cache_get_track_artists(abd->cache, abd->track_id);
if (!artists || artists->len == 0) { gtk_widget_set_visible(box, FALSE); return; }
// use `artists` within this call only — do not re-store it
```

After `library_cache_clear()`, `library_cache_get_track_artists()` returns `NULL` (arrays are reallocated but empty) or falls back to `db_ui` if the on-demand path is triggered. Either way, no dangling pointer is accessed.

### Row Creation Scope

During a row-creation function (e.g. `ui_create_track_row`), all use of cache interior pointers must complete before the function returns. This is safe because `library_cache_clear()` runs on the main thread and cannot race with the row-creation call:

```c
GtkWidget *ui_create_track_row(const library_track_info_t *track, ...) {
    // Immediate use of interior pointers — safe within this call scope:
    gtk_label_set_text(title_label, track->title);          // OK
    artwork_manager_get_thumbnail(art_mgr, track->album_id, art_image);  // OK

    // Do NOT store track->title, track->album_id, etc. in widget data for
    // later async use. Store entity IDs and re-fetch when needed.

    g_object_set_data(G_OBJECT(row), "track-id",
                      GSIZE_TO_POINTER((gsize)track->track_id));  // OK: ID, not pointer
}
```

### List View Data Lifecycle

**GtkListBox rows** (`GtkListBox` + `gtk_list_box_append`):
- Full widget trees created by `ui_create_track_row` / `ui_create_album_row` etc.
- When a view refreshes, all rows are removed (`gtk_list_box_remove`) and a fresh batch is created.
- Each row's tick callbacks are removed when the row widget is destroyed. Rows are destroyed synchronously when removed from the list.
- Between row removal and `library_cache_clear()`, no dangling pointers exist — the widgets are gone.

**GtkListItemFactory rows** (`lazy_list.c`, virtualized lists):
- Item widgets are recycled. `bind` runs to populate the widget for a specific model item; `unbind` runs to clear it before recycling.
- Cache pointers used during `bind` for immediate widget setup (setting labels, requesting artwork) are used and discarded within `bind`. They are not stored in the item widget for later re-use.
- Because the list can be displaying rows while `library_cache_clear()` fires, widget setup code in `bind` callbacks follows the same rule: use and discard cache pointers immediately, never store them.

**ArtworkManager thumbnails** (async, cross-invalidation safe):
- `artwork_manager_get_thumbnail(mgr, album_id, gtk_image)` registers an async callback.
- The callback uses `g_object_add_weak_pointer` on the `GtkImage`. If the row is destroyed before the callback fires, the weak pointer is nulled and the callback silently skips the update.
- This design is safe across both widget destruction and cache invalidation (artwork IDs are stable).

### Post-Reindex Lifecycle

```
Indexer thread: completes scan + artwork write
  → g_idle_add or GLib signal → fires on main thread

Main thread:
  library_cache_clear(cache)         ← joins warm thread, frees all slot arrays
                                        ALL interior pointers are now invalid
  artwork_manager_reload_library_atlas(...)  ← remaps atlas file
  library_cache_start_warming(cache) ← spawns fresh warm thread
  [views refresh to show loading state]

Warming thread: repopulates slot arrays under cache->lock (brief, no I/O)

Main thread (warm complete, g_idle_add from warm thread):
  on_cache_ready() → views refresh with new data
```

Between `clear()` and `on_cache_ready()`, `library_cache_get_*` calls fall back to `db_ui` for on-demand queries — views continue to function, they just don't have the full warmed dataset yet.

## Thread Safety

```
┌────────────────────────────────────────────────────────────────────────────┐
│ Operation                    │ Thread       │ DB Conn   │ Notes            │
├────────────────────────────────────────────────────────────────────────────┤
│ warming_thread_func()        │ Warming      │ db_warm   │ Holds lock brief │
│ search()                     │ UI           │ db_ui     │ ID query + cache │
│ get_artists()                │ UI           │ db_ui*    │ *sync fallback   │
│ get_albums()                 │ UI           │ db_ui*    │ *sync fallback   │
│ get_artists_filtered()       │ UI           │ db_ui     │ ID query + cache │
│ get_albums_filtered()        │ UI           │ db_ui     │ ID query + cache │
│ get_tracks_by_album()        │ UI           │ db_ui*    │ *on cache miss   │
│ get_artist()                 │ UI/Engine    │ —         │ Array lookup     │
│ get_album()                  │ UI/Engine    │ —         │ Array lookup     │
│ get_track()                  │ UI/Engine    │ db_ui*    │ *on cache miss   │
│ get_next_track_id()          │ Engine       │ —         │ Array lookup     │
│ get_prev_track_id()          │ Engine       │ —         │ Array lookup     │
│ prefetch_fullsize_artwork()  │ ArtworkMgr   │ —         │ Cache + fadvise  │
│ prefetch_audio_files()       │ AudioCache   │ —         │ Cache + fadvise  │
│ clear()                      │ UI           │ —         │ Joins warm thread│
└────────────────────────────────────────────────────────────────────────────┘
```

**Design for low contention:**

- Most lookups are direct array index — no DB, no hash
- `db_ui` and `db_warm` are separate connections — SQLite WAL allows concurrent readers
- Cache mutex held briefly (pointer writes only, no I/O under lock)
- Engine accesses (next/prev track) are pure array lookups — no lock needed after warming
- Warming thread only adds, never removes — no ABA races

## Memory Usage

Entity sizes (including strings):

- Artist info: ~100 bytes
- Album info: ~200 bytes
- Track info: ~300 bytes
- Pointer arrays: 8 bytes per slot

| Library Size   | Artists | Albums  | Tracks  | Arrays  | Total   |
| -------------- | ------- | ------- | ------- | ------- | ------- |
| 5,000 tracks   | ~50 KB  | ~200 KB | ~1.5 MB | ~80 KB  | ~1.8 MB |
| 50,000 tracks  | ~500 KB | ~2 MB   | ~15 MB  | ~800 KB | ~18 MB  |
| 500,000 tracks | ~5 MB   | ~20 MB  | ~150 MB | ~8 MB   | ~183 MB |

All data is loaded during warming. The flat arrays add minimal overhead (~8 bytes per entity slot) compared to the data itself.

## Integration with Window

```c
// In ui_window_new():
build_ui(w);
library_cache_set_ready_callback(w->library_cache, on_cache_ready, w);
library_cache_start_warming(w->library_cache);

// on_cache_ready delegates to centralized refresh:
static void on_cache_ready(void *data) {
    UiWindow *w = UI_WINDOW(data);
    refresh_library_views(w);
}

// refresh_library_views is the SINGLE place that decides what to refresh:
// - artists_view, albums_view (list rebuilds)
// - detail_view (if open — reloads current album/artist/meta-artist)
// - search results (if VIEW_SEARCH active)
//
// Called from: on_cache_ready (after warm), on_indexer_artwork_ready (after atlas reload)
```

**Important:** Warming must start *after* `build_ui()` to ensure views exist before the ready callback can fire, and to let the sync fallback complete before the warming thread can race with it.
