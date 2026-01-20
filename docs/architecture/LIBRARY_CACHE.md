# Library Cache

**Foundation layer** for all library data access. Provides database queries, ID → path resolution, prefetch syscalls, and track navigation. Other components (ArtworkManager, AudioCache, AudioPipeline, UI) depend on LibraryCache—it has no external dependencies.

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
│                          ┌───────────────┐                                  │
│                          │    SQLite     │                                  │
│                          │   Database    │                                  │
│                          └───────────────┘                                  │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Purpose

**Foundation layer.** LibraryCache is the core API that everything else depends on:

- **UI Views** → query for artists, albums, tracks, search
- **AudioPipeline** → query for next/previous track resolution
- **AudioCache** → call prefetch for audio files (ID → path resolution)
- **ArtworkManager** → call prefetch for artwork (ID → path resolution)

**Key principle:** LibraryCache has **no dependencies** on AudioCache, ArtworkManager, or AudioPipeline. The dependency arrows point inward.

**Benefits:**
- Clean dependency graph (no circular dependencies)
- Single place for ID → path resolution
- Consistent caching of database queries
- Prefetch syscalls in one place

### Audio Engine Integration

The Audio Engine queries LibraryCache for next-track resolution, enabling instant auto-advance:

```
Without Library Cache:                With Library Cache:
─────────────────────────             ─────────────────────────

Track ends                            Track ends
     │                                     │
     v                                     v
Signal UI for next track              Query library_cache_get_next_track_id()
     │                                     │
     v                                     v
UI queries database                   Already know next_track_id
     │                                     │
     v                                     v
UI calls set_player_track()           Start playback from preloaded buffer
     │                                     │
     v                                     │
Engine loads track                         │
     │                                     │
     v                                     v
Playback starts (~50-100ms)           Playback starts (~1ms)
```

## Data Structures

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
    uint16_t year;
    uint16_t track_count;
    uint16_t disc_count;
    uint32_t total_duration_ms;
} library_album_info_t;

typedef struct {
    int64_t track_id;
    int64_t album_id;
    int64_t artist_id;
    char* path;              // Full file path for decoding
    char* title;
    char* artist_name;
    char* album_title;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
} library_track_info_t;
```

### Search Results

```c
typedef struct {
    GPtrArray* artists;      // library_artist_info_t*
    GPtrArray* albums;       // library_album_info_t*
    GPtrArray* tracks;       // library_track_info_t*
    size_t total_artists;    // Total matches (may exceed array size)
    size_t total_albums;
    size_t total_tracks;
} library_search_results_t;

typedef enum {
    SEARCH_FILTER_ALL,
    SEARCH_FILTER_ARTISTS,
    SEARCH_FILTER_ALBUMS,
    SEARCH_FILTER_TRACKS,
} library_search_filter_t;
```

### Sort Options

```c
typedef enum {
    SORT_NAME_ASC,
    SORT_NAME_DESC,
    SORT_YEAR_ASC,
    SORT_YEAR_DESC,
    SORT_RECENT,             // Recently added
    SORT_TRACK_NUM,          // For tracks within album
} library_sort_t;
```

### Cache Structure

```c
typedef struct library_cache {
    // Entity caches
    GHashTable* artists;          // artist_id → library_artist_info_t*
    GHashTable* albums;           // album_id → library_album_info_t*
    GHashTable* tracks;           // track_id → library_track_info_t*

    // Relationship caches
    GHashTable* album_tracks;     // album_id → GArray of track_ids (ordered)
    GHashTable* artist_albums;    // artist_id → GArray of album_ids
    GHashTable* artist_appearances; // artist_id → GArray of album_ids (featured)

    // List caches (populated on first query)
    GPtrArray* all_artists;       // Sorted artist list (NULL if not loaded)
    GPtrArray* all_albums;        // Sorted album list (NULL if not loaded)
    library_sort_t artists_sort;
    library_sort_t albums_sort;

    // Search cache (most recent query)
    char* last_search_query;
    library_search_filter_t last_search_filter;
    library_search_results_t* last_search_results;

    GMutex lock;

    // Database connection
    sqlite3* db;
    const char* db_path;
    const char* music_base;      // Base path for resolving full file paths

    // No external dependencies - LibraryCache is the foundation layer
} library_cache_t;
```

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

LibraryCache has no external dependencies to configure.

### Search

```c
// Full-text search across artists, albums, and tracks.
// Results are cached - repeated identical queries return cached results.
// Caller must NOT free results (owned by cache).
const library_search_results_t* library_cache_search(
    library_cache_t* cache,
    const char* query,
    library_search_filter_t filter,
    size_t limit                    // Max results per type (0 = unlimited)
);

