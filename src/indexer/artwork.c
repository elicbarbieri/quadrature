/**
 * Album artwork processing: discovery, extraction, thumbnail generation,
 * and packed binary atlas creation.
 *
 * Uses libvips for fast in-memory image resizing (no subprocess overhead).
 * Thread-safe atlas builder supports concurrent album processing.
 */

#include <glib.h>
#include "internal.h"
#include "quadrature/artwork_atlas.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
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

static void replace_extension(char* out, size_t out_size, const char* path, const char* new_ext) {
    const char* dot = strrchr(path, '.');
    size_t base_len = dot ? (size_t)(dot - path) : strlen(path);
    size_t ext_len = strlen(new_ext);
    if (base_len + ext_len >= out_size) {
        base_len = out_size > ext_len + 1 ? out_size - ext_len - 1 : 0;
    }
    snprintf(out, out_size, "%.*s%s", (int)base_len, path, new_ext);
}

static int mkdir_p(const char* path) {
    char tmp[INDEXER_PATH_MAX];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Public API - Artwork Discovery
// -----------------------------------------------------------------------------

quadrature_result_t artwork_find(const char* album_dir, char* art_path,
                                  size_t path_size, art_source_t* source) {
    if (!album_dir || !art_path || path_size == 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (source) *source = ART_SOURCE_NONE;

    DIR* dir = opendir(album_dir);
    if (!dir) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    char best_path[INDEXER_PATH_MAX] = {0};
    int best_priority = 99;
    static const art_source_t priority_to_source[] = {
        ART_SOURCE_COVER, ART_SOURCE_FOLDER, ART_SOURCE_FRONT, ART_SOURCE_ALBUMART
    };

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
        strncpy(art_path, best_path, path_size - 1);
        art_path[path_size - 1] = '\0';
        if (source) *source = priority_to_source[best_priority];
        return QUADRATURE_OK;
    }

    return QUADRATURE_ERROR_FILE_NOT_FOUND;
}

quadrature_result_t artwork_extract_embedded(const char* audio_path,
                                              const char* output_path) {
    if (!audio_path || !output_path) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) != 0) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int pic_stream = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            pic_stream = i;
            break;
        }
    }

    if (pic_stream < 0) {
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    AVPacket* pkt = &fmt->streams[pic_stream]->attached_pic;

    const char* ext = ".jpg";
    AVCodecParameters* codecpar = fmt->streams[pic_stream]->codecpar;
    if (codecpar->codec_id == AV_CODEC_ID_PNG) {
        ext = ".png";
    }

    char actual_output[INDEXER_PATH_MAX];
    replace_extension(actual_output, sizeof(actual_output), output_path, ext);

    FILE* f = fopen(actual_output, "wb");
    if (!f) {
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_INTERNAL;
    }

    fwrite(pkt->data, 1, pkt->size, f);
    fclose(f);

    avformat_close_input(&fmt);
    return QUADRATURE_OK;
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
            g_atomic_int_set(&vips_initialized, 1);
            g_info("libvips initialized");
        } else {
            g_warning("Failed to initialize libvips: %s", vips_error_buffer());
            vips_error_clear();
        }
    }
    g_mutex_unlock(&init_mutex);
}

/**
 * Resize image data in memory using libvips.
 * Returns PNG data allocated with g_malloc (caller must g_free).
 *
 * @param input_data Raw image data (JPEG, PNG, etc.)
 * @param input_size Size of input data
 * @param thumb_size Target size (width and height)
 * @param output_data Output pointer for PNG data
 * @param output_size Output pointer for PNG data size
 * @return QUADRATURE_OK on success
 */
