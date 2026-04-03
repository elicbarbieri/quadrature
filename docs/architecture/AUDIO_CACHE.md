# Audio Cache

Thread-safe LRU cache for fully-decoded audio buffers. Track-ID keyed storage with background decode, lock-based eviction protection, and two-tier caching for instant track changes.

**Dependency:** AudioCache depends on [LibraryCache](LIBRARY_CACHE.md) for audio file prefetch (track_id → path resolution).

## Architecture

```
                   ┌─────────────────────────────────────────────────────────────┐
                   │                        AUDIO CACHE                          │
                   │                                                             │
  UI Thread        │   ┌─────────────────┐    ┌─────────────────────────────┐   │
  ─────────────────┼──>│  load(track_id) │───>│    GThreadPool (4 workers)  │   │
                   │   └─────────────────┘    │                             │   │
                   │                          │  ┌───────────────────────┐  │   │
                   │                          │  │  FFmpeg decode task   │  │   │
                   │                          │  │  - Open file          │  │   │
                   │                          │  │  - Resample to 48kHz  │  │   │
                   │                          │  │  - Convert to float32 │  │   │
                   │                          │  │  - Interleave stereo  │  │   │
                   │                          │  └───────────────────────┘  │   │
                   │                          └──────────────┬──────────────┘   │
                   │                                         │                  │
                   │                                         v                  │
                   │   ┌─────────────────────────────────────────────────────┐  │
                   │   │            GHashTable (track_id → buffer)           │  │
                   │   │                                                     │  │
                   │   │  ┌──────────────────────────────────────────────┐   │  │
                   │   │  │              audio_buffer_t                  │   │  │
                   │   │  │  track_id: 12345                             │   │  │
                   │   │  │  samples: [L0,R0,L1,R1,...] (float32)        │   │  │
                   │   │  │  num_frames: 8,467,200                       │   │  │
                   │   │  │  sample_rate: 48000                          │   │  │
                   │   │  │  memory_bytes: 64.6 MB                       │   │  │
                   │   │  │  lock_count: 1 (atomic)                      │   │  │
                   │   │  │  decode_complete: true (atomic)              │   │  │
                   │   │  └──────────────────────────────────────────────┘   │  │
                   │   └─────────────────────────────────────────────────────┘  │
                   │                          │                                 │
                   │   ┌──────────────────────┴──────────────────────────────┐  │
                   │   │                  GQueue (LRU order)                 │  │
                   │   │  HEAD [most recent] ←─────────────→ TAIL [oldest]   │  │
                   │   └─────────────────────────────────────────────────────┘  │
                   └─────────────────────────────────────────────────────────────┘
                                              │
  Audio Thread                                │ get_locked() / lock() / unlock()
  ────────────────────────────────────────────┴──────────────────────────────────
```

## UI Track Loading Flow

When a user loads a track, the UI handler has the track ID from the row data.

```
on_track_queued(track_id)
      │
      v
audio_cache_load(track_id) ─────────> ┌────────────────────┐
      │                               │    AUDIO CACHE     │ ──────> FFmpeg decode
      v                               └────────────────────┘ <────── buffer ready
audio_pipeline_set_player_track()                │
      │                                          │ lock() + get_locked()
      │                               ┌────────────────────┐
      │                               │   AUDIO ENGINE     │ ──────> PipeWire callback
      └──────────────────────────────>│                    │         reads samples
      │  sets pending_track_id        │  player.buffer ────┼───┐
      v                               └────────────────────┘   │
ui_channel_strip_set_track()                                   v
      │                                                 samples ready
      v
    done
```

## Design Principles

**Track ID as key.** The track_id from the database is the canonical identifier. The cache resolves track_id to file path internally via database lookup.
**Buffer-first architecture.** All playback happens from decoded PCM buffers. No streaming fallback, no decode-on-play. This guarantees deterministic audio callback performance.
**Locks prevent eviction.** Locked buffers (lock_count > 0) cannot be evicted. The engine locks current and next tracks to keep them in cache. Delayed unlock (200ms) ensures safe buffer transitions when audio callback may still be reading.
**Async decode, sync access.** Loading returns immediately, decode happens in thread pool. Buffer access via `get_locked()` is non-blocking (returns NULL if not ready or not locked).

## Two-Tier Caching

For instantaneous track changes, the cache supports two tiers of protection:

### Tier 1: Lock/Unlock (Decoded Buffers)

