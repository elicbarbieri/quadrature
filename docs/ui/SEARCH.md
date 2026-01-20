# Search View UI

Full-text search with type filtering and grouped results. All queries go through [LibraryCache](../architecture/LIBRARY_CACHE.md) for caching and prefetch coordination.

## Overview

Search results reuse the same row templates as library views (`library_artist_row.ui`, `library_album_row.ui`, `song_list_view.ui`). Row creation is handled by shared helper functions in `row_helpers.c`.

## Layout

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [🔍] Search tracks, artists, albums...                                   │
├─────────────────────────────────────────────────────────────────────────┤
│ [*ALL*] [ Artists ] [ Albums ] [ Songs ]                                │
├─────────────────────────────────────────────────────────────────────────┤
│ ARTISTS (3)                                                             │
│   The Beatles        4 albums, 48 tracks                           [>]  │
│   Beatles Cover Band 2 albums, 12 tracks                           [>]  │
├─────────────────────────────────────────────────────────────────────────┤
│ ALBUMS (5)                                                              │
│   [art] Abbey Road              The Beatles · 1969                 [>]  │
│   [art] Let It Be               The Beatles · 1970                 [>]  │
├─────────────────────────────────────────────────────────────────────────┤
│ SONGS (42)                                                              │
│   [art] Here Comes the Sun  Abbey Road  The Beatles         1969  3:06  │
│   [art] Something           Abbey Road  The Beatles         1969  3:02  │
└─────────────────────────────────────────────────────────────────────────┘
```

## Search Input

Search entry with placeholder text. Debounced 200ms before triggering `library_cache_search()`.

- Auto-focus on view activation
- Clear button appears when text present
- `Escape` clears search and returns focus

**Search Flow:**

```c
static void on_search_changed(GtkSearchEntry* entry, SearchView* view) {
    const char* query = gtk_editable_get_text(GTK_EDITABLE(entry));

    // Query through LibraryCache (cached, handles FTS)
    const library_search_results_t* results = library_cache_search(
        view->library,
        query,
        view->filter,
        view->filter == SEARCH_FILTER_ALL ? 10 : 0  // Limit in ALL mode
    );

    populate_results(view, results);

    // Trigger prefetch for visible results
    prefetch_visible_results(view, results);
}
```

## Filter Buttons

```
┌───────────────────────────────────────────────────────────┐
│ [*ALL*] [ Artists ] [ Albums ] [ Songs ]                  │
└───────────────────────────────────────────────────────────┘
```

| Filter  | Shortcut | Behavior                        |
| ------- | -------- | ------------------------------- |
| All     | `Ctrl+F` | Shows grouped results (default) |
| Artists | `Ctrl+A` | Artists only, unlimited         |
| Albums  | `Ctrl+B` | Albums only, unlimited          |
| Songs   | `Ctrl+S` | Songs only, unlimited           |

Active filter has cyan background (`.search-filter-active`).

## Result Sections

### All Mode (Default)

Results grouped by type with counts. Limited to prevent overwhelming:

- Artists: 5 results max
- Albums: 5 results max
- Songs: 10 results max

Each section shows "(N)" count and is collapsible.

### Artist Results

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ARTISTS (3)                                                             │
├─────────────────────────────────────────────────────────────────────────┤
│   The Beatles        4 albums   [art][art][art][art]               [>]  │
│     ↑ name           ↑ count    ↑ album art strip (48×48px each)        │
├─────────────────────────────────────────────────────────────────────────┤
│   Beatles Cover Band 2 albums   [art][art]                         [>]  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Album art strip**: Up to 5-6 thumbnails (48×48px each) on right side
- Click row → Artist detail view
- Chevron indicates drill-down available

### Album Results

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ALBUMS (5)                                                              │
├─────────────────────────────────────────────────────────────────────────┤
│   [48px] Abbey Road              The Beatles · 1969                [>]  │
│     ↑ art   ↑ title              ↑ artist · year                        │
├─────────────────────────────────────────────────────────────────────────┤
│   [48px] Let It Be               The Beatles · 1970                [>]  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Album art thumbnail**: 48×48px on left
- Click row → Album detail view

### Song Results

Uses the reusable song list component (`song_list_view.ui`).

```
┌─────────────────────────────────────────────────────────────────────────┐
│ SONGS (42)                                                              │
├─────────────────────────────────────────────────────────────────────────┤
│        TITLE              ALBUM           ARTIST              YEAR  DUR │
├─────────────────────────────────────────────────────────────────────────┤
│ [art] Here Comes the Sun  Abbey Road      The Beatles         1969 3:06 │
│ [art] Something           Abbey Road      The Beatles         1969 3:02 │
│ [art] My Sweet Lord       All Things...   George Harrison     1970 4:38 │
│ [art] Got My Mind Set...  Cloud Nine      George Harrison     1987 3:52 │
│                                           feat. Jeff Lynne              │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Columns**: `[art 48px][title flex][album flex][artist flex][year 4ch][duration 5ch]`
- **Artist column**: Shows album artist, with featured artists on second line if present
- **Album art thumbnail**: 48×48px on left
- **Duration**: Monospace, right-aligned
- Double-click or `Enter` → Navigate to parent album with track highlighted

## States

| State      | Display                          |
| ---------- | -------------------------------- |
| Empty      | Placeholder text in entry        |
| Typing     | Debounce indicator (subtle)      |
| Loading    | Spinner in results area          |
| Results    | Grouped sections                 |
| No Results | "No results for 'query'" message |
| Error      | Error message with retry option  |

