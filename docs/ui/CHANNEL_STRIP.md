# Channel Strip UI Layout

```
┌──────────────────────────────────────────────────────────────────────┐
│       ┌─────────────────────────────────────────────────┐ [🔁][▶▶]   │
│  [1]  │  Album Name                            -3:42:00 │ [P][Q]     │
│       │  Song Title (marquee)                           │ ▁▂▃▅▆▇▅▃   │
│       │  Artist                           3/12          │ ▃▅▆▇▅▃▂▁   │
│       │  Next: Next Track Title                         │            │
│       └─────────────────────────────────────────────────┘            │
│       [◀◀][-15][-5][▶][■][+5][+15][▶▶]  [═══════●═══════════] 1.0x   │
│                                                               [KEY]  │
│       [═══════════════════════●═════════════════════════════════]    │
│       0:00                                                    4:11   │
└──────────────────────────────────────────────────────────────────────┘
```

### Album Context

Each channel maintains:

| Property            | Description                                     |
| ------------------- | ----------------------------------------------- |
| Album playlist      | Ordered list of tracks from the album           |
| Current track index | Position within the album (1-based for display) |
| Track count         | Total tracks in the album                       |

### Auto-Advance Behavior

When the current track ends:

| Repeat State | Autoplay State | Behavior                                           |
| ------------ | -------------- | -------------------------------------------------- |
| Repeat OFF   | Autoplay ON    | Advance to next track in album, continue playing   |
| Repeat OFF   | Autoplay OFF   | Advance to next track, pause playback              |
| Repeat OFF   | Either         | Stop after last track (no next track available)    |
| Repeat ON    | Either         | Loop current track indefinitely (autoplay ignored) |

**Autoplay Default:** Enabled by default. Controls whether playback continues when advancing to the next track.

**Behavior Details:**

- The next track in the album always loads, regardless of autoplay state
- If autoplay is ON: channel remains in playing state, next track starts immediately
- If autoplay is OFF: channel pauses after loading next track (toggles play/pause state)
- At the last track in an album: playback stops regardless of autoplay state

Auto-advance is disabled in QUEUED and ON_AIR modes—playback continues but the channel remains on the current track to prevent unexpected content changes during live broadcast.

## Display Panel

The display panel shows a four-line hierarchy optimized for broadcast operation:

```
┌─────────────────────────────────────────────┐
│  Album Name                        -3:42:00 │
│  Song Title (marquee)                       │
│  Artist                           3/12      │
│  Next: Next Track Title                     │
└─────────────────────────────────────────────┘
```

### Information Hierarchy

1. **Album Name** (top-left) — Clickable, opens album in Library detail view.
1. **Time Remaining** (top-right) — Most critical information for broadcast cueing. Large, prominent display.
1. **Song Title** — Current track with marquee scrolling for long titles.
1. **Artist & Track Position** — Artist name (clickable, opens artist view) on left, track position (e.g., "3/12") on right.
1. **Next Track** — Shows upcoming track title

### Interactive Elements

The display panel contains several interactive text elements. Rather than using chevrons or other indicators, interactivity is communicated through hover feedback—elements brighten and slightly increase in size when hovered, with the cursor changing to a pointer.

| Element        | Click Action           | Hover Effect            |
| -------------- | ---------------------- | ----------------------- |
| Album Name     | Open album in Library  | Brighten + slight scale |
| Artist         | Open artist in Library | Brighten + slight scale |
| Next Track     | (non-interactive)      | —                       |
| Track Position | (non-interactive)      | —                       |
| Time Remaining | (non-interactive)      | —                       |
| Song Title     | (non-interactive)      | —                       |

### "Next Up" Display States

| Condition              | Display               | Interactive         |
| ---------------------- | --------------------- | ------------------- |
| Has next track         | `Next: {Track Title}` | Yes (click to skip) |
| Last track, repeat off | `Next: —`             | No                  |
| Repeat on              | `Repeating` (dimmed)  | No                  |
| Single                 | Hidden                | —                   |

### Click Behaviors

**Album Name:**

1. Emits `album-clicked` signal with album_id
1. Window handler opens Library panel (if not visible)
1. Library navigates to album detail view
1. Playback continues uninterrupted

**Artist Name:**

