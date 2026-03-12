/**
 * Album artwork processing: discovery, extraction, thumbnail generation,
 * and packed binary atlas creation.
 *
 * Uses libvips for fast in-memory image resizing (no subprocess overhead).
 * Thread-safe atlas builder supports concurrent album processing.
 */

#define G_LOG_DOMAIN "quadrature"

#include <glib.h>
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <stdatomic.h>

#include <libavformat/avformat.h>
#include <vips/vips.h>

// Image extensions
static const char* IMAGE_EXTS[] = {
    ".jpg", ".jpeg", ".png", ".webp", NULL
};

// Audio extensions for embedded art
static const char* AUDIO_EXTS[] = {
    ".mp3", ".flac", ".m4a", NULL
};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static int has_extension_in_list(const char* filename, const char** ext_list) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    for (const char** e = ext_list; *e; e++) {
        if (strcasecmp(ext, *e) == 0) return 1;
    }
    return 0;
}

#define has_image_extension(f) has_extension_in_list(f, IMAGE_EXTS)
#define has_audio_extension(f) has_extension_in_list(f, AUDIO_EXTS)

static int get_art_priority(const char* filename) {
    static const char* prefixes[] = { "cover", "folder", "front", "albumart" };
    for (int i = 0; i < 4; i++) {
        const char* prefix = prefixes[i];
        size_t len = strlen(prefix);
        int match = 1;
        for (size_t j = 0; j < len && match; j++) {
            if (!filename[j] || tolower((unsigned char)filename[j]) != prefix[j])
                match = 0;
        }
        if (match) return i;
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Artwork Discovery (file-local)
// -----------------------------------------------------------------------------

/**
 * Find the best cover image file in an album directory.
 * Priority: cover > folder > front > albumart.
 * Returns QUADRATURE_OK with the path written to art_path, or
 * QUADRATURE_ERROR_FILE_NOT_FOUND if no art file was found.
 */
quadrature_result_t artwork_find(const char* album_dir, char* art_path,
                                 size_t path_size) {
    DIR* dir = opendir(album_dir);
    if (!dir) return QUADRATURE_ERROR_FILE_NOT_FOUND;

    char best_path[INDEXER_PATH_MAX] = {0};
    int best_priority = 99;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!has_image_extension(entry->d_name)) continue;

        int priority = get_art_priority(entry->d_name);
        if (priority >= 0 && priority < best_priority) {
            snprintf(best_path, sizeof(best_path), "%s/%s", album_dir, entry->d_name);
            best_priority = priority;
            if (priority == 0) break;
        }
    }
    closedir(dir);

    if (best_path[0]) {
        g_snprintf(art_path, path_size, "%s", best_path);
        return QUADRATURE_OK;
    }

    return QUADRATURE_ERROR_FILE_NOT_FOUND;
}

/**
 * Return raw image bytes for the best artwork in an album directory.
 * Tries named cover files first (cover/folder/front/albumart), then falls back
 * to extracting the ATTACHED_PIC stream from audio files (up to 2 attempts).
 * On success, *data_out is g_malloc'd — caller must g_free().
 */
