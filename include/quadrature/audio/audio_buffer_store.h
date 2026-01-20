/**
 * Quadrature Audio Buffer Store
 *
 * Thread-safe LRU store for fully decoded audio buffers.
 * All playback happens from decoded PCM buffers - no streaming fallback.
 *
 * Key design principles:
 * - Path-based lookup (no track_id dependency)
 * - Audio callback (on_process) remains lock-free
 * - Decoding happens in background thread pool
 * - Reference counting prevents eviction during playback
 * - Buffer lookup via pre-resolved pointer (O(1) in audio callback)
 */

#ifndef QUADRATURE_AUDIO_BUFFER_STORE_H
#define QUADRATURE_AUDIO_BUFFER_STORE_H

#include "../core/types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default store memory limit: 512MB */
#define AUDIO_BUFFER_STORE_DEFAULT_MEMORY_LIMIT (512 * 1024 * 1024)

/* Maximum concurrent decode tasks */
#define AUDIO_BUFFER_STORE_MAX_DECODE_WORKERS 4

/* ═══════════════════════════════════════════════════════════════════════════
 * Opaque Types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct audio_buffer_store audio_buffer_store_t;
typedef struct audio_buffer audio_buffer_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Status
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    BUFFER_STATUS_NOT_FOUND,  /* Path not in store */
    BUFFER_STATUS_LOADING,    /* Decode in progress */
    BUFFER_STATUS_READY,      /* Decode complete, buffer available */
    BUFFER_STATUS_FAILED      /* Decode failed */
} buffer_status_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Decode Callback
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void (*audio_buffer_decode_callback_t)(
    audio_buffer_store_t* store,
    const char* path,
    bool success,
    void* user_data
);

/* ═══════════════════════════════════════════════════════════════════════════
 * Store Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create a buffer store with default memory limit.
 *
 * @param sample_rate  Target sample rate for decoded audio
 * @param out          Output pointer to created store
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_buffer_store_create(uint32_t sample_rate,
                                               audio_buffer_store_t** out);

/**
 * Create a buffer store with custom memory limit.
 *
 * @param sample_rate    Target sample rate for decoded audio
 * @param memory_limit   Maximum memory usage in bytes
 * @param out            Output pointer to created store
 * @return QUADRATURE_OK on success
 */
quadrature_result_t audio_buffer_store_create_with_limit(uint32_t sample_rate,
                                                          size_t memory_limit,
                                                          audio_buffer_store_t** out);

/**
 * Destroy a buffer store and free all resources.
 *
 * @param store  Store to destroy (may be NULL)
 */
void audio_buffer_store_destroy(audio_buffer_store_t* store);

/* ═══════════════════════════════════════════════════════════════════════════
 * Loading API (Background Decoding)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Start loading/decoding an audio file asynchronously.
 *
 * If the file is already loaded or loading, this is a no-op.
 * Use get_status() to monitor progress.
 *
 * @param store     Buffer store
 * @param path      Path to audio file
 * @param callback  Optional callback when decode completes
 * @param user_data User data for callback
 * @return QUADRATURE_OK on success (decode started or already in progress)
 */
quadrature_result_t audio_buffer_store_load(audio_buffer_store_t* store,
                                             const char* path,
                                             audio_buffer_decode_callback_t callback,
                                             void* user_data);

/**
 * Cancel a pending load operation.
 *
 * If the file is currently decoding, marks it for cancellation.
 * Has no effect if already complete or not loading.
 *
 * @param store  Buffer store
 * @param path   Path to cancel
 */
void audio_buffer_store_cancel_load(audio_buffer_store_t* store,
                                     const char* path);

/**
 * Cancel all pending load operations.
 *
 * @param store  Buffer store
 */
void audio_buffer_store_cancel_all_loads(audio_buffer_store_t* store);

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Access (Lock-Free for Audio Callback)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Try to acquire a buffer for playback (non-blocking).
 *
 * Returns NULL if buffer is not ready. On success, increments ref count
 * to prevent eviction. Must call release() when done.
 *
 * Thread-safe. Safe to call from audio callback.
 *
 * @param store  Buffer store
 * @param path   Path to audio file
 * @return Buffer pointer or NULL if not ready
 */