static quadrature_result_t vips_resize_memory(const void* input_data, size_t input_size,
                                               int thumb_size,
                                               uint8_t** output_data, size_t* output_size) {
    ensure_vips_init();

    VipsImage* out = NULL;
    void* buf = NULL;
    size_t len = 0;

    // Use vips_thumbnail_buffer for direct buffer-to-image thumbnail generation
    // This is the simplest and most efficient approach
    if (vips_thumbnail_buffer((void*)input_data, input_size, &out, thumb_size,
                               "height", thumb_size,
                               "size", VIPS_SIZE_BOTH,
                               "crop", VIPS_INTERESTING_CENTRE,
                               NULL) != 0) {
        g_warning("vips_thumbnail_buffer failed: %s", vips_error_buffer());
        vips_error_clear();
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Save as PNG to memory buffer
    if (vips_pngsave_buffer(out, &buf, &len,
                            "compression", 6,  // Balance speed vs size
                            NULL) != 0) {
        g_warning("vips_pngsave_buffer failed: %s", vips_error_buffer());
        vips_error_clear();
        g_object_unref(out);
        return QUADRATURE_ERROR_INTERNAL;
    }
    g_object_unref(out);

    *output_data = buf;
    *output_size = len;
    return QUADRATURE_OK;
}

/**
 * Load image file into memory buffer.
 * Returns data allocated with g_malloc (caller must g_free).
 */
static quadrature_result_t load_file_to_memory(const char* path,
                                                uint8_t** data_out,
                                                size_t* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 50 * 1024 * 1024) {  // 50MB limit
        fclose(f);
        return QUADRATURE_ERROR_INTERNAL;
    }

    uint8_t* data = g_malloc(file_size);
    size_t read_size = fread(data, 1, file_size, f);
    fclose(f);

    if (read_size != (size_t)file_size) {
        g_free(data);
        return QUADRATURE_ERROR_INTERNAL;
    }

    *data_out = data;
    *size_out = read_size;
    return QUADRATURE_OK;
}

/**
 * Extract embedded artwork to memory buffer.
 * Returns data allocated with g_malloc (caller must g_free).
 */
static quadrature_result_t extract_embedded_to_memory(const char* audio_path,
                                                       uint8_t** data_out,
                                                       size_t* size_out) {
    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, audio_path, NULL, NULL) != 0) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int pic_stream = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            pic_stream = i;
            break;
        }
    }

    if (pic_stream < 0) {
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    AVPacket* pkt = &fmt->streams[pic_stream]->attached_pic;

    uint8_t* data = g_malloc(pkt->size);
    memcpy(data, pkt->data, pkt->size);

    *data_out = data;
    *size_out = pkt->size;

    avformat_close_input(&fmt);
    return QUADRATURE_OK;
}

quadrature_result_t artwork_generate_thumbnail(const char* input_path,
                                                const char* output_path,
                                                int size) {
    if (!input_path || !output_path || size <= 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Load source image
    uint8_t* input_data = NULL;
    size_t input_size = 0;
    quadrature_result_t res = load_file_to_memory(input_path, &input_data, &input_size);
    if (res != QUADRATURE_OK) {
        return res;
    }

    // Resize with libvips
    uint8_t* output_data = NULL;
    size_t output_size = 0;
    res = vips_resize_memory(input_data, input_size, size, &output_data, &output_size);
    g_free(input_data);

    if (res != QUADRATURE_OK) {
        return res;
    }

    // Write PNG output
    char png_output[INDEXER_PATH_MAX];
    replace_extension(png_output, sizeof(png_output), output_path, ".png");

    FILE* f = fopen(png_output, "wb");
    if (!f) {
        g_free(output_data);
        return QUADRATURE_ERROR_INTERNAL;
    }

    fwrite(output_data, 1, output_size, f);
    fclose(f);
    g_free(output_data);

    return QUADRATURE_OK;
}

quadrature_result_t artwork_find_and_process(const char* album_dir,
                                              int64_t album_id,
                                              const char* cache_dir,
                                              int thumb_size,
                                              char* result_path,
                                              size_t result_size) {
    if (!album_dir || !result_path || result_size == 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (thumb_size <= 0) thumb_size = 48;

    #define CACHE_PATH_MAX (INDEXER_PATH_MAX - INDEXER_PATH_SUFFIX_RESERVE)

    char cache_path[CACHE_PATH_MAX];
    if (cache_dir && cache_dir[0]) {
        snprintf(cache_path, sizeof(cache_path), "%s", cache_dir);
    } else {
        snprintf(cache_path, sizeof(cache_path), "%s/.quadrature/art-cache", album_dir);
    }

    if (mkdir_p(cache_path) != 0) {
        g_warning("Failed to create art cache: %s", cache_path);
    }

    char output_path[INDEXER_PATH_MAX];
    snprintf(output_path, sizeof(output_path), "%s/%lld.png",
             cache_path, (long long)album_id);

    struct stat st;
    if (stat(output_path, &st) == 0) {
        strncpy(result_path, output_path, result_size - 1);
        result_path[result_size - 1] = '\0';
        return QUADRATURE_OK;
    }

    char art_source[INDEXER_PATH_MAX];
    art_source_t source_type;
    quadrature_result_t res = artwork_find(album_dir, art_source, sizeof(art_source),
                                            &source_type);

    if (res != QUADRATURE_OK) {
        DIR* dir = opendir(album_dir);
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                if (!has_audio_extension(entry->d_name)) continue;

                char audio_path[INDEXER_PATH_MAX];
                snprintf(audio_path, sizeof(audio_path), "%s/%s", album_dir, entry->d_name);

                char embedded_base[INDEXER_PATH_MAX];
                snprintf(embedded_base, sizeof(embedded_base), "%s/%lld_embedded",
                         cache_path, (long long)album_id);

                if (artwork_extract_embedded(audio_path, embedded_base) == QUADRATURE_OK) {
                    replace_extension(art_source, sizeof(art_source), embedded_base, ".jpg");
                    if (stat(art_source, &st) != 0) {
                        replace_extension(art_source, sizeof(art_source), embedded_base, ".png");
                    }
                    if (stat(art_source, &st) == 0) {
                        source_type = ART_SOURCE_EMBEDDED;
                        res = QUADRATURE_OK;
                        break;
                    }
                }
            }
            closedir(dir);
        }
    }

    if (res != QUADRATURE_OK) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    res = artwork_generate_thumbnail(art_source, output_path, thumb_size);
    if (res != QUADRATURE_OK) {
        return res;
    }

    strncpy(result_path, output_path, result_size - 1);
    result_path[result_size - 1] = '\0';

    g_debug("Generated artwork thumbnail: %s (source: %s)", output_path, art_source);
    return QUADRATURE_OK;

    #undef CACHE_PATH_MAX
}

