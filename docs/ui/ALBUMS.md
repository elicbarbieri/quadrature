# Albums View

Album browsing with list view, detail drill-down, and filtering capabilities for exploring your music library.

## List View

```
┌────────────────────────────────────────────────────────────┐
│ Albums · 247 albums                  [Sort: Title ▲] [⚙]   │
├────────────────────────────────────────────────────────────┤
│ [48px] Abbey Road                                   17 trk │
│        The Beatles · 1969                                  │
├────────────────────────────────────────────────────────────┤
│ [48px] Let It Be                                    12 trk │
│        The Beatles · 1970                                  │
├────────────────────────────────────────────────────────────┤
│ [48px] Goldberg Variations                          32 trk │
│        Johann Sebastian Bach · 1981                        │
└────────────────────────────────────────────────────────────┘
```

### Album Row

```
┌────────────────────────────────────────────────────────────┐
│ ┌──────┐  Album Title                            N trk     │
│ │ 48px │  Artist Name · Year                               │
│ │ art  │                                                   │
│ └──────┘                                                   │
└────────────────────────────────────────────────────────────┘
```

- **Album art**: 48×48px thumbnail on left
- **Title**: 14px medium weight, primary text color
- **Meta line**: Artist · Year, 12px, muted color
- **Track count**: Right-aligned, 12px, dim color

### Sort Options

```
┌─────────────────────────────────────────────────────────────┐
│ Sort: [Title ▲] [Year] [Artist] [Added]                     │
└─────────────────────────────────────────────────────────────┘
```

| Sort   | Default  | Description                        |
| ------ | -------- | ---------------------------------- |
| Title  | A-Z ▲    | Alphabetical by album title        |
| Year   | Newest ▼ | Release year descending            |
| Artist | A-Z ▲    | Alphabetical by artist, then album |
| Added  | Recent ▼ | Date added to library              |

Active sort shows indicator (▲ asc / ▼ desc). Click to toggle direction.

## Filtering

### Filter Panel

```
┌─────────────────────────────────────────────────────────────────┐
│ Albums · 247 albums                  [Sort: Title ▲] [Filter ▼] │
├─────────────────────────────────────────────────────────────────┤
│ ┌─────────────────────────────────────────────────────────────┐ │
│ │ Genre: [All ▼]  Year: [All ▼]  Artist: [All ▼]            │ │
│ │ Search: [_________________________]             [Clear]    │ │
│ └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

Filtering options expand below the header when the Filter button (⚙) is clicked.

### Filter Controls

| Control    | Type       | Options                                      | Behavior                                           |
| ---------- | ---------- | -------------------------------------------- | -------------------------------------------------- |
| **Genre**  | Dropdown   | All, Rock, Jazz, Classical, Electronic, etc. | Filter albums by genre tag                         |
| **Year**   | Dropdown   | All, 2020s, 2010s, 2000s, 1990s, etc.        | Filter by decade/era                               |
| **Artist** | Dropdown   | All, artist names from library               | Filter to specific artist                          |
| **Search** | Text input | Free text                                    | Real-time filter by album title (case-insensitive) |
| **Clear**  | Button     | -                                            | Reset all filters to default                       |

### Filter Behavior

**Genre Filter:**

- Shows list of all genres present in library
- Filters to albums with matching genre tag
- "All" shows all albums

**Year Filter:**

- Decade ranges: "2020s", "2010s", "2000s", etc.
- Also: "Pre-1960"
- Filters to albums released in that period
- "All" shows all albums

**Artist Filter:**

- Alphabetical dropdown of all artists
- Shows albums by selected artist only
- "All" shows all artists
- Alternative to navigating through Artists view

**Search Box:**

- Real-time filtering (200ms debounce)
- Matches album title substring
- Case-insensitive
- Shows match count: "Albums · 12 matching"
- Press `/` to focus search box (keyboard shortcut)
- Press `Escape` to clear and unfocus

**Combined Filters:**

- All filters are AND-ed together
- Example: Genre=Jazz + Year=1960s + Artist="Miles Davis" → Shows jazz albums from 1960s by Miles Davis

**Empty Results:**

- Shows "No albums match these filters"
- Offers "Clear Filters" button

### Filter State Indicator

When any filter is active:

```
┌─────────────────────────────────────────────────────────────────┐
│ Albums · 12 matching (filtered)              [Sort] [Filter ▼]● │
└─────────────────────────────────────────────────────────────────┘
```

- Count shows filtered results
- "(filtered)" indicator
- Filter button shows cyan dot when active

## Detail View

Clicking an album opens the **Album Detail** view in the Details pane.

See **[DETAILS_VIEW.md](DETAILS_VIEW.md)** for complete Album Detail documentation, including:

- Album header with art and metadata
- Featured artists display
- Track list (single-disc and multi-disc support)
- Shuffle functionality
- Navigation behavior

## States

| State   | Display                            |
| ------- | ---------------------------------- |
| Loading | Skeleton rows with pulse animation |
| Empty   | "No albums in library" message     |
| List    | Album rows with art thumbnails     |
| Detail  | Header + track list                |
| Playing | Current track highlighted (cyan)   |

## CSS Classes

| Class                 | Element               |
| --------------------- | --------------------- |
| `.library-header`     | View header container |
| `.library-title`      | "Albums" heading      |
| `.library-subtitle`   | "· 247 albums" count  |
| `.sort-button`        | Sort option button    |
| `.sort-button-active` | Active sort (cyan)    |
| `.library-list`       | Album list container  |
| `.album-row`          | Album list item       |
| `.album-art-thumb`    | 48×48 list thumbnail  |
| `.album-title`        | Album name text       |
| `.album-meta`         | "Artist · Year" text  |
| `.album-track-count`  | "N trk" text          |

## Mouse Gestures & Keyboard Shortcuts

**See [KEYBINDS.md](KEYBINDS.md) for complete keyboard shortcut reference.**

Quick reference for Albums view:

| Gesture/Key       | Action                                   |
| ----------------- | ---------------------------------------- |
| Single-click      | Select/highlight row                     |
| Double-click      | Open album detail view                   |
| Right-click album | Queue track 1 to focused channel         |
| `↑`/`↓`           | Move selection (GTK default)             |
| `Enter`           | Open selected album detail               |
| `1-4`             | Load selected album (track 1) to channel |
| `/`               | Focus filter search box                  |

## Integration

Uses `LibraryViewCallbacks` for:

- `on_album_selected(album_id)` → Push album detail
- `on_artist_selected(artist_id)` → Push artist detail (from link)
- `on_track_activated(track_id, channel)` → Load track
- `on_shuffle_album(album_id)` → Queue shuffled tracks

Album queries use `db_get_albums()` with sort parameters.
Track queries use `db_get_album_tracks(album_id)` with disc grouping.
