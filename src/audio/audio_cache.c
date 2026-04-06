/**
 * Quadrature Audio Cache Implementation
 *
 * Thread-safe LRU cache for fully decoded audio buffers.
 * Background decoding via GThreadPool, lock-based eviction protection.
 * Uses track_id as key with LibraryCache for path resolution.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/library.h"
#include "quadrature/settings.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

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
    int64_t last_access_us;                 /* Monotonic timestamp of last use */

    /* Thread-safe state */
    atomic_int lock_count;                  /* Eviction protection */
    atomic_bool decode_complete;
    atomic_bool decode_cancelled;
    atomic_bool decode_failed;

    /* Per-track loudness envelope (computed during decode) */
    float loudness[LOUDNESS_BINS];
    atomic_bool loudness_ready;

    /* Internal (do not access directly) */
    void* _lru_link;
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

    /* Delayed unlock tracking: track_id -> GSource timeout ID */
    GHashTable* pending_unlocks;
    uint32_t unlock_delay_ms;  /* Derived from audio quantum; see audio_cache_compute_unlock_delay */

    /* Decode events ring buffer (for statistics) */
    audio_cache_decode_event_t decode_events[AUDIO_CACHE_MAX_DECODE_EVENTS];
    atomic_uint_fast32_t event_head;   /* Next write position */
    atomic_uint_fast32_t event_count;  /* Total events (capped at MAX) */
    GMutex event_lock;                 /* Protects event writes */

    /* TTL sweep: evicts idle buffers periodically */
    guint sweep_timer_id;
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
    memset(b->loudness, 0, sizeof(b->loudness));
    atomic_store(&b->loudness_ready, false);

    return b;
}

