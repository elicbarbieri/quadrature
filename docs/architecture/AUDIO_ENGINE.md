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

## Track Loading Contract

**Caller must load tracks before setting them.** The `set_player_track()` function expects the track to already be in the audio cache (loaded or loading).

### Required Sequence

```c
// 1. Caller initiates load (non-blocking)
audio_cache_load(cache, track_id);

// 2. Caller sets the track (locks it, doesn't wait for decode)
audio_pipeline_set_player_track(pipeline, player_id, track_id);

// 3. If decode wasn't complete, audio outputs silence
// 4. 50ms timeout polls for decode completion
// 5. When ready: buffer attached, callback fires
```

### Why This Design?

- **Non-blocking UI**: `set_player_track()` returns immediately
- **Rapid skip support**: Press next 5 times in 1 second - UI updates instantly
- **Prefetch optimization**: UI can call `audio_cache_load()` early when browsing views
- **Clear contract**: Caller owns loading, engine owns locking and playback

### Preconditions

`audio_pipeline_set_player_track()` asserts:
- Track is in cache (NOT_FOUND state will crash with assertion)
- Caller must call `audio_cache_load()` before `set_player_track()`

### What set_player_track() Does

1. Unlocks old current and next tracks
2. Resets player position and state
3. Locks the new track (already in cache)
4. If decode READY: attaches buffer, fires callback immediately
5. If decode LOADING: marks pending, 50ms timeout will attach when ready
6. Preloads NEXT track (async) for instant auto-advance

### Prefetch for Instant Playback

For truly instant playback, prefetch audio files when entering views:

```c
// In detail view: prefetch visible tracks
int64_t *track_ids = get_visible_track_ids();
library_cache_prefetch_audio_files(cache, track_ids, count);

// Later: clicking a track is instant (file already in page cache)
audio_cache_load(cache, clicked_track_id);  // Fast: file cached
audio_pipeline_set_player_track(pipeline, player_id, clicked_track_id);
```

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
                    play()
         STOPPED ◄────────────► PLAYING
            │                       │
            │ (explicit stop only)  │ toggle_play()
            │                       ▼
            └───────────────────  PAUSED
```

**State is preserved during track changes.** When `set_player_track()` is called:
- PLAYING stays PLAYING (new track plays immediately)
- STOPPED stays STOPPED
- No intermediate LOADING state

**STOPPED only occurs from:**
- Explicit `stop()` call (user presses stop)
- Last track ends with repeat OFF
- ERROR state transition

## Player

References pre-decoded buffers from the [Audio Cache](AUDIO_CACHE.md). The audio engine does NOT load or decode audio - that's handled entirely by the cache.

The audio player reads decoded samples from the Audio Cache and writes them to PipeWire output buffers. If the buffer isn't ready yet (cache still decoding), the callback outputs silence.

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
// Create pipeline with library cache for track_id → path resolution and next-track queries
quadrature_result_t audio_pipeline_create(
    library_cache_t* library,      // Required for track_id support
    uint32_t sample_rate,
    audio_pipeline_t** out
);

void                audio_pipeline_destroy(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_start(audio_pipeline_t* pipeline);
quadrature_result_t audio_pipeline_stop(audio_pipeline_t* pipeline);
```

### Player Control

```c
// Set track for player (engine queries cache by track_id)
quadrature_result_t audio_pipeline_set_player_track(audio_pipeline_t* pipeline, int player_id, int64_t track_id);

// Get current track ID (0 if no track loaded)
int64_t audio_pipeline_get_player_track_id(audio_pipeline_t* pipeline, int player_id);

// Transport
quadrature_result_t audio_pipeline_player_play(pipeline, player_id);
quadrature_result_t audio_pipeline_player_stop(pipeline, player_id);
quadrature_result_t audio_pipeline_player_toggle_play(pipeline, player_id);
quadrature_result_t audio_pipeline_player_seek(pipeline, player_id, position_samples);

// Repeat mode (when enabled, next_track is unlocked)
quadrature_result_t audio_pipeline_player_set_repeat(pipeline, player_id, repeat);

// Autoplay mode (when disabled, stops after track advance instead of continuing)
quadrature_result_t audio_pipeline_player_set_autoplay(pipeline, player_id, autoplay);
bool audio_pipeline_player_get_autoplay(pipeline, player_id);
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

### Statistics

**Pipeline-level** (aggregate across all players):

```c
typedef struct {
    uint64_t callback_count;
    uint64_t underrun_count;
    float callback_time_avg_us;
    float callback_time_max_us;
    uint64_t track_changes;
    uint64_t instant_advances;
} audio_pipeline_stats_t;

