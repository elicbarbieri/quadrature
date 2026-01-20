/**
 * Quadrature Audio Buffer Store Implementation
 *
 * Thread-safe LRU store for fully decoded audio buffers.
 * Background decoding via GThreadPool, lock-free access for audio callback.
 * Uses file path as key (no track_id dependency).
 */

#include "internal.h"
#include "quadrature/audio/audio_buffer_store.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_buffer_store_t* store;
    audio_buffer_t* buffer;
    audio_buffer_decode_callback_t callback;
    void* user_data;
} decode_task_t;

struct audio_buffer {
    char* path;                             /* Canonical path (key) */
    float* samples;                         /* Interleaved stereo float samples */
    uint64_t num_frames;
    uint32_t sample_rate;
    size_t memory_bytes;

    /* Thread-safe state */
    atomic_int ref_count;
    atomic_bool decode_complete;
    atomic_uint_fast64_t decoded_frames;    /* Progress tracking */
    atomic_uint_fast64_t total_frames;      /* Expected total for progress calc */
    atomic_bool decode_cancelled;
    atomic_bool decode_failed;

    /* Internal (do not access directly) */
    void* _lru_link;
    void* _store;
};

struct audio_buffer_store {
    GHashTable* buffers;       /* path (string) -> audio_buffer_t* */
    GQueue lru;
    GMutex lock;

    uint32_t sample_rate;
    size_t memory_used;
    size_t memory_limit;

    GThreadPool* decode_pool;
    GHashTable* pending_decodes;  /* path (string) -> decode_task_t* */
    GMutex decode_lock;

    /* Deferred destruction queue for buffers with non-zero ref_count */
    GQueue deferred_destroy;