1. Emits `artist-clicked` signal with artist_id
1. Window handler opens Library panel (if not visible)
1. Library filters/navigates to artist view
1. Playback continues uninterrupted

## Top-Right Button Group

The top-right corner of each channel strip contains four transport buttons arranged in a 2×2 grid:

```
[🔁][▶▶]
[P][Q]
```

| Button   | Label/Icon | Type          | Description                                     | Default State |
| -------- | ---------- | ------------- | ----------------------------------------------- | ------------- |
| Repeat   | 🔁         | Toggle        | Loops current track when enabled                | OFF           |
| Autoplay | ▶▶         | Toggle        | Continues playback when advancing to next track | ON            |
| Preview  | P          | Toggle        | Activates PFL (pre-fade listen) mode            | OFF           |
| Queue    | Q          | Action/Double | Single-click queues; double-click exits ON_AIR  | -             |

### Repeat Button

When enabled, the current track loops indefinitely. Auto-advance to the next track is disabled when repeat is active.

### Autoplay Button

Controls whether playback continues when the track ends and advances to the next track in the album:

- **ON (default)**: Next track loads and starts playing automatically
- **OFF**: Next track loads but playback pauses (toggles play/pause state)
- **Ignored when**: Repeat is ON, or on last track of album, or in QUEUED/ON_AIR modes

Visual state follows standard toggle button styling (active/inactive appearance).

### Preview Button

Activates pre-fade listen mode. Integrates with the mixing console for audio routing.

### Queue Button

- **Single-click**: Enter QUEUED mode (ready for on-air)
- **Double-click** (in QUEUED or ON_AIR): Return to IDLE mode

## Per-Channel Transport Controls

Each channel strip includes its own track navigation, skip, and shuttle controls.

### Track Navigation

Track navigation buttons allow moving between tracks within the loaded album.

| Control  | Description                   |
| -------- | ----------------------------- |
| Previous | Go to previous track in album |
| Next     | Go to next track in album     |
| -15      | Skip backward 15 seconds      |
| -5       | Skip backward 5 seconds       |
| +5       | Skip forward 5 seconds        |
| +15      | Skip forward 15 seconds       |

### Shuttle Slider

Variable-speed playback with three modes. The shuttle slider, speed readout, and mode toggle are arranged horizontally with the slider taking available width and the readout + toggle stacked on the right.

| Input             | Action            |
| ----------------- | ----------------- |
| Left click + drag | Set shuttle speed |
| Right click       | Reset to 1.0x     |

### Shuttle Modes

The mode toggle button cycles through three modes:

| Mode    | Button Label | Speed Range | Behavior                          |
| ------- | ------------ | ----------- | --------------------------------- |
| OFF     | `OFF`        | Fixed 1.0x  | Shuttle disabled, slider inactive |
| KEYLOCK | `KEY`        | 0.5x – 4.0x | Pitch-preserved DJ-Style          |
| PITCHED | `PITCH`      | 0.5x – 1.5x | Vinyl-style pitch shift           |

The speed readout displays the current multiplier. Two decimal places are shown when speed is between 0.5x and 1.5x for fine control visibility (e.g., "0.95x", "1.02x", "1.48x"). Outside this range, one decimal place is used (e.g., "2.0x", "3.5x").

### Control Grouping

Transport controls are arranged with track navigation framing the time-seek controls:

```
[◀◀][-15][-5][▶][■][+5][+15][▶▶]  [═══════●═══════════] 1.0x
  │   └─Time Seek + Play─┘    │   └─ Shuttle Slider ──┘ [KEY]
  └─── Track Navigation ──────┘                        └─Mode─┘
```

- **Previous Track (◀◀)**: Leftmost position, intuitive "go back" placement
- **Time seek group**: Linked button style in center, seconds-based seeking with play/stop
- **Next Track (▶▶)**: Rightmost position (after +15), intuitive "go forward" placement
- **Shuttle group**: Slider expands to fill available width; speed readout and mode toggle stacked vertically on far right

### Slider Hover Animation

Both the shuttle slider and seek bar feature an animated slider thumb that grows when hovered. The thumb smoothly scales up on hover and returns to normal size when the cursor leaves.

## State Model

Channel state is layered into three orthogonal concerns:

### Device State

Hardware availability of the configured output device.

