/**
 * Quadrature Audio Cache Implementation
 *
 * Thread-safe LRU cache for fully decoded audio buffers.
 * Background decoding via GThreadPool, lock-based eviction protection.
 * Uses track_id as key with LibraryCache for path resolution.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/quadrature_library.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_cache_t* cache;
    audio_buffer_t* buffer;
    uint64_t start_time_ms;  /* Decode start timestamp */
} decode_task_t;

struct audio_buffer {
    int64_t track_id;                       /* Primary key */
    char* path;                             /* Resolved path for decode */
    float* samples;                         /* Interleaved stereo float samples */
    uint64_t num_frames;
    uint32_t sample_rate;
    size_t memory_bytes;

    /* Thread-safe state */
    atomic_int lock_count;                  /* Eviction protection */
    atomic_bool decode_complete;
    atomic_bool decode_cancelled;
    atomic_bool decode_failed;

    /* Decode completion signaling */
    GCond ready_cond;
    GMutex ready_mutex;

    /* Internal (do not access directly) */
    void* _lru_link;
    void* _cache;
};

struct audio_cache {
    GHashTable* buffers;       /* int64_t* track_id -> audio_buffer_t* */
    GQueue lru;
    GMutex lock;

    library_cache_t* library;  /* For track_id -> path resolution */
    uint32_t sample_rate;
    size_t memory_used;
    size_t memory_limit;

    GThreadPool* decode_pool;
    GHashTable* pending_decodes;  /* int64_t* track_id -> decode_task_t* */
    GMutex decode_lock;

    /* Delayed unlock tracking: track_id -> GSource timeout ID */
    GHashTable* pending_unlocks;

    /* Decode events ring buffer (for statistics) */
    audio_cache_decode_event_t decode_events[AUDIO_CACHE_MAX_DECODE_EVENTS];
    atomic_uint_fast32_t event_head;   /* Next write position */
    atomic_uint_fast32_t event_count;  /* Total events (capped at MAX) */
    GMutex event_lock;                 /* Protects event writes */

