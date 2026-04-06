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
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

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

    /* Fall back to disc subdirectories (multi-disc albums like "CD1/", "Disc 1/") */
    dir = opendir(album_dir);
    if (!dir) return QUADRATURE_ERROR_FILE_NOT_FOUND;

    while ((dent = readdir(dir)) != NULL) {
        if (dent->d_name[0] == '.') continue;
#ifdef _DIRENT_HAVE_D_TYPE
        if (dent->d_type != DT_DIR && dent->d_type != DT_UNKNOWN) continue;
#endif
        if (!is_disc_folder(dent->d_name)) continue;

        char subdir[INDEXER_PATH_MAX];
        snprintf(subdir, sizeof(subdir), "%s/%s", album_dir, dent->d_name);

        /* Recurse once — artwork_find_bytes checks cover files + embedded art */
        if (artwork_find_bytes(subdir, data_out, size_out) == QUADRATURE_OK) {
            closedir(dir);
            return QUADRATURE_OK;
        }
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

static int compare_int64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

// =============================================================================
// Atlas Reader (fixed-stride raw pixel entries, v2+v3)
// =============================================================================

// =============================================================================
// mmap-backed Atlas Reader (single authoritative read path)
// =============================================================================

struct artwork_atlas_reader {
    void *map;                   /* mmap base (MAP_PRIVATE, PROT_READ) */
    size_t map_size;             /* total file size */
    artwork_atlas_header_t header;
    const int64_t *album_ids;   /* sorted art-entry IDs (into mmap) */
    const uint8_t *pixel_data;  /* dense pixel blocks (into mmap) */
    uint32_t pixel_stride;      /* thumb_size * thumb_size * channels */
    const int64_t *no_art_ids;  /* sorted no-art IDs (v3: into mmap, v2: heap) */
    uint32_t no_art_count;
    bool no_art_heap;            /* true if no_art_ids was heap-allocated (v2 compat) */
};

artwork_atlas_reader_t* artwork_atlas_reader_open(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 ||
        (size_t)st.st_size < sizeof(artwork_atlas_header_t) + ATLAS_CRC32_SIZE) {
        close(fd); return NULL;
    }

    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  /* fd is not needed after mmap */
    if (map == MAP_FAILED) return NULL;

    const artwork_atlas_header_t *h = map;
    if (memcmp(h->magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE) != 0 ||
        h->version < 2 || h->version > ARTWORK_ATLAS_VERSION) {
        munmap(map, (size_t)st.st_size); return NULL;
    }

    /* CRC32 validation (zero-copy — computed directly over mmap) */
    size_t body_size = (size_t)st.st_size - ATLAS_CRC32_SIZE;
    uint32_t expected_crc;
    memcpy(&expected_crc, (const uint8_t *)map + body_size, sizeof(expected_crc));
    uint32_t actual_crc = atlas_crc32((const uint8_t *)map, body_size);
    if (actual_crc != expected_crc) {
        g_warning("Album atlas CRC32 mismatch in %s (expected 0x%08X, got 0x%08X)",
                  path, expected_crc, actual_crc);
        munmap(map, (size_t)st.st_size); return NULL;
    }
    g_debug("Album atlas CRC32 verified: %s (0x%08X)", path, actual_crc);

    artwork_atlas_reader_t* reader = g_new0(artwork_atlas_reader_t, 1);
    reader->map = map;
    reader->map_size = (size_t)st.st_size;
    reader->header = *h;
    reader->pixel_stride = h->thumb_size * h->thumb_size * h->channels;

    if (h->count > 0) {
        reader->album_ids = (const int64_t *)((const uint8_t *)map + sizeof(*h));
        reader->pixel_data = (const uint8_t *)reader->album_ids +
                             (size_t)h->count * sizeof(int64_t);
    }

    /* v3: no_art section follows pixel data (pointers into mmap) */
    if (h->version >= 3 && h->count > 0) {
        const uint8_t *after_pixels = reader->pixel_data +
                                      (size_t)h->count * reader->pixel_stride;
        uint32_t nac;
        memcpy(&nac, after_pixels, sizeof(nac));
        if (nac > 0) {
            reader->no_art_ids = (const int64_t *)(after_pixels + sizeof(nac));
            reader->no_art_count = nac;
        }
    } else if (h->version >= 3) {
        /* count == 0 but there may still be a no_art section */
        const uint8_t *after_header = (const uint8_t *)map + sizeof(*h);
        uint32_t nac;
        memcpy(&nac, after_header, sizeof(nac));
        if (nac > 0) {
            reader->no_art_ids = (const int64_t *)(after_header + sizeof(nac));
            reader->no_art_count = nac;
        }
    } else {
        /* v2 backward compat: derive no_art from #333333 placeholder pixels */
        if (h->count > 0) {
            uint8_t *placeholder = g_malloc(reader->pixel_stride);
            memset(placeholder, 0x33, reader->pixel_stride);

            uint32_t nac = 0;
            for (uint32_t i = 0; i < h->count; i++) {
                if (memcmp(reader->pixel_data + (size_t)i * reader->pixel_stride,
                           placeholder, reader->pixel_stride) == 0)
                    nac++;
            }
            if (nac > 0) {
                int64_t *ids = g_new(int64_t, nac);
                uint32_t j = 0;
                for (uint32_t i = 0; i < h->count && j < nac; i++) {
                    if (memcmp(reader->pixel_data + (size_t)i * reader->pixel_stride,
                               placeholder, reader->pixel_stride) == 0)
                        ids[j++] = reader->album_ids[i];
                }
                reader->no_art_ids = ids;
                reader->no_art_count = j;
                reader->no_art_heap = true;
            }
            g_free(placeholder);
        }
    }

    return reader;
}