// Clear search cache (e.g., on library change)
void library_cache_clear_search(library_cache_t* cache);
```

**Usage in SearchView:**

```c
static void on_search_changed(GtkSearchEntry* entry, SearchView* view) {
    const char* query = gtk_editable_get_text(GTK_EDITABLE(entry));

    const library_search_results_t* results = library_cache_search(
        view->library,
        query,
        view->current_filter,
        view->current_filter == SEARCH_FILTER_ALL ? 10 : 0
    );

    // Update UI with results
    populate_search_results(view, results);
}
```

### List Views

```c
// Get all artists (cached, sorted).
// Returns GPtrArray of library_artist_info_t*.
// Caller must NOT free array (owned by cache).
const GPtrArray* library_cache_get_artists(
    library_cache_t* cache,
    library_sort_t sort
);

// Get all albums (cached, sorted).
const GPtrArray* library_cache_get_albums(
    library_cache_t* cache,
    library_sort_t sort
);

// Get albums by artist.
// Returns GPtrArray of library_album_info_t*.
const GPtrArray* library_cache_get_albums_by_artist(
    library_cache_t* cache,
    int64_t artist_id
);

// Get tracks by album (ordered by disc_num, track_num).
// Returns GPtrArray of library_track_info_t*.
const GPtrArray* library_cache_get_tracks_by_album(
    library_cache_t* cache,
    int64_t album_id
);

// Get albums where artist appears but isn't primary artist ("Appears On").
const GPtrArray* library_cache_get_artist_appearances(
    library_cache_t* cache,
    int64_t artist_id
);

// Get tracks where artist appears on albums they don't own.
const GPtrArray* library_cache_get_artist_appearance_tracks(
    library_cache_t* cache,
    int64_t artist_id
);
```

**Usage in ArtistsView:**

```c
static void populate_artists_list(ArtistsView* view) {
    const GPtrArray* artists = library_cache_get_artists(
        view->library,
        SORT_NAME_ASC
    );

    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t* artist = g_ptr_array_index(artists, i);
        GtkWidget* row = ui_create_artist_row(artist);
        gtk_list_box_append(view->list, row);
    }
}
```

### Entity Info (Single Item)

```c
// Get artist by ID (cached).
const library_artist_info_t* library_cache_get_artist(
    library_cache_t* cache,
    int64_t artist_id
);

// Get album by ID (cached).
const library_album_info_t* library_cache_get_album(
    library_cache_t* cache,
    int64_t album_id
);

// Get track by ID (cached).
const library_track_info_t* library_cache_get_track(
    library_cache_t* cache,
    int64_t track_id
);
```

### Track Navigation (Instant Resolution)

These APIs are designed for **instant response** during rapid skip operations (skip-skip-skip).

```c
// Get next track in album.
// Returns 0 if current track is last in album.
// Handles multi-disc albums correctly (disc 1 track N → disc 2 track 1).
int64_t library_cache_get_next_track_id(
    library_cache_t* cache,
    int64_t current_track_id
);