quadrature_result_t artwork_find_bytes(const char* album_dir,
                                       uint8_t** data_out, size_t* size_out) {
    /* Try a named cover file first */
    char art_path[INDEXER_PATH_MAX];
    if (artwork_find(album_dir, art_path, sizeof(art_path)) == QUADRATURE_OK) {
        gchar *contents = NULL;
        gsize length = 0;
        if (g_file_get_contents(art_path, &contents, &length, NULL)) {
            *data_out = (uint8_t *)contents;
            *size_out = length;
            return QUADRATURE_OK;
        }
    }

    /* Fall back to embedded artwork in audio files */
    DIR* dir = opendir(album_dir);
    if (!dir) return QUADRATURE_ERROR_FILE_NOT_FOUND;

    struct dirent* dent;
    int attempts = 0;
    while ((dent = readdir(dir)) != NULL && attempts < 2) {
        if (dent->d_name[0] == '.') continue;
        if (!has_audio_extension(dent->d_name)) continue;
        attempts++;

        char audio_path[INDEXER_PATH_MAX];
        snprintf(audio_path, sizeof(audio_path), "%s/%s", album_dir, dent->d_name);

        AVDictionary* open_opts = NULL;
        av_dict_set(&open_opts, "probesize", "65536", 0);
        av_dict_set(&open_opts, "analyzeduration", "0", 0);

        AVFormatContext* fmt = NULL;
        if (avformat_open_input(&fmt, audio_path, NULL, &open_opts) != 0) {
            av_dict_free(&open_opts);
            continue;
        }
        av_dict_free(&open_opts);
        if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); continue; }

        int pic_stream = -1;
        for (unsigned int i = 0; i < fmt->nb_streams; i++) {
            if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                pic_stream = i; break;
            }
        }

        if (pic_stream >= 0) {
            AVPacket* pkt = &fmt->streams[pic_stream]->attached_pic;
            uint8_t* data = g_malloc(pkt->size);
            memcpy(data, pkt->data, pkt->size);
            *data_out = data;
            *size_out = pkt->size;
            avformat_close_input(&fmt);
            closedir(dir);
            return QUADRATURE_OK;
        }

        avformat_close_input(&fmt);
    }
    closedir(dir);
    return QUADRATURE_ERROR_FILE_NOT_FOUND;
}

// -----------------------------------------------------------------------------
// libvips-based Image Resizing (in-memory, thread-safe)
// -----------------------------------------------------------------------------

/* One-time VIPS initialization flag */
static gint vips_initialized = 0;

/**
 * Initialize libvips (thread-safe, idempotent).
 * Must be called before any VIPS operations.
 */
static void ensure_vips_init(void) {
    if (g_atomic_int_get(&vips_initialized)) {
        return;
    }

    static GMutex init_mutex;
    g_mutex_lock(&init_mutex);
    if (!g_atomic_int_get(&vips_initialized)) {
        if (VIPS_INIT("quadrature") == 0) {
            vips_concurrency_set(1);
            g_atomic_int_set(&vips_initialized, 1);
            g_info("libvips initialized (concurrency=1)");
        } else {
            g_warning("Failed to initialize libvips: %s", vips_error_buffer());
            vips_error_clear();
        }
    }
    g_mutex_unlock(&init_mutex);
}

/**
 * Flatten alpha + write raw RGB pixels from a VipsImage.
 * Shared by both file and buffer resize paths.
 * Unrefs `img` on success or failure. Caller must not unref after calling.
 */
static quadrature_result_t vips_to_raw_rgb(VipsImage* img, int thumb_size,
                                            uint8_t** output_data, size_t* output_size) {
    int bands = vips_image_get_bands(img);

    /* Convert grayscale (1-band) to 3-band sRGB */
    if (bands == 1) {
        VipsImage *rgb = NULL;
        if (vips_colourspace(img, &rgb, VIPS_INTERPRETATION_sRGB, NULL) != 0) {
            g_warning("vips_colourspace (grey→sRGB) failed: %s", vips_error_buffer());
            vips_error_clear();
            g_object_unref(img);
            return QUADRATURE_ERROR_INTERNAL;
        }
        g_object_unref(img);
        img = rgb;
    } else if (bands == 4) {
        /* Flatten alpha channel (RGBA → RGB) */
        VipsImage *flat = NULL;
        if (vips_flatten(img, &flat, NULL) != 0) {
            g_warning("vips_flatten failed: %s", vips_error_buffer());
            vips_error_clear();
            g_object_unref(img);
            return QUADRATURE_ERROR_INTERNAL;
        }
        g_object_unref(img);
        img = flat;
    }

    size_t len = 0;
    void *buf = vips_image_write_to_memory(img, &len);
    g_object_unref(img);

    if (!buf) {
        g_warning("vips_image_write_to_memory failed");
        return QUADRATURE_ERROR_INTERNAL;
    }

    size_t expected = (size_t)thumb_size * thumb_size * ARTWORK_ATLAS_CHANNELS;
    if (len != expected) {
        g_warning("vips_to_raw_rgb: unexpected pixel size %zu (expected %zu)", len, expected);
        g_free(buf);
        return QUADRATURE_ERROR_INTERNAL;
    }

    *output_data = buf;
    *output_size = len;
    return QUADRATURE_OK;
}

