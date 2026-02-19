# UI Components

Reusable components and shared patterns. All row types use templates and shared helper functions.

## Selection

Selection is handled automatically by the list view selection model:
- **Visual:** Selected row gets highlight styling
- **Keyboard:** Up/Down arrows navigate, Enter activates
- **1-4 keys:** Queue selected track to channel N (no-op if nothing selected)

## Interaction Behavior by Row Type

| Row Type | Activate (double-click / Enter) | Secondary (right-click) |
|----------|--------------------------------|------------------------|
| Track | Navigate to album, select track | Queue track |
| Album | Navigate to album detail | Queue track 1 |
| Artist | Navigate to artist detail | -- |

## Inline Navigation Elements

Shared interaction patterns for clickable artist and album elements within rows.

### Artist Buttons

Artists are displayed as clickable buttons with overflow handling based on available column space.

**Single artist button:** Flat, frameless button. Click navigates to artist detail. Multiple artists are separated by commas.

**Overflow menu button:** Displays "..." when artists exceed available space. Popover (300px width) contains all remaining artists alphabetically sorted. Each popover row navigates to that artist and closes the popover.

#### Overflow Behavior

The artist button container reads its allocated width and adds buttons left-to-right until space runs out. When adding the next button would overflow, previously added buttons are removed as needed to fit the "..." overflow button. The layout recalculates on window resize.

**Example Scenarios:**

| Scenario | Artists | Display |
|----------|---------|---------|
| Sufficient space | `["Artist A", "Artist B"]` | `Artist A, Artist B` |
| Tight space | `["Artist A", "Artist B", "Artist C"]` | `Artist A, Artist B, ...` |
| Very long names | `["Very Long Artist Name"]` | `Very Long Artist..., ...` |
| Featuring artists | `["Artist C", "Artist D"]` | `feat. Artist C, ...` |

### Album Button

Track rows include a clickable album name in amber. Click navigates to album detail. No popover; single click navigates directly. The label ellipsizes for overflow.

## Row Templates

### artist_row

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ [48px  │ Artist Name (18px, bold)                       │ [art][art][art][art][art] │
│ artist ├────────────────────────────────────────────────┤      48x48 thumbnails     │
│  art]  │ N albums · Appears on N tracks (12px, dim)     │    (up to 6, rowspan=2)   │
└──────────────────────────────────────────────────────────────────────────────────────┘
  col 0              col 1 (expand)                              col 2 (40%)
```

2-row, 3-column grid. Artist thumbnail spans both rows on the left (col 0, 48x48 circular clip). Name in col 1 row 0, metadata in col 1 row 1. Art strip spans both rows on the right (col 2, up to 6 album thumbnails at 48x48).

**Artist thumbnail:** 48x48px, raw pixels with no border or decoration. Loaded from the per-library artist atlas via `artwork_manager_get_artist_thumbnail()`. Shows a person-symbolic placeholder while loading or when no image is available. Artists without a MusicBrainz ID never have artwork (no source to fetch from).

### album_row

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│ [48px  │  Album Title (20px, bold)            │  Rock · Electronic             │    Year   │
│  art   ├───────────────────────────────────────────────────────────────────────────────────┤
│  2rows]│  Artist A, Artist B... (14px, dim)                         [lib-name] NN Tracks   │
└────────────────────────────────────────────────────────────────────────────────────────────┘
   col 0              col 1 (expand)                       col 2                col 3 (far right)
```

2-row, 4-column grid. Album art spans both rows (col 0). Title in col 1 row 0, genre pills in col 2 row 0, year fixed at far right (col 3). Artist buttons with overflow in col 1 row 1, track count and library name in col 2-3 row 1.

### track_row

Full track row for search results and "Appears On" sections.

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│ [48px] │  Track Title (20px, bold)     │  Album Name (16px)                  │    Year   │
│  art   │  Artist A, Artist B... (16px) │  feat. Artist C...        [lib-name]│    3:06   │
└──────────────────────────────────────────────────────────────────────────────────────────┘
           col_left (60% flexible)         col_right (40% flex)                  col_meta (fixed)