## Mouse Gestures & Keyboard Shortcuts

**See [KEYBINDS.md](KEYBINDS.md) for complete keyboard shortcut reference.**

Quick reference for Search view:

| Gesture/Key         | Action                                    |
| ------------------- | ----------------------------------------- |
| Single-click        | Select/highlight row                      |
| Double-click artist | Open artist detail view                   |
| Double-click album  | Open album detail view                    |
| Double-click song   | Open parent album with track highlighted  |
| Right-click album   | Queue track 1 of album to focused channel |
| Right-click song    | Queue song to focused channel             |
| `↑`/`↓`             | Move selection (GTK default)              |
| `Ctrl+F/A/B/S`      | Switch search filter                      |
| `Escape`            | Clear search                              |
| `1-4`               | Load selected song to channel 1-4         |

**Selection vs Loading:** Single-click selects rows for keyboard navigation. Double-click opens detail views. Right-click queues to the focused channel:

- Right-click on a **song** queues that specific track
- Right-click on an **album** queues track 1 of that album

If no channel is focused, a toast prompts the user to focus a channel first.

## CSS Classes

Search results use the same CSS classes as library views since they share templates.

### Search-Specific Classes

| Class                   | Element                  |
| ----------------------- | ------------------------ |
| `.search-entry`         | Search input field       |
| `.search-filters`       | Filter button container  |
| `.search-filter`        | Filter button            |
| `.search-filter-active` | Active filter (cyan)     |
| `.search-results`       | Results scroll container |
| `.search-section`       | Result group container   |

### Shared Library Row Classes

These classes are defined in the shared templates and apply to both library views and search results:

| Class                     | Element                      |
| ------------------------- | ---------------------------- |
| `.library-section-header` | Section header "ARTISTS (N)" |
| `.library-row`            | Base row styling             |
| `.library-album-row`      | Album row (from template)    |
| `.library-album-art`      | 48×48 album art thumbnail    |
| `.library-row-title`      | Primary text (title)         |
| `.library-row-subtitle`   | Secondary text (artist/meta) |
| `.library-row-duration`   | Duration (monospace)         |
| `.library-row-count`      | Track count                  |
| `.song-list-row`          | Song/track row               |
| `.column-headers`         | Table column header bar      |
| `.column-header`          | Individual column header     |

## Integration

### Row Creation

Search results are created using shared helper functions from `row_helpers.c`:

```c
// Create rows from LibraryCache results
GtkWidget *ui_create_artist_row(const library_artist_info_t *artist, gboolean show_art_strip);
GtkWidget *ui_create_album_row(const library_album_info_t *album, ArtworkManager *art_mgr, gboolean show_count);
GtkWidget *ui_create_track_row(const library_track_info_t *track, ArtworkManager *art_mgr, gboolean show_track_disc);
```

Each row stores its entity ID via `g_object_set_data()`:

- Artist rows: `"artist-id"`
- Album rows: `"album-id"`
- Track rows: `"track-id"`

### Callbacks

| Action            | Handler                         |
| ----------------- | ------------------------------- |
| Left-click artist | Navigate to artist detail view  |
| Left-click album  | Navigate to album detail view   |
| Right-click album | Load track 1 to focused channel |
| Right-click track | Load track to focused channel   |

### Track Loading

Right-click triggers track loading:

1. Get `track_id` (or `album_id` → resolve to track 1 via LibraryCache)
2. `audio_cache_load(track_id)` - starts background decode
3. `audio_pipeline_set_player_track(player_id, track_id)`
4. `ui_channel_strip_set_track(track_id)` - updates display

```c
static void on_album_right_click(GtkWidget* row, SearchView* view) {
    int64_t album_id = get_album_id_from_row(row);

    // Get first track via LibraryCache
    const GPtrArray* tracks = library_cache_get_tracks_by_album(view->library, album_id);
    if (tracks->len == 0) return;

    const library_track_info_t* track = g_ptr_array_index(tracks, 0);
    queue_track_to_player(view, track->track_id);
}
```

### Prefetch Coordination

When search results appear, prefetch audio files for faster decode:

```c
static void prefetch_visible_results(SearchView* view, const library_search_results_t* results) {
    // Prefetch audio files for track results (AudioCache → LibraryCache → posix_fadvise)
    int64_t track_ids[32];
    size_t count = 0;
    for (guint i = 0; i < results->tracks->len && count < 32; i++) {
        const library_track_info_t* track = g_ptr_array_index(results->tracks, i);
        track_ids[count++] = track->track_id;
    }
    audio_cache_prefetch_visible(view->audio_cache, track_ids, count);
}
```

**Note:** Thumbnails are loaded via ArtworkManager's atlas system (not kernel page cache prefetch).

### Navigation with Prefetch

When user clicks a result, prefetch before navigating:

```c
static void on_album_clicked(GtkWidget* row, SearchView* view) {
    int64_t album_id = get_album_id_from_row(row);

    // Prefetch full-size artwork (ArtworkManager → LibraryCache → posix_fadvise)
    artwork_manager_prefetch_fullsize(view->artwork, album_id);

    // Navigate to detail view
    detail_view_show_album(view->detail, album_id);
}
```

## Album Art Sizes

All album thumbnails in search results are **48×48px** (including artist row strips, album rows, and song rows).

Missing art shows placeholder with music note icon.