// Get previous track in album.
// Returns 0 if current track is first in album.
int64_t library_cache_get_prev_track_id(
    library_cache_t* cache,
    int64_t current_track_id
);
```

**When resolution is triggered:**

The Audio Engine calls `get_next_track_id()` **immediately** on:
- **Track load** - resolve next track for preloading
- **Skip forward** - resolve new next track after advancing
- **Skip backward** - resolve new next track after going back
- **Repeat toggle off** - resolve next track (was previously ignored)
- **Repeat toggle on** - clear next track (will loop instead)

This enables the rapid skip scenario:

```
User action:          skip → skip → skip → skip
                        │      │      │      │
Engine response:      [next already preloaded - instant!]
                        │      │      │      │
                        v      v      v      v
                      play   play   play   play
                      │      │      │      │
Background:           └──resolve next──────────┘
                           (async preload)
```

**Key guarantee:** The `next_track_id` is always resolved and preloaded *before* the user can skip again. Each skip is instant because the buffer is already decoded.

### Prefetch API

LibraryCache provides prefetch syscalls for other components. It resolves IDs to paths and calls `posix_fadvise(WILLNEED)` to hint the kernel.

```c
// Prefetch full-size album artwork into kernel page cache.
// Resolves album_id → album path, tries artwork files in discovery order.
// Called by ArtworkManager when user clicks album row.
void library_cache_prefetch_fullsize_artwork(
    library_cache_t* cache,
    int64_t album_id
);

// Prefetch audio files into kernel page cache.
// Resolves track_ids → file paths, prefetches each file.
// Called by AudioCache for visible search results / track lists.
void library_cache_prefetch_audio_files(
    library_cache_t* cache,
    const int64_t* track_ids,
    size_t count
);
```

**Implementation:**

```c
void library_cache_prefetch_fullsize_artwork(library_cache_t* cache, int64_t album_id) {
    // Resolve album_id → path
    const library_album_info_t* album = library_cache_get_album(cache, album_id);
    if (!album) return;

    // Build full path to album directory
    char album_path[PATH_MAX];
    snprintf(album_path, sizeof(album_path), "%s/%s", cache->music_base, album->path);

    // Try artwork files in discovery order
    static const char* art_names[] = {
        "art.jpg", "cover.jpg", "folder.jpg", "album.jpg", "front.jpg", NULL
    };

    for (const char** name = art_names; *name; name++) {
        char art_path[PATH_MAX];
        snprintf(art_path, sizeof(art_path), "%s/%s", album_path, *name);
        if (access(art_path, R_OK) == 0) {
            prefetch_file(art_path);
            return;
        }
    }
}

void library_cache_prefetch_audio_files(library_cache_t* cache,
                                         const int64_t* track_ids, size_t count) {
    g_mutex_lock(&cache->lock);

    for (size_t i = 0; i < count; i++) {
        const library_track_info_t* track = get_track_unlocked(cache, track_ids[i]);
        if (track) {
            prefetch_file(track->path);
        }
    }

    g_mutex_unlock(&cache->lock);
}

// Internal: prefetch file into kernel page cache
static void prefetch_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) == 0) {
        posix_fadvise(fd, 0, st.st_size, POSIX_FADV_WILLNEED);
    }
    close(fd);
}
```

**Who calls these APIs:**

| Caller         | API                              | When                          |
| -------------- | -------------------------------- | ----------------------------- |
| ArtworkManager | `prefetch_fullsize_artwork()`    | User clicks album row         |
| AudioCache     | `prefetch_audio_files()`         | Visible tracks in search/list |

**Note:** Thumbnails are handled entirely by ArtworkManager via its atlas system (pre-generated thumbnails, texture cache). LibraryCache is not involved in thumbnail loading.

### Cache Management

```c
// Invalidate single entity (after metadata edit).
void library_cache_invalidate_track(library_cache_t* cache, int64_t track_id);
void library_cache_invalidate_album(library_cache_t* cache, int64_t album_id);
void library_cache_invalidate_artist(library_cache_t* cache, int64_t artist_id);

// Clear all cached data (on library change or re-index).
void library_cache_clear(library_cache_t* cache);

