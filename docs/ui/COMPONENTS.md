# UI Components

Reusable components and shared patterns. All row types use GTK templates and shared helper functions.

## Navigation Architecture

Window-level navigation stack enables full back/forward through all views.

```c
typedef struct {
    const char *view_name;     // "search", "artists", "albums", "tracks", "detail"
    int64_t entity_id;         // album_id or artist_id for detail views
    DetailState detail_state;  // ALBUM or ARTIST for detail views
    int64_t selected_id;       // restore selection on back
    double scroll_pos;         // restore scroll on back
} NavStackEntry;
```

**Navigation flow example:**
```
Search → Album(123) → Artist(45) → Album(678)
         ↑ Back returns through entire stack to Search
```

## Selection

GTK handles selection automatically via `GtkSelectionModel` for list views:
- **Visual:** `:selected` CSS pseudo-class (GTK default)
- **Keyboard:** Up/down arrows navigate, Enter activates
- **1-4 keys:** Queue selected track to channel N (no-op if nothing selected)

## Shared Handlers (`window.c`)

All row interactions route through shared handlers for consistency:

```c
// Activate handlers (double-click / Enter) - primary action
static void on_track_activate(int64_t track_id, gpointer window);   // navigate to album, select track
static void on_album_activate(int64_t album_id, gpointer window);   // navigate to album detail
static void on_artist_activate(int64_t artist_id, gpointer window); // navigate to artist detail

// Secondary handlers (right-click) - queue to focused channel
static void on_track_secondary(int64_t track_id, gpointer window);  // queue track
static void on_album_secondary(int64_t album_id, gpointer window);  // queue track_1
```

### Interaction Behavior by Row Type

| Row Type | Activate (double-click / Enter) | Secondary (right-click) |
|----------|--------------------------------|------------------------|
| Track | Navigate to album, select track | Queue track |
| Album | Navigate to album detail | Queue track_1 |
| Artist | Navigate to artist detail | — |

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `1-4` | Queue selected track to channel N (no-op if nothing selected) |
| `↓/↑` | Move selection (GTK default) |
| `Enter` | Activate selected item (GTK default) |
| `Escape` | Clear selection |

## Row Helpers (`row_helpers.c`)

Shared functions for creating rows with consistent styling. All row creation functions store entity IDs for handler access.

```c
// Create rows from LibraryCache data
GtkWidget *ui_create_artist_row(const library_artist_info_t *artist, gboolean show_art_strip);
GtkWidget *ui_create_album_row(const library_album_info_t *album, ArtworkManager *art_mgr, gboolean show_count);
GtkWidget *ui_create_track_row(const library_track_info_t *track, ArtworkManager *art_mgr, gboolean show_album_info);

// Album detail components
GtkWidget *ui_create_album_detail_track_item(const library_track_info_t *track);
GtkWidget *ui_create_disc_header(uint16_t disc_num);

// Album card with automatic disc headers (for artist detail view)
// Automatically inserts disc headers when disc_num changes between tracks
// max_preview_tracks: limit of tracks to show (0 for all)
GtkWidget *ui_create_album_detail_card(const library_album_info_t *album,
                                        const GPtrArray *tracks,
                                        ArtworkManager *art_mgr,
                                        guint max_preview_tracks);
```

### Row Data Keys

Each row stores entity data via `g_object_set_data()`:

| Row Type | Keys |
|----------|------|
| Artist | `artist-id` |
| Album | `album-id` |
| Track | `track-id`, `track-path`, `album-id`, `track-artists` (GPtrArray of `db_track_artist_t`) |

### Attaching Handlers

After creating rows, attach handlers using the shared callback system:

```c
void ui_row_attach_handlers(GtkWidget *row, RowCallbacks *callbacks);

typedef struct {
    void (*on_click)(int64_t entity_id, gpointer data);
    void (*on_double_click)(int64_t entity_id, gpointer data);
    void (*on_right_click)(int64_t entity_id, gpointer data);
    gpointer user_data;
} RowCallbacks;
```

## Inline Navigation Elements

