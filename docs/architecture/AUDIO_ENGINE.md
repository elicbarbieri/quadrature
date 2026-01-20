# Audio Engine

4-channel player with PipeWire output. All audio flows through the [Audio Cache](AUDIO_CACHE.md) - no streaming, no fallbacks. Uses [Library Cache](LIBRARY_CACHE.md) for next-track resolution independent of UI.

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                            audio_pipeline_t                                  │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────────┐ │
│  │                     CACHES (shared across all players)                 │ │
│  │                                                                        │ │
│  │  ┌─────────────────────────┐    ┌─────────────────────────────────┐    │ │
│  │  │      AUDIO CACHE        │    │        LIBRARY CACHE            │    │ │
│  │  │  Decoded PCM buffers    │<───│  Track metadata + album order   │    │ │
│  │  │  lock/unlock for        │    │  get_next_track_id()            │    │ │
│  │  │  instant playback       │    │  get_prev_track_id()            │    │ │
│  │  └─────────────────────────┘    └─────────────────────────────────┘    │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                    │                                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  Player 0   │  │  Player 1   │  │  Player 2   │  │  Player 3   │         │
│  │  current_id │  │  current_id │  │  current_id │  │  current_id │         │
│  │  next_id    │  │  next_id    │  │  next_id    │  │  next_id    │         │
│  │  buffer     │  │  buffer     │  │  buffer     │  │  buffer     │         │
│  │  scrubber   │  │  scrubber   │  │  scrubber   │  │  scrubber   │         │
│  │  pw_stream  │  │  pw_stream  │  │  pw_stream  │  │  pw_stream  │         │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘         │
└─────────┼────────────────┼────────────────┼────────────────┼────────────────┘
          v                v                v                v
     PipeWire          PipeWire         PipeWire         PipeWire
```

See [AUDIO_CACHE.md](AUDIO_CACHE.md) for decoded buffer caching and [LIBRARY_CACHE.md](LIBRARY_CACHE.md) for track metadata caching.

## Shuttle Modes

Shuttle mode determines how variable-speed playback is processed. Mode is set per-player.

| Mode      | Processing          | Speed Range | Pitch Behavior    |
| --------- | ------------------- | ----------- | ----------------- |
| `OFF`     | Bypass (memcpy)     | Locked 1.0x | Unchanged         |
| `KEYLOCK` | Rubberband R2       | 0.5x - 4.0x | Preserved         |
| `PITCHED` | Cubic interpolation | 0.5x - 2.0x | Shifts with speed |

**SHUTTLE_MODE_OFF is a complete bypass.** When OFF:

- Speed value is ignored (UI enforces 1.0f, scrubber ignores speed)
- Audio callback is a direct memcpy from buffer to output
- Zero DSP overhead - use this for normal playback

**At exactly 1.0x speed (any mode):** Bypass is used as an optimization.

## Player State Machine

```
       set_track()           is_ready()            play()
(none) ────────────> LOADING ─────────────> STOPPED ─────────> PLAYING
                        │                       ^                  │
                        │ buffer unavailable    │ stop()           │ toggle_play()
                        v                       │                  v
                 NO_AUDIO <─────────────────────┴──────────────  PAUSED
