# Library Views

All library data flows through [LibraryCache](../architecture/LIBRARY_CACHE.md). Row creation uses shared helpers from [COMPONENTS.md](COMPONENTS.md).

## Navigation Structure

```
┌───────────┐
│  Search   │───┐
├───────────┤   │              ┌─────────────┐
│  Artists  │───┼─────────────>│ Details View│
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

Click artist/album from Search, Artists, or Albums to open the Details View. Back button returns to the origin view with scroll position preserved. Libraries is a standalone management view with no detail navigation.

## Search View

Full-text search with type filtering.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [search icon] Search tracks, artists, albums...                         │
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

**Songs section:** Uses track rows with inline album button and artist buttons (see [COMPONENTS.md](COMPONENTS.md)).

## Artists View

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Artists · 89 artists                                     [Filter bar]   │
├─────────────────────────────────────────────────────────────────────────┤
│ The Beatles                   [art][art][art][art]                 [>]  │
│   4 albums, 89 tracks                                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

Art strip shows up to 6 album thumbnails.

**Sort options:** Name (A-Z), Genre (alphabetical), Recent (last added).

## Albums View

```
┌────────────────────────────────────────────────────────────┐
│ Albums · 247 albums                        [Filter bar]     │
├────────────────────────────────────────────────────────────┤
│ [48px] Abbey Road                                   17 trk │
│        The Beatles · 1969                                   │
└────────────────────────────────────────────────────────────┘
```

**Sort options:** Name (A-Z), Date (newest first), Genre (alphabetical).

## Filter Bar

Unified filter bar shared by Artists and Albums views.

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ [Genre ▼][Year ▼][Search [________]][Advanced][Clear]  ...padding...  [Sort ▼]  │
└──────────────────────────────────────────────────────────────────────────────────┘
```

- **Genre:** Dropdown populated from library genres
- **Year:** Decade buckets (2020s, 2010s, ..., Pre-1960)
- **Search:** 200ms debounce, case-insensitive substring
- **Advanced:** Toggles additional filter options (hidden by default)
- **Clear:** Resets all filters to default
- **Sort:** View-specific sort dropdown (right-aligned, separated by padding)
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
│ │  250px   │  The Beatles                   (artist button > popover)   │
│ │   art    │  1969 · 17 songs · 47:23                                   │
│ └──────────┘                                                            │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Come Together        The Beatles                        [i]  4:19  │
│  2. Something            The Beatles                        [i]  3:02  │
│  3. Maxwell's Silver...  The Beatles                        [i]  3:27  │
└─────────────────────────────────────────────────────────────────────────┘
```

Track list uses compact track items with inline artist buttons. Multi-disc albums show "DISC N" headers.

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
│ │ 250x250 │  1969 · 17 tracks · 47:23                                   │
│ └─────────┘                                                            │
│   1. Come Together       The Beatles                             4:19  │
│   2. Something           The Beatles                             3:02  │
│      ...see all 17 tracks                                              │
├─────────────────────────────────────────────────────────────────────────┤
│ APPEARS ON                                        [ Albums | Tracks ]   │
│ ┌─────────┐  CONCERT FOR BANGLADESH                               [>] │
│ │ 250x250 │  Various Artists · 1971                                    │
│ └─────────┘                                                            │
└─────────────────────────────────────────────────────────────────────────┘
```

**Appears On section:** Toggle between Albums view (default) and Tracks view. Tracks view uses track rows with inline album and artist buttons.

### Back Button Behavior

| Current State | Came From     | Back Action                  |
| ------------- | ------------- | ---------------------------- |
| Album detail  | Albums view   | Albums view, restore scroll  |
| Album detail  | Artist detail | Artist detail                |
| Album detail  | Search        | Search view, restore results |
| Artist detail | Artists view  | Artists view, restore scroll |
| Artist detail | Search        | Search view, restore results |

## Libraries Tab

