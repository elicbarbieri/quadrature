# UI Conventions

Style reference for Quadrature UI.

## Colors

| Name               | Hex         | Usage                              |
| ------------------ | ----------- | ---------------------------------- |
| Primary Accent     | `#00d4ff`   | Focus, active nav, artist links    |
| Album Accent       | `#e5a640`   | Album links, album buttons         |
| Background         | `#121212`   | Window background                  |
| Surface            | `#2a2a2a`   | Row hover, card backgrounds        |
| Surface Selected   | `#00d4ff20` | Selected row (20% opacity accent)  |
| Text Primary       | `#e0e0e0`   | Titles, main text                  |
| Text Secondary     | `#888888`   | Metadata, subtitles                |
| Text Dim           | `#555555`   | Disabled state                     |

### Entity Color Coding

Interactive entity links are color-coded by type for instant visual differentiation:

| Entity  | Base      | Hover     |
| ------- | --------- | --------- |
| Artist  | `#00d4ff` | `#33ddff` |
| Album   | `#e5a640` | `#edb85c` |

These colors are consistent everywhere the entity appears: library rows, detail views, and search results. Both meet WCAG AAA contrast on `#121212`.

### State Colors

| State    | Hex       | Usage                      |
| -------- | --------- | -------------------------- |
| Online   | `#4ade80` | Library online, ready      |
| Indexing | `#60a5fa` | Progress, checking         |
| Error    | `#f87171` | Errors, time warning       |
| Offline  | `#6b7280` | Unavailable, disabled      |
| Preview  | `#ff9500` | PFL mode                   |
| On-Air   | `#00cc66` | Live, queued               |

## Sizing

### Album Art

| Context       | Size      | Notes |
| ------------- | --------- | ----- |
| Thumbnails    | 96x96px   | Managed by artwork manager |
| Detail header | 120x120px | Album detail view |
| Artist detail | 250x250px | Large artwork in album cards |

Missing art shows gray placeholder with music note icon.

### Row Heights

| Type               | Height |
| ------------------ | ------ |
| Album row          | 64px   |
| Artist row         | 68px   |
| Track row compact  | 36px   |
| Track row standard | 52px   |

### Typography

| Context              | Size  | Weight   |
| -------------------- | ----- | -------- |
| View title           | 24px  | Bold     |
| Detail view title    | 40px  | Bold     |
| Detail view metadata | 16px  | Regular  |
| Section header       | 11px  | Bold     |
| Library row title    | 20px  | Bold     |
| Library row meta     | 17px  | Medium   |
| Album button         | 16px  | Medium   |
| Artist button        | 16px  | Medium   |
| Row subtitle         | 12px  | Regular  |
| Duration             | 14px  | Regular  |
| Time display         | 18px+ | Bold     |

**Transforms:** Section headers and column headers use UPPERCASE. Album/artist names in detail views use natural case (no forced transformation).

### Row Vertical Alignment

All 2-row library rows (artist, album, track) use a center-hugging vertical alignment pattern:

| Row | Alignment | Effect |
|-----|-----------|--------|
| Top row (titles, album name, year) | bottom-aligned | Content sits at bottom of cell |
| Bottom row (artists, genres, duration) | top-aligned | Content sits at top of cell |
| Art thumbnail (spans both rows) | vertically centered | Centered across full row height |

This creates visual cohesion where primary content (top row) and secondary content (bottom row) both gravitate toward the center dividing line, with no wasted vertical whitespace between them.

For track rows (horizontal box layout), the column boxes are vertically centered, which achieves the same effect: the two stacked children within each column box naturally hug each other in the center of the row.

## Interactive States

**Hover:** Background to surface color, 100ms ease. Buttons brighten +10%, scale 1.02.

**Focus:** Cyan border (2px) or left accent (4px for rows).

**Active:** Scale 0.98, brightness -10%.

**Cursor:** All interactive elements show pointer on hover.

## Text Overflow Handling

| Context | Method | Visual Effect |
|---------|--------|---------------|
| Track title | Ellipsize | Standard "..." truncation |
| Album button label | Ellipsize | Standard "..." truncation |
| Artist buttons | Overflow "..." button | Remaining artists shown in alphabetical popover |
| Regular labels | Ellipsize | Standard "..." truncation |

See the Artist Buttons section in [COMPONENTS.md](COMPONENTS.md) for overflow behavior details.

## Icons

Symbolic icons from system theme (Adwaita). Common: `media-playback-start`, `media-playback-pause`, `go-next`, `go-previous`, `edit-find`, `audio-x-generic` (missing art).

| Context        | Size |
| -------------- | ---- |
| Navigation bar | 24px |
| Buttons/rows   | 16px |
| Missing art    | 32px |
