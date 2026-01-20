# UI Design Conventions

Minimal style guide for consistent UI implementation across Quadrature.

## Typography

### Font Sizes

| Context | Size | Weight | Usage |
| ------- | ---- | ------ | ----- |
| View title | 24px | Bold | "ABBEY ROAD", artist name in detail |
| Section header | 14px | Bold | "ALBUMS", "DISC 1" |
| Row title | 14px | Medium | Album title, track title, artist name |
| Row meta | 12px | Regular | "Artist · Year", track count, duration |
| Track number | 12px | Regular | Right-aligned, muted |
| Time display | 18px+ | Bold | Time remaining in channel strip |
| Button labels | 12px | Medium | Transport controls, buttons |

### Text Transforms

| Element | Transform | Example |
| ------- | --------- | ------- |
| Album title (detail header) | UPPERCASE | "ABBEY ROAD" |
| Artist name (detail header) | UPPERCASE | "THE BEATLES" |
| Section headers | UPPERCASE | "ALBUMS", "DISC 1" |
| Column headers | UPPERCASE | "TITLE", "ARTIST", "YEAR" |
| Everything else | Normal case | "Come Together" |

### Font Styles

- **Body text:** Sans-serif system font
- **Duration/time:** Monospace for alignment
- **Links:** Regular weight, cyan color (`#00d4ff`)

## Color Palette

### Semantic Colors

| Name | Hex | Usage |
| ---- | --- | ----- |
| **Primary Accent** | `#00d4ff` | Focus indicator, active nav, links, highlights |
| **Background** | `#1e1e1e` | Main window background |
| **Surface** | `#2a2a2a` | Row hover, card backgrounds |
| **Surface Selected** | `#00d4ff20` | Selected row (20% opacity accent) |
| **Text Primary** | `#e0e0e0` | Album titles, track titles, main text |
| **Text Secondary** | `#888888` | Metadata, "Artist · Year", track counts |
| **Text Dim** | `#555555` | Very subtle text, disabled state |

### State Colors

| State | Color | Hex | Usage |
| ----- | ----- | --- | ----- |
| Online/Ready | Green | `#4ade80` | Library online, device ready |
| Indexing/Loading | Blue | `#60a5fa` | Progress, checking state |
| Error/Warning | Red | `#f87171` | Errors, time warning (≤30s) |
| Offline/Disabled | Gray | `#6b7280` | Unavailable libraries, disabled controls |
| Preview | Orange | `#ff9500` | Preview/PFL mode |
| Queued | Green | `#00cc66` | Queued for broadcast |
| On-Air | Green | `#00cc66` | Live on air |

### Channel States

| State | Border Color | Hex |
| ----- | ------------ | --- |
| IDLE | Gray | `#555555` |
| FOCUSED | Cyan | `#00d4ff` |
| PREVIEW | Orange | `#ff9500` |
| QUEUED | Green | `#00cc66` |
| ON_AIR | Green | `#00cc66` |
| UNCONFIGURED | Light Red | `#ff6666` |
| INVALID | Dark Red | `#993333` |

## Spacing & Sizing

### Row Heights

| Row Type | Height | Usage |
| -------- | ------ | ----- |
| Album row | 64px | List view with 48px art + padding |
| Artist row | 68px | List view with art strip |
| Track row (compact) | 36px | Album detail view, artist detail track previews |
| Track row (standard) | 52px | Artist detail Tracks view, search results |
| Search result | 48-60px | Varies by result type |

### Padding & Margins

| Element | Padding/Margin |
| ------- | -------------- |
| Row internal | 8px vertical, 12px horizontal |
| View margins | 16px all sides |
| Section spacing | 24px between sections |
| Button padding | 6px vertical, 12px horizontal |
| Input padding | 8px all sides |

### Border Radius

| Element | Radius | Usage |
| ------- | ------ | ----- |
| Buttons | 4px | Standard buttons |
| Cards | 8px | Library cards, panels |
| Album art | 4px | Thumbnails, detail art |
| Inputs | 4px | Search box, text inputs |
| Channel strip | 8px | Entire strip container |

## Album Art Sizes

Standard sizes for album artwork across the UI:

| Context | Size | Usage |
| ------- | ---- | ----- |
| **Thumbnails** | 48×48px | All thumbnails: albums list, search results, track rows, artist rows, artist detail albums |
| **Album detail header** | 120×120px | Album detail view header |
| **Artist detail album art** | 250×250px | Full-resolution album art in artist detail view |

**Placeholder:** Missing art shows gray box with music note icon, matching size.

**Design principle:** All thumbnails in the UI are 48×48px for visual consistency. Only detail views show larger artwork loaded from full-resolution sources.

## Interactive States

### Hover Behavior

| Element | Hover Effect | Transition |
| ------- | ------------ | ---------- |
| Row | Background → Surface (`#2a2a2a`) | 100ms ease |
| Button | Brightness +10%, scale 1.02 | 100ms ease |
| Link | Opacity 80% → 100%, scale 1.02 | 150ms ease-out |
| Slider thumb | Scale 1.2 | 200ms ease-out |
| Channel badge | Glow effect | 150ms ease |

**Cursor:** All interactive elements show `cursor: pointer` on hover.

### Active/Pressed State