// Refresh lists (after re-index completes).
void library_cache_refresh_lists(library_cache_t* cache);
```

## Caching Strategy

### On-Demand with Album Prefetch

When a track is requested, the cache:

1. Check `tracks` hash table for cached info
2. If miss: query database for track
3. If album not cached: prefetch all tracks in album (for navigation)
4. Store in cache and return

```c
const library_track_info_t* library_cache_get_track(library_cache_t* cache, int64_t track_id) {
    g_mutex_lock(&cache->lock);

    library_track_info_t* info = g_hash_table_lookup(cache->tracks, &track_id);
    if (info) {
        g_mutex_unlock(&cache->lock);
        return info;
    }

    // Cache miss - fetch from database
    info = fetch_track_from_db(cache->db, track_id);
    if (!info) {
        g_mutex_unlock(&cache->lock);
        return NULL;
    }

    g_hash_table_insert(cache->tracks, &info->track_id, info);

    // Prefetch album for navigation
    if (!g_hash_table_contains(cache->album_tracks, &info->album_id)) {
        prefetch_album_tracks(cache, info->album_id);
    }

    g_mutex_unlock(&cache->lock);
    return info;
}
```

### Album Track Order

Tracks within an album are stored ordered by (disc_num, track_num):

```c
static void prefetch_album_tracks(library_cache_t* cache, int64_t album_id) {
    const char* sql =
        "SELECT id FROM tracks WHERE album_id = ? "
        "ORDER BY disc_num ASC, track_num ASC";

    GArray* track_ids = g_array_new(FALSE, FALSE, sizeof(int64_t));

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(cache->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, album_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        g_array_append_val(track_ids, id);
    }

    sqlite3_finalize(stmt);

    int64_t* key = g_new(int64_t, 1);
    *key = album_id;
    g_hash_table_insert(cache->album_tracks, key, track_ids);
}
```

### Next/Previous Resolution

```c
int64_t library_cache_get_next_track_id(library_cache_t* cache, int64_t current_track_id) {
    g_mutex_lock(&cache->lock);

    const library_track_info_t* info = library_cache_get_track_unlocked(cache, current_track_id);
    if (!info) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    GArray* album = g_hash_table_lookup(cache->album_tracks, &info->album_id);
    if (!album) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    // Find current position in album
    for (guint i = 0; i < album->len; i++) {
        if (g_array_index(album, int64_t, i) == current_track_id) {
            if (i + 1 < album->len) {
                int64_t next_id = g_array_index(album, int64_t, i + 1);
                g_mutex_unlock(&cache->lock);
                return next_id;
            }
            break;
        }
    }

    g_mutex_unlock(&cache->lock);
    return 0;  // Last track in album
}
```

## Thread Safety

```
┌────────────────────────────────────────────────────────────────────────────┐
│ Operation                    │ Thread       │ Lock      │ Notes            │
├────────────────────────────────────────────────────────────────────────────┤
│ search()                     │ UI           │ cache     │ May trigger DB   │
│ get_artists()                │ UI           │ cache     │ First loads all  │
│ get_albums()                 │ UI           │ cache     │ First loads all  │
│ get_albums_by_artist()       │ UI           │ cache     │ May trigger DB   │
│ get_tracks_by_album()        │ UI           │ cache     │ May trigger DB   │
│ get_artist()                 │ UI/Engine    │ cache     │ Hash lookup      │
│ get_album()                  │ UI/Engine    │ cache     │ Hash lookup      │
│ get_track()                  │ UI/Engine    │ cache     │ Hash lookup      │
│ get_next_track_id()          │ Engine       │ cache     │ Album cache req'd│
│ get_prev_track_id()          │ Engine       │ cache     │ Album cache req'd│
│ prefetch_fullsize_artwork()  │ ArtworkMgr   │ cache     │ Resolves path    │
│ prefetch_audio_files()       │ AudioCache   │ cache     │ Resolves paths   │
│ invalidate_*()               │ UI           │ cache     │ After edit       │
│ clear()                      │ UI           │ cache     │ On library switch│
└────────────────────────────────────────────────────────────────────────────┘
```

**Design for low contention:**
- Most lookups are cache hits (hash table, no DB query)
- Album/artist lists loaded once, reused for all queries
- Engine accesses (next/prev track) are fast hash lookups
- UI queries hold lock briefly; prefetch calls release lock before I/O
- Invalidation is rare (only on metadata edit)

## Memory Usage

Entity sizes (including strings):
- Artist info: ~100 bytes
- Album info: ~200 bytes
- Track info: ~300 bytes

| Library Size   | Artists | Albums  | Tracks  | Total     |
| -------------- | ------- | ------- | ------- | --------- |
| 5,000 tracks   | ~50 KB  | ~200 KB | ~1.5 MB | ~1.8 MB   |
| 50,000 tracks  | ~500 KB | ~2 MB   | ~15 MB  | ~18 MB    |
| 500,000 tracks | ~5 MB   | ~20 MB  | ~150 MB | ~175 MB   |

**Notes:**
- Full artist/album lists are loaded on first query and cached
- Track details are loaded on-demand per album
- Search results are cached until query changes
- For very large libraries (>100k tracks), consider lazy loading artist/album lists with pagination

## Integration with Audio Engine

The pipeline owns the library cache and passes it to players:

```c
struct audio_pipeline {
    audio_player_t players[4];
    audio_cache_t* cache;           // Decoded PCM buffers
    library_cache_t* library;       // Track metadata
    // ...
};
```

When a player needs the next track:

```c
// In audio_pipeline_set_player_track()
int64_t next_id = library_cache_get_next_track_id(pipeline->library, track_id);
if (next_id > 0 && !player->repeat) {
    audio_cache_lock(pipeline->cache, next_id);
    audio_cache_load(pipeline->cache, next_id, NULL, NULL);
    player->next_track_id = next_id;
}
```

## Database Schema Requirements

Library Cache expects the following schema:

```sql
CREATE TABLE artists (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL COLLATE NOCASE
);
CREATE INDEX idx_artists_name ON artists(name COLLATE NOCASE);

