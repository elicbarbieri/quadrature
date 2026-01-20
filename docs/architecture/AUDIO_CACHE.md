# Audio Cache

Thread-safe LRU cache for fully-decoded audio buffers. Track-ID keyed storage with background decode, reference counting, lock-free acquisition for real-time playback, and two-tier caching for instant track changes.

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
                   │   │  │  ref_count: 1 (atomic)                       │   │  │
                   │   │  │  lock_count: 0 (atomic)                      │   │  │
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
  Audio Thread                                │ try_acquire() / release()
  ────────────────────────────────────────────┴──────────────────────────────────
```

## UI Track Loading Flow

When a user loads a track, the UI handler has the track ID from the row data.

```
  UI THREAD                                 DECODE WORKERS            AUDIO THREAD
 ───────────                               ────────────────          ──────────────

  on_track_queued(track_id)
        │
        v
  audio_cache_load(track_id) ─────────> ┌────────────────────┐
        │                               │    AUDIO CACHE     │ ──────> FFmpeg decode
        v                               └────────────────────┘ <────── buffer ready
  audio_pipeline_set_player_track()                │
        │                                          │ try_acquire(track_id)
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

**Locks prevent eviction.** Locked buffers (lock_count > 0) cannot be evicted. The engine locks current and next tracks to keep them in cache. Ref counting is separate—it ensures memory safety via deferred destruction when evicting buffers that are still being read.

**Async decode, sync acquire.** Loading returns immediately, decode happens in thread pool. Acquisition is non-blocking (returns NULL if not ready) to avoid blocking the audio callback.

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
│   Note: ref_count is orthogonal to eviction. If an unlocked buffer is       │
│   evicted while ref_count > 0, destruction is deferred until released.      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Use case:** Audio Engine locks the current and next tracks. When user skips or track ends, next track starts instantly from preloaded buffer. Previously played tracks (unlocked) can be evicted via LRU.

### Tier 2: File Prefetch (Kernel Page Cache)

Hints to kernel to read file data into page cache. No decoding, just makes future decode faster.

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

Note: `ref_count` does not affect eviction. If an unlocked buffer with `ref_count > 0` is evicted, it's moved to the `deferred_destroy` queue and freed when `ref_count` reaches 0.

The eviction loop rotates skipped buffers to the head, preventing infinite loops when all buffers are in use.

## Thread Safety

```
┌────────────────────────────────────────────────────────────────────────────┐
│ Operation           │ Thread      │ Lock Required │ Notes                  │
├────────────────────────────────────────────────────────────────────────────┤
│ load()              │ UI          │ cache->lock   │ May start decode task  │
│ cancel_load()       │ UI          │ cache->lock   │ Sets atomic cancel flag│
│ get_status()        │ UI          │ cache->lock   │ Quick lookup           │
│ get_progress()      │ UI          │ cache->lock   │ Reads atomic counters  │
│ try_acquire()       │ UI/Audio    │ cache->lock   │ Increments ref_count   │
│ release()           │ UI/Audio    │ None          │ Atomic decrement only  │
│ lock()              │ UI/Engine   │ cache->lock   │ Increments lock_count  │
│ unlock()            │ UI/Engine   │ cache->lock   │ Decrements lock_count  │
│ evict()             │ UI          │ cache->lock   │ Respects lock_count    │
│ decode_worker()     │ Thread Pool │ (internal)    │ Updates atomics        │
└────────────────────────────────────────────────────────────────────────────┘
```

**Critical invariant:** The audio callback must never block. `try_acquire()` takes a mutex, but this is acceptable because:

1. Audio callback holds buffer pointer directly (`audio_player_t.buffer`)
2. Acquisition happens on UI thread before playback starts
3. Audio callback reads `buffer` via atomic load, never calls cache functions

## Buffer Lifecycle

