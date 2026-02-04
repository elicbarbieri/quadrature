# Library Views

All library data flows through [LibraryCache](../architecture/LIBRARY_CACHE.md). Row creation uses shared helpers from [COMPONENTS.md](COMPONENTS.md).

**Templates:** `nav_bar.ui` (navigation sidebar), `search_view.ui` (search page), `libraries_view.ui` (library management with indexing progress).

## Navigation Structure

```
┌───────────┐
│  Search   │───┐
├───────────┤   │              ┌─────────────┐
│  Artists  │───┼─────────────►│ Details View│
├───────────┤   │              │ (Album or   │
│  Albums   │───┘              │  Artist)    │
├───────────┤                  └─────────────┘
│ Libraries │
├───────────┤
│    ...    │
├───────────┤
│ Settings  │
├───────────┤
│ Help      │
└───────────┘
```

**Navigation flow:** Click artist/album from Search, Artists, or Albums → Details View shows that entity. Back button returns to origin view with scroll position preserved. Libraries is a standalone management view with no detail navigation.

## Search View

Full-text search with type filtering.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [🔍] Search tracks, artists, albums...                                   │
├─────────────────────────────────────────────────────────────────────────┤
│ [*ALL*] [ Artists ] [ Albums ] [ Songs ]                                │
├─────────────────────────────────────────────────────────────────────────┤
│ ARTISTS (3)                                                             │
│   The Beatles        4 albums   [art][art][art][art]               [>]  │
├─────────────────────────────────────────────────────────────────────────┤
│ ALBUMS (5)                                                              │
│   [art] Abbey Road              The Beatles · 1969                 [>]  │
├─────────────────────────────────────────────────────────────────────────┤
│ SONGS (42)                                                              │
│   [art] Here Comes the Sun  Abbey Road  The Beatles          1969 3:06  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Input:** 200ms debounce, auto-focus on view activation, `Escape` clears.

**Filters:** All (grouped, limited), Artists, Albums, Songs. `Ctrl+F/A/B/S` to switch.

**All mode limits:** 5 artists, 5 albums, 10 songs. Filtered modes are unlimited.

**Songs section:** Uses `library_track_row` with inline album button and artist buttons (see [COMPONENTS.md](COMPONENTS.md)).

## Artists View

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Artists · 89 artists                                     [Filter ▼]     │
├─────────────────────────────────────────────────────────────────────────┤
│ The Beatles                   [art][art][art][art]                 [>]  │
│   4 albums, 89 tracks                                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

Uses `library_artist_row` template. Art strip shows up to 6 album thumbnails.

## Albums View

```
┌────────────────────────────────────────────────────────────┐
│ Albums · 247 albums                  [Sort: Title ▲] [⚙]   │
├────────────────────────────────────────────────────────────┤
│ [48px] Abbey Road                                   17 trk │
│        The Beatles · 1969                                   │
└────────────────────────────────────────────────────────────┘
```

Uses `library_album_row` template.

**Sort options:** Title (A-Z), Year (newest), Artist (A-Z then album), Added (recent).

## Filter Panel

Shared filter UI for Artists and Albums views.

```
┌─────────────────────────────────────────────────────────────────┐
│ Genre: [All ▼]  Year: [All ▼]  Search: [__________]    [Clear] │
└─────────────────────────────────────────────────────────────────┘
```

- **Genre:** Dropdown from library genres
- **Year:** Decade buckets (2020s, 2010s, ..., Pre-1960)
- **Search:** 200ms debounce, case-insensitive substring
- All filters AND together
- `/` focuses search box, `Escape` clears

**Active indicator:** Filter button shows cyan dot, count shows "(filtered)".

## Details View

Context-aware view showing Album or Artist detail based on navigation.

### Album Detail

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [<] Back to Albums                                                      │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌──────────┐  ABBEY ROAD                                                │
│ │  250px   │  The Beatles                   (artist button → popover)   │
│ │   art    │  1969 · 17 songs · 47:23                                   │
│ └──────────┘                                                            │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Come Together        The Beatles                        [ℹ]  4:19  │
│  2. Something            The Beatles                        [ℹ]  3:02  │
│  3. Maxwell's Silver…   The Beatles                        [ℹ]  3:27  │
└─────────────────────────────────────────────────────────────────────────┘
```

Track list uses `album_detail_track_item` with inline artist buttons. Multi-disc albums show "DISC N" headers.

### Artist Detail

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [<] Back to Artists                                                     │
├─────────────────────────────────────────────────────────────────────────┤
│ THE BEATLES                                                             │
│ 4 albums · 89 tracks · 5h 23m                                           │
├─────────────────────────────────────────────────────────────────────────┤
│ ALBUMS                                                                  │
│ ┌─────────┐  ABBEY ROAD                                            [>] │
│ │ 250×250 │  1969 · 17 tracks · 47:23                                   │
│ └─────────┘                                                            │
│   1. Come Together       The Beatles                             4:19  │
│   2. Something           The Beatles                             3:02  │
│      ...see all 17 tracks                                              │
├─────────────────────────────────────────────────────────────────────────┤
│ APPEARS ON                                        [ Albums | Tracks ]   │
│ ┌─────────┐  CONCERT FOR BANGLADESH                               [>] │
│ │ 250×250 │  Various Artists · 1971                                    │
│ └─────────┘                                                            │
└─────────────────────────────────────────────────────────────────────────┘
```

**Appears On section:** Toggle between Albums view (default) and Tracks view. Tracks view uses `library_track_row` with inline album and artist buttons.

### Back Button Behavior

| Current State | Came From     | Back Action                  |
| ------------- | ------------- | ---------------------------- |
| Album detail  | Albums view   | Albums view, restore scroll  |
| Album detail  | Artist detail | Artist detail                |
| Album detail  | Search        | Search view, restore results |
| Artist detail | Artists view  | Artists view, restore scroll |
| Artist detail | Search        | Search view, restore results |

## Libraries Tab

Manages library sources. Shows NAS status and portable drives.

```
┌──────────────────────────────────────────────────────────┐
│ Libraries                               [◐ 1 indexing]   │
├──────────────────────────────────────────────────────────┤
│ PRIMARY LIBRARY                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ Studio Main                              ● Online    │ │
│ │ /mnt/nas/music · 45,231 tracks           [Refresh]  │ │
│ └──────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────┤
│ CONNECTED DRIVES                                         │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ DJ Bob's Collection                    ◐ Indexing    │ │
│ │ ████████████████░░░░░░░░  62%                        │ │
│ └──────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

**States:** Online (green), Checking/Indexing (blue), Ready (green), Error (red), Offline (gray), New (gray).

**Actions by state:** Online → [Refresh], Indexing → [Cancel], Ready → [Rescan][Eject], Error → [Retry][Eject], Offline → [Forget], New → [Index][Ignore].

## Gestures & Shortcuts

See [KEYBINDS.md](KEYBINDS.md) for full reference. Common patterns:

| Gesture/Key  | Action                     |
| ------------ | -------------------------- |
| Single-click | Select/highlight row       |
| Double-click | Open detail view           |
| Right-click  | Queue to focused channel   |
| `↑`/`↓`      | Navigate list              |
| `Enter`      | Open detail or load track  |
| `Escape`     | Go back / clear            |
| `1-4`        | Load selected to channel N |
| `/`          | Focus filter search        |

**Right-click behavior:**

- Track: Queue that track
- Album: Queue first track of album

No focused channel? Toast: "Focus a channel first (click channel number or press 1-4)"