    atomic_uint_fast64_t hits;
    atomic_uint_fast64_t misses;
    atomic_uint_fast64_t evictions;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static audio_buffer_t* buffer_new(const char* path, uint32_t sample_rate) {
    audio_buffer_t* b = g_new0(audio_buffer_t, 1);
    b->path = g_strdup(path);
    b->sample_rate = sample_rate;

    atomic_store(&b->ref_count, 0);
    atomic_store(&b->decode_complete, false);
    atomic_store(&b->decoded_frames, 0);
    atomic_store(&b->total_frames, 0);
    atomic_store(&b->decode_cancelled, false);
    atomic_store(&b->decode_failed, false);

    return b;
}

/**
 * Free a buffer immediately. Only call when ref_count is guaranteed to be 0.
 */
static void buffer_free_immediate(audio_buffer_t* b) {
    if (!b) return;
    g_free(b->path);
    g_free(b->samples);
    g_free(b);
}

/**
 * Process deferred destruction queue, freeing buffers that are no longer referenced.
 * Call with store->lock held.
 */
static void cleanup_deferred(audio_buffer_store_t* store) {
    GList* link = store->deferred_destroy.head;
    while (link) {
        GList* next = link->next;
        audio_buffer_t* b = link->data;

        if (atomic_load(&b->ref_count) == 0) {
            g_queue_delete_link(&store->deferred_destroy, link);
            buffer_free_immediate(b);
        }
        link = next;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LRU Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static void touch_buffer(audio_buffer_store_t* store, audio_buffer_t* buffer) {
    GList* link = buffer->_lru_link;
    if (link) {
        g_queue_unlink(&store->lru, link);
        g_queue_push_head_link(&store->lru, link);
    }
}

static void evict_lru(audio_buffer_store_t* store) {
    /* Process any previously deferred buffers first */
    cleanup_deferred(store);

    uint32_t queue_len = g_queue_get_length(&store->lru);
    uint32_t iterations = 0;

    while (store->memory_used > store->memory_limit &&
           !g_queue_is_empty(&store->lru) &&
           iterations < queue_len) {

        GList* tail = g_queue_peek_tail_link(&store->lru);
        if (!tail) break;

        audio_buffer_t* buffer = tail->data;
        iterations++;

        /* Skip in-use or still-decoding buffers */
        if (atomic_load(&buffer->ref_count) > 0 ||
            !atomic_load(&buffer->decode_complete)) {
            g_queue_unlink(&store->lru, tail);
            g_queue_push_head_link(&store->lru, tail);
            continue;
        }

        /* Evict this buffer */
        store->memory_used -= buffer->memory_bytes;
        atomic_fetch_add(&store->evictions, 1);

        g_debug("audio_buffer_store: evicting %s (%.1f MB)",
                buffer->path, buffer->memory_bytes / (1024.0 * 1024.0));

        g_hash_table_remove(store->buffers, buffer->path);
        g_queue_delete_link(&store->lru, tail);
        buffer_free_immediate(buffer);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Decode Worker
 * ═══════════════════════════════════════════════════════════════════════════ */

static void decode_worker(gpointer data, gpointer user_data) {
    decode_task_t* task = data;
    audio_buffer_store_t* store = task->store;
    audio_buffer_t* buffer = task->buffer;
    bool success = false;

    g_message("audio_buffer_store: decoding %s", buffer->path);

    ffmpeg_decoder_t dec;
    quadrature_result_t r = ffmpeg_decoder_open(&dec, buffer->path, buffer->sample_rate);
    if (r != QUADRATURE_OK) {
        g_critical("audio_buffer_store: cannot open %s", buffer->path);
        atomic_store(&buffer->decode_failed, true);
        goto cleanup;
    }

    uint64_t total_frames = ffmpeg_decoder_duration(&dec);
    if (total_frames == 0) {
        g_critical("audio_buffer_store: cannot determine duration for %s", buffer->path);
        ffmpeg_decoder_close(&dec);
        atomic_store(&buffer->decode_failed, true);
        goto cleanup;
    }

    /* Store total for progress calculation */
    atomic_store(&buffer->total_frames, total_frames);

    /* Add 10% buffer for resampler flush */
    size_t buffer_frames = total_frames + total_frames / 10;
    size_t buffer_size = buffer_frames * 2 * sizeof(float);

    buffer->samples = g_malloc(buffer_size);
    if (!buffer->samples) {
        g_critical("audio_buffer_store: cannot allocate %.1f MB for %s",
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
        atomic_store(&buffer->decoded_frames, decoded);

        if (decoded >= buffer_frames - 1024) {
            g_warning("audio_buffer_store: decode buffer nearly full, stopping");
            break;
        }
    }

    ffmpeg_decoder_close(&dec);

    if (atomic_load(&buffer->decode_cancelled)) {
        g_message("audio_buffer_store: decode cancelled for %s", buffer->path);
        goto cleanup;
    }

    buffer->num_frames = decoded;
    buffer->memory_bytes = decoded * 2 * sizeof(float);
    atomic_store(&buffer->decode_complete, true);
    success = true;

    g_message("audio_buffer_store: decoded %s (%" G_GUINT64_FORMAT " frames, %.1f MB)",
              buffer->path, decoded, buffer->memory_bytes / (1024.0 * 1024.0));

cleanup:
    if (success) {
        g_mutex_lock(&store->lock);
        store->memory_used += buffer->memory_bytes;
        evict_lru(store);
        g_mutex_unlock(&store->lock);
    } else {
        g_mutex_lock(&store->lock);
        GList* link = buffer->_lru_link;
        if (link) {
            g_queue_delete_link(&store->lru, link);
        }
        g_hash_table_remove(store->buffers, buffer->path);
        g_mutex_unlock(&store->lock);
        /* Buffer was just created, ref_count guaranteed 0 */
        buffer_free_immediate(buffer);
        buffer = NULL;
    }

    g_mutex_lock(&store->decode_lock);
    if (task->buffer)
        g_hash_table_remove(store->pending_decodes, task->buffer->path);
    g_mutex_unlock(&store->decode_lock);

    if (task->callback) {
        task->callback(store, task->buffer ? task->buffer->path : "", success, task->user_data);
    }

    g_free(task);
    (void)user_data;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Accessors
 * ═══════════════════════════════════════════════════════════════════════════ */

const float* audio_buffer_get_samples(const audio_buffer_t* buf) {
    return buf ? buf->samples : NULL;
}

uint64_t audio_buffer_get_num_frames(const audio_buffer_t* buf) {
    return buf ? buf->num_frames : 0;
}

uint32_t audio_buffer_get_sample_rate(const audio_buffer_t* buf) {
    return buf ? buf->sample_rate : 0;
}

const char* audio_buffer_get_path(const audio_buffer_t* buf) {
    return buf ? buf->path : NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_buffer_store_create(uint32_t sample_rate,
                                               audio_buffer_store_t** out) {
    return audio_buffer_store_create_with_limit(sample_rate,
                                                 AUDIO_BUFFER_STORE_DEFAULT_MEMORY_LIMIT,
                                                 out);
}

quadrature_result_t audio_buffer_store_create_with_limit(uint32_t sample_rate,
                                                          size_t memory_limit,
                                                          audio_buffer_store_t** out) {
    if (!out) return QUADRATURE_ERROR_INVALID_PARAM;

    audio_buffer_store_t* store = g_new0(audio_buffer_store_t, 1);
    g_mutex_init(&store->lock);
    g_mutex_init(&store->decode_lock);

    store->sample_rate = sample_rate;
    store->memory_limit = memory_limit;
    store->memory_used = 0;

    atomic_store(&store->hits, 0);
    atomic_store(&store->misses, 0);
    atomic_store(&store->evictions, 0);

    /* Use string hash/equal for path-based lookup */
    store->buffers = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        NULL,  /* key is owned by buffer */
        NULL   /* value freed manually */
    );

    g_queue_init(&store->lru);
    g_queue_init(&store->deferred_destroy);

    store->pending_decodes = g_hash_table_new(g_str_hash, g_str_equal);

    GError* error = NULL;
    store->decode_pool = g_thread_pool_new(
        decode_worker,
        store,
        AUDIO_BUFFER_STORE_MAX_DECODE_WORKERS,
        FALSE,
        &error
    );

    if (!store->decode_pool) {
        g_critical("audio_buffer_store: cannot create thread pool: %s",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        g_hash_table_destroy(store->buffers);
        g_hash_table_destroy(store->pending_decodes);
        g_mutex_clear(&store->lock);
        g_mutex_clear(&store->decode_lock);
        g_free(store);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_message("audio_buffer_store: created with %.0f MB limit",
              memory_limit / (1024.0 * 1024.0));
    *out = store;
    return QUADRATURE_OK;
}

void audio_buffer_store_destroy(audio_buffer_store_t* store) {
    if (!store) return;

    g_message("audio_buffer_store: destroying");

    audio_buffer_store_cancel_all_loads(store);

    g_thread_pool_free(store->decode_pool, TRUE, TRUE);

    g_mutex_lock(&store->lock);

    /* Free all buffers in LRU queue */
    GList* link = store->lru.head;
    while (link) {
        GList* next = link->next;
        audio_buffer_t* b = link->data;
        /* During shutdown, warn but proceed if still referenced */
        if (atomic_load(&b->ref_count) > 0) {
            g_warning("audio_buffer_store: destroying buffer with ref_count=%d (%s)",
                      atomic_load(&b->ref_count), b->path);
        }
        buffer_free_immediate(b);
        link = next;
    }
    g_queue_clear(&store->lru);

    /* Free any deferred buffers */
    link = store->deferred_destroy.head;
    while (link) {
        GList* next = link->next;
        buffer_free_immediate(link->data);
        link = next;
    }
    g_queue_clear(&store->deferred_destroy);

    g_hash_table_destroy(store->buffers);

    g_mutex_unlock(&store->lock);

    g_mutex_lock(&store->decode_lock);
    g_hash_table_destroy(store->pending_decodes);
    g_mutex_unlock(&store->decode_lock);

    g_mutex_clear(&store->lock);
    g_mutex_clear(&store->decode_lock);

    g_free(store);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Loading API
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t audio_buffer_store_load(audio_buffer_store_t* store,
                                             const char* path,
                                             audio_buffer_decode_callback_t callback,
                                             void* user_data) {
    if (!store || !path || !path[0]) return QUADRATURE_ERROR_INVALID_PARAM;

    g_mutex_lock(&store->lock);
    audio_buffer_t* existing = g_hash_table_lookup(store->buffers, path);
    if (existing) {
        touch_buffer(store, existing);
        g_mutex_unlock(&store->lock);
        if (callback && atomic_load(&existing->decode_complete)) {
            callback(store, path, true, user_data);
        }
        return QUADRATURE_OK;
    }
    g_mutex_unlock(&store->lock);

    g_mutex_lock(&store->decode_lock);
    if (g_hash_table_contains(store->pending_decodes, path)) {
        g_mutex_unlock(&store->decode_lock);
        return QUADRATURE_OK;
    }
    g_mutex_unlock(&store->decode_lock);

    audio_buffer_t* buffer = buffer_new(path, store->sample_rate);
    buffer->_store = store;

    g_mutex_lock(&store->lock);
    g_hash_table_insert(store->buffers, buffer->path, buffer);
    g_queue_push_head(&store->lru, buffer);
    buffer->_lru_link = store->lru.head;
    g_mutex_unlock(&store->lock);

    decode_task_t* task = g_new0(decode_task_t, 1);
    task->store = store;
    task->buffer = buffer;
    task->callback = callback;
    task->user_data = user_data;

    g_mutex_lock(&store->decode_lock);
    g_hash_table_insert(store->pending_decodes, buffer->path, task);
    g_mutex_unlock(&store->decode_lock);

    GError* error = NULL;
    if (!g_thread_pool_push(store->decode_pool, task, &error)) {
        g_critical("audio_buffer_store: cannot queue decode task: %s",
                   error ? error->message : "unknown");
        g_clear_error(&error);

        g_mutex_lock(&store->decode_lock);
        g_hash_table_remove(store->pending_decodes, path);
        g_mutex_unlock(&store->decode_lock);

        g_mutex_lock(&store->lock);
        g_queue_delete_link(&store->lru, buffer->_lru_link);
        g_hash_table_remove(store->buffers, path);
        g_mutex_unlock(&store->lock);

        /* Buffer was just created, ref_count guaranteed 0 */
        buffer_free_immediate(buffer);
        g_free(task);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_debug("audio_buffer_store: queued decode for %s", path);
    return QUADRATURE_OK;
}

void audio_buffer_store_cancel_load(audio_buffer_store_t* store, const char* path) {
    if (!store || !path) return;

    g_mutex_lock(&store->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(store->buffers, path);
    if (buffer && !atomic_load(&buffer->decode_complete)) {
        atomic_store(&buffer->decode_cancelled, true);
    }
    g_mutex_unlock(&store->lock);
}

void audio_buffer_store_cancel_all_loads(audio_buffer_store_t* store) {
    if (!store) return;

    g_mutex_lock(&store->lock);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, store->buffers);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        audio_buffer_t* buffer = value;
        if (!atomic_load(&buffer->decode_complete)) {
            atomic_store(&buffer->decode_cancelled, true);
        }
    }
    g_mutex_unlock(&store->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buffer Access (Lock-Free for Audio Callback)
 * ═══════════════════════════════════════════════════════════════════════════ */

audio_buffer_t* audio_buffer_store_try_acquire(audio_buffer_store_t* store,
                                                const char* path) {
    if (!store || !path) return NULL;

    g_mutex_lock(&store->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(store->buffers, path);

    if (!buffer || !atomic_load(&buffer->decode_complete)) {
        atomic_fetch_add(&store->misses, 1);
        g_mutex_unlock(&store->lock);
        return NULL;
    }

    atomic_fetch_add(&buffer->ref_count, 1);
    touch_buffer(store, buffer);
    atomic_fetch_add(&store->hits, 1);

    g_mutex_unlock(&store->lock);
    return buffer;
}

void audio_buffer_store_release(audio_buffer_store_t* store, audio_buffer_t* buffer) {
    if (!store || !buffer) return;

    int prev = atomic_fetch_sub(&buffer->ref_count, 1);
    if (prev <= 0) {
        g_warning("audio_buffer_store: release called with ref_count=%d", prev);
        atomic_store(&buffer->ref_count, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Status Queries
 * ═══════════════════════════════════════════════════════════════════════════ */

buffer_status_t audio_buffer_store_get_status(audio_buffer_store_t* store,
                                               const char* path) {
    if (!store || !path) return BUFFER_STATUS_NOT_FOUND;

    g_mutex_lock(&store->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(store->buffers, path);

    if (!buffer) {
        g_mutex_unlock(&store->lock);
        return BUFFER_STATUS_NOT_FOUND;
    }

    buffer_status_t status;
    if (atomic_load(&buffer->decode_failed)) {
        status = BUFFER_STATUS_FAILED;
    } else if (atomic_load(&buffer->decode_complete)) {
        status = BUFFER_STATUS_READY;
    } else {
        status = BUFFER_STATUS_LOADING;
    }

    g_mutex_unlock(&store->lock);
    return status;
}

float audio_buffer_store_get_progress(audio_buffer_store_t* store, const char* path) {
    if (!store || !path) return -1.0f;

    g_mutex_lock(&store->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(store->buffers, path);
    if (!buffer) {
        g_mutex_unlock(&store->lock);
        return -1.0f;
    }

    if (atomic_load(&buffer->decode_failed)) {
        g_mutex_unlock(&store->lock);
        return -1.0f;
    }

    if (atomic_load(&buffer->decode_complete)) {
        g_mutex_unlock(&store->lock);
        return 1.0f;
    }

    uint64_t decoded = atomic_load(&buffer->decoded_frames);
    uint64_t total = atomic_load(&buffer->total_frames);
    float progress = (total > 0) ? (float)decoded / (float)total : 0.0f;
    g_mutex_unlock(&store->lock);

    return progress;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Store Management
 * ═══════════════════════════════════════════════════════════════════════════ */

void audio_buffer_store_evict(audio_buffer_store_t* store, const char* path) {
    if (!store || !path) return;

    g_mutex_lock(&store->lock);
    audio_buffer_t* buffer = g_hash_table_lookup(store->buffers, path);
    if (!buffer) {
        g_mutex_unlock(&store->lock);
        return;
    }

    if (atomic_load(&buffer->ref_count) > 0) {
        g_mutex_unlock(&store->lock);
        return;
    }

    store->memory_used -= buffer->memory_bytes;

    GList* link = buffer->_lru_link;
    if (link) {
        g_queue_delete_link(&store->lru, link);
    }
    g_hash_table_remove(store->buffers, path);

    g_mutex_unlock(&store->lock);

    /* ref_count was checked above, safe to free immediately */
    buffer_free_immediate(buffer);
}

void audio_buffer_store_clear(audio_buffer_store_t* store) {
    if (!store) return;

    audio_buffer_store_cancel_all_loads(store);

    g_mutex_lock(&store->lock);

    GList* link = store->lru.head;
    while (link) {
        GList* next = link->next;
        audio_buffer_t* buffer = link->data;

        if (atomic_load(&buffer->ref_count) == 0) {
            store->memory_used -= buffer->memory_bytes;
            g_hash_table_remove(store->buffers, buffer->path);
            g_queue_delete_link(&store->lru, link);
            buffer_free_immediate(buffer);
        }

        link = next;
    }

    /* Clean up any deferred buffers as well */
    cleanup_deferred(store);

    g_mutex_unlock(&store->lock);
}

void audio_buffer_store_set_memory_limit(audio_buffer_store_t* store,
                                          size_t memory_limit) {
    if (!store) return;

    g_mutex_lock(&store->lock);
    store->memory_limit = memory_limit;
    evict_lru(store);
    g_mutex_unlock(&store->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t audio_buffer_store_get_memory_used(audio_buffer_store_t* store) {
    if (!store) return 0;

    g_mutex_lock(&store->lock);
    size_t used = store->memory_used;
    g_mutex_unlock(&store->lock);

    return used;
}

size_t audio_buffer_store_get_memory_limit(audio_buffer_store_t* store) {
    if (!store) return 0;
    return store->memory_limit;
}

void audio_buffer_store_get_stats(audio_buffer_store_t* store,
                                   uint64_t* hits,
                                   uint64_t* misses,
                                   uint64_t* evictions) {
    if (!store) return;

    if (hits) *hits = atomic_load(&store->hits);
    if (misses) *misses = atomic_load(&store->misses);
    if (evictions) *evictions = atomic_load(&store->evictions);
}

size_t audio_buffer_store_get_count(audio_buffer_store_t* store) {
    if (!store) return 0;

    g_mutex_lock(&store->lock);
    size_t count = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, store->buffers);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        audio_buffer_t* buffer = value;
        if (atomic_load(&buffer->decode_complete)) {
            count++;
        }
    }
    g_mutex_unlock(&store->lock);

    return count;
}