Shared interaction patterns for clickable artist and album elements within rows.

### Artist Buttons (`GtkButton` / `GtkMenuButton`)

Artists are displayed as clickable buttons with intelligent overflow handling based on available column space.

#### Artist Button Types

1. **Individual Artist Button** (`GtkButton`)
   - Flat button (`has-frame=false`)
   - CSS class: `.artist-btn`
   - Click navigates to artist detail via `on_artist_activate(artist_id)`
   - Separated by comma labels (`, `)

2. **Overflow Menu Button** (`GtkMenuButton`)
   - Displays "…" when artists exceed available space
   - CSS class: `.artist-btn`
   - Popover (300px width) contains all remaining artists alphabetically sorted
   - Each popover row navigates to artist and closes popover

#### Space-Aware Population Algorithm

The `populate_artist_buttons()` function dynamically calculates available width and intelligently truncates the artist list:

**Algorithm:**
1. Read available width directly from the artist box's allocated size (`gtk_widget_get_width()`)
2. Add "feat. " prefix label (for featuring artists only)
3. Iterate through filtered artists:
   - Measure width of next button + comma (using `gtk_widget_measure()`)
   - If adding would exceed available space:
     - Remove previously added buttons until overflow button fits
     - Create overflow button with ALL remaining artists
     - Break loop
   - Otherwise: add button + comma, continue
4. If all artists fit: use standard layout (no overflow)
5. Recalculates on window resize via `size-allocate` signal

**Column Width Allocations:**
- **Library track row - Primary artists (col 1, row 1):** 60% of content width
- **Library track row - Metadata box featuring artists (col 2, row 1):** Flexible (after year/duration)
- **Album detail track - Featuring artists (col 2):** 30% of row width

**Example Scenarios:**

| Scenario | Available Space | Artists | Display |
|----------|----------------|---------|---------|
| Sufficient space | 250px | `["Artist A", "Artist B"]` | `Artist A, Artist B` |
| Tight space | 200px | `["Artist A", "Artist B", "Artist C"]` | `Artist A, Artist B, …` |
| Overflow button | — | Click "…" | Popover: `Artist C` |
| Very long names | 180px | `["Very Long Artist Name"]` | `Very Long Artist…, …` |
| Featuring artists | 150px | `["Artist C", "Artist D"]` | `feat. Artist C, …` |
| Overflow doesn't fit | 120px | `["Artist A", "Artist B"]` | `Artist A, …` (removes B too) |

#### Display Format

| Location | Label format | Visibility | Overflow |
|----------|-------------|------------|----------|
| Primary artists (col 1, row 1) | `"Artist A, Artist B, …"` | Always when track has primary artists | Yes (width-aware) |
| Featuring artists (col 2, row 1) | `"feat. Artist C, Artist D, …"` | Only when featuring artists exist | Yes (width-aware) |
| Year (col 3, row 0) | `"2024"` | When available | No (fixed 4 chars) |
| Duration (col 3, row 1) | `"3:45"` | Always | No (fixed 5 chars) |

#### Overflow Popover Behavior

- **Trigger:** Click "…" button
- **Content:** `GtkListBox` (300px width) with all remaining artists alphabetically sorted
- **Row action:** Navigate to artist detail + dismiss popover
- **Template:** `artist_popover.ui` with dynamic rows from `artist_popover_row.ui`

#### Implementation Details

**Width Measurement (GTK4):**
```c
int min_width, natural_width;
gtk_widget_measure(button, GTK_ORIENTATION_HORIZONTAL, -1, 
                   &min_width, &natural_width, NULL, NULL);
```

**Box Width Calculation:**
```c
int max_width = gtk_widget_get_width(box);
if (max_width <= 0) max_width = 400;  // fallback before first layout
```

**Responsive Resize:**
Connected via tick callback monitoring the artist box's allocated width. When width changes (window resize, proportional reflow), artist buttons are recalculated and repopulated to fit the new space.

**Comma Separator Width:**
Measured once per population (typically ~8-10px including spacing).

#### CSS Styling

