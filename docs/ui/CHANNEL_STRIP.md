# Channel Strip

4-channel transport UI for broadcast operation. Template: `channel_strip.ui`.

## Layout

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
└──────────────────────────────────────────────────────────────────────┘
```

## Display Panel

Four-line hierarchy optimized for broadcast:

1. **Album Name** (top-left) — Clickable, opens album in library
2. **Time Remaining** (top-right) — Large, prominent for cueing
3. **Song Title** — Marquee scroll for long titles
4. **Artist & Track Position** — Artist (clickable) left, "3/12" right
5. **Next Track** — Shows upcoming track

**Interactive elements:** Album and Artist names brighten + scale 1.02 on hover, cursor changes to pointer.

| Element    | Click Action           |
| ---------- | ---------------------- |
| Album Name | Open album in library  |
| Artist     | Open artist in library |

**Next display states:**

| Condition        | Display              |
| ---------------- | -------------------- |
| Has next track   | `Next: {Title}`      |
| Last track       | `Next: —`            |
| Repeat on        | `Repeating` (dimmed) |

## Top-Right Buttons (2×2 Grid)

```
[🔁][▶▶]
[P][Q]
```

| Button   | Type   | Description                                      |
| -------- | ------ | ------------------------------------------------ |
| Repeat   | Toggle | Loop current track (disables auto-advance)       |
| Autoplay | Toggle | Continue playing on track advance (default: ON)  |
| Preview  | Toggle | PFL mode (integrates with mixing console)        |
| Queue    | Action | Single: enter QUEUED. Double: exit QUEUED/ON_AIR |

## Transport Controls

```
[◀◀][-15][-5][▶][■][+5][+15][▶▶]  [═══════●═══════════] 1.0x [KEY]
```

- **◀◀ / ▶▶**: Previous/Next track in album
- **-15/-5/+5/+15**: Time seek (seconds)
- **▶/■**: Play/Stop
- **Shuttle slider**: Variable speed playback

### Shuttle Modes

| Mode    | Label   | Range       | Behavior                  |
| ------- | ------- | ----------- | ------------------------- |
| OFF     | `OFF`   | Fixed 1.0x  | Slider disabled           |
| KEYLOCK | `KEY`   | 0.5x – 4.0x | Pitch-preserved           |
| PITCHED | `PITCH` | 0.5x – 1.5x | Vinyl-style pitch shift   |

Right-click shuttle slider resets to 1.0x.

## State Model

### Device State

| State        | Border          | Description               |
| ------------ | --------------- | ------------------------- |
| VALID        | —               | Device available          |
| UNCONFIGURED | `#ff6666` (red) | No output device assigned |
| INVALID      | `#993333` (red) | Device configured but missing |

### Operational Mode

Requires `DEVICE_STATE_VALID`.

| Mode    | Styling              | Description                            |
| ------- | -------------------- | -------------------------------------- |
| IDLE    | Gray `#555555`       | Normal operation                       |
| PREVIEW | Orange `#ff9500`     | PFL active                             |
| QUEUED  | Green `#00cc66` pulse| Ready for air                          |
| ON_AIR  | Green `#00cc66` solid| Live, controls locked                  |
| FOCUSED | Cyan number `#00d4ff`| Target for track loading               |

**Focus rules:**
- Only one channel focused at a time
- Entering QUEUED clears focus
- Focus only on IDLE or PREVIEW channels
- FOCUSED + PREVIEW can coexist

### Visual Precedence

```
INVALID > UNCONFIGURED > ON_AIR > QUEUED > PREVIEW > FOCUSED > IDLE
```

## Control Sensitivity

| Mode         | Play | Seek | Skip | Prev/Next | Shuttle | Load | Queue |
| ------------ | ---- | ---- | ---- | --------- | ------- | ---- | ----- |
| UNCONFIGURED | —    | —    | —    | —         | —       | —    | —     |
| INVALID      | —    | —    | —    | —         | —       | —    | —     |
| IDLE         | ✓    | ✓    | ✓    | ✓†        | ✓       | ✓    | ✓     |
| PREVIEW      | ✓    | ✓    | ✓    | ✓†        | ✓       | ✓    | ✓     |
| QUEUED       | ✓*   | —    | —    | —         | —       | —    | ✓     |
| ON_AIR       | —    | —    | —    | —         | —       | —    | ✓     |

*Play in QUEUED transitions to ON_AIR
†Only when adjacent track exists in library

## Track Navigation

When user clicks next/prev:
1. UI queries `library_cache_get_next/prev_track_id()`
2. UI updates display immediately (title, artist, album)
3. UI calls `audio_cache_load()` then `audio_pipeline_set_player_track()`
4. Audio catches up when decode completes

This gives instant visual feedback even if decode takes time.

**Player state preserved:** PLAYING stays PLAYING, STOPPED stays STOPPED.

**STOPPED only occurs from:**
- User presses stop
- Last track ends with repeat OFF

Auto-advance disabled in QUEUED/ON_AIR modes.

## Mode Transitions

```
         queue        play
IDLE ──────────► QUEUED ──────────► ON_AIR
  │                 │                  │
  │ preview         │ double-click Q   │ double-click Q
  v                 │                  │
PREVIEW             └──────────────────┴──► IDLE (keeps playing)
```

**QUEUED entry:** Cues to 0:00, clears focus, exits preview.

**ON_AIR exit:** Playback continues.

## API

```c
// Device state
void ui_channel_strip_set_device_state(UiChannelStrip *s, DeviceState state);

// Mode
void ui_channel_strip_set_mode(UiChannelStrip *s, ChannelMode mode);

// Focus
void ui_channel_strip_set_focused(UiChannelStrip *s, gboolean focused);
gboolean ui_channel_strip_is_active(UiChannelStrip *s);  // valid && not queued/on-air

// Album context
void ui_channel_strip_set_album_context(UiChannelStrip *s, int64_t album_id,
                                         const char *album_name,
                                         const db_track_t *tracks,
                                         int track_count, int current_index);
void ui_channel_strip_clear_album_context(UiChannelStrip *s);

// Track navigation
gboolean ui_channel_strip_previous_track(UiChannelStrip *s);
gboolean ui_channel_strip_next_track(UiChannelStrip *s);
gboolean ui_channel_strip_can_go_previous(UiChannelStrip *s);
gboolean ui_channel_strip_can_go_next(UiChannelStrip *s);

// Autoplay
void ui_channel_strip_set_autoplay(UiChannelStrip *s, gboolean autoplay);
```

## Signals

| Signal           | Parameters                          | Description                  |
| ---------------- | ----------------------------------- | ---------------------------- |
| `clicked`        | `int channel_id`                    | Channel badge clicked        |
| `mode-changed`   | `int channel_id, int new_mode`      | Mode changed                 |
| `album-clicked`  | `int channel_id, int64_t album_id`  | Album name clicked           |
| `artist-clicked` | `int channel_id, int64_t artist_id` | Artist name clicked          |
| `track-changed`  | `int channel_id, int64_t track_id`  | Track changed (auto-advance) |

## CSS Classes

| Class                         | State                    |
| ----------------------------- | ------------------------ |
| `.channel-strip-focused`      | Cyan number, load target |
| `.channel-strip-preview`      | Orange border            |
| `.channel-strip-queued`       | Pulsing green            |
| `.channel-strip-on-air`       | Solid green              |
| `.channel-strip-unconfigured` | Light red                |
| `.channel-strip-invalid`      | Dark red                 |
| `.time-warning`               | Red pulse (≤30s left)    |