static void buffer_free(audio_buffer_t* b) {
    if (!b) return;
    g_free(b->path);
    g_free(b->samples);
    g_free(b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LRU Management
 * ═══════════════════════════════════════════════════════════════════════════ */

static void touch_buffer(audio_cache_t* cache, audio_buffer_t* buffer) {
    buffer->last_access_us = g_get_monotonic_time();
    GList* link = buffer->_lru_link;
    if (link) {
        g_queue_unlink(&cache->lru, link);
        g_queue_push_head_link(&cache->lru, link);
    }
}

static void evict_lru(audio_cache_t* cache) {
    /* Walk from tail (oldest) toward head. Locked/loading buffers stay in place
     * to preserve their true recency — moving them to head would give them
     * artificial freshness and waste cache space after unlock. */
    GList* cursor = g_queue_peek_tail_link(&cache->lru);
    uint32_t scanned = 0;
    uint32_t queue_len = g_queue_get_length(&cache->lru);

    while (cache->memory_used > cache->memory_limit &&
           cursor != NULL &&
           scanned < queue_len) {

        GList* prev = cursor->prev; /* save before potential deletion */
        audio_buffer_t* buffer = cursor->data;
        scanned++;

        /* Skip locked or still-decoding buffers in place */
        if (atomic_load(&buffer->lock_count) > 0 ||
            !atomic_load(&buffer->decode_complete)) {
            cursor = prev;
            continue;
        }

        /* Evict this buffer from cache */
        cache->memory_used -= buffer->memory_bytes;

        g_debug("audio_cache: evicting track %" G_GINT64_FORMAT " (%.1f MB)",
                buffer->track_id, buffer->memory_bytes / (1024.0 * 1024.0));

        g_hash_table_remove(cache->buffers, &buffer->track_id);
        g_queue_delete_link(&cache->lru, cursor);
        buffer_free(buffer);

        cursor = prev;
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

    g_debug("audio_cache: decoding track %" G_GINT64_FORMAT " (%s)",
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
    buffer->memory_bytes = buffer_size;

    /* Decode loop — accumulates per-bin RMS energy for waveform display.
     * RMS reveals energy density (vs peak which saturates on compressed audio). */
    uint64_t decoded = 0;
    double bin_energy[LOUDNESS_BINS];  /* sum of squares — double to avoid float precision loss */
    int    bin_counts[LOUDNESS_BINS];  /* sample count per bin */
    memset(bin_energy, 0, sizeof(bin_energy));
    memset(bin_counts, 0, sizeof(bin_counts));

    /* Pre-compute bin boundaries — monotonic scan replaces per-sample division */
    uint64_t bin_edges[LOUDNESS_BINS + 1];
    for (int b = 0; b <= LOUDNESS_BINS; b++)
        bin_edges[b] = (uint64_t)b * total_frames / LOUDNESS_BINS;
    int cur_bin = 0;

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

        /* Accumulate RMS energy per bin.  cur_bin advances monotonically
         * (amortized O(1) per sample), no per-sample division. */
        for (int i = 0; i < frames_read; i++) {
            uint64_t abs_frame = decoded + (uint64_t)i;
            while (cur_bin < LOUDNESS_BINS - 1 && abs_frame >= bin_edges[cur_bin + 1])
                cur_bin++;
            float L = out_ptr[i * 2];
            float R = out_ptr[i * 2 + 1];
            float sample = fmaxf(fabsf(L), fabsf(R));
            bin_energy[cur_bin] += (double)(sample * sample);
            bin_counts[cur_bin] += 1;
        }

        decoded += frames_read;

        if (decoded >= buffer_frames - 1024) {
            g_warning("audio_cache: decode buffer nearly full, stopping");
            break;
        }
    }

    ffmpeg_decoder_close(&dec);

    if (atomic_load(&buffer->decode_cancelled)) {
        g_debug("audio_cache: decode cancelled for track %" G_GINT64_FORMAT, buffer->track_id);
        goto cleanup;
    }

    /* Compute RMS per bin, then normalize to 0.0–1.0 relative to max RMS */
    {
        float bin_rms[LOUDNESS_BINS];
        for (int i = 0; i < LOUDNESS_BINS; i++) {
            bin_rms[i] = (bin_counts[i] > 0)
                ? sqrtf((float)(bin_energy[i] / bin_counts[i]))
                : 0.0f;
        }
        float max_rms = 0.0f;
        for (int i = 0; i < LOUDNESS_BINS; i++) {
            if (bin_rms[i] > max_rms) max_rms = bin_rms[i];
        }
        if (max_rms > 0.0f) {
            float inv = 1.0f / max_rms;
            for (int i = 0; i < LOUDNESS_BINS; i++)
                buffer->loudness[i] = bin_rms[i] * inv;
        }
        atomic_store(&buffer->loudness_ready, true);
    }

    buffer->num_frames = decoded;
    buffer->memory_bytes = decoded * 2 * sizeof(float);
    buffer->last_access_us = g_get_monotonic_time();
    atomic_store(&buffer->decode_complete, true);

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

    g_debug("audio_cache: decoded track %" G_GINT64_FORMAT " (%" G_GUINT64_FORMAT " frames, %.1f MB, %" G_GUINT64_FORMAT " ms)",
            buffer->track_id, decoded, buffer->memory_bytes / (1024.0 * 1024.0), decode_time_ms);

cleanup:
    if (success) {
        g_mutex_lock(&cache->lock);
        cache->memory_used += buffer->memory_bytes;
        evict_lru(cache);
        g_mutex_unlock(&cache->lock);
    } else {
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

const float* audio_buffer_get_loudness(const audio_buffer_t* buf) {
    return (buf && atomic_load(&buf->loudness_ready)) ? buf->loudness : NULL;
}

bool audio_buffer_is_loudness_ready(const audio_buffer_t* buf) {
    return buf ? atomic_load(&buf->loudness_ready) : false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TTL Sweep Timer — evicts idle buffers every 10s
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AUDIO_CACHE_SWEEP_INTERVAL_MS  10000      /* 10 seconds */
#define AUDIO_CACHE_TTL_US             (60 * G_USEC_PER_SEC)  /* 60 seconds */

static gboolean sweep_timer_cb(gpointer data) {
    audio_cache_t* cache = data;
    audio_cache_sweep_stale(cache, AUDIO_CACHE_TTL_US);
    return G_SOURCE_CONTINUE;
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
    g_mutex_init(&cache->event_lock);

    cache->library = library;  /* May be NULL for testing */
    cache->sample_rate = sample_rate;
    cache->memory_limit = AUDIO_CACHE_DEFAULT_MEMORY_LIMIT;
    cache->memory_used = 0;
    cache->unlock_delay_ms = audio_cache_compute_unlock_delay(
        APP_SETTINGS_DEFAULT_QUANTUM, sample_rate);

    /* Initialize decode events ring buffer */
    memset(cache->decode_events, 0, sizeof(cache->decode_events));
    atomic_store(&cache->event_head, 0);
    atomic_store(&cache->event_count, 0);

    /* Use int64 hash/equal for track_id-based lookup */
    cache->buffers = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        free_track_id_key,  /* key is separate allocation */
        NULL                /* value freed manually */
    );

    g_queue_init(&cache->lru);

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
        g_hash_table_destroy(cache->pending_unlocks);
        g_mutex_clear(&cache->lock);
        g_free(cache);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Start periodic TTL sweep (evicts idle buffers) */
    cache->sweep_timer_id = g_timeout_add(AUDIO_CACHE_SWEEP_INTERVAL_MS, sweep_timer_cb, cache);

    g_message("audio_cache: created with %.0f MB limit, TTL=%ds",
              cache->memory_limit / (1024.0 * 1024.0),
              (int)(AUDIO_CACHE_TTL_US / G_USEC_PER_SEC));
    *out = cache;
    return QUADRATURE_OK;
}

void audio_cache_destroy(audio_cache_t* cache) {
    if (!cache) return;

    g_message("audio_cache: destroying");

    /* Stop TTL sweep timer */
    if (cache->sweep_timer_id > 0) {
        g_source_remove(cache->sweep_timer_id);
        cache->sweep_timer_id = 0;
    }

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

    g_mutex_clear(&cache->lock);
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

    /* Resolve track_id -> absolute path via LibraryCache */
    if (!cache->library) {
        g_warning("audio_cache: no library cache, cannot resolve track %" G_GINT64_FORMAT, track_id);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }
    char* resolved_path = library_cache_resolve_track_path(cache->library, track_id);
    if (!resolved_path) {
        g_warning("audio_cache: cannot resolve track %" G_GINT64_FORMAT " to path", track_id);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    audio_buffer_t* buffer = buffer_new(track_id, resolved_path, cache->sample_rate);
    g_free(resolved_path);

    g_mutex_lock(&cache->lock);
    g_hash_table_insert(cache->buffers, make_track_id_key(track_id), buffer);
    g_queue_push_head(&cache->lru, buffer);
    buffer->_lru_link = cache->lru.head;
    g_mutex_unlock(&cache->lock);

    decode_task_t* task = g_new0(decode_task_t, 1);
    task->cache = cache;
    task->buffer = buffer;
    task->start_time_ms = time_ms();

    GError* error = NULL;
    if (!g_thread_pool_push(cache->decode_pool, task, &error)) {
        g_critical("audio_cache: cannot queue decode task: %s",
                   error ? error->message : "unknown");
        g_clear_error(&error);

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

    guint source_id = g_timeout_add(cache->unlock_delay_ms, delayed_unlock_callback, data);
    g_hash_table_insert(cache->pending_unlocks, make_track_id_key(track_id),
                        GUINT_TO_POINTER(source_id));

    g_debug("audio_cache: scheduled delayed unlock for track %" G_GINT64_FORMAT " (%u ms)",
            track_id, cache->unlock_delay_ms);
}

void audio_cache_set_quantum(audio_cache_t* cache, uint32_t quantum_frames) {
    g_assert(cache != NULL);
    cache->unlock_delay_ms = audio_cache_compute_unlock_delay(
        quantum_frames, cache->sample_rate);
    g_message("audio_cache: unlock delay updated to %u ms (quantum=%u)",
              cache->unlock_delay_ms, quantum_frames);
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

void audio_cache_sweep_stale(audio_cache_t* cache, int64_t max_age_us) {
    g_assert(cache != NULL);

    int64_t now = g_get_monotonic_time();
    int64_t cutoff = now - max_age_us;

    g_mutex_lock(&cache->lock);

    /* Walk from LRU tail (oldest) — stop once we hit a fresh buffer since
     * the LRU order approximates access-time order. */
    GList* link = g_queue_peek_tail_link(&cache->lru);
    while (link) {
        GList* prev = link->prev;
        audio_buffer_t* buffer = link->data;

        /* Skip locked (in-use) or still-decoding buffers */
        if (atomic_load(&buffer->lock_count) > 0 ||
            !atomic_load(&buffer->decode_complete)) {
            link = prev;
            continue;
        }

        /* Stop at first buffer newer than cutoff — everything above is fresher */
        if (buffer->last_access_us > cutoff)
            break;

        g_debug("audio_cache: TTL evicting track %" G_GINT64_FORMAT
                " (idle %.1fs, %.1f MB)",
                buffer->track_id,
                (double)(now - buffer->last_access_us) / 1e6,
                buffer->memory_bytes / (1024.0 * 1024.0));

        cache->memory_used -= buffer->memory_bytes;
        g_hash_table_remove(cache->buffers, &buffer->track_id);
        g_queue_delete_link(&cache->lru, link);
        buffer_free(buffer);

        link = prev;
    }

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