```

Horizontal box with proportional column sizing (60/40). Art and metadata columns are fixed width; the remaining space is split between left (title + primary artists) and right (album + featuring artists).

### album_detail_track_item

Compact track row for album detail track lists (used in both album detail and artist detail cards). Does not display primary artists, since those appear in the album header.

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│  3.  Track Title                        feat. Artist C...             [i]        3:02│
└──────────────────────────────────────────────────────────────────────────────────────┘
  col 0  col 1                            col 2                      col 3      col 4
```

5-column grid. Track number (fixed), title (expand), featuring artists (expand, hidden when empty), info button (fixed), duration (fixed).

### disc_header

Separator for multi-disc albums. Inserted before first track of each disc.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ DISC 2                                                                  │
└─────────────────────────────────────────────────────────────────────────┘
```

Uppercase, muted text, subtle top border.

### album_card

Self-contained album card for artist detail view. Shows album header with large artwork, metadata, and a preview track list. Automatic disc header insertion when disc number changes between tracks.

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ ┌──────────┐  Album Title                                                            │
│ │  250px   │  Artist A, Artist B  (primary artists shown here)                      │
│ │   art    │  2024 · 12 tracks · 45:32                                               │
│ └──────────┘                                                                         │
│  1. Track One                                                                   3:02 │
│  2. Track Two                         feat. Artist C...                         4:15 │
│  3. Track Three                                                                 3:45 │
│ ...see all 12 tracks                                                                 │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

- **Album art:** 250x250px
- **Title:** Bold, natural case (no forced uppercase)
- **Artists:** All primary artists, comma-separated. Shown in header only, not repeated in track list.
- **Stats:** Year, track count, and total duration separated by centered dot
- **Track list:** Compact track items without primary artist names (only featuring artists shown inline). No info button in card context.
- **See all:** Shows when more than 5 preview tracks

## Detail Views

### Album Detail View

Full-page album view with header (artwork + metadata) and track list. Primary artists are displayed in the header only, not repeated in the track list.

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│  <- Back to Artists                                                                  │
│                                                                                      │
│  ┌─────────┐  Album Title Here (Natural Case)                                       │
│  │ 250x250 │  [Primary Artists] [Year]  <- Artist button navigates to artist        │
│  │  album  │  12 tracks - 45:32                                                     │
│  │   art   │                                                                        │
│  └─────────┘                                                                        │
│                                                                                      │
│   1  Track Title One                                                          3:45  │
│   2  Track Title Two              feat. Artist X                              4:12  │
│   3  Track Title Three                                                        2:58  │
│                                                                                      │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

**Header layout:**
- **Album art:** 250x250px (left side)
- **Album title:** 36px, bold, natural case, ellipsizes if too long, selectable
- **Primary artists:** 16px, clickable button (navigates to artist detail)
- **Year:** 16px, selectable
- **Stats:** 16px, selectable (track count and duration)

**Track list:**
- No primary artist names shown (since they are in the header)
- Only featuring artists appear inline (when present)
- Track number, title, featuring artists (optional), info button, duration

## Shared Row Styling

All three row templates (artist, album, track) use shared styling for inline elements so that typography, colors, and hover behavior are consistent across search results and list views.

### Entity Buttons

| Element | Color | Hover | Size | Weight |
|---------|-------|-------|------|--------|
| Album link | `#e5a640` (amber) | `#edb85c` | 16px | 500 |
| Artist link | `#00d4ff` (cyan) | `#33ddff` | 16px | 500 |

Both button types: transparent background, no border, underline on hover. Album button label uses ellipsize for overflow; artist buttons use width-aware overflow with "..." popover.

### Row Title

Primary entity name in each row (track title, album title, artist name). 20px, bold, `#e0e0e0`. Brightens to `#ffffff` on row hover.

### Row Meta

Secondary metadata (year, track count). 17px monospace, `#999999`.