// =============================================================================
// Atlas Reader (for loading cached entries from existing atlas)
// =============================================================================

struct artwork_atlas_reader {
    FILE* file;
    artwork_atlas_header_t header;
    artwork_atlas_entry_t* index;
    size_t data_offset;  // Byte offset where data section starts
};

artwork_atlas_reader_t* artwork_atlas_reader_open(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    artwork_atlas_reader_t* reader = g_new0(artwork_atlas_reader_t, 1);
    reader->file = f;

    // Read header
    if (fread(&reader->header, sizeof(reader->header), 1, f) != 1) {
        fclose(f);
        g_free(reader);
        return NULL;
    }

    // Validate magic
    if (memcmp(reader->header.magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE) != 0) {
        fclose(f);
        g_free(reader);
        return NULL;
    }

    // Read index
    if (reader->header.count > 0) {
        reader->index = g_new(artwork_atlas_entry_t, reader->header.count);
        if (fread(reader->index, sizeof(artwork_atlas_entry_t), reader->header.count, f)
            != reader->header.count) {
            g_free(reader->index);
            fclose(f);
            g_free(reader);
            return NULL;
        }
    }

    reader->data_offset = sizeof(artwork_atlas_header_t) +
                          sizeof(artwork_atlas_entry_t) * reader->header.count;

    return reader;
}

void artwork_atlas_reader_close(artwork_atlas_reader_t* reader) {
    if (!reader) return;
    if (reader->file) fclose(reader->file);
    g_free(reader->index);
    g_free(reader);
}

// Binary search for album_id in sorted index
static artwork_atlas_entry_t* atlas_find_entry(artwork_atlas_reader_t* reader, int64_t album_id) {
    if (!reader || !reader->index || reader->header.count == 0) return NULL;

    size_t lo = 0, hi = reader->header.count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (reader->index[mid].album_id < album_id) {
            lo = mid + 1;
        } else if (reader->index[mid].album_id > album_id) {
            hi = mid;
        } else {
            return &reader->index[mid];
        }
    }
    return NULL;
}

// Read PNG data for an album (caller must g_free the result)
uint8_t* artwork_atlas_reader_get_png(artwork_atlas_reader_t* reader, int64_t album_id, size_t* size_out) {
    if (!reader || !size_out) return NULL;
    *size_out = 0;

    artwork_atlas_entry_t* entry = atlas_find_entry(reader, album_id);
    if (!entry) return NULL;

    uint8_t* data = g_malloc(entry->size);
    if (!data) return NULL;

    fseek(reader->file, (long)(reader->data_offset + entry->offset), SEEK_SET);
    if (fread(data, 1, entry->size, reader->file) != entry->size) {
        g_free(data);
        return NULL;
    }

    *size_out = entry->size;
    return data;
}

// Get count of entries in atlas
size_t artwork_atlas_reader_get_count(artwork_atlas_reader_t* reader) {
    if (!reader) return 0;
    return reader->header.count;
}