audio_buffer_t* audio_buffer_store_try_acquire(audio_buffer_store_t* store,
                                                const char* path);

/**
 * Release a previously acquired buffer.
 *
 * Decrements ref count, allowing eviction when memory pressure requires.
 *
 * @param store   Buffer store
 * @param buffer  Buffer to release (may be NULL)
 */
void audio_buffer_store_release(audio_buffer_store_t* store,
                                 audio_buffer_t* buffer);

/* ═══════════════════════════════════════════════════════════════════════════
 * Status Queries
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get the status of a buffer for a given path.
 *
 * @param store  Buffer store
 * @param path   Path to query
 * @return Buffer status
 */
buffer_status_t audio_buffer_store_get_status(audio_buffer_store_t* store,
                                               const char* path);

/**
 * Get decode progress for a loading buffer.
 *
 * @param store  Buffer store
 * @param path   Path to query
 * @return Progress 0.0-1.0, or -1.0 if not loading/failed
 */
float audio_buffer_store_get_progress(audio_buffer_store_t* store,
                                       const char* path);

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Accessors
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get the interleaved stereo float samples from a buffer.
 *
 * @param buf  Buffer (may be NULL)
 * @return Pointer to samples or NULL
 */
const float* audio_buffer_get_samples(const audio_buffer_t* buf);

/**
 * Get the number of frames in a buffer.
 *
 * @param buf  Buffer (may be NULL)
 * @return Number of frames or 0
 */
uint64_t audio_buffer_get_num_frames(const audio_buffer_t* buf);

/**
 * Get the sample rate of a buffer.
 *
 * @param buf  Buffer (may be NULL)
 * @return Sample rate or 0
 */
uint32_t audio_buffer_get_sample_rate(const audio_buffer_t* buf);

/**
 * Get the file path of a buffer.
 *
 * @param buf  Buffer (may be NULL)
 * @return Path string or NULL
 */
const char* audio_buffer_get_path(const audio_buffer_t* buf);

/* ═══════════════════════════════════════════════════════════════════════════
 * Store Management
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Evict a specific buffer from the store.
 *
 * Does nothing if buffer is currently acquired (ref_count > 0).
 *
 * @param store  Buffer store
 * @param path   Path to evict
 */
void audio_buffer_store_evict(audio_buffer_store_t* store, const char* path);

/**
 * Clear all buffers from the store.
 *
 * Only evicts buffers with ref_count == 0.
 *
 * @param store  Buffer store
 */
void audio_buffer_store_clear(audio_buffer_store_t* store);

/**
 * Set the memory limit for the store.
 *
 * May trigger LRU eviction if new limit is lower than current usage.
 *
 * @param store         Buffer store
 * @param memory_limit  New memory limit in bytes
 */
void audio_buffer_store_set_memory_limit(audio_buffer_store_t* store,
                                          size_t memory_limit);

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get current memory usage.
 *
 * @param store  Buffer store
 * @return Bytes currently used
 */
size_t audio_buffer_store_get_memory_used(audio_buffer_store_t* store);

/**
 * Get memory limit.
 *
 * @param store  Buffer store
 * @return Memory limit in bytes
 */
size_t audio_buffer_store_get_memory_limit(audio_buffer_store_t* store);

/**
 * Get cache statistics.
 *
 * @param store      Buffer store
 * @param hits       Output: number of successful acquires
 * @param misses     Output: number of failed acquires
 * @param evictions  Output: number of evictions
 */
void audio_buffer_store_get_stats(audio_buffer_store_t* store,
                                   uint64_t* hits,
                                   uint64_t* misses,
                                   uint64_t* evictions);

/**
 * Get number of ready buffers in store.
 *
 * @param store  Buffer store
 * @return Number of fully decoded buffers
 */
size_t audio_buffer_store_get_count(audio_buffer_store_t* store);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_AUDIO_BUFFER_STORE_H */