**No gradients or masks on artist columns** — overflow is handled purely through the "…" button mechanism. Individual artist buttons may ellipsize if their text is too long, but the column itself does not fade.

See **Library Row Shared Styling** for `.artist-btn` color and typography values.

### Column-Specific Overflow Handling

Different columns have different overflow strategies based on their content type:

| Column | Strategy | Implementation |
|--------|----------|----------------|
| **Track Title** (col_left, top) | GTK ellipsize | Standard "…" truncation |
| **Album Name** (col_right, top) | GTK ellipsize | Standard "…" truncation via album button label |
| **Year** (col_meta, top) | Fixed width | `width-chars=4`, right-aligned |
| **Primary Artists** (col_left, bottom) | Overflow button | Width-aware calculation, "…" button with popover |
| **Featuring Artists** (col_right, bottom) | Overflow button | Width-aware calculation, "…" button with popover |
| **Duration** (col_meta, bottom) | Fixed width | `width-chars=5`, right-aligned |

#### Design Rationale

- **Track title / Album name:** GTK ellipsize provides standard "…" truncation
- **Primary artists:** Multiple discrete entities; overflow button allows full navigation without UI clutter
- **Year:** Fixed-width at far right of top row for consistent alignment across rows
- **Duration:** Fixed-width at far right of bottom row, vertically aligned with year
- **Featuring artists:** Own column with overflow button
- **2-row layout:** Emphasizes track title and album name while de-emphasizing metadata through smaller font and dimmed color

### Album Button (`GtkButton`)

Library track rows include a clickable album name in amber (`#e5a640`). `GtkButton` with `has-frame=false`, child `GtkLabel` with `ellipsize=end` for overflow. Click navigates to album detail via `on_album_activate(album_id)`. No popover — single click navigates directly. See **Library Row Shared Styling** for `.album-btn` values.

## Detail Views

### Album Detail View

Full-page album view with header (artwork + metadata) and track list. **Primary artists are displayed in the header only**, not repeated in the track list.

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│  ← Back to Artists                                                               │
│                                                                                  │
│  ┌─────────┐  Album Title Here (Natural Case)                                   │
│  │ 250×250 │  [Primary Artists] [Year]  ← Artist button navigates to artist    │
│  │  album  │  12 tracks - 45:32                                                 │
│  │   art   │                                                                    │
│  └─────────┘                                                                    │
│                                                                                  │
│   1  Track Title One                                                      3:45  │
│   2  Track Title Two              feat. Artist X                          4:12  │
│   3  Track Title Three                                                    2:58  │
│                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────┘
```

**Header layout:**
- **Album art:** 250×250px (left side)
- **Metadata box (right side):**
  - **Album title:** 36px, bold, natural case, ellipsizes if too long, selectable
  - **Primary artists:** 16px, clickable button (navigates to artist detail)
  - **Year:** 16px, selectable
  - **Stats:** 16px, selectable (track count and duration)

**Track list:**
- Uses `album_detail_track_item.ui` rows
- **No primary artist names shown** (since they're in the header)
- Only featuring artists appear inline (when present)
- Track number, title, featuring artists (optional), info button, duration

## Library Row Shared Styling

All three `library-*-row` templates (artist, album, track) use shared CSS classes for inline elements so that typography, colors, and hover behavior are consistent when rows appear together in search results and list views.

### Entity Buttons

| Element | CSS Class | Color | Hover | Size | Weight |
|---------|-----------|-------|-------|------|--------|
| Album link | `.album-btn` | `#e5a640` (amber) | `#edb85c` | 16px | 500 |
| Artist link | `.artist-btn` | `#00d4ff` (cyan) | `#33ddff` | 16px | 500 |

Both button types: `background: transparent`, `border: none`, underline on hover with `text-underline-offset: 2px`. Album button label uses GTK ellipsize for overflow; artist buttons use width-aware overflow with "..." popover.

### Row Title

`.library-row-title` provides the primary entity name in each row (track title, album title, artist name). Base style: 20px, bold, `#e0e0e0`. Brightens to `#ffffff` on row hover.

### Row Meta

