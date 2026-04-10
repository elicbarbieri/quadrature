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

Full-text search with type filtering and optional metadata mode.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [🔍 Search...              ] [*ALL*] [Artists] [Albums] [Songs] [Meta] │
│ [Genre ▼▼] [Year ▼▼]                                                    │
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

**Metadata mode active:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [🔍 Search...              ] [*ALL*] [Artists] [Albums] [Songs] [*Meta*]│
│ [Genre ▼▼] [Year ▼▼] [Role ▼▼]                                         │
├─────────────────────────────────────────────────────────────────────────┤
│ ...                                                                     │
└─────────────────────────────────────────────────────────────────────────┘
```

**Row 1 — Search & type toggles:**
- **Search:** 200ms debounce, auto-focus on view activation, `Escape` clears
- **Type toggles:** All, Artists, Albums, Songs. `Ctrl+F/A/B/S` to switch
- **Metadata toggle:** `Ctrl+M`. Switches search to credit/metadata matching

**Row 2 — Facet filters:**
- **Genre and Year** are always visible — multi-select, stay open until click-away (same behavior as filter bar)
- **Role** appears when Metadata is active, hidden otherwise — multi-select dropdown of credit roles (Producer, Engineer, Composer, Mixer, etc.). Stays open until click-away; multiple roles can be selected; results match any selected role

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

Unified filter bar shared by Artists and Albums views. Two rows.

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ [Genre ▼] [Year ▼]                          ...padding...  [Clear]   [Sort ▼]  │
│ [Search [____________________________]]  [Default ▼]                            │
└──────────────────────────────────────────────────────────────────────────────────┘
```

**Row 1 — Filters & Sort:**
- **Genre:** Multi-select dropdown populated from library genres. Stays open until click-away.
- **Year:** Multi-select dropdown, decade buckets (2020s, 2010s, ..., Pre-1960). Stays open until click-away.
- **Clear:** Resets all filters to default (right-aligned)
- **Sort:** Single-select dropdown, closes on selection (right-aligned, after Clear)

**Row 2 — Search:**
- **Search:** 200ms debounce, case-insensitive substring
- **Search mode** (`[Default ▼]`): Single-select dropdown, closes on selection
  - *Default:* Plaintext search across names/titles
  - *Metadata:* Searches credits, labels, and other metadata fields

**Metadata mode** (search mode set to Metadata):
- Role dropdown appears alongside Genre and Year on row 1
- All three facets (Genre, Year, Role) visible simultaneously

**Dropdown behavior:**
- Multi-select (Genre, Year, Role): popover stays open until user clicks outside — allows toggling multiple values
- Single-select (Sort, Search mode): popover closes immediately on selection

**Behavior:**
- All filters AND together
- `/` focuses search box, `Escape` clears
- Active indicator: filter button shows cyan dot, count shows "(filtered)"

## Details View

Context-aware view showing Album or Artist detail based on navigation.

### Album Detail

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [<] Back to Albums                                                      │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌──────────┐  ABBEY ROAD                                                │
│ │  250px   │  The Beatles                   (artist button > popover)   │
│ │   art    │  Album · September 26, 1969                                │
│ │          │  Label: Apple Records                                      │
│ └──────────┘  17 songs · 47:23                                          │
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
│ │ 250x250 │  Album · September 26, 1969                                 │
│ │         │  Label: Apple Records                                       │
│ └─────────┘  17 tracks · 47:23                                          │
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