| State          | Border              | Description                     |
| -------------- | ------------------- | ------------------------------- |
| `VALID`        | -                   | Device configured and available |
| `UNCONFIGURED` | Light red `#ff6666` | No output device assigned       |
| `INVALID`      | Dark red `#993333`  | Device configured but missing   |

### Operational Mode

Broadcast automation state. Requires `DEVICE_STATE_VALID`.

| Mode      | Stying                        | Animation | Description                                                                                                                  |
| --------- | ----------------------------- | --------- | ---------------------------------------------------------------------------------------------------------------------------- |
| `IDLE`    | Gray Border `#555555`         | None      | Normal operation                                                                                                             |
| `PREVIEW` | Orange Border `#ff9500`       | Glow      | Preview Active (Integrates with Mixing Console for Toggling)                                                                 |
| `QUEUED`  | Green Border `#00cc66`        | Pulse     | Ready for on-air (pressing play in the UI or the mixing console will switch track to playing and update state to ON_AIR)     |
| `ON_AIR`  | Green Border `#00cc66`        | Solid     | Disables accidentially loading in a new track, seeking, play-pause-stop in the UI. Can double-click the queue button to exit |
| `FOCUSED` | Cyan Channel-Number `#00d4ff` | None      | Focused channel. Left clicked albums/tracks will be sent to this channel                                                     |

Only one channel can be focused at a time. Focus is mutually exclusive with QUEUED/ON_AIR modes:

- Entering QUEUED mode **clears focus** from that channel
- Focus can only be set on IDLE or PREVIEW channels
- Channels can be have FOCUSED and PREVIEW at the same time

### Track Loading Behavior

**Single-click** selects/highlights rows for keyboard navigation. **Double-click** opens detail views. Neither action queues tracks.

**Right-click** queues to the focused channel:

| Target | Action                                             |
| ------ | -------------------------------------------------- |
| Track  | Queue that specific track with album context       |
| Album  | Queue track 1 of the album with full album context |

| Condition                            | Result                                                                  |
| ------------------------------------ | ----------------------------------------------------------------------- |
| Focused channel exists and is active | Load track to focused channel with album context                        |
| No focused channel                   | Show toast: "Focus a channel first (click channel number or press 1-4)" |
| Focused channel is inactive          | Show toast with reason (unconfigured, queued, or on-air)                |

## Visual Precedence

When multiple states apply, the highest priority wins:

```
INVALID > UNCONFIGURED > ON_AIR > QUEUED > PREVIEW > FOCUSED > IDLE
```

## Active Channel

A channel is "active" (can receive songs) when:

```c
device_state == DEVICE_STATE_VALID && mode != QUEUED && mode != ON_AIR
```

Attempting to load a track to a non-active channel shows a toast notification.

## Mode Transitions

```
                    ┌─────────────────────────────────────┐
                    │                                     │
                    ▼                                     │
┌──────┐  queue   ┌────────┐  play    ┌────────┐          │
│ IDLE │─────────►│ QUEUED │─────────►│ ON_AIR │          │
└──────┘          └────────┘          └────────┘          │
    │                 │                    │              │
    │ preview         │ double-click       │ double-click │
    ▼                 │ queue              │ queue        │
┌─────────┐           │                    │              │
│ PREVIEW │           └────────────────────┴──────────────┘
└─────────┘                        │
    │                              │
    │ preview off                  ▼
    └─────────────────────────► IDLE (keeps playing)
```

### QUEUED Entry

- Track cues to position 0:00
- Focus is cleared if this channel was focused
- Preview mode is exited

### ON_AIR Exit

- Channel continues playback (does not stop)

## Control Sensitivity

| Mode         | Play | Stop | Repeat | Autoplay | Seek | Skip | Shuttle | Prev/Next | Preview | Queue | Load |
| ------------ | ---- | ---- | ------ | -------- | ---- | ---- | ------- | --------- | ------- | ----- | ---- |
| UNCONFIGURED | -    | -    | -      | -        | -    | -    | -       | -         | -       | -     | -    |
| INVALID      | -    | -    | -      | -        | -    | -    | -       | -         | -       | -     | -    |
| IDLE         | ✓    | ✓    | ✓      | ✓        | ✓    | ✓    | ✓       | ✓         | ✓       | ✓     | ✓    |
| PREVIEW      | ✓    | ✓    | ✓      | ✓        | ✓    | ✓    | ✓       | ✓         | ✓       | ✓     | ✓    |
| QUEUED       | ✓\*  | -    | ✓      | ✓        | -    | -    | -       | -         | -       | ✓     | -    |
| ON_AIR       | -    | -    | ✓      | ✓        | -    | -    | -       | -         | -       | ✓     | -    |