void artwork_atlas_reader_close(artwork_atlas_reader_t* reader) {
    if (!reader) return;
    if (reader->no_art_heap)
        g_free((int64_t *)reader->no_art_ids);
    if (reader->map)
        munmap(reader->map, reader->map_size);
    g_free(reader);
}

int32_t artwork_atlas_reader_lookup(const artwork_atlas_reader_t* reader, int64_t album_id) {
    if (!reader || !reader->album_ids) return -1;
    uint32_t lo = 0, hi = reader->header.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int64_t mid_id = reader->album_ids[mid];
        if (mid_id == album_id) return (int32_t)mid;
        if (mid_id < album_id) lo = mid + 1; else hi = mid;
    }
    return -1;
}

bool artwork_atlas_reader_has_no_art(const artwork_atlas_reader_t* reader, int64_t album_id) {
    if (!reader || !reader->no_art_ids || reader->no_art_count == 0) return false;
    return bsearch(&album_id, reader->no_art_ids, reader->no_art_count,
                   sizeof(int64_t), compare_int64) != NULL;
}

uint32_t artwork_atlas_reader_get_no_art_count(const artwork_atlas_reader_t* reader) {
    return reader ? reader->no_art_count : 0;
}

size_t artwork_atlas_reader_get_count(const artwork_atlas_reader_t* reader) {
    return reader ? reader->header.count : 0;
}

uint32_t artwork_atlas_reader_get_pixel_stride(const artwork_atlas_reader_t* reader) {
    return reader ? reader->pixel_stride : 0;
}

uint32_t artwork_atlas_reader_get_thumb_size(const artwork_atlas_reader_t* reader) {
    return reader ? reader->header.thumb_size : 0;
}

uint8_t artwork_atlas_reader_get_channels(const artwork_atlas_reader_t* reader) {
    return reader ? reader->header.channels : 0;
}

int64_t artwork_atlas_reader_get_album_id_at(const artwork_atlas_reader_t* reader, size_t index) {
    if (!reader || !reader->album_ids || index >= reader->header.count) return -1;
    return reader->album_ids[index];
}

const uint8_t* artwork_atlas_reader_get_pixels_at(const artwork_atlas_reader_t* reader, size_t index) {
    if (!reader || !reader->pixel_data || index >= reader->header.count) return NULL;
    return reader->pixel_data + index * reader->pixel_stride;
}

size_t artwork_atlas_reader_get_file_size(const artwork_atlas_reader_t* reader) {
    return reader ? reader->map_size : 0;
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

    /* Known no-artwork album IDs (v3 no_art section) */
    int64_t *no_art_ids;
    size_t no_art_count;
    size_t no_art_capacity;

    /* Incremental update: only rewrite atlas if entries changed */
    bool dirty;

    /* Fanart.tv album cover cache directory (optional fallback) */
    char *fanart_covers_dir;                /* NULL = no fanart fallback */
    GHashTable *album_rg_map;              /* album_id (gint64*) → release_group_id (char*), owned */
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

    builder->no_art_capacity = 256;
    builder->no_art_ids = g_new0(int64_t, builder->no_art_capacity);
    builder->no_art_count = 0;
    builder->dirty = false;

    builder->fanart_covers_dir = NULL;

    g_info("Atlas builder created: path=%s size=%d", atlas_path, builder->thumb_size);

    *out = builder;
    return QUADRATURE_OK;
}