"Locked" tracks stay in cache regardless of LRU order, but aren't actively being read. This enables preloading the next track for instant playback.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              EVICTION POLICY                                │
│                                                                             │
│   LOCKED (lock_count > 0)     ──── Cannot evict, pinned in cache            │
│   UNLOCKED (lock_count == 0)  ──── Can evict via LRU                        │
│                                                                             │
│   IMPORTANT: Use delayed unlock (200ms) when transitioning tracks to        │
│   ensure the audio callback has finished reading from the old buffer.       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Use case:** Audio Engine locks the current and next tracks. When user skips or track ends, the engine swaps to the new buffer pointer, then schedules a delayed unlock (200ms) for the old track. This ensures the audio callback completes any in-flight reads before the buffer becomes evictable.

### Tier 2: File Prefetch (Kernel Page Cache)

Hints to kernel to read file data into page cache. No decoding, just makes audio file-loading faster.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         KERNEL PAGE CACHE                                   │
│                                                                             │
│   UI scrolls search results                                                 │
│         │                                                                   │
│         v                                                                   │
│   audio_cache_prefetch_visible(track_ids[], count)                          │
│         │                                                                   │
│         v                                                                   │
│   AudioCache calls LibraryCache to resolve paths and prefetch               │
│         │                                                                   │
│         v                                                                   │
│   Kernel reads file blocks in background                                    │
│                                                                             │
│   Later: audio_cache_load(track_id) → decode from hot page cache            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Use case:** Search view prefetches visible results. If user clicks a result, decode starts with file data already in memory.

## Memory Model

Default limit: **512 MB**. At 48kHz stereo float32, this holds approximately:

| Track Length | Memory  | Capacity   |
| ------------ | ------- | ---------- |
| 3 min        | ~33 MB  | ~15 tracks |
| 5 min        | ~55 MB  | ~9 tracks  |
| 10 min       | ~110 MB | ~4 tracks  |

LRU eviction triggers when `memory_used > memory_limit`. Eviction skips:

- Buffers with `lock_count > 0` (locked for quick access)
- Buffers with `decode_complete == false` (still loading)

The eviction loop rotates skipped buffers to the head, preventing infinite loops when all buffers are in use.

## Thread Safety

```
┌────────────────────────────────────────────────────────────────────────────┐
│ Operation           │ Thread      │ Lock Required │ Notes                  │
├────────────────────────────────────────────────────────────────────────────┤
│ load()              │ UI          │ cache->lock   │ May start decode task  │
│ cancel_load()       │ UI          │ cache->lock   │ Sets atomic cancel flag│
│ get_status()        │ UI          │ cache->lock   │ Quick lookup           │
│ lock()              │ UI/Engine   │ cache->lock   │ Increments lock_count  │
│ wait_ready()        │ UI/Engine   │ buffer->mutex │ Blocks on GCond        │
│ unlock()            │ UI/Engine   │ cache->lock   │ Decrements lock_count  │
│ unlock_delayed()    │ UI/Engine   │ cache->lock   │ Schedules unlock+200ms │
│ get_locked()        │ UI/Engine   │ cache->lock   │ Returns buffer if ready│
│ get_stats()         │ Any         │ cache->lock   │ Iterates buffer list   │
│ get_decode_events() │ Any         │ event_lock    │ Copies event buffer    │
│ evict()             │ UI          │ cache->lock   │ Respects lock_count    │
│ decode_worker()     │ Thread Pool │ (internal)    │ Signals GCond on done  │
└────────────────────────────────────────────────────────────────────────────┘
```

**Critical invariant:** The audio callback must never block. All cache operations happen on the UI/Engine thread:

1. Audio callback holds buffer pointer directly (`audio_player_t.buffer`)
1. Lock/unlock and buffer access happen on UI thread before playback starts
1. Audio callback reads `buffer` via atomic load, never calls cache functions
1. Delayed unlock (200ms) ensures audio callback finishes reading before eviction

## Buffer Lifecycle

```
                  load()                   decode complete
NOT_FOUND ──────────> LOADING ─────────────────────────> READY
                          │                                │
                          │ cancel_load()                  │ evict()
                          │ decode error                   │ (lock_count == 0)
                          v                                v
                        FAILED                          NOT_FOUND
```

**State transitions are atomic.** `decode_complete`, `decode_failed`, and `decode_cancelled` are `atomic_bool`. The decode worker checks `decode_cancelled` between chunk reads for responsive cancellation.