\*Play in QUEUED transitions to ON_AIR

### Track Navigation Sensitivity

Additional sensitivity rules for Previous/Next track buttons:

| Condition          | Previous | Next     |
| ------------------ | -------- | -------- |
| On first track     | Disabled | Enabled  |
| On last track      | Enabled  | Disabled |
| Single track album | Disabled | Disabled |
| No album loaded    | Disabled | Disabled |

## CSS Classes

### Channel Strip States

| Class                         | State                                             |
| ----------------------------- | ------------------------------------------------- |
| `.channel-strip-focused`      | Cyan channel number with glow, target for loading |
| `.channel-strip-preview`      | Orange border, PFL active                         |
| `.channel-strip-queued`       | Pulsing green, ready for air                      |
| `.channel-strip-on-air`       | Solid green, live                                 |
| `.channel-strip-unconfigured` | Light red, no device                              |
| `.channel-strip-invalid`      | Dark red, device missing                          |
| `.time-warning`               | Red pulse, ≤30s remaining                         |

### Transport Controls

| Class                    | Description                               |
| ------------------------ | ----------------------------------------- |
| `.repeat-button`         | Repeat toggle button                      |
| `.autoplay-button`       | Autoplay toggle button                    |
| `.btn-xs`                | Compact button style (skip, track nav)    |
| `.channel-shuttle-scale` | Shuttle slider with gradient trough       |
| `.channel-shuttle-label` | Shuttle speed readout (e.g., "1.0x")      |
| `.shuttle-mode-btn`      | Shuttle mode toggle button                |
| `.shuttle-mode-off`      | Mode button in OFF state                  |
| `.shuttle-mode-keylock`  | Mode button in KEYLOCK state              |
| `.shuttle-mode-pitched`  | Mode button in PITCHED state              |
| `.shuttle-info-box`      | Container for speed readout + mode toggle |

### Interactive Display Elements

All interactive display elements share common hover behavior: brighten + slight scale increase (transform: scale(1.02)), with 150ms ease-out transition. Cursor changes to pointer on hover.

| Class                  | Element                         | Hover Behavior                       |
| ---------------------- | ------------------------------- | ------------------------------------ |
| `.album-link`          | Album name                      | Brighten to 100% opacity, scale 1.02 |
| `.artist-link`         | Artist name                     | Brighten to 100% opacity, scale 1.02 |
| `.next-track`          | "Next: ..." label               | Brighten to 100% opacity, scale 1.02 |
| `.next-track-inactive` | Next label when non-interactive | No hover effect, reduced opacity     |

Base state for interactive elements uses ~80% opacity; hover brings to full brightness. The subtle scale creates a "lift" effect without being distracting.

### Position Indicators

| Class             | Description                                              |
| ----------------- | -------------------------------------------------------- |
| `.track-position` | Track position indicator (e.g., "3/12"), non-interactive |

### Sliders

| Class       | Description                         |
| ----------- | ----------------------------------- |
| `.seek-bar` | Seek bar with hover thumb animation |

## API

### Channel Strip

```c
// Device state
void ui_channel_strip_set_device_state(UiChannelStrip *s, DeviceState state);
DeviceState ui_channel_strip_get_device_state(UiChannelStrip *s);

// Operational mode
void ui_channel_strip_set_mode(UiChannelStrip *s, ChannelMode mode);
ChannelMode ui_channel_strip_get_mode(UiChannelStrip *s);

// Focus
void ui_channel_strip_set_focused(UiChannelStrip *s, gboolean focused);
gboolean ui_channel_strip_get_focused(UiChannelStrip *s);

// Active check (device valid AND not queued/on-air)
gboolean ui_channel_strip_is_active(UiChannelStrip *s);
```

### Album Context