void artwork_atlas_builder_set_fanart_covers_dir(artwork_atlas_builder_t* builder,
                                                  const char* dir) {
    if (!builder) return;
    g_free(builder->fanart_covers_dir);
    builder->fanart_covers_dir = dir ? g_strdup(dir) : NULL;
}

void artwork_atlas_builder_set_album_rg_map(artwork_atlas_builder_t* builder,
                                             GHashTable* album_id_to_rg_id) {
    if (!builder) return;
    if (builder->album_rg_map) g_hash_table_destroy(builder->album_rg_map);
    builder->album_rg_map = album_id_to_rg_id;  // takes ownership
}

quadrature_result_t artwork_atlas_builder_load_existing(artwork_atlas_builder_t* builder) {
    if (!builder) return QUADRATURE_ERROR_INVALID_PARAM;

    FILE* f = fopen(builder->atlas_path, "rb");
    if (!f) return QUADRATURE_OK;  // No existing atlas — start fresh

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size < (long)(sizeof(artwork_atlas_header_t) + ATLAS_CRC32_SIZE)) {
        fclose(f);
        return QUADRATURE_OK;
    }

    artwork_atlas_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) { fclose(f); return QUADRATURE_OK; }

    if (memcmp(header.magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE) != 0 ||
        header.version < 2 || header.version > ARTWORK_ATLAS_VERSION) {
        fclose(f); return QUADRATURE_OK;  // Incompatible — start fresh
    }

    // Validate CRC32
    {
        size_t body_size = (size_t)file_size - ATLAS_CRC32_SIZE;
        rewind(f);
        uint8_t* buf = g_malloc(body_size);
        if (fread(buf, 1, body_size, f) != body_size) { g_free(buf); fclose(f); return QUADRATURE_OK; }
        uint32_t expected_crc;
        if (fread(&expected_crc, sizeof(expected_crc), 1, f) != 1) { g_free(buf); fclose(f); return QUADRATURE_OK; }
        uint32_t actual_crc = atlas_crc32(buf, body_size);
        g_free(buf);
        if (actual_crc != expected_crc) {
            g_warning("atlas load_existing: CRC32 mismatch, starting fresh");
            fclose(f);
            return QUADRATURE_OK;
        }
    }

    uint32_t pixel_stride = header.thumb_size * header.thumb_size * header.channels;

    // Read album_id array
    fseek(f, sizeof(header), SEEK_SET);
    int64_t* ids = NULL;
    uint8_t* pixels = NULL;

    if (header.count > 0) {
        ids = g_new(int64_t, header.count);
        if (fread(ids, sizeof(int64_t), header.count, f) != header.count) {
            g_free(ids); fclose(f); return QUADRATURE_OK;
        }

        size_t total_pixels = (size_t)header.count * pixel_stride;
        pixels = g_malloc(total_pixels);
        if (fread(pixels, 1, total_pixels, f) != total_pixels) {
            g_free(ids); g_free(pixels); fclose(f); return QUADRATURE_OK;
        }
    }

    // For v2 files, detect placeholders via SIMD-friendly memcmp and split into art/no_art.
    // For v3 files, all entries in the main section are real art.
    bool is_v2 = (header.version == 2);

    for (uint32_t i = 0; i < header.count; i++) {
        bool is_placeholder = false;

        if (is_v2 && pixel_stride == builder->pixel_stride) {
            is_placeholder = (memcmp(pixels + (size_t)i * pixel_stride,
                                     builder->fallback_pixels, pixel_stride) == 0);
        }

        if (is_placeholder) {
            // v2 placeholder → add to no_art list
            if (builder->no_art_count >= builder->no_art_capacity) {
                builder->no_art_capacity *= 2;
                builder->no_art_ids = g_realloc(builder->no_art_ids,
                    builder->no_art_capacity * sizeof(int64_t));
            }
            builder->no_art_ids[builder->no_art_count++] = ids[i];
        } else if (pixel_stride == builder->pixel_stride) {
            // Real art entry — copy pixels
            g_mutex_lock(&builder->entries_lock);
            if (builder->entry_count >= builder->entry_capacity) {
                builder->entry_capacity *= 2;
                builder->entries = g_realloc(builder->entries,
                    builder->entry_capacity * sizeof(atlas_build_entry_t));
            }
            atlas_build_entry_t* e = &builder->entries[builder->entry_count++];
            e->album_id = ids[i];
            e->pixel_data = g_malloc(pixel_stride);
            memcpy(e->pixel_data, pixels + (size_t)i * pixel_stride, pixel_stride);
            g_mutex_unlock(&builder->entries_lock);
        }
        // else: thumb size changed — drop entry (will be re-processed)
    }

    g_free(ids);
    g_free(pixels);

    // v3: read no_art section after pixel data
    if (!is_v2) {
        uint32_t no_art_count = 0;
        if (fread(&no_art_count, sizeof(no_art_count), 1, f) == 1 && no_art_count > 0) {
            while (builder->no_art_count + no_art_count > builder->no_art_capacity) {
                builder->no_art_capacity *= 2;
                builder->no_art_ids = g_realloc(builder->no_art_ids,
                    builder->no_art_capacity * sizeof(int64_t));
            }
            size_t read = fread(builder->no_art_ids + builder->no_art_count,
                                sizeof(int64_t), no_art_count, f);
            builder->no_art_count += read;
        }
    }

    fclose(f);
    builder->dirty = false;  // Just loaded — nothing changed yet

    g_info("Atlas load_existing: %zu art, %zu no-art (v%u source)",
           builder->entry_count, builder->no_art_count, header.version);
    return QUADRATURE_OK;
}