Manages library sources. Each library is a LibraryCard, a self-contained card that owns both its stats and its indexing progress. Multiple cards can show progress simultaneously.

```
┌──────────────────────────────────────────────────────────┐
│ Libraries                              [+ Add Library]   │
├──────────────────────────────────────────────────────────┤
│                                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ Eli's EC (click to rename)      * Online  [Rescan]  │ │
│ │ /mnt/elicb/drive                                     │ │
│ ├──────────────────────────────────────────────────────┤ │
│ │  45,231 tracks · 312 albums · 89 artists · 2d 14h   │ │
│ │  Last scanned Feb 18, 2026                           │ │
│ └──────────────────────────────────────────────────────┘ │
│                                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ DJ Bob's Collection             ~ Indexing [Cancel]  │ │
│ │ /media/usb/djbob                                     │ │
│ ├──────────────────────────────────────────────────────┤ │
│ │  Scanning directories...                      pulse  │ │
│ │  progress bar                                        │ │
│ │                                                      │ │
│ │  Extracting metadata           1,204 / 3,891         │ │
│ │  progress bar                              384 trk/s │ │
│ │                                                      │ │
│ │  Processing artwork                         Waiting  │ │
│ │  progress bar                                        │ │
│ │                                                      │ │
│ │  MusicBrainz resolution                     Waiting  │ │
│ │  progress bar                                        │ │
│ └──────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

The card body crossfades between a stats panel and a progress panel. Both panels are the same height so no layout shift occurs during the transition.

### Library Name Editing

The library name is an inline editable label. It looks like a bold label at rest and becomes a text field on click. Enter or focus-out commits; Escape cancels. The new name is saved to settings.ini.

### Stats Panel

Shown when idle. Two rows:

- **Counts row:** `45,231 tracks · 312 albums · 89 artists · 2d 14h`
- **Meta row:** `Last scanned Feb 18, 2026` and `! 3 errors` (errors button hidden when count = 0, clicking opens the errors view)

Stats rationale: tracks = library size, albums/artists = shape and breadth, duration = total playable time (directly useful for set planning), last scanned = data freshness signal, error count = operational health alert.

### Progress Panel

Shown while indexing. Four vertically stacked phase rows, always present. Visual state driven by phase status, no widgets added or removed.

Each phase row has: phase title (left) + status text (right), a progress bar, and a rate/ETA label below. The Scan phase pulses (indeterminate); Metadata and Artwork show fractional fill; MusicBrainz pulses.

| Phase state | Visual |
|---|---|
| Not yet reached | Dimmed |
| Active | Full opacity, cyan title |
| Complete | Green bar fill |
| Error | Red tint |

### Card States and Actions

| State | Body | Actions |
|-------|------|---------|
| Online / Ready | stats | [Rescan] [Remove] |
| Indexing | progress | [Cancel] |
| Error | stats (with error btn) | [Retry] [Remove] |
| Offline | stats (dim) | [Remove] |

### Transition Sequence

1. **[Rescan]**: all 4 phase rows reset to dimmed, card crossfades to progress panel
2. **Progress updates**: phase rows update in place (bars, labels, state)
3. **Complete**: 2s hold, card crossfades back to stats panel with updated counts

## Gestures and Shortcuts

See [KEYBINDS.md](KEYBINDS.md) for full reference. Common patterns:

| Gesture/Key  | Action                     |
| ------------ | -------------------------- |
| Single-click | Select/highlight row       |
| Double-click | Open detail view           |
| Right-click  | Queue to focused channel   |
| Up/Down      | Navigate list              |
| Enter        | Open detail or load track  |
| Escape       | Go back / clear            |
| 1-4          | Load selected to channel N |
| /            | Focus filter search        |

**Right-click behavior:**

- Track: Queue that track
- Album: Queue first track of album

No focused channel? Toast: "Focus a channel first (click channel number or press 1-4)"