/**
 * Resize in-memory image data to raw RGB pixels using vips_thumbnail_buffer.
 * Used for embedded artwork extracted from audio files (already in memory).
 */
static quadrature_result_t vips_resize_buffer(const void* input_data, size_t input_size,
                                               int thumb_size,
                                               uint8_t** output_data, size_t* output_size) {
    ensure_vips_init();

    VipsImage* out = NULL;
    if (vips_thumbnail_buffer((void*)input_data, input_size, &out, thumb_size,
                               "height", thumb_size,
                               "size", VIPS_SIZE_BOTH,
                               "crop", VIPS_INTERESTING_CENTRE,
                               NULL) != 0) {
        g_warning("vips_thumbnail_buffer failed: %s", vips_error_buffer());
        vips_error_clear();
        return QUADRATURE_ERROR_INTERNAL;
    }

    return vips_to_raw_rgb(out, thumb_size, output_data, output_size);
}

// =============================================================================
// Atlas v2 Reader (fixed-stride raw pixel entries)
// =============================================================================

struct artwork_atlas_reader {
    artwork_atlas_header_t header;
    int64_t *album_ids;      // Sorted album ID array (heap-allocated from fread)
    uint8_t *pixel_data;     // All pixel data (heap-allocated from fread)
    uint32_t pixel_stride;   // thumb_size * thumb_size * channels
};

artwork_atlas_reader_t* artwork_atlas_reader_open(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    artwork_atlas_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f); return NULL;
    }

    if (memcmp(header.magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE) != 0 ||
        header.version != ARTWORK_ATLAS_VERSION) {
        fclose(f); return NULL;
    }

    artwork_atlas_reader_t* reader = g_new0(artwork_atlas_reader_t, 1);
    reader->header = header;
    reader->pixel_stride = header.thumb_size * header.thumb_size * header.channels;

    if (header.count > 0) {
        // Read sorted album_id array
        reader->album_ids = g_new(int64_t, header.count);
        if (fread(reader->album_ids, sizeof(int64_t), header.count, f) != header.count) {
            g_free(reader->album_ids); g_free(reader); fclose(f); return NULL;
        }

        // Read all pixel data in one shot
        size_t total_pixels = (size_t)header.count * reader->pixel_stride;
        reader->pixel_data = g_malloc(total_pixels);
        if (fread(reader->pixel_data, 1, total_pixels, f) != total_pixels) {
            g_free(reader->pixel_data); g_free(reader->album_ids);
            g_free(reader); fclose(f); return NULL;
        }
    }

    fclose(f);
    return reader;
}

void artwork_atlas_reader_close(artwork_atlas_reader_t* reader) {
    if (!reader) return;
    g_free(reader->album_ids);
    g_free(reader->pixel_data);
    g_free(reader);
}

size_t artwork_atlas_reader_get_count(artwork_atlas_reader_t* reader) {
    if (!reader) return 0;
    return reader->header.count;
}

uint32_t artwork_atlas_reader_get_pixel_stride(artwork_atlas_reader_t* reader) {
    if (!reader) return 0;
    return reader->pixel_stride;
}

int64_t artwork_atlas_reader_get_album_id_at(artwork_atlas_reader_t* reader, size_t index) {
    if (!reader || !reader->album_ids || index >= reader->header.count) return -1;
    return reader->album_ids[index];
}

const uint8_t* artwork_atlas_reader_get_pixels_at(artwork_atlas_reader_t* reader, size_t index) {
    if (!reader || !reader->pixel_data || index >= reader->header.count) return NULL;
    return reader->pixel_data + index * reader->pixel_stride;
}

// =============================================================================
// Profiling — zero-overhead pipeline timing (vDSO clock_gettime ~25ns/call)
// =============================================================================

static inline int64_t profile_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// =============================================================================
// Thread-Safe Artwork Atlas Builder
// =============================================================================