## Data Structures

```c
typedef struct audio_buffer {
    int64_t track_id;                    // Database track ID (key in hash table)
    float* samples;                      // Interleaved stereo [L0,R0,L1,R1,...]
    uint64_t num_frames;                 // Total frames (samples/2)
    uint32_t sample_rate;                // Always 48000 (resampled on decode)
    size_t memory_bytes;                 // Actual allocation size

    // Thread-safe state
    atomic_int lock_count;               // Eviction protection (pinned in cache while > 0)
    atomic_bool decode_complete;         // Ready for playback
    atomic_bool decode_cancelled;        // Cancellation flag
    atomic_bool decode_failed;           // Error state

    void* _lru_link;                     // GList* in LRU queue
    void* _cache;                        // Back-pointer for cleanup
} audio_buffer_t;

typedef struct audio_cache {
    GHashTable* buffers;                 // track_id → audio_buffer_t*
    GQueue lru;                          // Eviction order (head = most recent)
    GMutex lock;                         // Protects buffers + lru

    uint32_t sample_rate;                // Target sample rate (48000)
    size_t memory_used;                  // Current memory usage
    size_t memory_limit;                 // Eviction threshold

    GThreadPool* decode_pool;            // 4 workers max
    GHashTable* pending_decodes;         // In-flight decode tasks
    GMutex decode_lock;                  // Protects pending_decodes

    GHashTable* pending_unlocks;         // track_id → timeout source for delayed unlocks

    // Decode events ring buffer (for statistics)
    audio_cache_decode_event_t decode_events[100];
    atomic_uint_fast32_t event_head;     // Next write position
    atomic_uint_fast32_t event_count;    // Total events (capped at 100)
    GMutex event_lock;                   // Protects event writes

    // Prefetch counter
    atomic_uint_fast32_t prefetch_tracks;
} audio_cache_t;
```

## Decode Pipeline

```
FFmpeg                          SwrContext                     audio_buffer_t
┌──────────────────┐    ┌─────────────────────────┐    ┌───────────────────────┐
│ avformat_open    │───>│ Source → Stereo Float32 │───>│ samples[num_frames*2] │
│ avcodec_decode   │    │ Source rate → 48000 Hz  │    │                       │
└──────────────────┘    └─────────────────────────┘    └───────────────────────┘
```

Buffer allocation includes 10% headroom for resampler flush frames.

## API Reference

### Lifecycle

```c
quadrature_result_t audio_cache_create(
    library_cache_t* library,    // For file prefetch (track_id → path resolution)
    uint32_t sample_rate,
    audio_cache_t** out
);
void audio_cache_destroy(audio_cache_t* cache);
```

### Loading (Async)

```c
// Start decode (non-blocking, returns immediately)
// Callback invoked on completion (from thread pool)
quadrature_result_t audio_cache_load(
    audio_cache_t* cache,
    int64_t track_id,
);

// Cancel pending decode
void audio_cache_cancel_load(audio_cache_t* cache, int64_t track_id);
void audio_cache_cancel_all_loads(audio_cache_t* cache);
```

### Lock/Unlock (For Playback and Preloading)

```c
// Lock result indicates decode status
typedef enum {
    AUDIO_CACHE_LOCK_READY,    // Buffer available now
    AUDIO_CACHE_LOCK_LOADING,  // Decode in progress, call wait_ready()
    AUDIO_CACHE_LOCK_FAILED    // Decode failed or track not found
} audio_cache_lock_result_t;

// Lock track to prevent LRU eviction. Returns decode status.
// Can be called multiple times (lock_count increments).
// Use for current track and next-track preloading.
// PRECONDITION: Track must be in cache (via audio_cache_load()).
audio_cache_lock_result_t audio_cache_lock(audio_cache_t* cache, int64_t track_id);

// Wait for decode completion on a locked track (blocks until ready or timeout).
// Uses GCond for instant wakeup when decode finishes - no polling overhead.
// Returns true if decode completed successfully, false if failed or timeout.
// PRECONDITION: Track must be in cache and locked.
bool audio_cache_wait_ready(audio_cache_t* cache, int64_t track_id, int64_t timeout_ms);

// Unlock track immediately. Only use when certain no audio callback is reading.
// Must be called once for each corresponding lock() call.
void audio_cache_unlock(audio_cache_t* cache, int64_t track_id);

// Unlock track after 200ms delay. Use when transitioning away from a playing track.
// The delay ensures any in-flight audio callback completes before eviction is allowed.
// Cancels any pending delayed unlock for this track before scheduling new one.
void audio_cache_unlock_delayed(audio_cache_t* cache, int64_t track_id);

// Get buffer for a locked track. Returns NULL if not locked or not ready.
// Does not modify lock_count - caller must have already called lock().
audio_buffer_t* audio_cache_get_locked(audio_cache_t* cache, int64_t track_id);
```

