# Cross-Library Deduplication

**Ensures that the same artist or album appears once in every view**, regardless of how many libraries contain it. Builds on the existing multi-library cache (see LIBRARY_CACHE.md) and MusicBrainz resolution (see LIBRARY_SYSTEM.md).

______________________________________________________________________

## Problem

Each library has its own SQLite database with independent artist/album/track tables. When two libraries both contain "Daft Punk", the cache holds two separate `library_artist_info_t` structs with different global IDs. Without dedup, every list view and search returns both — users see duplicate rows.

The merge infrastructure (`rebuild_merged_artists()`) detects MBID-matched artists and marks a representative. Dedup correctness depends entirely on MusicBrainz resolution coverage — artists without MBIDs are never merged. The resolver uses a three-stage fallback chain (ISRC → fingerprint → text search) to maximize coverage.

______________________________________________________________________

## Architecture

```
                    rebuild_merged_artists()
                    ────────────────────────
                    Called at end of warming under cache->lock.
                    Single pass — MBID only:

                    ┌────────────────────────────────────────────┐
                    │  MBID merge                                 │
                    │                                            │
                    │  For each artist with musicbrainz_id:      │
                    │    If MBID unseen → register as rep         │
                    │    If MBID seen  → merge into existing rep  │
                    │                                            │
                    │  Artists without MBID are never merged.     │
                    │  Correctness depends on high MusicBrainz   │
                    │  resolution coverage (see resolver docs).  │
                    └────────────────────────────────────────────┘
                                      │
                                      ▼
                    ┌────────────────────────────────────────────┐
                    │  merged_source_set (GHashTable<int64_t>)   │
                    │  Set of global artist IDs that are merge   │
                    │  sources (non-representative duplicates).  │
                    │  Built during merge. Used by query funcs   │
                    │  to skip sources — O(1) lookup per artist. │
                    ├────────────────────────────────────────────┤
                    │  merged_album_sources (GHashTable<int64_t>)│
                    │  Set of global album IDs that are merge    │
                    │  sources. Built alongside artist merge.    │
                    │  Used to skip duplicate albums by MBID.    │
                    └────────────────────────────────────────────┘
                                      │
                    ┌─────────────────┼─────────────────┐
                    ▼                 ▼                 ▼
          get_artists_filtered  get_albums_filtered  run_search_queries
          ──────────────────    ──────────────────   ──────────────────
          Skip artists in       Dedup albums by      Both: skip source
          merged_source_set     musicbrainz_release  artists + dedup
                                _id via seen_mbids   albums by MBID
```

______________________________________________________________________

## Merge State

### Artist Merge

Merge state lives on `library_artist_info_t`:

| Field                 | Single-library artist | Merged representative      | Merge source       |
| --------------------- | --------------------- | -------------------------- | ------------------ |
| `library_index`       | `0..N` (source slot)  | `-1`                       | `0..N` (unchanged) |
| `merged_source_count` | `0`                   | `>0`                       | `0`                |
| `merged_source_ids`   | `NULL`                | Array of source global IDs | `NULL`             |
| `album_count`         | Own count             | Accumulated across sources | Own count          |
| `track_count`         | Own count             | Accumulated across sources | Own count          |

A source artist's fields are not modified (except counts accumulated into the rep). The `merged_source_set` on the cache determines whether an artist is skipped in query results.

### Album Merge

Merge state also lives on `library_album_info_t`:

| Field                 | Single-library album | Merged representative      | Merge source       |
| --------------------- | -------------------- | -------------------------- | ------------------ |
| `library_index`       | `0..N` (source slot) | `-1`                       | `0..N` (unchanged) |
| `merged_source_count` | `0`                  | `>0`                       | `0`                |
| `merged_source_ids`   | `NULL`               | Array of source global IDs | `NULL`             |

Albums are merged by `musicbrainz_release_id` (same release in multiple libraries). The `merged_album_sources` hash table on the cache determines whether an album is skipped in query results.

______________________________________________________________________

## ⚠ Merge Invariants — CRITICAL

1. **Every source artist ID in `merged_source_set` MUST appear in exactly one representative's `merged_source_ids[]`.** If an ID is in the set but no rep claims it, that artist vanishes from all views.

1. **A representative MUST NOT be in `merged_source_set`.** Self-referential skip would hide the merged artist entirely.

1. **`merged_source_set` MUST be rebuilt from scratch on every `rebuild_merged_artists()` call.** Stale entries from removed libraries would cause invisible artists.

1. **Artists without MBIDs are never merged.** Dedup correctness depends entirely on MusicBrainz resolution coverage. Unresolved artists appear once per library — this is intentional (no false merges).

______________________________________________________________________

## Dedup in Query Functions

### Artist Queries

`library_cache_get_artists_filtered()` and `run_search_queries()` (artist section):

```
For each slot:
  db_get_artist_ids_filtered(slot->db, ...) → local IDs
  For each local ID:
    info = get_artist_unlocked(slot, local_id)
    if info->artist_id ∈ merged_source_set → skip
    add to result
```

Cost: one hash lookup per artist. The set is pre-built during warming — no per-query allocation.

