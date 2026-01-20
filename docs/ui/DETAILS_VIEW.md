# Details View

Dynamic, context-aware view that displays either Album or Artist detail based on current navigation state. All data queries go through [LibraryCache](../architecture/LIBRARY_CACHE.md) for caching and prefetch coordination.

### 1. Empty State (Default)

When no artist or album has been selected, or when returning from all selections.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                                                                         │
│                                                                         │
│                        Select an artist or album                        │
│                           to view details here                          │
│                                                                         │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2. Album Detail State

When an album is selected (from Albums view, Search, or Artist detail).

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [<] Back to Albums                                                      │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌──────────┐  ABBEY ROAD                                                │
│ │  250px   │  The Beatles · 1969            (clickable artist)          │
│ │   art    │  17 songs · 47:23                                          │
│ └──────────┘                                                            │
│                                                                         │
│ Featuring: George Harrison, Paul McCartney, John Lennon                │
│            ↑ clickable artist links                                     │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Come Together                                                 4:19  │
│  2. Something                                                     3:02  │
│  3. Maxwell's Silver Hammer                                       3:27  │
│  4. Oh! Darling                                                   3:27  │
│  5. Octopus's Garden                                              2:51  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Single-Disc Albums:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│  1. Track Title                                                   3:02  │
│  ↑  ↑ title (hexpand)                                             ↑ dur │
│  num                                                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

**Multi-Disc Albums:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│ DISC 1                                                                  │
├─────────────────────────────────────────────────────────────────────────┤
│   1. Come Together                                                4:19  │
│   2. Something                                                    3:02  │
│   3. Maxwell's Silver Hammer                                      3:27  │
├─────────────────────────────────────────────────────────────────────────┤
│ DISC 2                                                                  │
├─────────────────────────────────────────────────────────────────────────┤
│   1. Here Comes the Sun                                           3:06  │
│   2. Because                                                      2:45  │
│   3. You Never Give Me Your Money                                 4:02  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Disc headers**: Only shown when album has multiple discs (12px, uppercase, muted)
- **Track display**:
  - Single-disc: "1. Title"
  - Multi-disc: "1. Title" (numbered within disc)
- **Title**: 14px, primary color, ellipsis overflow
- **Duration**: Monospace, right-aligned

### 3. Artist Detail State

When an artist is selected (from Artists view or Search).

```
┌─────────────────────────────────────────────────────────────────────────┐
│ [<] Back to Artists                                                     │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│ THE BEATLES                                                             │
│ 4 albums · 89 tracks · 5h 23m                                           │
│                                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│ ALBUMS                                                                  │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌─────────┐  ABBEY ROAD                                            [>] │
│ │         │  1969                                                      │
│ │ 250×250 │  17 tracks · 47:23                                         │
│ │  album  │  Rock · Apple Records                                      │
│ │   art   │                                                            │
│ └─────────┘                                                            │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Come Together                                                 4:19  │
│  2. Something                                                     3:02  │
│  3. Maxwell's Silver Hammer                                       3:27  │
│  4. Oh! Darling                                                   3:27  │
│  5. Octopus's Garden                                              2:51  │
│     ...see all 17 tracks                                               │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌─────────┐  LET IT BE                                             [>] │
│ │         │  1970                                                      │
│ │ 250×250 │  12 tracks · 35:09                                         │
│ │  album  │  Rock · Apple Records                                      │
│ │   art   │                                                            │
│ └─────────┘                                                            │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Two of Us                                                     3:36  │
│  2. Dig a Pony                                                    3:54  │
│  3. Across the Universe                                           3:48  │
│     ...see all 12 tracks                                               │
├─────────────────────────────────────────────────────────────────────────┤
│ APPEARS ON                                        [ Albums | Tracks ]   │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌─────────┐  CONCERT FOR BANGLADESH                               [>] │
│ │         │  Various Artists · 1971                                    │
│ │ 250×250 │  23 tracks · 1h 43m                                        │
│ │  album  │  Rock · Apple Records                                      │
│ │   art   │                                                            │
│ └─────────┘                                                            │
├─────────────────────────────────────────────────────────────────────────┤
│ ┌─────────┐  ANTHOLOGY VOL. 1                                      [>] │
│ │         │  Various Artists · 1995                                    │
│ │ 250×250 │  60 tracks · 2h 34m                                        │
│ │  album  │  Rock · Capitol Records                                    │
│ │   art   │                                                            │
│ └─────────┘                                                            │
└─────────────────────────────────────────────────────────────────────────┘

```

**Appears On Section:**

This section shows albums or tracks where the artist is featured but is not the primary album artist.

**Albums View (Default):**
- Same visual layout as artist albums section
- Shows albums where artist appears on at least one track
- Album artist name shown (e.g., "Various Artists · 1971")
- Click album to view full album detail
- Sorted by release year (newest first)

**Tracks View:**

Uses the reusable song list component (`song_list_view.ui`).

```
┌─────────────────────────────────────────────────────────────────────────┐
│ APPEARS ON                                        [ Albums | Tracks ]   │
├─────────────────────────────────────────────────────────────────────────┤
│        TITLE              ALBUM           ARTIST              YEAR  DUR │
├─────────────────────────────────────────────────────────────────────────┤
│ [art] While My Guitar...  The White Album The Beatles         1968 4:45 │
│ [art] Something in the... James Taylor    James Taylor        1968 3:55 │
│ [art] Badge               Goodbye         Cream               1969 2:44 │
│                                           feat. Eric Clapton            │
└─────────────────────────────────────────────────────────────────────────┘
```

**Tracks View:**
- Uses same table layout as Search song results
- **Columns**: `[art 48px][title flex][album flex][artist flex][year 4ch][duration 5ch]`
- Artist column shows album artist with featured artists below if present
- Album name and album artist are clickable (navigate to respective details)
- Sorted by release year (newest first), then album, then track number