```

## Player

Waits on Audio Cache for audio samples. The audio engine doesn't handle reading songs from disk or decoding.

The audio player reads decoded samples from the Audio Cache and writes them to PipeWire output buffers. If a seek occurs, the player updates its read position within the cached buffer.

### Player State

```c
typedef struct audio_player {
    int64_t current_track_id;   // Currently loaded track
    int64_t next_track_id;      // Locked in cache for instant advance (0 if none/repeat)
    audio_buffer_t* buffer;     // Acquired buffer for reading
    bool repeat;                // Repeat current track
    // ... scrubber, pw_stream, etc.
} audio_player_t;
```

The engine resolves `next_track_id` via Library Cache - no UI involvement. When `set_player_track()` is called:

1. Unlocks old current and next tracks
2. Locks and loads new track
3. Queries `library_cache_get_next_track_id()` for next track
4. If not in repeat mode and next exists: locks and preloads it

## API

### Lifecycle

```c
quadrature_result_t audio_pipeline_create(uint32_t sample_rate, audio_pipeline_t** out);
void                audio_pipeline_destroy(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_start(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_stop(audio_pipeline_t* pipeline);
```

### Player Control

```c
// Set track for player (audio thread queries cache by track_id)
quadrature_result_t audio_pipeline_set_player_track(audio_pipeline_t* pipeline, int player_id, int64_t track_id);

// Check if buffer acquired and ready
bool audio_pipeline_player_is_ready(audio_pipeline_t* pipeline, int player_id);

// Transport
quadrature_result_t audio_pipeline_player_play(pipeline, player_id);
quadrature_result_t audio_pipeline_player_stop(pipeline, player_id);
quadrature_result_t audio_pipeline_player_toggle_play(pipeline, player_id);
quadrature_result_t audio_pipeline_player_seek(pipeline, player_id, position_samples);

// Skip to next/previous track (instant if preloaded)
quadrature_result_t audio_pipeline_player_next(pipeline, player_id);
quadrature_result_t audio_pipeline_player_prev(pipeline, player_id);

// Repeat mode (when enabled, next_track is unlocked)
quadrature_result_t audio_pipeline_player_set_repeat(pipeline, player_id, repeat);
```

### Speed Control

```c
// Set speed (-4.0 to +4.0). Requires buffer acquired.
// Ignored when shuttle mode is OFF.
quadrature_result_t audio_pipeline_player_set_speed(pipeline, player_id, speed);

// Set shuttle mode (determines processing path)
quadrature_result_t audio_pipeline_player_set_shuttle_mode(pipeline, player_id, mode);
```

### Device Routing

```c
// Route to specific PipeWire sink (NULL for default)
quadrature_result_t audio_pipeline_set_player_device(pipeline, player_id, device_name);
```

### Monitoring

```c
channel_state_t audio_pipeline_get_player_state(pipeline, player_id);
uint64_t        audio_pipeline_get_player_position(pipeline, player_id);
uint64_t        audio_pipeline_get_player_length(pipeline, player_id);
uint32_t        audio_pipeline_get_sample_rate(pipeline);

// Interpolated position for smooth UI (seqlock-based)
double audio_pipeline_get_player_position_smooth(pipeline, player_id, float* out_speed);

// Spectrum (24 bands, 0.0-1.0)
void audio_pipeline_get_player_spectrum(pipeline, player_id, float* bars, int num_bars);
```

## Audio Callback

The PipeWire callback reads directly from the acquired buffer through the scrubber:

```
buffer ──> scrubber ──> metering ──> spectrum ringbuf ──> PipeWire sink
```

**Forbidden in callback:** malloc, mutex, syscalls, file I/O, FFmpeg.

Budget: ~21ms at 48kHz/1024 frames. Typical: 0.1-2ms depending on shuttle mode.

## Scrubber Processing Paths

```c
// Zone selection (audio_scrub.c)
if (mode == SHUTTLE_MODE_OFF || speed ≈ 1.0) {
    // BYPASS: memcpy, zero CPU
    memcpy(output, buffer + pos, frames);
}
else if (mode == SHUTTLE_MODE_PITCHED) {
    // TURNTABLE: cubic Hermite interpolation
    // Pitch shifts with speed, quality degrades >2x
}
else { // SHUTTLE_MODE_KEYLOCK
    // RUBBERBAND: R2 engine, pitch preserved
    // Ring buffer handles variable output size
    // Ducking applied above 2x to reduce harshness
}
```

Crossfade (10ms) smooths transitions between zones.

## Position Interpolation

For smooth waveform display between audio callbacks:

```
Audio thread: writes {position, timestamp, speed, playing} via seqlock
UI thread:    reads snapshot, interpolates based on elapsed time
```

Provides sub-sample accuracy without polling overhead.

## Auto-Advance and Preloading

The engine handles track transitions autonomously using Library Cache for next-track resolution. This enables instant playback without UI round-trips.

### Instant Skip Design (skip-skip-skip)

**Goal:** Rapid skip operations must respond instantaneously, even when the user clicks skip multiple times in quick succession.

**Mechanism:** `get_next_track_id()` is called **immediately** after every track state change:

| Trigger              | Action                                        |
| -------------------- | --------------------------------------------- |
| Track loaded         | Resolve and preload next track                |
| Skip forward         | Play preloaded next, resolve new next         |
| Skip backward        | Play previous, resolve new next               |
| Repeat toggled OFF   | Resolve and preload next track                |
| Repeat toggled ON    | Unlock and clear next track (will loop)       |

**Result:** The next track is always preloaded before the user can skip again.

```
User action:    [skip] ─────> [skip] ─────> [skip] ─────> [skip]
                  │              │              │              │
                  v              v              v              v
Engine state:   play T2       play T3       play T4       play T5
                (T2 was       (T3 was       (T4 was       (T5 was
                preloaded)    preloaded)    preloaded)    preloaded)
                  │              │              │              │
Background:     preload T3    preload T4    preload T5    preload T6
                (async)       (async)       (async)       (async)
```

Each skip is **instant** because:
1. Buffer is already decoded and locked in AudioCache
2. No database query needed (LibraryCache has album order cached)
3. No UI round-trip required

### Flow: Track Set

```
UI THREAD                           AUDIO ENGINE                   LIBRARY CACHE
─────────                           ────────────                   ─────────────

audio_pipeline_set_player_track(player_id, track_id)
      │
      └──> audio_cache_unlock(old_track_id)
           audio_cache_unlock(old_next_id)
                 │
                 ├──> audio_cache_lock(track_id)
                 │    audio_cache_load(track_id)
                 │
                 ├──> next_id = library_cache_get_next_track_id(track_id)
                 │                                      │
                 │                                      └──> Query album order
                 │                                           Return next track
                 │
                 └──> if (next_id && !repeat):
                          audio_cache_lock(next_id)
                          audio_cache_load(next_id)
```

### Flow: Track Ends (Auto-Advance)

```
AUDIO ENGINE (background)                                          AUDIO CACHE
─────────────────────────                                          ───────────

on_track_position >= track_length:
      │
      ├──> if (repeat):
      │        seek(0)
      │        continue playing
      │        return
      │
      ├──> old_track = current_track_id
      │    current_track_id = next_track_id     // Already preloaded!
      │
      ├──> audio_cache_unlock(old_track)
      │
      ├──> new_next = library_cache_get_next_track_id(current_track_id)
      │
      └──> if (new_next):
               audio_cache_lock(new_next)
               audio_cache_load(new_next)
               next_track_id = new_next
           else:
               next_track_id = 0
      │
      └──> emit track_changed signal (for UI update)
```

### Flow: Skip to Next

```
UI THREAD                           AUDIO ENGINE
─────────                           ────────────

audio_pipeline_player_next(player_id)
      │
      └──> if (!next_track_id) return QUADRATURE_NOT_FOUND
           │
           └──> (same as auto-advance flow)
                // Instant playback - next was preloaded!
```

### Flow: Repeat Toggled

```
UI THREAD                           AUDIO ENGINE
─────────                           ────────────

audio_pipeline_player_set_repeat(player_id, enabled)
      │
      └──> player->repeat = enabled
           │
           └──> if (enabled && next_track_id):
                    audio_cache_unlock(next_track_id)
                    next_track_id = 0
                else if (!enabled):
                    next_id = library_cache_get_next_track_id(current)
                    if (next_id):
                        audio_cache_lock(next_id)
                        audio_cache_load(next_id)
                        next_track_id = next_id
```

### Callback: Track Changed

When a track changes (user skip or auto-advance), emit signal so UI can update:

```c
typedef void (*audio_track_changed_cb)(int player_id, int64_t track_id, void* user_data);

void audio_pipeline_set_track_changed_callback(
    audio_pipeline_t* pipeline,
    audio_track_changed_cb callback,
    void* user_data
);
```