| Element | Effect |
| ------- | ------ |
| Button | Scale 0.98, brightness -10% |
| Row | Background → darker surface |
| Toggle (on) | Accent background, white icon |

### Focus State

| Element | Effect |
| ------- | ------ |
| Input | Cyan border, subtle glow |
| Button | Cyan outline (2px) |
| Row (keyboard nav) | Cyan left accent (4px) |

## Widget Patterns

### Standard Row

```
┌────────────────────────────────────────────────────────┐
│ [icon] Title Text                         Meta Text    │
│        Subtitle Text                                   │
└────────────────────────────────────────────────────────┘
```

- **Icon:** Left-aligned (art, symbol)
- **Title:** Primary text, medium weight
- **Subtitle:** Secondary text below title
- **Meta:** Right-aligned, secondary color

### Header Pattern

```
┌────────────────────────────────────────────────────────┐
│ TITLE TEXT                                  [Button]   │
│ Subtitle · Metadata                                    │
└────────────────────────────────────────────────────────┘
```

- **Title:** UPPERCASE, bold, 24px
- **Subtitle:** Regular weight, secondary color
- **Action button:** Right-aligned (Shuffle, etc.)

### List View Pattern

```
┌────────────────────────────────────────────────────────┐
│ View Name · Count                    [Sort Controls]   │
├────────────────────────────────────────────────────────┤
│ [Row 1]                                                │
│ [Row 2]                                                │
│ [Row 3]                                                │
└────────────────────────────────────────────────────────┘
```

- **Header:** View title + item count + controls
- **Separator:** Subtle border below header
- **Rows:** Scrollable list

## Animations

### Duration Guidelines

| Animation Type | Duration | Easing |
| -------------- | -------- | ------ |
| Hover feedback | 100-150ms | ease-out |
| Button press | 100ms | ease-in-out |
| View transition | 200ms | ease-out |
| Slider movement | 200ms | ease-out |
| Pulse animation | 2s loop | ease-in-out |
| Toast appear | 300ms | ease-out |
| Toast dismiss | 200ms | ease-in |

### Special Animations

**Pulse (QUEUED state):**
- Opacity: 100% → 60% → 100%
- Duration: 2s loop
- Easing: ease-in-out

**Glow (hover on channel badge):**
- Box-shadow: 0 0 0 → 0 0 8px
- Color: Matches state color
- Duration: 150ms ease

**Marquee (long text):**
- Horizontal scroll when text overflows
- Pause 2s, scroll at 30px/s, pause 2s, reset
- Only active on hover

## Responsive Behavior

### Minimum Widths

| Panel | Min Width | Behavior Below Min |
| ----- | --------- | ------------------ |
| Library content | 400px | Horizontal scroll |
| Channel strip | 280px | Compact layout |
| Navigation bar | 56px | Icons only (always) |
| Album art (detail) | 120px | Never scales down |

### Text Overflow

| Element | Overflow Behavior |
| ------- | ----------------- |
| Row titles | Ellipsis (...) |
| Row metadata | Ellipsis (...) |
| Long track titles | Marquee scroll on hover |
| Time display | Never truncate |

## Accessibility

### Contrast Ratios

- **Primary text on background:** 4.5:1 minimum
- **Secondary text on background:** 3:1 minimum
- **Accent color on background:** 4.5:1 minimum

### Focus Indicators

- All interactive elements must show visible focus state
- Minimum 2px outline or border
- Cyan accent color for consistency

### Keyboard Navigation

- Tab order follows visual hierarchy
- All actions accessible via keyboard
- See [KEYBINDS.md](KEYBINDS.md) for shortcuts

## Icons

### Icon Set

Use symbolic icons from system icon theme (typically Adwaita).

**Common Icons:**
- `media-playback-start` - Play
- `media-playback-pause` - Pause
- `media-playback-stop` - Stop
- `media-skip-backward` - Previous
- `media-skip-forward` - Next
- `go-next` - Chevron right (drill-down)
- `go-previous` - Back arrow
- `edit-find` - Search
- `audio-x-generic` - Music note (missing art)

### Icon Sizes

| Context | Size |
| ------- | ---- |
| Navigation bar | 24px |
| Buttons | 16px |
| Row indicators | 16px |
| Missing album art | 32px |

## CSS Class Naming

### Conventions

- Use kebab-case: `.album-row`, `.song-title`
- Component prefix: `.channel-strip-focused`
- State suffix: `-active`, `-hover`, `-disabled`
- Modifier suffix: `-compact`, `-large`

### Common Patterns

| Pattern | Example | Usage |
| ------- | ------- | ----- |
| Component | `.channel-strip` | Top-level widget |
| Element | `.album-art-thumb` | Child element |
| State | `.nav-item-active` | Interactive state |
| Modifier | `.song-row-compact` | Size/style variant |

## Implementation Notes

### GTK4 Specifics

- Use GtkBox with CSS for flexible layouts
- Apply CSS classes via `gtk_widget_add_css_class()`
- Custom drawing for spectrum display only
- Prefer CSS over custom drawing where possible

### CSS Variables

Define common values as CSS variables:

```css
:root {
  --accent-color: #00d4ff;
  --bg-color: #1e1e1e;
  --surface-color: #2a2a2a;
  --text-primary: #e0e0e0;
  --text-secondary: #888888;
  --spacing-unit: 8px;
}
```

Use in styles: `background: var(--bg-color);`