`.library-row-meta` provides secondary metadata (year, track count). In album and track rows: 17px monospace, `#999999`.

## Templates

### `library_artist_row.ui`

Artist row for library list views. Uses `GtkGrid` with 2-row layout matching the unified design language, with album art strip on the right spanning both rows.

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ Artist Name (20px, bold)                                │ [art][art][art][art][art] │
├─────────────────────────────────────────────────────────┤      48×48 thumbnails     │
│ N albums · Appears on N tracks (12px, dim)              │    (up to 6, rowspan=2)   │
└──────────────────────────────────────────────────────────────────────────────────────┘
                     col 0 (60%)                                    col 1 (40%)
```

**Grid structure:**
- **2 rows** with album art strip spanning both rows on the right (rowspan=2)
- **2 columns** with 60/40 content split (col 0 / col 1)
- Row spacing: 4px for compact vertical spacing
- Column spacing: 12px

**Column configuration:**
- **Col 0, Row 0 (Artist Name):** `hexpand=true`, `ellipsize=end`, `xalign=0`, max-width=60%
- **Col 0, Row 1 (Metadata):** Container box with album count and track count stats (12px, dimmed)
- **Col 1 (Art Strip):** Album art strip box, rowspan=2, valign=center, max 6 thumbnails at 48×48px each

**Typography:**
- **Top row (Row 0):** `.library-row-title` — 20px, bold (see Shared Styling)
- **Bottom row (Row 1):** 12px font for metadata (de-emphasized, dimmed)
- **Colors:** Artist name `#e0e0e0`, metadata `#888888` (gray/dimmed)

**Vertical alignment:**
Row 0 (artist name) uses `valign=end` (bottom-aligned). Row 1 (metadata) uses `valign=start` (top-aligned). Art strip uses `valign=center` (spans both rows). This center-hugging pattern matches the album and track row alignment.

**Layout implementation:**
- Uses `GtkGrid` with `column-spacing=12`, `row-spacing=4`
- Album art strip positioned at col=1, row=0, row-span=2
- Art strip shows up to 6 thumbnails (48×48px each) from artist's albums
- Stats format: `"N albums · Appears on N tracks"` or variations based on counts
- Min-height: 60px (accommodates 2 rows + spacing)

### `library_album_row.ui`

Album row for library list views. Uses `GtkGrid` with 2-row, 4-column layout for visual hierarchy with year at the far right.

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│ [48px  │  Album Title (20px, bold)            │          N trk │              Year         │
│  art   ├──────────────────────────────────────┼────────────────┴─────────────────────────┤
│  2rows]│  Artist A, Artist B… (14px, dim)     │              Rock · Electronic  (col 2-3) │
└────────────────────────────────────────────────────────────────────────────────────────────┘
   col 0              col 1 (expand)               col 2             col 3 (far right)
```

**Grid structure:**
- **2 rows** with album art spanning both rows (rowspan=2)
- **4 columns** with year in its own column at the far right
- Row spacing: 4px for compact vertical spacing
- Column spacing: 12px

**Column configuration:**
- **Col 0 (Art):** Fixed 48×48px, rowspan=2, valign=center, no expand
- **Col 1, Row 0 (Title):** `hexpand=true`, `ellipsize=end`, `xalign=0`
- **Col 2, Row 0 (Track Count):** `xalign=1`, `halign=end`, hidden when empty
- **Col 3, Row 0 (Year):** `xalign=1`, `halign=end`, `width-chars=4`, far-right position
- **Col 1, Row 1 (Primary Artists):** Artist buttons with overflow mechanism
- **Col 2-3, Row 1 (Genres):** Genre pills container, `halign=end`, `column-span=2`

**Typography:**
- **Top row (Row 0):** `.library-row-title` — 20px, bold (see Shared Styling)
- **Bottom row (Row 1):** `.artist-btn` at 16px (see Shared Styling)
- **Year (Row 0, Col 3):** `.library-row-meta` — 17px monospace, `#999999`
- **Track Count (Row 0, Col 2):** `.library-row-meta` — 17px monospace, `#999999`
- **Genres (Row 1, Col 2-3):** Genre pill styling
- **Colors:** Title `#e0e0e0`, artists `#00d4ff` (cyan), year/count `#999999`

