# Artists View

Artist browsing with album art strips, detail view, and filtering capabilities.

## List View

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Artists · 89 artists                                     [Filter ▼]     │
├─────────────────────────────────────────────────────────────────────────┤
│ The Beatles                   [art][art][art][art]                 [>]  │
│   4 albums, 89 tracks                                                   │
├─────────────────────────────────────────────────────────────────────────┤
│ Ludwig van Beethoven          [art][art][art][art][art][art]       [>]  │
│   12 albums, 147 tracks                                                 │
├─────────────────────────────────────────────────────────────────────────┤
│ Alexander Borodin             [art][art][art]                      [>]  │
│   3 albums, 24 tracks                                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

### Artist Row

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Artist Name                   [art][art][art][art][art][art]       [>]  │
│   N albums, N tracks              ↑ album art strip (48×48 each)        │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Name**: 14px medium weight, primary text color
- **Meta**: "N albums, N tracks", 12px, muted color
- **Album art strip**: Up to 6 thumbnails (48×48px each)
  - Horizontally scrollable if more than 6 albums
  - Shows most recent albums first
- **Chevron**: Indicates drill-down available

## Filtering

### Filter Panel

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Artists · 89 artists                                     [Filter ▼]     │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌─────────────────────────────────────────────────────────────────────┐ │
│ │ Genre: [All ▼]   Year: [All ▼]   Search: [____________]     [Clear]│ │
│ └─────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

Filtering options expand below the header when the Filter button is clicked.

### Filter Controls

| Control    | Type       | Options                                      | Behavior                                           |
| ---------- | ---------- | -------------------------------------------- | -------------------------------------------------- |
| **Genre**  | Dropdown   | All, Rock, Jazz, Classical, Electronic, etc. | Filter artists with albums in selected genre       |
| **Year**   | Dropdown   | All, 2020s, 2010s, 2000s, 1990s, etc.        | Filter by decade/era of album releases             |
| **Search** | Text input | Free text                                    | Real-time filter by artist name (case-insensitive) |
| **Clear**  | Button     | -                                            | Reset all filters to default (All/All/empty)       |

### Filter Behavior

**Genre Filter:**

- Shows list of all genres present in library
- Filters to artists who have at least one album in that genre
- "All" shows all artists

**Year Filter:**

- Decade ranges: "2020s", "2010s", "2000s", etc.
- Also: "Pre-1960"
- Filters to artists with albums released in that period
- "All" shows all artists

**Search Box:**

- Real-time filtering (200ms debounce)
- Matches artist name substring
- Case-insensitive
- Shows match count: "Artists · 12 matching"
- Press `/` to focus search box (keyboard shortcut)
- Press `Escape` to clear and unfocus

**Combined Filters:**

- All filters are AND-ed together
- Example: Genre=Jazz + Year=1960s + Search="col" → Shows jazz artists from 1960s with "col" in name (e.g., "John Coltrane")

**Empty Results:**

- Shows "No artists match these filters"
- Offers "Clear Filters" button

### Filter State Indicator

When any filter is active (not All/All/empty):

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Artists · 12 matching (filtered)                         [Filter ▼]●   │
└─────────────────────────────────────────────────────────────────────────┘
```

- Count shows filtered results
- "(filtered)" indicator
- Filter button shows cyan dot when active

## Artist Detail View

Clicking an artist opens the **Artist Detail** view in the Details pane.

See **[DETAILS_VIEW.md](DETAILS_VIEW.md)** for complete Artist Detail documentation, including:

- Artist header with stats
- "Appears On" section with Albums/Tracks toggle
- Albums view (with thumbnails and navigation)
- Tracks view (flat list of all tracks)
- Shuffle functionality
- Navigation behavior

## States

| State          | Display                                            |
| -------------- | -------------------------------------------------- |
| Loading        | Skeleton rows with pulse animation                 |
| Empty          | "No artists in library" message                    |
| List           | Artist rows with art strips                        |
| Detail         | Header + albums list                               |
| Filtered Empty | "No artists match these filters" with Clear button |

## CSS Classes

| Class                     | Element                   |
| ------------------------- | ------------------------- |
| `.library-header`         | View header container     |
| `.library-title`          | "Artists" heading         |
| `.library-subtitle`       | Item count                |
| `.filter-button`          | Filter menu button        |
| `.filter-button-active`   | When filters applied      |
| `.filter-panel`           | Filter controls container |
| `.filter-controls`        | Filter inputs row         |
| `.genre-filter`           | Genre dropdown            |
| `.year-filter`            | Year dropdown             |
| `.artist-search`          | Search text input         |
| `.clear-filters`          | Clear button              |
| `.library-list`           | List container            |
| `.artist-row`             | Artist list item          |
| `.artist-name`            | Artist name text          |
| `.artist-meta`            | "N albums, N tracks"      |
| `.album-art-strip`        | Horizontal art container  |
| `.album-art-strip-scroll` | Scrollable strip wrapper  |
| `.album-art-strip-thumb`  | 48×48 strip thumbnail     |

## Mouse Gestures & Keyboard Shortcuts

See [KEYBINDS.md](KEYBINDS.md) for complete keyboard shortcut reference.

| Gesture/Key       | Action                       |
| ----------------- | ---------------------------- |
| Single-click      | Select/highlight row         |
| Double-click      | Open artist detail view      |
| `↑`/`↓`           | Move selection (GTK default) |
| `Enter`           | Open selected artist detail  |
| `Left` / `Escape` | Back to list                 |
| `Right`           | Open detail                  |
| `Shift+Enter`     | Shuffle artist               |
| `/`               | Focus filter search box      |

## Integration

Uses `LibraryViewCallbacks` for:

- `on_artist_selected(artist_id)` → Push artist detail
- `on_album_selected(album_id)` → Push album detail (from artist detail)
- `on_shuffle_artist(artist_id)` → Queue shuffled tracks

### Database Queries

**Artist List:**

```c
db_get_artists(db, &artists, &count);
```

**Artist Detail:**

```c
db_get_artist_albums(db, artist_id, &albums, &count);
db_get_artist_stats(db, artist_id, &album_count, &track_count, &total_duration);
```

**Filter Queries:**

```c
db_get_genres(db, &genres, &count);  // For genre dropdown
db_get_artists_filtered(db, genre, year_start, year_end, search_text, &artists, &count);
```

## Album Art Sizes

All album thumbnails in this view are **48×48px** (including artist row strips).

Missing art shows placeholder with music note icon.

Artist detail album art (250×250px full-resolution) documented in [DETAILS_VIEW.md](DETAILS_VIEW.md).

## Implementation Notes

### Genre List

Genres are extracted from album metadata during indexing. The genre filter dropdown is populated dynamically based on genres present in the library.

### Year Filtering

Year filter uses decade buckets for simplicity:

- "2020s" → `year >= 2020 AND year <= 2029`
- "2010s" → `year >= 2010 AND year <= 2019`
- "Pre-1960" → `year < 1960`

### Search Performance

Artist name search uses case-insensitive substring matching:

```sql
SELECT * FROM artists WHERE name LIKE '%search%' COLLATE NOCASE;
```

For large libraries (>10,000 artists), consider FTS5 (full-text search) index.

### Filter State Persistence

Filter state persists within the session but resets on app restart. Consider storing in app settings if needed.

### Album Art Strip Scrolling

The album art strip uses horizontal scrolling when more than 6 albums:

- Smooth scrolling enabled
- No scrollbar visible (overlay style)
- Touch/trackpad gestures supported