CREATE TABLE albums (
    id INTEGER PRIMARY KEY,
    artist_id INTEGER NOT NULL REFERENCES artists(id),
    title TEXT NOT NULL,
    path TEXT NOT NULL,          -- Relative path to album directory
    year INTEGER,
    UNIQUE(artist_id, title)
);
CREATE INDEX idx_albums_artist ON albums(artist_id);
CREATE INDEX idx_albums_year ON albums(year);

CREATE TABLE tracks (
    id INTEGER PRIMARY KEY,
    album_id INTEGER NOT NULL REFERENCES albums(id),
    artist_id INTEGER NOT NULL REFERENCES artists(id),
    path TEXT NOT NULL,          -- Full file path
    title TEXT,
    duration_ms INTEGER,
    track_num INTEGER DEFAULT 1,
    disc_num INTEGER DEFAULT 1
);
CREATE INDEX idx_tracks_album ON tracks(album_id, disc_num, track_num);
CREATE INDEX idx_tracks_artist ON tracks(artist_id);

-- Full-text search
CREATE VIRTUAL TABLE tracks_fts USING fts5(
    title,
    content='tracks',
    content_rowid='id'
);

CREATE VIRTUAL TABLE artists_fts USING fts5(
    name,
    content='artists',
    content_rowid='id'
);

CREATE VIRTUAL TABLE albums_fts USING fts5(
    title,
    content='albums',
    content_rowid='id'
);
```

**Key requirements:**
- `disc_num` and `track_num` enable correct ordering for multi-disc albums
- `albums.path` stores relative path for artwork resolution
- FTS5 tables enable fast full-text search
- `COLLATE NOCASE` on artist names for case-insensitive sorting