/* Atlas entry with raw pixel data (used during building) */
typedef struct {
    int64_t album_id;
    uint8_t *pixel_data;
} atlas_build_entry_t;

/* Thread-safe atlas builder */
struct artwork_atlas_builder {
    char *atlas_path;           /* Final atlas path */
    char *temp_path;            /* Temp path for atomic write */
    int thumb_size;             /* Thumbnail size */

    /* Thread-safe entry collection */
    atlas_build_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
    GMutex entries_lock;        /* Protects entries array */

    /* Derived: thumb_size * thumb_size * channels */
    uint32_t pixel_stride;

    /* Pre-generated placeholder pixel data for albums without artwork */
    uint8_t *fallback_pixels;

    /* Atomic progress counters */
    atomic_size_t processed_count;
    atomic_size_t error_count;
    atomic_int cancel_flag;

    /* Per-album profiling (atomic accumulators, nanoseconds) */
    atomic_int_fast64_t prof_find_ns;       /* artwork discovery + file load (embedded only) */
    atomic_int_fast64_t prof_resize_ns;     /* vips_resize_file or vips_resize_buffer */
    atomic_size_t prof_fallback_count;      /* albums with no artwork */
};

/* Comparison function for sorting entries by album_id */
static int compare_atlas_entries(const void *a, const void *b) {
    const atlas_build_entry_t *ea = a;
    const atlas_build_entry_t *eb = b;
    if (ea->album_id < eb->album_id) return -1;
    if (ea->album_id > eb->album_id) return 1;
    return 0;
}

/**
 * Generate a solid dark-grey (#333333) raw pixel array.
 * Used as placeholder for albums that have no cover art.
 * 0x33 conveniently fills all 3 RGB bytes to get #333333.
 */
static uint8_t *generate_fallback_pixels(uint32_t pixel_stride) {
    uint8_t *buf = g_malloc(pixel_stride);
    memset(buf, 0x33, pixel_stride);
    return buf;
}