### Album Queries

`library_cache_get_albums_filtered()` and `run_search_queries()` (album section):

```
seen_mbids = new GHashTable (per-query, transient)
For each slot:
  db_get_album_ids_filtered(slot->db, ...) → local IDs
  For each local ID:
    info = get_album_unlocked(slot, local_id)
    if info->musicbrainz_release_id != NULL:
      if musicbrainz_release_id ∈ seen_mbids → skip
      add musicbrainz_release_id to seen_mbids
    add to result
destroy seen_mbids
```

Albums without a `musicbrainz_release_id` (unresolved) always pass through — they cannot be identified as duplicates.

### Track Queries

No dedup. Tracks are per-library by nature — the same song from different sources (different rips, bitrates, formats) are genuinely separate entities that should remain visible.

______________________________________________________________________

## Library Filtering

### Concept

Every query function accepts a `library_filter` parameter:

- `-1` = all libraries (default). Dedup is applied.
- `0..N` = specific library index. Only that slot is queried. Dedup still runs but is effectively a no-op (a single library can't have MBID duplicates with itself).

### API

```c
GPtrArray* library_cache_get_artists_filtered(
    library_cache_t* cache,
    library_sort_t sort,
    const char* search_text,
    const db_search_opts_t* filters,
    int library_filter                      // NEW: -1 = all, 0..N = specific
);

GPtrArray* library_cache_get_albums_filtered(
    library_cache_t* cache,
    library_sort_t sort,
    const char* search_text,
    const db_search_opts_t* filters,
    int library_filter                      // NEW
);

library_search_results_t* library_cache_search(
    library_cache_t* cache,
    const char* query,
    library_search_filter_t filter,
    size_t limit,
    const db_search_opts_t* opts,
    int library_filter                      // NEW
);
```

Implementation: at the top of the slot loop, `if (library_filter >= 0 && slot->lib_idx != library_filter) continue;`

### UI Integration

`FilterBarState` gains:

```c
int selected_library;           // -1 = all, 0..N = specific
GtkWidget *filter_library;      // GtkMenuButton (dropdown)
```

- Shown only when `library_cache_get_library_count() > 1`
- Popover: radio buttons — "All Libraries" (default), then one per library name
- Filter state flows through `filter_bar_get_library()` → callers pass to cache query APIs
- Cleared by `filter_bar_clear()` (resets to -1)

### Library Indicator

When viewing "All Libraries", each artist/album row shows a small muted label with the library display name (from `library_cache_get_library_name()`). Merged artists (`library_index == -1`) show no badge. Hidden when a specific library is selected (redundant).

______________________________________________________________________

## Data Flow

### Warming → Merge → Query

```
Warming thread (per slot):
  Phase 1-5: load artists, albums, tracks, relationships
  Phase 6: g_mutex_lock → rebuild_merged_artists(cache) → g_mutex_unlock
           Builds merged_source_set
           Marks reps with library_index=-1, accumulated counts
           Invalidates reps' cached artist_albums[] for lazy rebuild
           Sets warm_state = READY
           g_idle_add(ready_cb)

Main thread (on ready):
  refresh_library_views()
    → library_cache_get_artists_filtered(..., library_filter)
      → iterates slots, skips unavailable, skips library_filter mismatches
      → skips artists in merged_source_set
      → returns deduplicated list

    → library_cache_get_albums_filtered(..., library_filter)
      → iterates slots, skips unavailable, skips library_filter mismatches
      → skips albums with duplicate musicbrainz_release_id
      → returns deduplicated list
```

### Artist Detail (already correct)

`library_cache_get_albums_by_artist()` already handles merged artists:

```
Given merged artist (library_index == -1):
  1. Fetch albums from representative's own slot
  2. For each merged_source_id:
     Decode global ID → (lib_idx, local_id)
     Fetch albums from source slot
     Dedup by musicbrainz_release_id via seen_mbids
  3. Return unified, deduplicated album list
```

Each album in the result carries its own `library_index` — the detail view uses this to show library origin.

______________________________________________________________________

## Edge Cases

| Scenario                                       | Behavior                                                                                                                                                                                           |
| ---------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Same artist, MBID in lib A, no MBID in lib B   | A merges normally; B appears as separate unresolved entry. Once B resolves and gets same MBID, next rebuild merges them.                                                                           |
| Same name, different MBIDs (different artists) | Two separate reps. No false merge.                                                                                                                                                                 |
| Same name, neither has MBID                    | Two separate entries (one per library). No merge until MBIDs are resolved.                                                                                                                         |
| Library becomes unavailable                    | Filtered queries skip unavailable slots. Merged_source_set is NOT rebuilt (would require re-warming). Source artists from unavailable slot simply won't appear in DB queries → effectively hidden. |
| Library removed entirely                       | `library_cache_remove_slot()` → `rebuild_merged_artists()` → rebuilds everything from scratch.                                                                                                     |
| Single library                                 | `merged_source_set` is empty. All skip checks are no-ops. `library_filter` dropdown hidden. Zero overhead.                                                                                         |
