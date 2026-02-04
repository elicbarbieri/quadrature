# UI Conventions

Style reference for Quadrature UI.

## Colors

| Name               | Hex         | Usage                              |
| ------------------ | ----------- | ---------------------------------- |
| Primary Accent     | `#00d4ff`   | Focus, active nav, artist links    |
| Album Accent       | `#e5a640`   | Album links, album buttons         |
| Background         | `#1e1e1e`   | Window background                  |
| Surface            | `#2a2a2a`   | Row hover, card backgrounds        |
| Surface Selected   | `#00d4ff20` | Selected row (20% opacity accent)  |
| Text Primary       | `#e0e0e0`   | Titles, main text                  |
| Text Secondary     | `#888888`   | Metadata, subtitles                |
| Text Dim           | `#555555`   | Disabled state                     |

### Entity Color Coding

Interactive entity links are color-coded by type for instant visual differentiation:

| Entity  | Base      | Hover     | CSS Class             |
| ------- | --------- | --------- | --------------------- |
| Artist  | `#00d4ff` | `#33ddff` | `.artist-btn`         |
| Album   | `#e5a640` | `#edb85c` | `.album-btn`          |

These colors are consistent everywhere the entity appears — library rows, detail views, and search results. Both meet WCAG AAA contrast on `#1e1e1e`.

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
| Thumbnails    | 96x96px   | Managed by `artwork_manager_get_thumb_size()` |
| Detail header | 120x120px | Album detail view |
| Artist detail | 250x250px | Large artwork in album cards |

**Note:** Thumbnail size is set programmatically via `artwork_manager_get_thumb_size()`, not hardcoded in templates. CSS fallback minimum is 48px. Current deployment uses 96px thumbnails.

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

All 2-row library rows (artist, album, track) use a **center-hugging** vertical alignment pattern:

| Row | Alignment | Effect |
|-----|-----------|--------|
| Top row (titles, album name, year) | `valign=end` | Content sits at bottom of cell |
| Bottom row (artists, genres, duration) | `valign=start` | Content sits at top of cell |
| Art thumbnail (spans both rows) | `valign=center` | Centered across full row height |

This creates visual cohesion where primary content (top row) and secondary content (bottom row) both gravitate toward the center dividing line, with no wasted vertical whitespace between them.

For track rows (horizontal GtkBox layout), the column boxes use `valign=center` which achieves the same effect — the two stacked children within each column box naturally hug each other in the center of the row.

## Interactive States

**Hover:** Background to surface color, 100ms ease. Buttons brighten +10%, scale 1.02.

**Focus:** Cyan border (2px) or left accent (4px for rows).

**Active:** Scale 0.98, brightness -10%.

**Cursor:** All interactive elements show `pointer` on hover.

## CSS Variables

GTK4 doesn't support CSS custom properties, so these are reference values only (hardcoded in the stylesheets):

```
--accent-color: #00d4ff
--album-accent:  #e5a640
--bg-color:      #1e1e1e
--surface-color: #2a2a2a
--text-primary:  #e0e0e0
--text-secondary:#888888
```

## Text Overflow Handling

Different UI contexts use different overflow strategies based on content type and user interaction needs:

| Context | Method | Visual Effect |
|---------|--------|---------------|
| Track title | GTK ellipsize | Standard "…" truncation |
| Album button label | GTK ellipsize | Standard "…" truncation |
| Artist buttons | Overflow "…" button | Remaining artists shown in alphabetical popover |
| Regular labels | GTK ellipsize | Standard "…" truncation |

### Width-Aware Overflow

Artist columns use intelligent overflow calculation:
- Reads available width directly from the artist box's allocated size (`gtk_widget_get_width()`)
- Adds artist buttons dynamically while space available
- When space exhausted, shows "…" button with popover containing remaining artists
- Recalculates on resize via tick callback monitoring box width

See **Artist Buttons** section in `COMPONENTS.md` for detailed algorithm.

## Icons

Symbolic icons from system theme (Adwaita). Common: `media-playback-start`, `media-playback-pause`, `go-next`, `go-previous`, `edit-find`, `audio-x-generic` (missing art).

| Context        | Size |
| -------------- | ---- |
| Navigation bar | 24px |
| Buttons/rows   | 16px |
| Missing art    | 32px |