quadrature_result_t artwork_atlas_builder_create(const char* atlas_path,
                                                  int thumb_size,
                                                  artwork_atlas_builder_t** out) {
    if (!atlas_path || !out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    artwork_atlas_builder_t* builder = g_new0(artwork_atlas_builder_t, 1);
    if (!builder) {
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    builder->atlas_path = g_strdup(atlas_path);
    builder->temp_path = g_strdup_printf("%s.tmp", atlas_path);
    builder->thumb_size = thumb_size > 0 ? thumb_size : ARTWORK_ATLAS_THUMB_SIZE;

    builder->pixel_stride = (uint32_t)builder->thumb_size * builder->thumb_size * ARTWORK_ATLAS_CHANNELS;

    builder->entry_capacity = 256;
    builder->entries = g_new0(atlas_build_entry_t, builder->entry_capacity);
    builder->entry_count = 0;

    g_mutex_init(&builder->entries_lock);
    atomic_init(&builder->processed_count, 0);
    atomic_init(&builder->error_count, 0);
    atomic_init(&builder->cancel_flag, 0);
    atomic_init(&builder->prof_find_ns, 0);
    atomic_init(&builder->prof_resize_ns, 0);
    atomic_init(&builder->prof_fallback_count, 0);

    /* Ensure parent directory exists */
    char *dir = g_path_get_dirname(atlas_path);
    if (dir) {
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
    }

    builder->fallback_pixels = generate_fallback_pixels(builder->pixel_stride);

    g_info("Atlas builder created: path=%s size=%d", atlas_path, builder->thumb_size);

    *out = builder;
    return QUADRATURE_OK;
}

/**
 * Find artwork bytes for an album directory and resize to raw RGB pixels.
 *
 * @param album_dir  Album directory to search
 * @param thumb_size Target thumbnail size in pixels
 * @param pixels_out Output: raw RGB pixel data (caller must g_free)
 * @param size_out   Output: pixel data size (thumb_size * thumb_size * 3)
 * @param find_ns    Output: nanoseconds spent in artwork discovery (may be NULL)
 * @param resize_ns  Output: nanoseconds spent in vips resize (may be NULL)
 * @return QUADRATURE_OK on success, QUADRATURE_ERROR_FILE_NOT_FOUND if no art
 */
static quadrature_result_t find_and_resize_artwork(const char* album_dir,
                                                    int thumb_size,
                                                    uint8_t** pixels_out,
                                                    size_t* size_out,
                                                    int64_t* find_ns,
                                                    int64_t* resize_ns) {
    int64_t t0 = profile_now_ns();

    uint8_t* art_data = NULL;
    size_t art_size = 0;
    quadrature_result_t res = artwork_find_bytes(album_dir, &art_data, &art_size);
    if (find_ns) *find_ns = profile_now_ns() - t0;

    if (res != QUADRATURE_OK) {
        if (resize_ns) *resize_ns = 0;
        return res;
    }

    int64_t t1 = profile_now_ns();
    res = vips_resize_buffer(art_data, art_size, thumb_size, pixels_out, size_out);
    g_free(art_data);
    if (resize_ns) *resize_ns = profile_now_ns() - t1;
    return res;
}

quadrature_result_t artwork_atlas_process_album(artwork_atlas_builder_t* builder,
                                                 int64_t album_id,
                                                 const char* album_dir,
                                                 bool *used_fallback) {
    if (!builder || !album_dir) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (used_fallback) *used_fallback = false;

    if (atomic_load(&builder->cancel_flag)) {
        return QUADRATURE_ERROR_CANCELLED;
    }

    int64_t find_ns = 0, resize_ns = 0;
    uint8_t* pixel_data = NULL;
    size_t pixel_size = 0;
    quadrature_result_t res = find_and_resize_artwork(album_dir, builder->thumb_size,
                                                       &pixel_data, &pixel_size,
                                                       &find_ns, &resize_ns);
    atomic_fetch_add(&builder->prof_find_ns, find_ns);
    atomic_fetch_add(&builder->prof_resize_ns, resize_ns);

    if (res != QUADRATURE_OK) {
        /* No artwork found — store placeholder so this album isn't re-queued on next scan */
        artwork_atlas_add_cached_pixels(builder, album_id,
                                         builder->fallback_pixels, builder->pixel_stride);
        atomic_fetch_add(&builder->prof_fallback_count, 1);
        if (used_fallback) *used_fallback = true;
        return QUADRATURE_OK;
    }

    /* Add to builder (mutex-protected) */
    g_mutex_lock(&builder->entries_lock);

    if (builder->entry_count >= builder->entry_capacity) {
        builder->entry_capacity *= 2;
        builder->entries = g_realloc(builder->entries,
                                     builder->entry_capacity * sizeof(atlas_build_entry_t));
    }

    atlas_build_entry_t* entry = &builder->entries[builder->entry_count++];
    entry->album_id = album_id;
    entry->pixel_data = pixel_data;

    g_mutex_unlock(&builder->entries_lock);

    atomic_fetch_add(&builder->processed_count, 1);
    return QUADRATURE_OK;
}

quadrature_result_t artwork_atlas_add_cached_pixels(artwork_atlas_builder_t* builder,
                                                     int64_t album_id,
                                                     const void* pixel_data,
                                                     size_t pixel_size) {
    if (!builder || !pixel_data || pixel_size != builder->pixel_stride) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    uint8_t* data_copy = g_malloc(pixel_size);
    memcpy(data_copy, pixel_data, pixel_size);

    g_mutex_lock(&builder->entries_lock);
    if (builder->entry_count >= builder->entry_capacity) {
        builder->entry_capacity *= 2;
        builder->entries = g_realloc(builder->entries,
                                     builder->entry_capacity * sizeof(atlas_build_entry_t));
    }
    atlas_build_entry_t* entry = &builder->entries[builder->entry_count++];
    entry->album_id = album_id;
    entry->pixel_data = data_copy;
    g_mutex_unlock(&builder->entries_lock);

    atomic_fetch_add(&builder->processed_count, 1);
    return QUADRATURE_OK;
}

void artwork_atlas_builder_get_progress(artwork_atlas_builder_t* builder,
                                        size_t* processed_out,
                                        size_t* errors_out) {
    if (!builder) return;
    if (processed_out) *processed_out = atomic_load(&builder->processed_count);
    if (errors_out) *errors_out = atomic_load(&builder->error_count);
}

void artwork_atlas_builder_get_profile(artwork_atlas_builder_t* builder,
                                        artwork_atlas_profile_t* out) {
    if (!builder || !out) return;
    out->find_ns = atomic_load(&builder->prof_find_ns);
    out->resize_ns = atomic_load(&builder->prof_resize_ns);
    out->fallback_count = atomic_load(&builder->prof_fallback_count);
}

bool artwork_atlas_builder_is_cancelled(artwork_atlas_builder_t* builder) {
    if (!builder) return true;
    return atomic_load(&builder->cancel_flag) != 0;
}

void artwork_atlas_builder_cancel(artwork_atlas_builder_t* builder) {
    if (!builder) return;
    atomic_store(&builder->cancel_flag, 1);
}

quadrature_result_t artwork_atlas_builder_finish(artwork_atlas_builder_t* builder) {
    if (!builder) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    g_info("Atlas finish: %zu entries", builder->entry_count);

    if (builder->entry_count == 0) {
        return QUADRATURE_OK;
    }

    /* Sort entries by album_id */
    qsort(builder->entries, builder->entry_count,
          sizeof(atlas_build_entry_t), compare_atlas_entries);

    FILE *f = fopen(builder->temp_path, "wb");
    if (!f) {
        g_warning("Failed to create atlas temp file: %s", builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Write v2 header */
    artwork_atlas_header_t header = {0};
    memcpy(header.magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE);
    header.version = ARTWORK_ATLAS_VERSION;
    header.count = (uint32_t)builder->entry_count;
    header.flags = 0;
    header.thumb_size = (uint32_t)builder->thumb_size;
    header.channels = ARTWORK_ATLAS_CHANNELS;

    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f); unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Write sorted album_id array — batch into flat array for single fwrite */
    {
        int64_t *ids = g_malloc(builder->entry_count * sizeof(int64_t));
        for (size_t i = 0; i < builder->entry_count; i++)
            ids[i] = builder->entries[i].album_id;
        size_t written = fwrite(ids, sizeof(int64_t), builder->entry_count, f);
        g_free(ids);
        if (written != builder->entry_count) {
            fclose(f); unlink(builder->temp_path);
            return QUADRATURE_ERROR_INTERNAL;
        }
    }

    /* Write pixel data (fixed stride per entry) */
    for (size_t i = 0; i < builder->entry_count; i++) {
        if (fwrite(builder->entries[i].pixel_data, 1, builder->pixel_stride, f)
            != builder->pixel_stride) {
            fclose(f); unlink(builder->temp_path);
            return QUADRATURE_ERROR_INTERNAL;
        }
    }

    fclose(f);

    if (rename(builder->temp_path, builder->atlas_path) != 0) {
        g_warning("Failed to rename atlas file: %s -> %s",
                  builder->temp_path, builder->atlas_path);
        unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    size_t total_size = sizeof(header) + builder->entry_count * (sizeof(int64_t) + builder->pixel_stride);
    g_info("Atlas v2 created: %s (%zu entries, %zu bytes total)",
           builder->atlas_path, builder->entry_count, total_size);

    return QUADRATURE_OK;
}

void artwork_atlas_builder_destroy(artwork_atlas_builder_t* builder) {
    if (!builder) return;

    if (builder->entries) {
        for (size_t i = 0; i < builder->entry_count; i++) {
            g_free(builder->entries[i].pixel_data);
        }
        g_free(builder->entries);
    }

    g_free(builder->fallback_pixels);

    /* Remove temp file if it exists */
    if (builder->temp_path) {
        unlink(builder->temp_path);
        g_free(builder->temp_path);
    }

    g_free(builder->atlas_path);
    g_mutex_clear(&builder->entries_lock);
    g_free(builder);
}