```c
// Set album context when loading a track
// tracks: array of track info (title, path, etc.)
// track_count: number of tracks in album
// current_index: 0-based index of currently playing track
void ui_channel_strip_set_album_context(UiChannelStrip *s,
                                         int64_t album_id,
                                         const char *album_name,
                                         const db_track_t *tracks,
                                         int track_count,
                                         int current_index);

// Clear album context (single track mode)
void ui_channel_strip_clear_album_context(UiChannelStrip *s);

// Get current album state
int64_t ui_channel_strip_get_album_id(UiChannelStrip *s);
int ui_channel_strip_get_track_index(UiChannelStrip *s);
int ui_channel_strip_get_track_count(UiChannelStrip *s);
gboolean ui_channel_strip_has_album_context(UiChannelStrip *s);

// Track navigation (returns TRUE if navigation occurred)
gboolean ui_channel_strip_previous_track(UiChannelStrip *s);
gboolean ui_channel_strip_next_track(UiChannelStrip *s);

// Check navigation availability
gboolean ui_channel_strip_can_go_previous(UiChannelStrip *s);
gboolean ui_channel_strip_can_go_next(UiChannelStrip *s);

// Autoplay control
void ui_channel_strip_set_autoplay(UiChannelStrip *s, gboolean autoplay);
gboolean ui_channel_strip_get_autoplay(UiChannelStrip *s);
```

### Window Focus Management

```c
void ui_window_set_focused_channel(UiWindow *w, int channel);
void ui_window_clear_focus(UiWindow *w);
void ui_window_show_toast(UiWindow *w, const char *message);

// Library navigation (opens library and navigates to item)
void ui_window_show_album(UiWindow *w, int64_t album_id);
void ui_window_show_artist(UiWindow *w, const char *artist);
```

## Signals

### Existing Signals

| Signal         | Parameters                     | Description              |
| -------------- | ------------------------------ | ------------------------ |
| `clicked`      | `int channel_id`               | Channel badge clicked    |
| `mode-changed` | `int channel_id, int new_mode` | Operational mode changed |

### New Signals

| Signal           | Parameters                          | Description                             |
| ---------------- | ----------------------------------- | --------------------------------------- |
| `album-clicked`  | `int channel_id, int64_t album_id`  | Album name clicked, open in library     |
| `artist-clicked` | `int channel_id, int64_t artist_id` | Artist name clicked, open in library    |
| `track-changed`  | `int channel_id, int new_index`     | Track changed (via nav or auto-advance) |
| `track-ended`    | `int channel_id`                    | Current track reached end               |

## Toast Messages

| Condition    | Message                                  |
| ------------ | ---------------------------------------- |
| UNCONFIGURED | "Channel has no Audio Output Configured" |
| INVALID      | "Channel has no Audio Output Configured" |
| QUEUED       | "Channel is Queued"                      |
| ON_AIR       | "Channel is On-Air"                      |

## Implementation Notes

### Track Loading with Album Context

When a track is selected from the library:

1. Query `db_get_tracks_by_album()` to get all tracks for the album
1. Call `ui_channel_strip_set_album_context()` with full track list
1. Call `ui_channel_strip_load_track()` for the selected track
1. Display updates automatically to show album context and "Next" track

### Auto-Advance Implementation

In the channel update tick:

1. Check if track has ended (`position >= length - threshold`)
1. If Repeat is ON: seek to 0, continue playing (autoplay is ignored)
1. If Repeat is OFF and has next track:
   1. Remember current play state
   1. Call `ui_channel_strip_next_track()` to load next track
   1. If autoplay is OFF: toggle play/pause to pause the newly loaded track
   1. If autoplay is ON: track continues playing automatically
1. If Repeat is OFF and at last track: stop playback, emit `track-ended`

### Library Integration

The `album-clicked` signal should be handled by the main window:

```c
static void on_album_clicked(UiChannelStrip *strip, int channel_id,
                              int64_t album_id, gpointer data) {
    UiWindow *window = UI_WINDOW(data);

    // Show library panel if hidden
    ui_window_show_library(window);

    // Navigate to album detail
    ui_library_show_album(window->library, album_id);
}
```

## Future: Axia GPIO

External GPIO signals will trigger mode transitions:

- GPIO play signal: QUEUED → ON_AIR
- GPIO stop signal: QUEUED/ON_AIR → IDLE