**Toggle Behavior:**
- Two-state toggle button in section header
- "Albums" selected by default
- Click to switch between Albums and Tracks views
- State persists during session (not across app restarts)

**Empty State:**
- If no appearances found, section is hidden entirely
- Section only appears when artist has featured appearances

### Entering Details View

The Details view automatically updates when:

1. **User clicks nav bar "Details" button**

   - If detail context exists: Shows current detail
   - If no context: Shows empty state

1. **User clicks artist/album in any view**

   - Updates Details view content
   - Switches to Details view in nav bar
   - Stores navigation context

1. **User clicks album from artist detail**

   - Updates Details view to album detail
   - Remains in Details view

1. **User clicks artist link from album detail**

   - Updates Details view to artist detail
   - Remains in Details view

### Back Button Behavior

The "Back" button in Details view is **context-aware**:

| Current State | Came From     | Back Button Action                              |
| ------------- | ------------- | ----------------------------------------------- |
| Album detail  | Albums view   | Return to Albums view, restore scroll position  |
| Album detail  | Artist detail | Return to Artist detail (show artist's albums)  |
| Album detail  | Search        | Return to Search view, restore results          |
| Artist detail | Artists view  | Return to Artists view, restore scroll position |
| Artist detail | Search        | Return to Search view, restore results          |
| Empty         | Any view      | No back button shown                            |

### Navigation Stack

The Details view maintains a navigation stack internally:

```
[Albums View] → [Album Detail]
[Artists View] → [Artist Detail] → [Album Detail]
[Search View] → [Artist Detail]
[Search View] → [Album Detail]
```

Pressing `Escape` or clicking Back pops the stack and returns to the previous view.

### Switching Between Details

When in Details view, clicking an artist/album link:

1. Updates the current detail content
1. Pushes previous detail to navigation stack
1. Remains in Details view (doesn't switch nav bar)

**Example Flow:**

1. User in Artists view
2. Double-click "The Beatles" → Switches to Details view, shows Artist detail
3. Double-click "Abbey Road" album → Stays in Details view, shows Album detail
4. Click "The Beatles" artist link → Stays in Details view, back to Artist detail
5. Press Back → Returns to Artists view

## Mouse Gestures & Keyboard Shortcuts

**See [KEYBINDS.md](KEYBINDS.md) for complete keyboard shortcut reference.**

### Album Detail (Track List)

| Gesture/Key       | Action                                  |
| ----------------- | --------------------------------------- |
| Single-click      | Select/highlight track                  |
| Double-click      | (No action - use right-click to queue)  |
| Right-click track | Queue track to focused channel          |
| `↑`/`↓`           | Move selection (GTK default)            |
| `1-4`             | Load selected track to channel 1-4      |

### Artist Detail (Albums List)

| Gesture/Key       | Action                                  |
| ----------------- | --------------------------------------- |
| Single-click      | Select/highlight album                  |
| Double-click      | Open album detail view                  |
| Right-click album | Queue track 1 to focused channel        |
| `↑`/`↓`           | Move selection (GTK default)            |

### Interactive Links

Artist name and album name links in headers use single-click to navigate (they are styled as links, not list items).

## Data Loading via LibraryCache

All detail view data is fetched through LibraryCache, never directly from the database.

### Album Detail

```c
void detail_view_show_album(DetailView* view, int64_t album_id) {
    // Get album info (cached)
    const library_album_info_t* album = library_cache_get_album(view->library, album_id);
    if (!album) return;

    // Get track list (cached, ordered by disc/track)
    const GPtrArray* tracks = library_cache_get_tracks_by_album(view->library, album_id);

    // Get artist info for header link
    const library_artist_info_t* artist = library_cache_get_artist(view->library, album->artist_id);

    // Populate UI
    populate_album_header(view, album, artist);
    populate_track_list(view, tracks, album->disc_count > 1);
}
```

### Artist Detail

```c
void detail_view_show_artist(DetailView* view, int64_t artist_id) {
    // Get artist info (cached)
    const library_artist_info_t* artist = library_cache_get_artist(view->library, artist_id);
    if (!artist) return;

    // Get artist's albums (cached)
    const GPtrArray* albums = library_cache_get_albums_by_artist(view->library, artist_id);

    // Get "Appears On" albums (cached)
    const GPtrArray* appearances = library_cache_get_artist_appearances(view->library, artist_id);

    // Populate UI
    populate_artist_header(view, artist);
    populate_albums_list(view, albums);

    if (appearances->len > 0) {
        populate_appearances_section(view, appearances);
    }
}
```

### Prefetch on Navigation

When navigating to album detail, prefetch full-size artwork:

```c
static void on_album_selected(DetailView* view, int64_t album_id) {
    // Prefetch full-size artwork (ArtworkManager → LibraryCache → posix_fadvise)
    artwork_manager_prefetch_fullsize(view->artwork, album_id);

    // Show detail view
    detail_view_show_album(view, album_id);
}
```

**Note:** Artist detail view uses thumbnails which are handled by ArtworkManager's atlas system (no kernel page cache prefetch needed).

### Track Loading from Detail

When right-clicking a track in album detail:

```c
static void on_track_right_click(GtkWidget* row, DetailView* view) {
    int64_t track_id = get_track_id_from_row(row);

    // Track info already cached from album load
    const library_track_info_t* track = library_cache_get_track(view->library, track_id);
    if (!track) return;

    // Load to focused player
    int player_id = get_focused_player();
    audio_cache_load(view->audio, track_id, NULL, NULL);
    audio_pipeline_set_player_track(view->pipeline, player_id, track_id);
    ui_channel_strip_set_track(get_channel_strip(player_id), track);
}
```