**Vertical alignment:**
Row 0 items (title, count, year) use `valign=end` (bottom-aligned). Row 1 items (artists, genres) use `valign=start` (top-aligned). Art thumbnail uses `valign=center` (spans both rows). This center-hugging pattern ensures primary and secondary content gravitate toward the center dividing line.

**Layout implementation:**
- Uses `GtkGrid` with `column-spacing=12`, `row-spacing=4`
- Album art positioned at col=0, row=0, row-span=2
- Year in its own column (col 3) ensures it's always at the absolute right edge
- Track count hidden (`gtk_widget_set_visible(FALSE)`) when empty, eliminating spacing gaps
- Genre pills span col 2-3 in row 1, right-aligned
- Artist buttons use width-aware overflow (see Artist Buttons section)
- Min-height: 60px (accommodates 2 rows + spacing)

### `library_track_row.ui`

Full track row for search results and "Appears On" sections. Uses a horizontal `GtkBox` with proportional column sizing (60/40) enforced programmatically via tick callback in `row_helpers.c`.

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│ [48px] │  Track Title (20px, bold)     │  Album Name (16px)   │     Year                │
│  art   │  Artist A, Artist B… (16px)   │  feat. Artist C…     │     3:06                │
└──────────────────────────────────────────────────────────────────────────────────────────┘
           col_left (60% flexible)         col_right (40% flex)    col_meta (fixed)
```

**Box structure:**
- Horizontal `GtkBox` with 4 children: art, col_left, col_right, col_meta
- Spacing: 12px between children
- Proportional sizing: tick callback reads row width, subtracts fixed elements (art, meta, spacing), and applies 60/40 split to col_left/col_right via `gtk_widget_set_size_request()`

**Column configuration:**
- **Art:** Fixed thumbnail, `valign=center`
- **col_left (Title + Primary Artists):** Vertical box, `valign=center`. Title with `ellipsize=end`, `xalign=0`. Primary artists box below.
- **col_right (Album + Featuring Artists):** Vertical box, `valign=center`. Album button with `ellipsize=end`. Featuring artists box below.
- **col_meta (Year + Duration):** Vertical box, `halign=end`, `valign=center`. Year (`width-chars=4`) and duration (`width-chars=5`).

**Typography:**
- **Top items:** `.library-row-title` — 20px, bold. `.album-btn` — 16px, amber (see Shared Styling)
- **Year:** `.library-row-meta` — 17px monospace, `#999999`
- **Bottom items:** `.artist-btn` — 16px cyan. `.library-row-subtitle` — 16px in track rows (CSS override)
- **Duration:** 14px monospace, `#888888`
- **Colors:** Title `#e0e0e0`, album `#e5a640` (amber), artists `#00d4ff` (cyan), year `#999999`

**Vertical alignment:**
Column boxes use `valign=center`, so the stacked title/artists pair naturally hugs the center of the row. This achieves the same center-hugging effect as the GtkGrid rows (where Row 0 uses `valign=end` and Row 1 uses `valign=start`).

**Proportional sizing implementation:**
- `track_row_proportional_tick()` callback fires once per frame
- Reads row width, subtracts art width + meta width + spacing (12px × 3)
- Applies 60% to col_left, 40% to col_right via `gtk_widget_set_size_request()`
- Skips recalculation when width changes less than 5px (debounce)

### `album_detail_track_item.ui`

Compact track row for album detail track lists (used in both album-detail and artist-detail cards). Uses `GtkGrid` for precise column control. **Does not display primary artists** — those are shown in the album header to the right of the album art.

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│  3.  Track Title                        feat. Artist C…             [ℹ]        3:02  │
└──────────────────────────────────────────────────────────────────────────────────────┘
  col 0  col 1                            col 2                      col 3      col 4