quadrature_result_t artwork_atlas_builder_add_no_art(artwork_atlas_builder_t* builder,
                                                      int64_t album_id) {
    if (!builder) return QUADRATURE_ERROR_INVALID_PARAM;

    g_mutex_lock(&builder->entries_lock);

    // Check not already tracked
    for (size_t i = 0; i < builder->no_art_count; i++) {
        if (builder->no_art_ids[i] == album_id) {
            g_mutex_unlock(&builder->entries_lock);
            return QUADRATURE_OK;
        }
    }

    if (builder->no_art_count >= builder->no_art_capacity) {
        builder->no_art_capacity *= 2;
        builder->no_art_ids = g_realloc(builder->no_art_ids,
            builder->no_art_capacity * sizeof(int64_t));
    }
    builder->no_art_ids[builder->no_art_count++] = album_id;
    builder->dirty = true;
    g_mutex_unlock(&builder->entries_lock);
    return QUADRATURE_OK;
}

size_t artwork_atlas_builder_sweep_no_art(artwork_atlas_builder_t* builder,
                                          GArray* promoted_ids) {
    if (!builder || !builder->fanart_covers_dir || !builder->album_rg_map)
        return 0;

    size_t promoted = 0;
    size_t dst = 0;
    for (size_t src = 0; src < builder->no_art_count; src++) {
        const char* rg_id = g_hash_table_lookup(builder->album_rg_map,
                                                 &builder->no_art_ids[src]);
        if (rg_id) {
            char path[INDEXER_PATH_MAX];
            g_snprintf(path, sizeof(path), "%s/%s.jpg",
                       builder->fanart_covers_dir, rg_id);
            if (g_file_test(path, G_FILE_TEST_EXISTS)) {
                if (promoted_ids)
                    g_array_append_val(promoted_ids, builder->no_art_ids[src]);
                promoted++;
                builder->dirty = true;
                continue;  // drop from no_art — will be re-processed
            }
        }
        builder->no_art_ids[dst++] = builder->no_art_ids[src];
    }
    builder->no_art_count = dst;
    if (promoted > 0) {
        g_info("Atlas sweep: promoted %zu albums from no_art (fanart covers found)", promoted);
    }
    return promoted;
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
        /* Try fanart.tv-sourced cover cache as a fallback (keyed by release group UUID) */
        if (builder->fanart_covers_dir && builder->album_rg_map) {
            const char* rg_id = g_hash_table_lookup(builder->album_rg_map, &album_id);
            char fanart_path[INDEXER_PATH_MAX];
            if (rg_id) {
                g_snprintf(fanart_path, sizeof(fanart_path),
                           "%s/%s.jpg", builder->fanart_covers_dir, rg_id);
            } else {
                fanart_path[0] = '\0';
            }

            if (g_file_test(fanart_path, G_FILE_TEST_EXISTS)) {
                gchar* contents = NULL;
                gsize length = 0;
                if (g_file_get_contents(fanart_path, &contents, &length, NULL) && length > 0) {
                    int64_t t1 = profile_now_ns();
                    res = vips_resize_buffer((const void*)contents, length, builder->thumb_size,
                                             &pixel_data, &pixel_size);
                    atomic_fetch_add(&builder->prof_resize_ns, profile_now_ns() - t1);
                    g_free(contents);

                    if (res == QUADRATURE_OK) {
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
                } else {
                    g_free(contents);
                }
            }
        }

        /* No artwork found — record in no_art section (v3) */
        artwork_atlas_builder_add_no_art(builder, album_id);
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
    builder->dirty = true;
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

    /* Incremental: skip write if nothing changed */
    if (!builder->dirty) {
        g_info("Atlas finish: unchanged (%zu art, %zu no-art)",
               builder->entry_count, builder->no_art_count);
        return QUADRATURE_OK;
    }

    if (builder->entry_count == 0 && builder->no_art_count == 0) {
        return QUADRATURE_OK;
    }

    /* Sort entries by album_id */
    qsort(builder->entries, builder->entry_count,
          sizeof(atlas_build_entry_t), compare_atlas_entries);
    qsort(builder->no_art_ids, builder->no_art_count,
          sizeof(int64_t), compare_int64);

    FILE *f = fopen(builder->temp_path, "wb");
    if (!f) {
        g_warning("Failed to create atlas temp file: %s", builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Incremental CRC32 — computed during writes, no read-back needed */
    uint32_t crc = ATLAS_CRC32_INIT;
    bool write_ok = true;

#define CRC_WRITE(ptr, sz) do { \
    size_t _sz = (sz); \
    if (write_ok && fwrite((ptr), 1, _sz, f) == _sz) \
        crc = atlas_crc32_update(crc, (const uint8_t*)(ptr), _sz); \
    else write_ok = false; \
} while (0)

    /* Write v3 header */
    artwork_atlas_header_t header = {0};
    memcpy(header.magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE);
    header.version = ARTWORK_ATLAS_VERSION;
    header.count = (uint32_t)builder->entry_count;
    header.no_art_count = (uint32_t)builder->no_art_count;
    header.thumb_size = (uint32_t)builder->thumb_size;
    header.channels = ARTWORK_ATLAS_CHANNELS;
    CRC_WRITE(&header, sizeof(header));

    /* Write sorted album_id array */
    if (builder->entry_count > 0) {
        int64_t *ids = g_malloc(builder->entry_count * sizeof(int64_t));
        for (size_t i = 0; i < builder->entry_count; i++)
            ids[i] = builder->entries[i].album_id;
        CRC_WRITE(ids, builder->entry_count * sizeof(int64_t));
        g_free(ids);
    }

    /* Write pixel data (fixed stride per entry) */
    for (size_t i = 0; i < builder->entry_count && write_ok; i++)
        CRC_WRITE(builder->entries[i].pixel_data, builder->pixel_stride);

    /* Write v3 no_art section */
    uint32_t nac = (uint32_t)builder->no_art_count;
    CRC_WRITE(&nac, sizeof(nac));
    if (builder->no_art_count > 0)
        CRC_WRITE(builder->no_art_ids, builder->no_art_count * sizeof(int64_t));

    /* Append CRC32 (not included in its own checksum) */
    uint32_t final_crc = atlas_crc32_final(crc);
    if (write_ok)
        write_ok = (fwrite(&final_crc, sizeof(final_crc), 1, f) == 1);

#undef CRC_WRITE

    fclose(f);

    if (!write_ok) {
        g_warning("Failed to write atlas temp file: %s", builder->temp_path);
        unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (rename(builder->temp_path, builder->atlas_path) != 0) {
        g_warning("Failed to rename atlas file: %s -> %s",
                  builder->temp_path, builder->atlas_path);
        unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_info("Atlas v3 written: %s (%zu art, %zu no-art)",
           builder->atlas_path, builder->entry_count, builder->no_art_count);

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
    g_free(builder->no_art_ids);

    g_free(builder->fallback_pixels);

    /* Remove temp file if it exists */
    if (builder->temp_path) {
        unlink(builder->temp_path);
        g_free(builder->temp_path);
    }

    g_free(builder->fanart_covers_dir);
    if (builder->album_rg_map) g_hash_table_destroy(builder->album_rg_map);
    g_free(builder->atlas_path);
    g_mutex_clear(&builder->entries_lock);
    g_free(builder);
}