```
                  load()                   decode complete
    NOT_FOUND ──────────> LOADING ─────────────────────────> READY
                              │                                │
                              │ cancel_load()                  │ evict()
                              │ decode error                   │ (ref_count == 0)
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
    atomic_int ref_count;                // Memory safety (deferred destruction if evicted while > 0)
    atomic_int lock_count;               // Eviction protection (pinned in cache while > 0)
    atomic_bool decode_complete;         // Ready for playback
    atomic_uint_fast64_t decoded_frames; // Progress tracking
    atomic_uint_fast64_t total_frames;   // For progress calculation
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

    GQueue deferred_destroy;             // Buffers awaiting ref_count == 0

    // Statistics (atomic)
    atomic_uint_fast64_t hits;
    atomic_uint_fast64_t misses;
    atomic_uint_fast64_t evictions;
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

Decode happens in 4096-frame chunks. Progress is updated atomically after each chunk, enabling UI feedback:

```c
float progress = audio_cache_get_progress(cache, track_id);  // 0.0-1.0
```

Buffer allocation includes 10% headroom for resampler flush frames.

## API Reference

### Lifecycle

```c
quadrature_result_t audio_cache_create(
    library_cache_t* library,    // For file prefetch (track_id → path resolution)
    uint32_t sample_rate,
    size_t memory_limit,
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
    audio_cache_callback_t callback,  // optional
    void* user_data
);

// Cancel pending decode
void audio_cache_cancel_load(audio_cache_t* cache, int64_t track_id);
void audio_cache_cancel_all_loads(audio_cache_t* cache);
```

### Acquisition (For Playback)

```c
// Try to acquire buffer (non-blocking, may return NULL)
// On success, increments ref_count - must call release()
audio_buffer_t* audio_cache_try_acquire(audio_cache_t* cache, int64_t track_id);

// Release buffer (decrements ref_count)
void audio_cache_release(audio_cache_t* cache, audio_buffer_t* buffer);
```

### Lock/Unlock (For Preloading)

```c
// Lock track to prevent LRU eviction. Starts load if not in cache.
// Can be called multiple times (lock_count increments).
// Use for next-track preloading - keeps buffer ready for instant playback.
void audio_cache_lock(audio_cache_t* cache, int64_t track_id);

// Unlock track, allowing LRU eviction when lock_count reaches 0.
// Must be called once for each corresponding lock() call.
void audio_cache_unlock(audio_cache_t* cache, int64_t track_id);
```

### File Prefetch (Kernel Page Cache)

```c
// Prefetch audio files for visible tracks into kernel page cache.
// Calls LibraryCache to resolve track_ids → paths and do posix_fadvise.
// Makes future decode faster (file data already in memory).
void audio_cache_prefetch_visible(
    audio_cache_t* cache,
    const int64_t* track_ids,
    size_t count
);
```

**Implementation:**

```c
void audio_cache_prefetch_visible(audio_cache_t* cache,
                                   const int64_t* track_ids, size_t count) {
    // Delegate to LibraryCache which handles:
    // 1. Resolve track_ids → file paths
    // 2. Call posix_fadvise(WILLNEED) for each file
    library_cache_prefetch_audio_files(cache->library, track_ids, count);
}
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

// Progress during decode (0.0-1.0, -1.0 on error/not found)
float audio_cache_get_progress(audio_cache_t* cache, int64_t track_id);
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

// Statistics
size_t audio_cache_get_memory_used(audio_cache_t* cache);
size_t audio_cache_get_memory_limit(audio_cache_t* cache);
size_t audio_cache_get_count(audio_cache_t* cache);
```

## Usage Patterns

### Basic Playback

```c
// 1. Start decode (async)
audio_cache_load(cache, track_id, on_decode_complete, ctx);

// 2. Poll status in UI tick
audio_cache_status_t status = audio_cache_get_status(cache, track_id);
if (status == AUDIO_CACHE_LOADING) {
    float progress = audio_cache_get_progress(cache, track_id);
    update_loading_indicator(progress);
}

// 3. Acquire for playback
if (status == AUDIO_CACHE_READY) {
    audio_buffer_t* buf = audio_cache_try_acquire(cache, track_id);
    if (buf) {
        atomic_store(&player->buffer, buf);
    }
}