```

**Column configuration:**
- **Col 0 (Track#):** Fixed width (`width-chars=3`), `xalign=0`, no expand
- **Col 1 (Title):** `hexpand=true`, `ellipsize=end`, `xalign=0`
- **Col 2 (Secondary):** `hexpand=true`, `ellipsize=end` in button label, hidden when empty (featuring artists only)
- **Col 3 (Info):** Fixed 24px button, no expand
- **Col 4 (Duration):** Fixed width (`width-chars=5`), `xalign=0`, no expand

**Layout implementation:**
- Uses `GtkGrid` with `column-spacing=6`
- All expanding columns have `hexpand=true`, `halign=fill`
- Fixed columns (track#, info, duration) have `hexpand=false`
- Grid automatically aligns columns across all rows in the list

### `disc_header.ui`

Separator for multi-disc albums. Inserted before first track of each disc.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ DISC 2                                                                  │
└─────────────────────────────────────────────────────────────────────────┘
```

Uppercase, muted text, subtle top border.

### `album_card.ui`

Self-contained album card for artist detail view. Shows album header with large artwork, metadata (including artist names), and a preview track list. `ui_create_album_detail_card()` handles automatic disc header insertion.

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ ┌──────────┐  Album Title                                                            │
│ │  250px   │  Artist A, Artist B  (← Primary artists shown here)                    │
│ │   art    │  2024 · 12 tracks · 45:32                                               │
│ └──────────┘                                                                         │
│  1. Track One                                                                   3:02  │
│  2. Track Two                         feat. Artist C…                           4:15  │
│  3. Track Three                                                                 3:45  │
│ ...see all 12 tracks                                                                 │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

- **Album art:** 250×250px
- **Title:** Bold (natural case, no forced uppercase)
- **Artist:** All primary artists (comma-separated). Uses `artist_display` from `library_album_info_t`. **Shown in header only, not repeated in track list.**
- **Stats:** Year, track count, and total duration separated by `·`
- **Track list:** Uses `album_detail_track_item.ui` without primary artist names (only featuring artists shown inline). No `[ℹ]` button in card context.
- **See all:** Shows when more than 5 preview tracks

## CSS Classes

| Class | Element |
|-------|---------|
| `.library-row` | Base row styling |
| `.library-artist-row` | Artist row (2-row grid layout) |
| `.library-album-row` | Album row (2-row grid layout) |
| `.library-track-row` | Track row (2-row grid layout) |
| `.library-album-art` | 48×48 thumbnail |
| `.library-row-title` | Primary text (20px bold, top row) |
| `.library-row-subtitle` | Secondary text (12px, dimmed) |
| `.library-row-duration` | Duration (monospace) |
| `.library-track-row-bottom` | Bottom row container (12px, gray) |
| `.track-title-column` | Track title column (60% width, fade gradient) |
| `.album-column` | Album button column (40% width) |
| `.album-title-column` | Album title column (60% width, fade gradient) |
| `.album-metadata-column` | Album metadata column (40% width) - track count, year |
| `.artist-name-column` | Artist name column (60% width, fade gradient) |
| `.artist-metadata-column` | Artist metadata bottom row (60% width) - album count, track count |
| `.album-art-strip` | Album art strip container (40% width, right side, rowspan=2) |
| `.artists-column` | Primary artists column (60% width, bottom row) |
| `.metadata-column` | Year + Duration + Featuring column (40% width, bottom row) |
| `.album-detail-header` | Album detail header |
| `.album-detail-disc` | Disc separator |
| `.artist-btn` | Artist button, cyan `#00d4ff` (no frame) |
| `.artist-btn-secondary` | Featuring artist button (`feat.` prefix) |
| `.album-btn` | Album button, amber `#e5a640` (no frame) |
| `.selected` | Blue border on selected row |

## Loading States

All list views support:

| State | Display |
|-------|---------|
| Loading | Skeleton rows with pulse animation |
| Empty | "No items" message |
| Error | Error message with retry option |

```c
void ui_list_view_set_loading(GtkWidget *list, gboolean loading);
void ui_list_view_set_empty(GtkWidget *list, const char *message);
void ui_list_view_set_error(GtkWidget *list, const char *message, GCallback retry_cb);
```
