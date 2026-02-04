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
build_ui(w);  // Views populate via sync fallback

library_cache_set_ready_callback(w->library_cache, on_cache_ready, w);
library_cache_start_warming(w->library_cache);

// Callback fires on main thread when warming completes:
static void on_cache_ready(void *data) {
    UiWindow *w = UI_WINDOW(data);
    library_view_refresh(w->artists_view);
    library_view_refresh(w->albums_view);
}

// After re-indexing:
library_cache_clear(w->library_cache);
library_cache_start_warming(w->library_cache);
library_view_refresh(w->artists_view);
library_view_refresh(w->albums_view);
```

**Important:** Warming must start *after* `build_ui()` to ensure views exist before the ready callback can fire, and to let the sync fallback complete before the warming thread can race with it.