// 4. Release on track change
audio_buffer_t* old = atomic_exchange(&player->buffer, NULL);
if (old) {
    audio_cache_release(cache, old);
}
```

### Next-Track Preloading (Audio Engine)

```c
// When track changes, preload next track for instant advance
void on_track_set(int64_t old_id, int64_t old_next_id, int64_t new_id, int64_t new_next_id) {
    // Release old locks
    if (old_id > 0)      audio_cache_unlock(cache, old_id);
    if (old_next_id > 0) audio_cache_unlock(cache, old_next_id);

    // Lock and load new tracks
    audio_cache_lock(cache, new_id);
    audio_cache_load(cache, new_id, NULL, NULL);

    if (new_next_id > 0) {
        audio_cache_lock(cache, new_next_id);
        audio_cache_load(cache, new_next_id, NULL, NULL);
    }
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

Pipeline API sets the track, audio thread handles acquisition:

```c
// Set track for player (audio thread will query cache for buffer)
quadrature_result_t audio_pipeline_set_player_track(audio_pipeline_t* pipeline, int player_id, int64_t track_id);

// Check if buffer is acquired and ready
bool audio_pipeline_player_is_ready(audio_pipeline_t* pipeline, int player_id);
```

If buffer is already cached, acquisition happens immediately and player transitions to STOPPED state. Otherwise, player stays in LOADING until the audio thread acquires the buffer.

## Performance Characteristics

| Operation          | Complexity | Lock     | Notes                     |
| ------------------ | ---------- | -------- | ------------------------- |
| load()             | O(1)       | GMutex   | Hash insert + queue push  |
| try_acquire()      | O(1)       | GMutex   | Hash lookup + atomic incr |
| release()          | O(1)       | None     | Atomic decrement only     |
| get_status()       | O(1)       | GMutex   | Hash lookup               |
| evict_lru()        | O(n)       | GMutex   | Scans queue until freed   |
| decode (3min song) | ~1-3s      | Internal | Depends on codec/disk     |

The cache adds ~100-300 microseconds of latency to track start (cache hit) due to mutex acquisition. Cache misses add decode time (typically 1-3 seconds for a full track).

## Eviction Policy

LRU with lock-count protection:

1. Eviction triggers when `memory_used > memory_limit`
2. Walk LRU queue from tail (oldest)
3. Skip if `lock_count > 0` (locked) or `decode_complete == false` (loading)
4. Skipped buffers move to head (prevents starvation)
5. Continue until under limit or full rotation

```c
// Eviction loop pseudocode
while (cache->memory_used > cache->memory_limit) {
    audio_buffer_t* buf = lru_get_tail(cache);
    if (!buf) break;

    if (buf->lock_count > 0 ||     // locked - cannot evict
        !buf->decode_complete) {   // still loading
        lru_move_to_head(cache, buf);  // prevent starvation
        if (rotated_full_queue) break;
        continue;
    }

    // Safe to evict - but may need deferred destruction
    if (buf->ref_count > 0) {
        // Buffer still being read - defer destruction
        g_queue_push_tail(&cache->deferred_destroy, buf);
        g_hash_table_remove(cache->buffers, &buf->track_id);
        cache->memory_used -= buf->memory_bytes;
    } else {
        // Safe to free immediately
        evict_buffer(cache, buf);
    }
}
```

### Deferred Destruction

When `release()` is called on an evicted buffer (one in `deferred_destroy` queue):

```c
void audio_cache_release(audio_cache_t* cache, audio_buffer_t* buffer) {
    if (atomic_fetch_sub(&buffer->ref_count, 1) == 1) {
        // ref_count reached 0 - check if deferred
        g_mutex_lock(&cache->lock);
        if (g_queue_remove(&cache->deferred_destroy, buffer)) {
            // Was deferred - now safe to free
            audio_buffer_free(buffer);
        }
        g_mutex_unlock(&cache->lock);
    }
}
```

This ensures the audio thread never reads from freed memory, while allowing the cache to reclaim space from unlocked buffers.

### Memory Budget with Locking

With 4 players, worst case locked memory:
- 4 current tracks × ~55MB = ~220MB
- 4 next tracks × ~55MB = ~220MB
- **Total locked: ~440MB**

With default 512MB limit, leaves ~72MB for LRU. Consider increasing to 768MB or 1GB for more LRU headroom.

## Error Handling

Decode failures set `decode_failed = true` and remove the buffer from the cache. The player transitions to NO_AUDIO state (buffer unavailable).

Common failure modes:

- File not found / permission denied
- Unsupported codec
- Corrupt audio data
- Out of memory (allocation failure)

All failures are logged via `g_critical()` and propagate through the status API.