void audio_pipeline_get_stats(pipeline, &stats);
```

**Per-player** (dashboard-grade — rates, percentages, fault counts):

```c
typedef struct {
    // Callback performance
    float callback_time_avg_us;   // Average processing time per callback
    float callback_time_max_us;   // All-time peak processing time
    float budget_pct;             // Avg time as % of period budget (<30% = healthy)
    uint64_t budget_overruns;     // Callbacks exceeding 50% budget (should be 0)

    // Audio health
    float underrun_rate_pct;      // Underruns as % of callbacks (0 = healthy)
    float jitter_ms;              // Average callback scheduling jitter

    // Fault events (should be 0 in normal operation)
    uint64_t dequeue_failures;    // PipeWire couldn't provide output buffer
    uint64_t scrubber_underflows; // Rubberband couldn't fill requested frames
    uint64_t deferred_advances;   // Track advance with audible gap (preload miss)

    // Advance quality
    float advance_hit_rate_pct;   // Preloaded advances as % of total (100 = perfect)
} audio_player_stats_t;

void audio_pipeline_get_player_stats(pipeline, player_id, &stats);
```

**Dashboard panels:**

| Panel | Data Source | Type |
|-------|-----------|------|
| Callback Latency Distribution | `perf_get_histogram_stats(&perf->callback_time[id], &hist)` | Histogram (p50/p90/p99/max) |
| Budget Utilization Trend | `perf_get_timeseries(&perf->budget_pct[id], ...)` | Line chart (%, 1 sample/sec) |
| Underrun Rate | `stats.underrun_rate_pct` | Gauge |
| Scheduling Jitter | `stats.jitter_ms` | Gauge |
| Anomaly Events | `perf_read_logs(perf, ...)` | Event log (timestamped) |

**Health thresholds:**

- `budget_pct` < 30%: healthy. > 50%: at risk. > 80%: logged as warning.
- `underrun_rate_pct` = 0: target. Any non-zero indicates audio gaps.
- `advance_hit_rate_pct` = 100%: perfect preloading. < 100%: audible gaps between tracks.
- `jitter_ms` < 1.0: normal. High jitter indicates audio graph scheduling pressure.

**Callback timing uses `clock_gettime(CLOCK_MONOTONIC)`** which is VDSO-mapped on Linux
(~20ns userspace, no kernel transition). Same mechanism used by PipeWire, JACK, and Ardour.

## Audio Callback

The PipeWire callback reads directly from the acquired buffer through the scrubber:

```
buffer ──> scrubber ──> metering ──> spectrum ringbuf ──> PipeWire sink
```

**Forbidden in callback:** malloc, mutex, file I/O, FFmpeg.

Budget: ~10.67ms at 48kHz/512 frames. Typical: 0.1-2ms depending on shuttle mode.

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

Auto-advance is fully automatic. The audio callback detects track end and swaps buffers atomically. Deferred cleanup runs on a GLib timeout (internal to pipeline) to handle mutex operations that can't run in the RT callback.

```
AUDIO CALLBACK (RT thread)              ADVANCE HANDLER (main thread, GLib timeout)
──────────────────────────              ───────────────────────────────────────────

on_track_position >= track_length:
      │
      ├──> if (repeat):
      │        seek(0)
      │        continue playing
      │        return
      │
      ├──> old_track = current_track_id
      │    current_track_id = next_track_id
      │    buffer = next_buffer              // Atomic swap - instant!
      │    next_buffer = NULL
      │
      └──> advance_pending = true            ────────────────> on advance_pending:
           advance_old_track_id = old_track                         │
                                                                    ├──> audio_cache_unlock_delayed(old_track)
                                                                    │
                                                                    ├──> new_next = library_cache_get_next_track_id()
                                                                    │
                                                                    ├──> if (new_next):
                                                                    │        audio_cache_lock(new_next)
                                                                    │        audio_cache_load(new_next)
                                                                    │        next_buffer = get_locked(new_next)
                                                                    │
                                                                    └──> track_changed_callback()
                                                                         (fires on main thread)
```

**Why deferred cleanup?** The audio callback must be lock-free (no mutexes, no malloc). Cache operations require mutexes. The pipeline internally runs a GLib timeout (~50ms) that checks for pending advances and handles cleanup on the main thread. The `track_changed_callback` fires on the main thread, making it safe for GTK UI updates.

### Flow: Skip (Prev/Next)

Skip is handled in UI layer for instant visual feedback:

```
UI THREAD (channel_strip.c)
───────────────────────────

ui_channel_strip_next_track():
      │
      ├──> next_id = library_cache_get_next_track_id(current_id)
      │
      ├──> track = library_cache_get_track(next_id)
      │
      ├──> Update UI immediately (title, artist, album)
      │    // User sees change instantly
      │
      ├──> audio_cache_load(cache, next_id)
      │
      └──> audio_pipeline_set_player_track(pipeline, next_id)
           // Non-blocking - audio catches up when decode completes
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