**Usage pattern for waiting on decode:**

```c
audio_cache_load(cache, track_id);  // Start async decode

audio_cache_lock_result_t result = audio_cache_lock(cache, track_id);
if (result == AUDIO_CACHE_LOCK_FAILED) {
    return QUADRATURE_ERROR_DECODE;
}
if (result == AUDIO_CACHE_LOCK_LOADING) {
    // Wait for decode (blocks, instant wake when done)
    if (!audio_cache_wait_ready(cache, track_id, 30000)) {
        audio_cache_unlock(cache, track_id);
        return QUADRATURE_ERROR_TIMEOUT;
    }
}

// Buffer is now ready
audio_buffer_t* buf = audio_cache_get_locked(cache, track_id);
```

### File Prefetch (Kernel Page Cache)

```c
// Prefetch audio files into kernel page cache.  Called by UI for top search results, or tracks in an album-detail view.
// Calls LibraryCache to resolve track_ids → paths and do posix_fadvise.
void audio_cache_prefetch(
    audio_cache_t* cache,
    const int64_t* track_ids,
    size_t count
);
```

### Status

```c
typedef enum {
    AUDIO_CACHE_NOT_FOUND,  // Track not in cache
    AUDIO_CACHE_LOADING,    // Decode in progress
    AUDIO_CACHE_READY,      // Available for playback
    AUDIO_CACHE_FAILED      // Decode failed
} audio_cache_status_t;

audio_cache_status_t audio_cache_get_status(audio_cache_t* cache, int64_t track_id);

```

### Buffer Accessors

```c
const float* audio_buffer_get_samples(const audio_buffer_t* buf);
uint64_t audio_buffer_get_num_frames(const audio_buffer_t* buf);
uint32_t audio_buffer_get_sample_rate(const audio_buffer_t* buf);
int64_t audio_buffer_get_track_id(const audio_buffer_t* buf);
```

### Cache Management

```c
void audio_cache_evict(audio_cache_t* cache, int64_t track_id);
void audio_cache_clear(audio_cache_t* cache);
void audio_cache_set_memory_limit(audio_cache_t* cache, size_t limit);
size_t audio_cache_get_memory_used(audio_cache_t* cache);
size_t audio_cache_get_count(audio_cache_t* cache);
```

### Statistics

The cache maintains a ring buffer of recent decode events for performance monitoring. Simple metrics are available via `get_stats()`, while raw events can be retrieved for detailed analysis (histogram computation, latency analysis, etc.).

```c
#define AUDIO_CACHE_MAX_DECODE_EVENTS 100

// Raw decode event stored in ring buffer
typedef struct {
    int64_t track_id;
    uint64_t file_size;           // File size in bytes
    uint32_t audio_duration_ms;   // Track length in milliseconds
    char filetype[8];             // File extension (e.g., "mp3", "flac")
    uint32_t decode_duration_ms;  // How long decode took
    uint64_t timestamp_ms;        // When load() was called (monotonic)
} audio_cache_decode_event_t;

// Simple cache metrics
typedef struct {
    float memory_usage_pct;           // (used / limit) * 100
    uint32_t cached_buffer_seconds;   // Total seconds of audio in decoded buffers
    uint32_t prefetch_tracks;         // Total tracks passed to audio_cache_prefetch()
    uint32_t event_count;             // Number of events in ring buffer
} audio_cache_stats_t;

void audio_cache_get_stats(audio_cache_t* cache, audio_cache_stats_t* stats);

// Access raw events for detailed analysis (histogram, latency distribution, etc.)
uint32_t audio_cache_get_decode_events(
    audio_cache_t* cache,
    audio_cache_decode_event_t* out_events,
    uint32_t max_events
);
```

**Key metrics:**