    /* Prefetch counter (tracks UI activity) */
    atomic_uint_fast32_t prefetch_tracks;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Hash Table Key Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int64_t* make_track_id_key(int64_t track_id) {
    int64_t* key = g_new(int64_t, 1);
    *key = track_id;
    return key;
}

static void free_track_id_key(gpointer key) {
    g_free(key);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static audio_buffer_t* buffer_new(int64_t track_id, const char* path, uint32_t sample_rate) {
    audio_buffer_t* b = g_new0(audio_buffer_t, 1);
    b->track_id = track_id;
    b->path = g_strdup(path);
    b->sample_rate = sample_rate;

    atomic_store(&b->lock_count, 0);
    atomic_store(&b->decode_complete, false);
    atomic_store(&b->decode_cancelled, false);
    atomic_store(&b->decode_failed, false);

    g_cond_init(&b->ready_cond);
    g_mutex_init(&b->ready_mutex);

    return b;
}

static void buffer_free(audio_buffer_t* b) {
    if (!b) return;
    g_cond_clear(&b->ready_cond);
    g_mutex_clear(&b->ready_mutex);
    g_free(b->path);
    g_free(b->samples);
    g_free(b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LRU Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static void touch_buffer(audio_cache_t* cache, audio_buffer_t* buffer) {
    GList* link = buffer->_lru_link;
    if (link) {
        g_queue_unlink(&cache->lru, link);
        g_queue_push_head_link(&cache->lru, link);
    }
}

static void evict_lru(audio_cache_t* cache) {
    uint32_t queue_len = g_queue_get_length(&cache->lru);
    uint32_t iterations = 0;

    while (cache->memory_used > cache->memory_limit &&
           !g_queue_is_empty(&cache->lru) &&
           iterations < queue_len) {

        GList* tail = g_queue_peek_tail_link(&cache->lru);
        if (!tail) break;

        audio_buffer_t* buffer = tail->data;
        iterations++;

        /* Skip locked or still-decoding buffers */
        if (atomic_load(&buffer->lock_count) > 0 ||
            !atomic_load(&buffer->decode_complete)) {
            g_queue_unlink(&cache->lru, tail);
            g_queue_push_head_link(&cache->lru, tail);
            continue;
        }

        /* Evict this buffer from cache */
        cache->memory_used -= buffer->memory_bytes;

        g_debug("audio_cache: evicting track %" G_GINT64_FORMAT " (%.1f MB)",
                buffer->track_id, buffer->memory_bytes / (1024.0 * 1024.0));

        g_hash_table_remove(cache->buffers, &buffer->track_id);
        g_queue_delete_link(&cache->lru, tail);
        buffer_free(buffer);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Decode Worker
 * ═══════════════════════════════════════════════════════════════════════════ */

static void decode_worker(gpointer data, gpointer user_data) {
    decode_task_t* task = data;
    audio_cache_t* cache = task->cache;
    audio_buffer_t* buffer = task->buffer;
    bool success = false;

    g_message("audio_cache: decoding track %" G_GINT64_FORMAT " (%s)",
              buffer->track_id, buffer->path);

    ffmpeg_decoder_t dec;
    quadrature_result_t r = ffmpeg_decoder_open(&dec, buffer->path, buffer->sample_rate);
    if (r != QUADRATURE_OK) {
        g_critical("audio_cache: cannot open %s", buffer->path);
        atomic_store(&buffer->decode_failed, true);
        goto cleanup;
    }

    uint64_t total_frames = ffmpeg_decoder_duration(&dec);
    if (total_frames == 0) {
        g_critical("audio_cache: cannot determine duration for %s", buffer->path);
        ffmpeg_decoder_close(&dec);
        atomic_store(&buffer->decode_failed, true);
        goto cleanup;
    }

    /* Add 10% buffer for resampler flush */
    size_t buffer_frames = total_frames + total_frames / 10;
    size_t buffer_size = buffer_frames * 2 * sizeof(float);

    buffer->samples = g_malloc(buffer_size);
    if (!buffer->samples) {
        g_critical("audio_cache: cannot allocate %.1f MB for %s",
                   buffer_size / (1024.0 * 1024.0), buffer->path);
        ffmpeg_decoder_close(&dec);
        atomic_store(&buffer->decode_failed, true);
        goto cleanup;
    }
    buffer->memory_bytes = buffer_size;

    /* Decode loop */
    uint64_t decoded = 0;

    while (!atomic_load(&buffer->decode_cancelled)) {
        size_t chunk_size = 4096;
        if (decoded + chunk_size > buffer_frames) {
            chunk_size = buffer_frames - decoded;
        }
        if (chunk_size == 0) break;

        float* out_ptr = buffer->samples + decoded * 2;
        int frames_read = ffmpeg_decoder_read(&dec, out_ptr, chunk_size);

        if (frames_read < 0) {
            ffmpeg_decoder_close(&dec);
            atomic_store(&buffer->decode_failed, true);
            goto cleanup;
        }

        if (frames_read == 0) {
            /* EOF */
            break;
        }

        decoded += frames_read;

        if (decoded >= buffer_frames - 1024) {
            g_warning("audio_cache: decode buffer nearly full, stopping");
            break;
        }
    }

    ffmpeg_decoder_close(&dec);

    if (atomic_load(&buffer->decode_cancelled)) {
        g_message("audio_cache: decode cancelled for track %" G_GINT64_FORMAT, buffer->track_id);
        goto cleanup;
    }

    buffer->num_frames = decoded;
    buffer->memory_bytes = decoded * 2 * sizeof(float);
    atomic_store(&buffer->decode_complete, true);

    /* Signal waiters that decode is complete */
    g_mutex_lock(&buffer->ready_mutex);
    g_cond_broadcast(&buffer->ready_cond);
    g_mutex_unlock(&buffer->ready_mutex);

    success = true;

    /* Record decode event */
    uint64_t decode_time_ms = time_ms() - task->start_time_ms;

    g_mutex_lock(&cache->event_lock);
    {
        uint32_t head = atomic_load(&cache->event_head);
        audio_cache_decode_event_t* event = &cache->decode_events[head];

        event->track_id = buffer->track_id;
        event->decode_duration_ms = (uint32_t)decode_time_ms;
        event->timestamp_ms = task->start_time_ms;

        /* Get file size from stat */
        struct stat st;
        if (stat(buffer->path, &st) == 0) {
            event->file_size = (uint64_t)st.st_size;
        } else {
            event->file_size = 0;
        }

        /* Calculate audio duration from decoded frames */
        event->audio_duration_ms = (uint32_t)((decoded * 1000) / buffer->sample_rate);

        /* Extract file extension */
        const char* dot = strrchr(buffer->path, '.');
        if (dot && dot[1]) {
            strncpy(event->filetype, dot + 1, sizeof(event->filetype) - 1);
            event->filetype[sizeof(event->filetype) - 1] = '\0';
            /* Lowercase the extension */
            for (char* p = event->filetype; *p; p++) {
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            }
        } else {
            event->filetype[0] = '\0';
        }

        /* Advance ring buffer */
        atomic_store(&cache->event_head, (head + 1) % AUDIO_CACHE_MAX_DECODE_EVENTS);
        uint32_t count = atomic_load(&cache->event_count);
        if (count < AUDIO_CACHE_MAX_DECODE_EVENTS) {
            atomic_store(&cache->event_count, count + 1);
        }
    }
    g_mutex_unlock(&cache->event_lock);

    g_message("audio_cache: decoded track %" G_GINT64_FORMAT " (%" G_GUINT64_FORMAT " frames, %.1f MB, %" G_GUINT64_FORMAT " ms)",
              buffer->track_id, decoded, buffer->memory_bytes / (1024.0 * 1024.0), decode_time_ms);

cleanup:
    if (success) {
        g_mutex_lock(&cache->lock);
        cache->memory_used += buffer->memory_bytes;
        evict_lru(cache);
        g_mutex_unlock(&cache->lock);
    } else {
        /* Signal waiters that decode failed before freeing buffer */
        if (buffer) {
            g_mutex_lock(&buffer->ready_mutex);
            g_cond_broadcast(&buffer->ready_cond);
            g_mutex_unlock(&buffer->ready_mutex);
        }

        g_mutex_lock(&cache->lock);
        GList* link = buffer->_lru_link;
        if (link) {
            g_queue_delete_link(&cache->lru, link);
        }
        g_hash_table_remove(cache->buffers, &buffer->track_id);
        g_mutex_unlock(&cache->lock);
        buffer_free(buffer);
        buffer = NULL;
    }

    g_mutex_lock(&cache->decode_lock);
    if (task->buffer)
        g_hash_table_remove(cache->pending_decodes, &task->buffer->track_id);
    g_mutex_unlock(&cache->decode_lock);

    g_free(task);
    (void)user_data;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Accessors
 * ═══════════════════════════════════════════════════════════════════════════ */

int64_t audio_buffer_get_track_id(const audio_buffer_t* buf) {
    return buf ? buf->track_id : 0;
}

const float* audio_buffer_get_samples(const audio_buffer_t* buf) {
    return buf ? buf->samples : NULL;
}

uint64_t audio_buffer_get_num_frames(const audio_buffer_t* buf) {
    return buf ? buf->num_frames : 0;
}

uint32_t audio_buffer_get_sample_rate(const audio_buffer_t* buf) {
    return buf ? buf->sample_rate : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_cache_create(library_cache_t* library,
                                        uint32_t sample_rate,
                                        audio_cache_t** out) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    audio_cache_t* cache = g_new0(audio_cache_t, 1);
    g_mutex_init(&cache->lock);
    g_mutex_init(&cache->decode_lock);
    g_mutex_init(&cache->event_lock);

    cache->library = library;  /* May be NULL for testing */
    cache->sample_rate = sample_rate;
    cache->memory_limit = AUDIO_CACHE_DEFAULT_MEMORY_LIMIT;
    cache->memory_used = 0;

    /* Initialize decode events ring buffer */
    memset(cache->decode_events, 0, sizeof(cache->decode_events));
    atomic_store(&cache->event_head, 0);
    atomic_store(&cache->event_count, 0);
    atomic_store(&cache->prefetch_tracks, 0);

    /* Use int64 hash/equal for track_id-based lookup */
    cache->buffers = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        free_track_id_key,  /* key is separate allocation */
        NULL                /* value freed manually */
    );

    g_queue_init(&cache->lru);

    cache->pending_decodes = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        free_track_id_key,
        NULL
    );

    /* Delayed unlock tracking */
    cache->pending_unlocks = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        free_track_id_key,
        NULL  /* GSource IDs stored as GINT_TO_POINTER */
    );

    GError* error = NULL;
    cache->decode_pool = g_thread_pool_new(
        decode_worker,
        cache,
        AUDIO_CACHE_MAX_DECODE_WORKERS,
        FALSE,
        &error
    );

    if (!cache->decode_pool) {
        g_critical("audio_cache: cannot create thread pool: %s",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        g_hash_table_destroy(cache->buffers);
        g_hash_table_destroy(cache->pending_decodes);
        g_hash_table_destroy(cache->pending_unlocks);
        g_mutex_clear(&cache->lock);
        g_mutex_clear(&cache->decode_lock);
        g_free(cache);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_message("audio_cache: created with %.0f MB limit",
              cache->memory_limit / (1024.0 * 1024.0));
    *out = cache;
    return QUADRATURE_OK;
}

void audio_cache_destroy(audio_cache_t* cache) {
    if (!cache) return;

    g_message("audio_cache: destroying");

    audio_cache_cancel_all_loads(cache);

    g_thread_pool_free(cache->decode_pool, TRUE, TRUE);

    /* Cancel all pending delayed unlocks */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, cache->pending_unlocks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        guint source_id = GPOINTER_TO_UINT(value);
        if (source_id > 0) {
            g_source_remove(source_id);
        }
    }
    g_hash_table_destroy(cache->pending_unlocks);

    g_mutex_lock(&cache->lock);

    /* Free all buffers in LRU queue */
    GList* link = cache->lru.head;
    while (link) {
        GList* next = link->next;
        audio_buffer_t* b = link->data;
        if (atomic_load(&b->lock_count) > 0) {
            g_warning("audio_cache: destroying buffer with lock_count=%d (track %" G_GINT64_FORMAT ")",
                      atomic_load(&b->lock_count), b->track_id);
        }
        buffer_free(b);
        link = next;
    }
    g_queue_clear(&cache->lru);

    g_hash_table_destroy(cache->buffers);

    g_mutex_unlock(&cache->lock);

    g_mutex_lock(&cache->decode_lock);
    g_hash_table_destroy(cache->pending_decodes);
    g_mutex_unlock(&cache->decode_lock);

    g_mutex_clear(&cache->lock);
    g_mutex_clear(&cache->decode_lock);
    g_mutex_clear(&cache->event_lock);

    g_free(cache);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Loading API
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_cache_load(audio_cache_t* cache, int64_t track_id) {
    if (!cache || track_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    g_mutex_lock(&cache->lock);
    audio_buffer_t* existing = g_hash_table_lookup(cache->buffers, &track_id);
    if (existing) {
        touch_buffer(cache, existing);
        g_mutex_unlock(&cache->lock);
        return QUADRATURE_OK;
    }
    g_mutex_unlock(&cache->lock);

    g_mutex_lock(&cache->decode_lock);
    if (g_hash_table_contains(cache->pending_decodes, &track_id)) {
        g_mutex_unlock(&cache->decode_lock);
        return QUADRATURE_OK;
    }
    g_mutex_unlock(&cache->decode_lock);

    /* Resolve track_id -> path via LibraryCache */
    const char* path = NULL;
    if (cache->library) {
        const library_track_info_t* track_info = library_cache_get_track(cache->library, track_id);
        if (!track_info || !track_info->path) {
            g_warning("audio_cache: cannot resolve track %" G_GINT64_FORMAT " to path", track_id);
            return QUADRATURE_ERROR_FILE_NOT_FOUND;
        }
        path = track_info->path;
    } else {
        /* No library - cannot resolve path */
        g_warning("audio_cache: no library cache, cannot resolve track %" G_GINT64_FORMAT, track_id);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    audio_buffer_t* buffer = buffer_new(track_id, path, cache->sample_rate);
    buffer->_cache = cache;

    g_mutex_lock(&cache->lock);
    g_hash_table_insert(cache->buffers, make_track_id_key(track_id), buffer);
    g_queue_push_head(&cache->lru, buffer);
    buffer->_lru_link = cache->lru.head;
    g_mutex_unlock(&cache->lock);

    decode_task_t* task = g_new0(decode_task_t, 1);
    task->cache = cache;
    task->buffer = buffer;
    task->start_time_ms = time_ms();

    g_mutex_lock(&cache->decode_lock);
    g_hash_table_insert(cache->pending_decodes, make_track_id_key(track_id), task);
    g_mutex_unlock(&cache->decode_lock);

    GError* error = NULL;
    if (!g_thread_pool_push(cache->decode_pool, task, &error)) {
        g_critical("audio_cache: cannot queue decode task: %s",
                   error ? error->message : "unknown");
        g_clear_error(&error);

        g_mutex_lock(&cache->decode_lock);
        g_hash_table_remove(cache->pending_decodes, &track_id);
        g_mutex_unlock(&cache->decode_lock);

        g_mutex_lock(&cache->lock);
        g_queue_delete_link(&cache->lru, buffer->_lru_link);
        g_hash_table_remove(cache->buffers, &track_id);
        g_mutex_unlock(&cache->lock);

        buffer_free(buffer);
        g_free(task);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_debug("audio_cache: queued decode for track %" G_GINT64_FORMAT, track_id);
    return QUADRATURE_OK;
}

void audio_cache_cancel_load(audio_cache_t* cache, int64_t track_id) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    if (buffer && !atomic_load(&buffer->decode_complete)) {
        atomic_store(&buffer->decode_cancelled, true);
    }
    g_mutex_unlock(&cache->lock);
}

void audio_cache_cancel_all_loads(audio_cache_t* cache) {
    g_assert(cache != NULL);

    g_mutex_lock(&cache->lock);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, cache->buffers);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        audio_buffer_t* buffer = value;
        if (!atomic_load(&buffer->decode_complete)) {
            atomic_store(&buffer->decode_cancelled, true);
        }
    }
    g_mutex_unlock(&cache->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lock/Unlock API (Eviction Protection)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Lock a track to prevent LRU eviction and return its decode status.
 *
 * PRECONDITION: Track must already be in cache (via audio_cache_load()).
 * Crashes if track_id is not found - this indicates a caller bug.
 *
 * @return AUDIO_CACHE_LOCK_READY if buffer is ready for use
 *         AUDIO_CACHE_LOCK_LOADING if decode is in progress (call wait_ready())
 *         AUDIO_CACHE_LOCK_FAILED if decode failed
 */
audio_cache_lock_result_t audio_cache_lock(audio_cache_t* cache, int64_t track_id) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    if (!buffer) {
        g_mutex_unlock(&cache->lock);
        g_error("audio_cache_lock: track %" G_GINT64_FORMAT " not in cache - "
                "call audio_cache_load() first", track_id);
    }
    atomic_fetch_add(&buffer->lock_count, 1);

    audio_cache_lock_result_t result;
    if (atomic_load(&buffer->decode_failed)) {
        result = AUDIO_CACHE_LOCK_FAILED;
    } else if (atomic_load(&buffer->decode_complete)) {
        result = AUDIO_CACHE_LOCK_READY;
    } else {
        result = AUDIO_CACHE_LOCK_LOADING;
    }

    g_debug("audio_cache: locked track %" G_GINT64_FORMAT " (lock_count=%d, status=%d)",
            track_id, atomic_load(&buffer->lock_count), result);
    g_mutex_unlock(&cache->lock);

    return result;
}

/**
 * Unlock a track to allow LRU eviction.
 *
 * PRECONDITION: Track must be in cache and have lock_count > 0.
 * Crashes on underflow (more unlocks than locks) - this indicates a caller bug.
 */
void audio_cache_unlock(audio_cache_t* cache, int64_t track_id) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    if (!buffer) {
        g_mutex_unlock(&cache->lock);
        g_error("audio_cache_unlock: track %" G_GINT64_FORMAT " not in cache", track_id);
    }
    int prev = atomic_fetch_sub(&buffer->lock_count, 1);
    if (prev <= 0) {
        g_mutex_unlock(&cache->lock);
        g_error("audio_cache_unlock: lock_count underflow for track %" G_GINT64_FORMAT
                " (was %d)", track_id, prev);
    }
    g_debug("audio_cache: unlocked track %" G_GINT64_FORMAT " (lock_count=%d)",
            track_id, atomic_load(&buffer->lock_count));
    g_mutex_unlock(&cache->lock);
}

/* Delayed unlock callback data */
typedef struct {
    audio_cache_t* cache;
    int64_t track_id;
} delayed_unlock_data_t;

static gboolean delayed_unlock_callback(gpointer user_data) {
    delayed_unlock_data_t* data = user_data;

    /* Remove from pending unlocks */
    g_hash_table_remove(data->cache->pending_unlocks, &data->track_id);

    /* Perform the actual unlock */
    audio_cache_unlock(data->cache, data->track_id);

    g_debug("audio_cache: delayed unlock completed for track %" G_GINT64_FORMAT, data->track_id);

    g_free(data);
    return G_SOURCE_REMOVE;
}

/**
 * Schedule an unlock after AUDIO_CACHE_UNLOCK_DELAY_MS.
 * Use when transitioning away from a playing track to ensure audio callback
 * finishes reading before eviction is allowed.
 *
 * PRECONDITION: Track must be in cache with lock_count > 0.
 */
void audio_cache_unlock_delayed(audio_cache_t* cache, int64_t track_id) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    /* Cancel any existing pending unlock for this track */
    gpointer existing = g_hash_table_lookup(cache->pending_unlocks, &track_id);
    if (existing) {
        guint source_id = GPOINTER_TO_UINT(existing);
        if (source_id > 0) {
            g_source_remove(source_id);
        }
        g_hash_table_remove(cache->pending_unlocks, &track_id);
    }

    /* Schedule delayed unlock */
    delayed_unlock_data_t* data = g_new(delayed_unlock_data_t, 1);
    data->cache = cache;
    data->track_id = track_id;

    guint source_id = g_timeout_add(AUDIO_CACHE_UNLOCK_DELAY_MS, delayed_unlock_callback, data);
    g_hash_table_insert(cache->pending_unlocks, make_track_id_key(track_id),
                        GUINT_TO_POINTER(source_id));

    g_debug("audio_cache: scheduled delayed unlock for track %" G_GINT64_FORMAT " (%d ms)",
            track_id, AUDIO_CACHE_UNLOCK_DELAY_MS);
}

/**
 * Wait for decode completion on a locked track.
 *
 * Blocks until the decode completes (success or failure) or timeout expires.
 * Uses GCond for instant wakeup when decode finishes - no polling.
 *
 * PRECONDITION: Track must be in cache (via audio_cache_load()) and locked.
 *
 * @param cache      Cache instance
 * @param track_id   Track to wait for
 * @param timeout_ms Maximum time to wait (0 = wait indefinitely)
 * @return true if decode completed successfully, false if failed or timeout
 */
bool audio_cache_wait_ready(audio_cache_t* cache, int64_t track_id, int64_t timeout_ms) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    g_mutex_unlock(&cache->lock);

    if (!buffer) {
        g_warning("audio_cache_wait_ready: track %" G_GINT64_FORMAT " not in cache", track_id);
        return false;
    }

    g_mutex_lock(&buffer->ready_mutex);
    while (!atomic_load(&buffer->decode_complete) &&
           !atomic_load(&buffer->decode_failed)) {
        if (timeout_ms > 0) {
            gint64 end_time = g_get_monotonic_time() + timeout_ms * 1000;
            if (!g_cond_wait_until(&buffer->ready_cond, &buffer->ready_mutex, end_time)) {
                g_mutex_unlock(&buffer->ready_mutex);
                g_debug("audio_cache_wait_ready: timeout waiting for track %" G_GINT64_FORMAT, track_id);
                return false;  /* Timeout */
            }
        } else {
            g_cond_wait(&buffer->ready_cond, &buffer->ready_mutex);
        }
    }
    g_mutex_unlock(&buffer->ready_mutex);

    bool ready = atomic_load(&buffer->decode_complete);
    g_debug("audio_cache_wait_ready: track %" G_GINT64_FORMAT " %s",
            track_id, ready ? "ready" : "failed");
    return ready;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Access (For Locked Tracks)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get buffer for a track that has been loaded and locked.
 *
 * PRECONDITIONS:
 * - Track must be in cache (via audio_cache_load())
 * - Track must be locked (via audio_cache_lock())
 *
 * Returns NULL if decode is still in progress (LOADING state).
 * Crashes if preconditions are violated.
 */
audio_buffer_t* audio_cache_get_locked(audio_cache_t* cache, int64_t track_id) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);

    if (!buffer) {
        g_mutex_unlock(&cache->lock);
        g_error("audio_cache_get_locked: track %" G_GINT64_FORMAT " not in cache - "
                "call audio_cache_load() first", track_id);
    }

    if (atomic_load(&buffer->lock_count) <= 0) {
        g_mutex_unlock(&cache->lock);
        g_error("audio_cache_get_locked: track %" G_GINT64_FORMAT " not locked - "
                "call audio_cache_lock() first", track_id);
    }

    /* Decode still in progress - return NULL (caller should retry later) */
    if (!atomic_load(&buffer->decode_complete)) {
        g_mutex_unlock(&cache->lock);
        return NULL;
    }

    touch_buffer(cache, buffer);

    g_mutex_unlock(&cache->lock);
    return buffer;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Prefetch API
 * ═══════════════════════════════════════════════════════════════════════════ */

void audio_cache_prefetch(audio_cache_t* cache,
                          const int64_t* track_ids,
                          size_t count) {
    if (!cache || !track_ids || count == 0 || !cache->library) return;

    /* Track prefetch calls for statistics */
    atomic_fetch_add(&cache->prefetch_tracks, (uint32_t)count);

    /* Delegate to LibraryCache for actual prefetch syscalls */
    library_cache_prefetch_audio_files(cache->library, track_ids, count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Status Query
 * ═══════════════════════════════════════════════════════════════════════════ */

audio_cache_status_t audio_cache_get_status(audio_cache_t* cache, int64_t track_id) {
    if (!cache || track_id <= 0) return AUDIO_CACHE_NOT_FOUND;

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);

    if (!buffer) {
        g_mutex_unlock(&cache->lock);
        return AUDIO_CACHE_NOT_FOUND;
    }

    audio_cache_status_t status;
    if (atomic_load(&buffer->decode_failed)) {
        status = AUDIO_CACHE_FAILED;
    } else if (atomic_load(&buffer->decode_complete)) {
        status = AUDIO_CACHE_READY;
    } else {
        status = AUDIO_CACHE_LOADING;
    }

    g_mutex_unlock(&cache->lock);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Cache Management
 * ═══════════════════════════════════════════════════════════════════════════ */

void audio_cache_evict(audio_cache_t* cache, int64_t track_id) {
    g_assert(cache != NULL);
    g_assert(track_id > 0);

    g_mutex_lock(&cache->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    if (!buffer) {
        g_mutex_unlock(&cache->lock);
        return;
    }

    /* Cannot evict locked buffers */
    if (atomic_load(&buffer->lock_count) > 0) {
        g_mutex_unlock(&cache->lock);
        return;
    }

    cache->memory_used -= buffer->memory_bytes;

    GList* link = buffer->_lru_link;
    if (link) {
        g_queue_delete_link(&cache->lru, link);
    }
    g_hash_table_remove(cache->buffers, &track_id);

    g_mutex_unlock(&cache->lock);

    buffer_free(buffer);
}

void audio_cache_clear(audio_cache_t* cache) {
    g_assert(cache != NULL);

    audio_cache_cancel_all_loads(cache);

    g_mutex_lock(&cache->lock);

    GList* link = cache->lru.head;
    while (link) {
        GList* next = link->next;
        audio_buffer_t* buffer = link->data;

        /* Skip locked buffers */
        if (atomic_load(&buffer->lock_count) > 0) {
            link = next;
            continue;
        }

        cache->memory_used -= buffer->memory_bytes;
        g_hash_table_remove(cache->buffers, &buffer->track_id);
        g_queue_delete_link(&cache->lru, link);
        buffer_free(buffer);

        link = next;
    }

    g_mutex_unlock(&cache->lock);
}

void audio_cache_set_memory_limit(audio_cache_t* cache, size_t memory_limit) {
    g_assert(cache != NULL);

    g_mutex_lock(&cache->lock);
    cache->memory_limit = memory_limit;
    evict_lru(cache);
    g_mutex_unlock(&cache->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t audio_cache_get_memory_used(audio_cache_t* cache) {
    g_assert(cache != NULL);

    g_mutex_lock(&cache->lock);
    size_t used = cache->memory_used;
    g_mutex_unlock(&cache->lock);

    return used;
}

void audio_cache_get_stats(audio_cache_t* cache, audio_cache_stats_t* stats) {
    g_assert(cache != NULL);
    g_assert(stats != NULL);

    memset(stats, 0, sizeof(*stats));

    /* Memory usage percentage */
    g_mutex_lock(&cache->lock);
    if (cache->memory_limit > 0) {
        stats->memory_usage_pct = ((float)cache->memory_used / (float)cache->memory_limit) * 100.0f;
    }

    /* Calculate total cached audio duration */
    uint64_t total_frames = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, cache->buffers);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        audio_buffer_t* buffer = value;
        if (atomic_load(&buffer->decode_complete)) {
            total_frames += buffer->num_frames;
        }
    }
    stats->cached_buffer_seconds = (uint32_t)(total_frames / cache->sample_rate);
    g_mutex_unlock(&cache->lock);

    /* Prefetch counter */
    stats->prefetch_tracks = atomic_load(&cache->prefetch_tracks);

    /* Event count */
    stats->event_count = atomic_load(&cache->event_count);
}

size_t audio_cache_get_count(audio_cache_t* cache) {
    if (!cache) return 0;

    g_mutex_lock(&cache->lock);
    size_t count = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, cache->buffers);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        audio_buffer_t* buffer = value;
        if (atomic_load(&buffer->decode_complete)) {
            count++;
        }
    }
    g_mutex_unlock(&cache->lock);

    return count;
}

uint32_t audio_cache_get_decode_events(audio_cache_t* cache,
                                        audio_cache_decode_event_t* out_events,
                                        uint32_t max_events) {
    if (!cache || !out_events || max_events == 0) return 0;

    g_mutex_lock(&cache->event_lock);

    uint32_t count = atomic_load(&cache->event_count);
    uint32_t to_copy = (count < max_events) ? count : max_events;

    if (to_copy > 0) {
        uint32_t head = atomic_load(&cache->event_head);
        /* Start from oldest event */
        uint32_t start = (count < AUDIO_CACHE_MAX_DECODE_EVENTS)
                       ? 0
                       : head;

        /* Copy events in chronological order (oldest first) */
        for (uint32_t i = 0; i < to_copy; i++) {
            /* Copy from the end (most recent) if we're limiting */
            uint32_t src_offset = (count <= max_events) ? i : (count - max_events + i);
            uint32_t idx = (start + src_offset) % AUDIO_CACHE_MAX_DECODE_EVENTS;
            out_events[i] = cache->decode_events[idx];
        }
    }

    g_mutex_unlock(&cache->event_lock);

    return to_copy;
}