// Get album_id at index (for iteration)
int64_t artwork_atlas_reader_get_album_id_at(artwork_atlas_reader_t* reader, size_t index) {
    if (!reader || !reader->index || index >= reader->header.count) return -1;
    return reader->index[index].album_id;
}

// Read PNG data by index (caller must g_free the result)
uint8_t* artwork_atlas_reader_get_png_at(artwork_atlas_reader_t* reader, size_t index, size_t* size_out) {
    if (!reader || !size_out || !reader->index || index >= reader->header.count) {
        if (size_out) *size_out = 0;
        return NULL;
    }

    artwork_atlas_entry_t* entry = &reader->index[index];
    uint8_t* data = g_malloc(entry->size);
    if (!data) {
        *size_out = 0;
        return NULL;
    }

    fseek(reader->file, (long)(reader->data_offset + entry->offset), SEEK_SET);
    if (fread(data, 1, entry->size, reader->file) != entry->size) {
        g_free(data);
        *size_out = 0;
        return NULL;
    }

    *size_out = entry->size;
    return data;
}

// =============================================================================
// Thread-Safe Artwork Atlas Builder
// =============================================================================

/* Atlas entry with PNG data (used during building) */
typedef struct {
    int64_t album_id;
    uint8_t *png_data;
    size_t png_size;
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

    /* Atomic progress counters */
    atomic_size_t processed_count;
    atomic_size_t error_count;
    atomic_int cancel_flag;
};

/* Comparison function for sorting entries by album_id */
static int compare_atlas_entries(const void *a, const void *b) {
    const atlas_build_entry_t *ea = a;
    const atlas_build_entry_t *eb = b;
    if (ea->album_id < eb->album_id) return -1;
    if (ea->album_id > eb->album_id) return 1;
    return 0;
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

    builder->entry_capacity = 256;
    builder->entries = g_new0(atlas_build_entry_t, builder->entry_capacity);
    builder->entry_count = 0;

    g_mutex_init(&builder->entries_lock);
    atomic_init(&builder->processed_count, 0);
    atomic_init(&builder->error_count, 0);
    atomic_init(&builder->cancel_flag, 0);

    /* Ensure parent directory exists */
    char *dir = g_path_get_dirname(atlas_path);
    if (dir) {
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
    }

    g_info("Atlas builder created: path=%s size=%d", atlas_path, builder->thumb_size);

    *out = builder;
    return QUADRATURE_OK;
}

/**
 * Find artwork source for an album (cover file or embedded).
 * Loads the image data into memory.
 */
static quadrature_result_t find_and_load_artwork(const char* album_dir,
                                                  uint8_t** data_out,
                                                  size_t* size_out) {
    /* Try to find cover file first */
    char art_path[INDEXER_PATH_MAX];
    art_source_t source_type;
    quadrature_result_t res = artwork_find(album_dir, art_path, sizeof(art_path), &source_type);

    if (res == QUADRATURE_OK) {
        return load_file_to_memory(art_path, data_out, size_out);
    }

    /* Try embedded artwork */
    DIR* dir = opendir(album_dir);
    if (!dir) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!has_audio_extension(entry->d_name)) continue;

        char audio_path[INDEXER_PATH_MAX];
        snprintf(audio_path, sizeof(audio_path), "%s/%s", album_dir, entry->d_name);

        res = extract_embedded_to_memory(audio_path, data_out, size_out);
        if (res == QUADRATURE_OK) {
            closedir(dir);
            return QUADRATURE_OK;
        }
    }
    closedir(dir);

    return QUADRATURE_ERROR_FILE_NOT_FOUND;
}

quadrature_result_t artwork_atlas_process_album(artwork_atlas_builder_t* builder,
                                                 int64_t album_id,
                                                 const char* album_dir) {
    if (!builder || !album_dir) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (atomic_load(&builder->cancel_flag)) {
        return QUADRATURE_ERROR_CANCELLED;
    }

    /* Find and load artwork to memory */
    uint8_t* source_data = NULL;
    size_t source_size = 0;
    quadrature_result_t res = find_and_load_artwork(album_dir, &source_data, &source_size);
    if (res != QUADRATURE_OK) {
        atomic_fetch_add(&builder->error_count, 1);
        return res;
    }

    /* Resize with libvips (thread-safe) */
    uint8_t* png_data = NULL;
    size_t png_size = 0;
    res = vips_resize_memory(source_data, source_size, builder->thumb_size,
                              &png_data, &png_size);
    g_free(source_data);

    if (res != QUADRATURE_OK) {
        atomic_fetch_add(&builder->error_count, 1);
        return res;
    }

    /* Add to builder (mutex-protected) */
    g_mutex_lock(&builder->entries_lock);

    /* Grow array if needed */
    if (builder->entry_count >= builder->entry_capacity) {
        builder->entry_capacity *= 2;
        builder->entries = g_realloc(builder->entries,
                                     builder->entry_capacity * sizeof(atlas_build_entry_t));
    }

    /* Store entry */
    atlas_build_entry_t* entry = &builder->entries[builder->entry_count++];
    entry->album_id = album_id;
    entry->png_data = png_data;
    entry->png_size = png_size;

    g_mutex_unlock(&builder->entries_lock);

    atomic_fetch_add(&builder->processed_count, 1);
    return QUADRATURE_OK;
}