- **memory_usage_pct**: Cache pressure indicator
- **cached_buffer_seconds**: Total audio duration currently cached
- **prefetch_tracks**: UI activity indicator (kernel page cache hints)
- **Raw events**: For detailed analysis (latency histogram, filetype distribution, correlation with file size, etc.)

### Next-Track Preloading (Audio Engine)

```c
// When track changes, preload next track for instant advance
void on_track_set(int64_t old_id, int64_t old_next_id, int64_t new_id, int64_t new_next_id) {
    // Lock and load new tracks FIRST (before unlocking old)
    audio_cache_lock(cache, new_id);
    audio_cache_load(cache, new_id);

    if (new_next_id > 0) {
        audio_cache_lock(cache, new_next_id);
        audio_cache_load(cache, new_next_id);
    }

    // Swap buffer pointer atomically (audio callback will read from new buffer)
    audio_buffer_t* new_buf = audio_cache_get_locked(cache, new_id);
    atomic_store(&player->buffer, new_buf);

    // Release old locks with 200ms delay (ensures audio callback finishes reading)
    if (old_id > 0)      audio_cache_unlock_delayed(cache, old_id);
    if (old_next_id > 0) audio_cache_unlock_delayed(cache, old_next_id);
}
```

### Search Results Prefetch (UI)

```c
// Prefetch visible search results for faster decode when clicked
static void on_search_results_visible(SearchView* view, const library_search_results_t* results) {
    int64_t track_ids[32];
    size_t count = MIN(results->tracks->len, 32);

    for (size_t i = 0; i < count; i++) {
        const library_track_info_t* track = g_ptr_array_index(results->tracks, i);
        track_ids[i] = track->track_id;
    }

    // AudioCache calls LibraryCache internally for path resolution
    audio_cache_prefetch_visible(view->audio_cache, track_ids, count);
}
```

## Integration with Audio Pipeline

The cache is owned by `audio_pipeline_t` and shared across all 4 players:

```c
struct audio_pipeline {
    audio_player_t players[4];
    audio_cache_t* cache;
    // ...
};
```

Pipeline API sets the track, engine handles lock management:

```c
// Set track for player (locks track, gets buffer when ready)
quadrature_result_t audio_pipeline_set_player_track(audio_pipeline_t* pipeline, int player_id, int64_t track_id);

// Check if buffer is locked and ready
bool audio_pipeline_player_is_ready(audio_pipeline_t* pipeline, int player_id);
```

If buffer is already cached, `get_locked()` returns immediately and player transitions to STOPPED state. Otherwise, player stays in LOADING until decode completes and buffer becomes available.

## Performance Characteristics

| Operation           | Complexity | Lock     | Notes                         |
| ------------------- | ---------- | -------- | ----------------------------- |
| load()              | O(1)       | GMutex   | Hash insert + queue push      |
| lock()              | O(1)       | GMutex   | Hash lookup + atomic incr     |
| wait_ready()        | Blocking   | GCond    | Blocks until decode complete  |
| unlock()            | O(1)       | GMutex   | Atomic decrement              |
| unlock_delayed()    | O(1)       | GMutex   | Schedules g_timeout           |
| get_locked()        | O(1)       | GMutex   | Hash lookup                   |
| get_status()        | O(1)       | GMutex   | Hash lookup                   |
| get_stats()         | O(n)       | GMutex   | Iterates buffers for duration |
| get_decode_events() | O(n)       | GMutex   | Copies event ring buffer      |
| evict_lru()         | O(n)       | GMutex   | Scans queue until freed       |
| decode (3min song)  | ~1-3s      | Internal | Recorded in decode events     |

The cache adds ~100-300 microseconds of latency to track start (cache hit) due to mutex acquisition. Cache misses add decode time (typically 1-3 seconds for a full track). The 200ms delayed unlock adds negligible overhead—buffers remain locked slightly longer but this doesn't impact cache pressure in practice.

## Eviction Policy

LRU with lock-count protection:

1. Eviction triggers when `memory_used > memory_limit`
1. Walk LRU queue from tail (oldest)
1. Skip if `lock_count > 0` (locked) or `decode_complete == false` (loading)
1. Skipped buffers move to head (prevents starvation)
1. Continue until under limit or full rotation

## Error Handling

Decode failures set `decode_failed = true` and remove the buffer from the cache. The player transitions to NO_AUDIO state (buffer unavailable).

All failures are logged via `g_critical()` and propagate through the status API.