quadrature_result_t artwork_atlas_add_cached_png(artwork_atlas_builder_t* builder,
                                                  int64_t album_id,
                                                  const void* png_data,
                                                  size_t png_size) {
    if (!builder || !png_data || png_size == 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    uint8_t* data_copy = g_malloc(png_size);
    memcpy(data_copy, png_data, png_size);

    g_mutex_lock(&builder->entries_lock);
    if (builder->entry_count >= builder->entry_capacity) {
        builder->entry_capacity *= 2;
        builder->entries = g_realloc(builder->entries,
                                     builder->entry_capacity * sizeof(atlas_build_entry_t));
    }
    atlas_build_entry_t* entry = &builder->entries[builder->entry_count++];
    entry->album_id = album_id;
    entry->png_data = data_copy;
    entry->png_size = png_size;
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
        /* No artwork - don't create empty atlas */
        return QUADRATURE_OK;
    }

    /* Sort entries by album_id */
    qsort(builder->entries, builder->entry_count,
          sizeof(atlas_build_entry_t), compare_atlas_entries);

    /* Open temp file for writing */
    FILE *f = fopen(builder->temp_path, "wb");
    if (!f) {
        g_warning("Failed to create atlas temp file: %s", builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Write header */
    artwork_atlas_header_t header = {0};
    memcpy(header.magic, ARTWORK_ATLAS_MAGIC, ARTWORK_ATLAS_MAGIC_SIZE);
    header.version = ARTWORK_ATLAS_VERSION;
    header.count = (uint32_t)builder->entry_count;
    header.flags = 0;
    header.thumb_size = (uint32_t)builder->thumb_size;

    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Write index entries */
    uint32_t current_offset = 0;
    for (size_t i = 0; i < builder->entry_count; i++) {
        artwork_atlas_entry_t entry = {
            .album_id = builder->entries[i].album_id,
            .offset = current_offset,
            .size = (uint32_t)builder->entries[i].png_size
        };
        if (fwrite(&entry, sizeof(entry), 1, f) != 1) {
            fclose(f);
            unlink(builder->temp_path);
            return QUADRATURE_ERROR_INTERNAL;
        }
        current_offset += (uint32_t)builder->entries[i].png_size;
    }

    /* Write data section (PNG blobs) */
    size_t total_data_size = 0;
    for (size_t i = 0; i < builder->entry_count; i++) {
        if (fwrite(builder->entries[i].png_data, 1, builder->entries[i].png_size, f)
            != builder->entries[i].png_size) {
            fclose(f);
            unlink(builder->temp_path);
            return QUADRATURE_ERROR_INTERNAL;
        }
        total_data_size += builder->entries[i].png_size;
    }

    fclose(f);

    /* Atomic rename */
    if (rename(builder->temp_path, builder->atlas_path) != 0) {
        g_warning("Failed to rename atlas file: %s -> %s",
                  builder->temp_path, builder->atlas_path);
        unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_info("Atlas created: %s (%zu entries, %zu bytes data)",
           builder->atlas_path, builder->entry_count, total_data_size);

    return QUADRATURE_OK;
}

void artwork_atlas_builder_destroy(artwork_atlas_builder_t* builder) {
    if (!builder) return;

    /* Free PNG data */
    if (builder->entries) {
        for (size_t i = 0; i < builder->entry_count; i++) {
            g_free(builder->entries[i].png_data);
        }
        g_free(builder->entries);
    }

    /* Remove temp file if it exists */
    if (builder->temp_path) {
        unlink(builder->temp_path);
        g_free(builder->temp_path);
    }

    g_free(builder->atlas_path);
    g_mutex_clear(&builder->entries_lock);
    g_free(builder);
}

