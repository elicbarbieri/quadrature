/**
 * Global artist atlas: UUID-keyed binary atlas shared across all libraries.
 *
 * File layout (v1):
 *   [Header 32B]
 *   [uuid_keys: uint8_t[art_count][16]]          sorted binary UUIDs with artwork
 *   [pixels: uint8_t[art_count][pixel_stride]]    dense RGB pixel data
 *   [no_art_count: uint32_t]                      trailing count
 *   [no_art_uuids: uint8_t[no_art_count][16]]     sorted binary UUIDs with no artwork
 *
 * Write serialization: callers hold flock() on artists.atlas.lock.
 * Readers mmap the file and do binary search on 16-byte UUID keys.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

#include <glib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// =============================================================================
// UUID Helpers
// =============================================================================

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool mbid_parse(const char* str, uint8_t out[ARTIST_ATLAS_UUID_SIZE]) {
    if (!str) return false;
    // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" = 36 chars
    int byte_idx = 0;
    for (int i = 0; i < 36 && byte_idx < 16; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (str[i] != '-') return false;
            continue;
        }
        int hi = hex_digit(str[i]);
        int lo = hex_digit(str[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[byte_idx++] = (uint8_t)((hi << 4) | lo);
        i++;  // consumed two chars
    }
    return byte_idx == 16 && str[36] == '\0';
}

void mbid_format(const uint8_t uuid[ARTIST_ATLAS_UUID_SIZE], char out[37]) {
    static const int dash_positions[] = {8, 12, 16, 20};
    int dash_idx = 0;
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (dash_idx < 4 && i == dash_positions[dash_idx]) {
            out[pos++] = '-';
            dash_idx++;
        }
        static const char hex[] = "0123456789abcdef";
        out[pos++] = hex[uuid[i] >> 4];
        out[pos++] = hex[uuid[i] & 0x0f];
    }
    out[pos] = '\0';
}

// =============================================================================
// Artist Atlas Builder
// =============================================================================

typedef struct {
    uint8_t uuid[ARTIST_ATLAS_UUID_SIZE];
    uint8_t *pixel_data;  // owned, NULL for no-art entries
} artist_atlas_entry_t;

struct artist_atlas_builder {
    char *atlas_path;
    char *temp_path;
    int thumb_size;
    uint32_t pixel_stride;

    /* Entries with artwork */
    artist_atlas_entry_t *art_entries;
    size_t art_count;
    size_t art_capacity;

    /* Known no-artwork UUIDs */
    uint8_t (*no_art_uuids)[ARTIST_ATLAS_UUID_SIZE];
    size_t no_art_count;
    size_t no_art_capacity;
};

static int compare_uuids(const void* a, const void* b) {
    return memcmp(a, b, ARTIST_ATLAS_UUID_SIZE);
}

static int compare_art_entries(const void* a, const void* b) {
    const artist_atlas_entry_t *ea = a;
    const artist_atlas_entry_t *eb = b;
    return memcmp(ea->uuid, eb->uuid, ARTIST_ATLAS_UUID_SIZE);
}

quadrature_result_t artist_atlas_builder_create(const char* atlas_path,
                                                 int thumb_size,
                                                 artist_atlas_builder_t** out) {
    if (!atlas_path || thumb_size <= 0 || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    artist_atlas_builder_t* b = g_new0(artist_atlas_builder_t, 1);
    b->atlas_path = g_strdup(atlas_path);
    b->temp_path = g_strdup_printf("%s.tmp.%d", atlas_path, (int)getpid());
    b->thumb_size = thumb_size;
    b->pixel_stride = (uint32_t)thumb_size * thumb_size * ARTWORK_ATLAS_CHANNELS;

    b->art_capacity = 256;
    b->art_entries = g_new0(artist_atlas_entry_t, b->art_capacity);

    b->no_art_capacity = 256;
    b->no_art_uuids = g_malloc(b->no_art_capacity * ARTIST_ATLAS_UUID_SIZE);

    *out = b;
    return QUADRATURE_OK;
}

quadrature_result_t artist_atlas_builder_load_existing(artist_atlas_builder_t* builder) {
    if (!builder) return QUADRATURE_ERROR_INVALID_PARAM;

    int fd = open(builder->atlas_path, O_RDONLY);
    if (fd < 0) return QUADRATURE_OK;  // No existing atlas — that's fine

    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(artist_atlas_header_t) + ATLAS_CRC32_SIZE) {
        close(fd);
        return QUADRATURE_OK;
    }

    void* map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return QUADRATURE_OK;

    const artist_atlas_header_t* h = map;
    if (memcmp(h->magic, ARTIST_ATLAS_MAGIC, ARTIST_ATLAS_MAGIC_SIZE) != 0 ||
        h->version != ARTIST_ATLAS_VERSION) {
        munmap(map, st.st_size);
        return QUADRATURE_OK;  // Incompatible version — start fresh
    }

    // Validate CRC32 trailing checksum
    {
        size_t body_size = (size_t)st.st_size - ATLAS_CRC32_SIZE;
        uint32_t expected_crc;
        memcpy(&expected_crc, (const uint8_t*)map + body_size, sizeof(expected_crc));
        uint32_t actual_crc = atlas_crc32((const uint8_t*)map, body_size);
        if (actual_crc != expected_crc) {
            g_warning("artist_atlas: CRC32 mismatch in existing atlas, starting fresh");
            munmap(map, st.st_size);
            return QUADRATURE_OK;
        }
    }

    uint32_t pixel_stride = h->thumb_size * h->thumb_size * h->channels;

    // Validate file size
    size_t expected_min = sizeof(*h)
        + (size_t)h->art_count * ARTIST_ATLAS_UUID_SIZE
        + (size_t)h->art_count * pixel_stride
        + sizeof(uint32_t)
        + (size_t)h->no_art_count * ARTIST_ATLAS_UUID_SIZE
        + ATLAS_CRC32_SIZE;

    if ((size_t)st.st_size < expected_min) {
        munmap(map, st.st_size);
        g_warning("artist_atlas: existing file too small, starting fresh");
        return QUADRATURE_OK;
    }

    const uint8_t* ptr = (const uint8_t*)map + sizeof(*h);
    const uint8_t* uuid_keys = ptr;
    const uint8_t* pixels = uuid_keys + (size_t)h->art_count * ARTIST_ATLAS_UUID_SIZE;

    // Load artwork entries
    for (uint32_t i = 0; i < h->art_count; i++) {
        if (builder->art_count >= builder->art_capacity) {
            builder->art_capacity *= 2;
            builder->art_entries = g_realloc(builder->art_entries,
                builder->art_capacity * sizeof(artist_atlas_entry_t));
        }
        artist_atlas_entry_t* e = &builder->art_entries[builder->art_count++];
        memcpy(e->uuid, uuid_keys + i * ARTIST_ATLAS_UUID_SIZE, ARTIST_ATLAS_UUID_SIZE);

        // Only copy pixel data if thumb sizes match
        if (pixel_stride == builder->pixel_stride) {
            e->pixel_data = g_malloc(builder->pixel_stride);
            memcpy(e->pixel_data, pixels + (size_t)i * pixel_stride, pixel_stride);
        } else {
            // Thumb size changed — entry will be dropped (no pixel data)
            e->pixel_data = NULL;
            builder->art_count--;
        }
    }

    // Load no-art entries — these are after the pixel data
    const uint8_t* after_pixels = pixels + (size_t)h->art_count * pixel_stride;
    // Skip the uint32_t no_art_count (already in header)
    const uint8_t* no_art_data = after_pixels + sizeof(uint32_t);

    for (uint32_t i = 0; i < h->no_art_count; i++) {
        if (builder->no_art_count >= builder->no_art_capacity) {
            builder->no_art_capacity *= 2;
            builder->no_art_uuids = g_realloc(builder->no_art_uuids,
                builder->no_art_capacity * ARTIST_ATLAS_UUID_SIZE);
        }
        memcpy(builder->no_art_uuids[builder->no_art_count++],
               no_art_data + i * ARTIST_ATLAS_UUID_SIZE,
               ARTIST_ATLAS_UUID_SIZE);
    }

    uint32_t loaded_art = h->art_count;
    uint32_t loaded_no_art = h->no_art_count;
    munmap(map, st.st_size);
    g_info("artist_atlas: loaded existing atlas (%u art, %u no-art)",
           loaded_art, loaded_no_art);
    return QUADRATURE_OK;
}

/**
 * Find existing entry by UUID. Returns index or -1.
 * Linear scan — only used during building (not display-time).
 */
static int find_art_entry(artist_atlas_builder_t* b, const uint8_t uuid[16]) {
    for (size_t i = 0; i < b->art_count; i++) {
        if (memcmp(b->art_entries[i].uuid, uuid, ARTIST_ATLAS_UUID_SIZE) == 0)
            return (int)i;
    }
    return -1;
}

static int find_no_art_entry(artist_atlas_builder_t* b, const uint8_t uuid[16]) {
    for (size_t i = 0; i < b->no_art_count; i++) {
        if (memcmp(b->no_art_uuids[i], uuid, ARTIST_ATLAS_UUID_SIZE) == 0)
            return (int)i;
    }
    return -1;
}

quadrature_result_t artist_atlas_builder_add_art(artist_atlas_builder_t* builder,
                                                  const uint8_t uuid[16],
                                                  const void* pixel_data,
                                                  size_t pixel_size) {
    if (!builder || !uuid || !pixel_data || pixel_size != builder->pixel_stride)
        return QUADRATURE_ERROR_INVALID_PARAM;

    // Remove from no-art list if present (artist now has art)
    int no_art_idx = find_no_art_entry(builder, uuid);
    if (no_art_idx >= 0) {
        builder->no_art_count--;
        if ((size_t)no_art_idx < builder->no_art_count) {
            memmove(&builder->no_art_uuids[no_art_idx],
                    &builder->no_art_uuids[no_art_idx + 1],
                    (builder->no_art_count - no_art_idx) * ARTIST_ATLAS_UUID_SIZE);
        }
    }

    // Update existing or add new
    int existing = find_art_entry(builder, uuid);
    if (existing >= 0) {
        memcpy(builder->art_entries[existing].pixel_data, pixel_data, pixel_size);
        return QUADRATURE_OK;
    }

    if (builder->art_count >= builder->art_capacity) {
        builder->art_capacity *= 2;
        builder->art_entries = g_realloc(builder->art_entries,
            builder->art_capacity * sizeof(artist_atlas_entry_t));
    }

    artist_atlas_entry_t* e = &builder->art_entries[builder->art_count++];
    memcpy(e->uuid, uuid, ARTIST_ATLAS_UUID_SIZE);
    e->pixel_data = g_malloc(pixel_size);
    memcpy(e->pixel_data, pixel_data, pixel_size);
    return QUADRATURE_OK;
}

quadrature_result_t artist_atlas_builder_add_no_art(artist_atlas_builder_t* builder,
                                                     const uint8_t uuid[16]) {
    if (!builder || !uuid) return QUADRATURE_ERROR_INVALID_PARAM;

    // Don't add if already in art list (has real artwork)
    if (find_art_entry(builder, uuid) >= 0) return QUADRATURE_OK;

    // Don't add duplicates
    if (find_no_art_entry(builder, uuid) >= 0) return QUADRATURE_OK;

    if (builder->no_art_count >= builder->no_art_capacity) {
        builder->no_art_capacity *= 2;
        builder->no_art_uuids = g_realloc(builder->no_art_uuids,
            builder->no_art_capacity * ARTIST_ATLAS_UUID_SIZE);
    }

    memcpy(builder->no_art_uuids[builder->no_art_count++], uuid, ARTIST_ATLAS_UUID_SIZE);
    return QUADRATURE_OK;
}

quadrature_result_t artist_atlas_builder_finish(artist_atlas_builder_t* builder) {
    if (!builder) return QUADRATURE_ERROR_INVALID_PARAM;

    if (builder->art_count == 0 && builder->no_art_count == 0) {
        g_info("artist_atlas: nothing to write (0 art, 0 no-art)");
        return QUADRATURE_OK;
    }

    // Sort both arrays by UUID
    qsort(builder->art_entries, builder->art_count,
          sizeof(artist_atlas_entry_t), compare_art_entries);
    qsort(builder->no_art_uuids, builder->no_art_count,
          ARTIST_ATLAS_UUID_SIZE, compare_uuids);

    FILE* f = fopen(builder->temp_path, "w+b");
    if (!f) {
        g_warning("artist_atlas: failed to create temp file: %s", builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Write header
    artist_atlas_header_t header = {0};
    memcpy(header.magic, ARTIST_ATLAS_MAGIC, ARTIST_ATLAS_MAGIC_SIZE);
    header.version = ARTIST_ATLAS_VERSION;
    header.art_count = (uint32_t)builder->art_count;
    header.no_art_count = (uint32_t)builder->no_art_count;
    header.thumb_size = (uint32_t)builder->thumb_size;
    header.channels = ARTWORK_ATLAS_CHANNELS;

    if (fwrite(&header, sizeof(header), 1, f) != 1) goto write_error;

    // Write sorted UUID keys (art entries)
    for (size_t i = 0; i < builder->art_count; i++) {
        if (fwrite(builder->art_entries[i].uuid, ARTIST_ATLAS_UUID_SIZE, 1, f) != 1)
            goto write_error;
    }

    // Write pixel data
    for (size_t i = 0; i < builder->art_count; i++) {
        if (fwrite(builder->art_entries[i].pixel_data, builder->pixel_stride, 1, f) != 1)
            goto write_error;
    }

    // Write trailing no-art section
    uint32_t no_art_count = (uint32_t)builder->no_art_count;
    if (fwrite(&no_art_count, sizeof(no_art_count), 1, f) != 1)
        goto write_error;

    if (builder->no_art_count > 0) {
        if (fwrite(builder->no_art_uuids, ARTIST_ATLAS_UUID_SIZE,
                   builder->no_art_count, f) != builder->no_art_count)
            goto write_error;
    }

    // Compute and append CRC32 trailing checksum
    {
        long body_size = ftell(f);
        if (body_size < 0) goto write_error;
        rewind(f);
        uint8_t* buf = g_malloc(body_size);
        if (fread(buf, 1, body_size, f) != (size_t)body_size) {
            g_free(buf);
            goto write_error;
        }
        uint32_t crc = atlas_crc32(buf, body_size);
        g_free(buf);
        fseek(f, 0, SEEK_END);
        if (fwrite(&crc, sizeof(crc), 1, f) != 1) goto write_error;
    }

    fclose(f);

    // Atomic rename
    if (rename(builder->temp_path, builder->atlas_path) != 0) {
        g_warning("artist_atlas: rename failed: %s -> %s",
                  builder->temp_path, builder->atlas_path);
        unlink(builder->temp_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_info("artist_atlas: written %s (%u art, %u no-art, %u×%upx)",
           builder->atlas_path, header.art_count, header.no_art_count,
           header.thumb_size, header.thumb_size);
    return QUADRATURE_OK;

write_error:
    fclose(f);
    unlink(builder->temp_path);
    return QUADRATURE_ERROR_INTERNAL;
}

void artist_atlas_builder_destroy(artist_atlas_builder_t* builder) {
    if (!builder) return;
    for (size_t i = 0; i < builder->art_count; i++)
        g_free(builder->art_entries[i].pixel_data);
    g_free(builder->art_entries);
    g_free(builder->no_art_uuids);
    g_free(builder->atlas_path);
    g_free(builder->temp_path);
    g_free(builder);
}

// =============================================================================
// Artist Atlas Reader
// =============================================================================

struct artist_atlas_reader {
    void* map;
    size_t map_size;
    int fd;
    const artist_atlas_header_t* header;
    const uint8_t* uuid_keys;      // sorted art UUIDs
    const uint8_t* pixel_data;     // dense pixel arrays
    const uint8_t* no_art_uuids;   // sorted no-art UUIDs
    uint32_t pixel_stride;
};

artist_atlas_reader_t* artist_atlas_reader_open(const char* path) {
    if (!path) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(artist_atlas_header_t) + ATLAS_CRC32_SIZE) {
        close(fd);
        return NULL;
    }

    void* map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }

    const artist_atlas_header_t* h = map;
    if (memcmp(h->magic, ARTIST_ATLAS_MAGIC, ARTIST_ATLAS_MAGIC_SIZE) != 0 ||
        h->version != ARTIST_ATLAS_VERSION) {
        munmap(map, st.st_size);
        close(fd);
        return NULL;
    }

    /* Validate CRC32 trailing checksum */
    {
        size_t body_size = (size_t)st.st_size - ATLAS_CRC32_SIZE;
        uint32_t expected_crc;
        memcpy(&expected_crc, (const uint8_t*)map + body_size, sizeof(expected_crc));
        uint32_t actual_crc = atlas_crc32((const uint8_t*)map, body_size);
        if (actual_crc != expected_crc) {
            g_warning("Artist atlas CRC32 mismatch in %s (expected 0x%08X, got 0x%08X)",
                      path, expected_crc, actual_crc);
            munmap(map, st.st_size);
            close(fd);
            return NULL;
        }
        g_debug("Artist atlas CRC32 verified: %s (0x%08X)", path, actual_crc);
    }

    uint32_t pixel_stride = h->thumb_size * h->thumb_size * h->channels;

    size_t expected = sizeof(*h)
        + (size_t)h->art_count * ARTIST_ATLAS_UUID_SIZE
        + (size_t)h->art_count * pixel_stride
        + sizeof(uint32_t)
        + (size_t)h->no_art_count * ARTIST_ATLAS_UUID_SIZE
        + ATLAS_CRC32_SIZE;

    if ((size_t)st.st_size < expected) {
        munmap(map, st.st_size);
        close(fd);
        return NULL;
    }

    artist_atlas_reader_t* r = g_new0(artist_atlas_reader_t, 1);
    r->map = map;
    r->map_size = st.st_size;
    r->fd = fd;
    r->header = h;
    r->pixel_stride = pixel_stride;

    const uint8_t* ptr = (const uint8_t*)map + sizeof(*h);
    r->uuid_keys = ptr;
    r->pixel_data = ptr + (size_t)h->art_count * ARTIST_ATLAS_UUID_SIZE;

    const uint8_t* after_pixels = r->pixel_data + (size_t)h->art_count * pixel_stride;
    // Skip uint32_t no_art_count (redundant with header)
    r->no_art_uuids = after_pixels + sizeof(uint32_t);

    g_info("artist_atlas: opened %s (%u art, %u no-art)",
           path, h->art_count, h->no_art_count);
    return r;
}

void artist_atlas_reader_close(artist_atlas_reader_t* reader) {
    if (!reader) return;
    if (reader->map && reader->map != MAP_FAILED)
        munmap(reader->map, reader->map_size);
    if (reader->fd >= 0) close(reader->fd);
    g_free(reader);
}

/**
 * Binary search on sorted 16-byte UUID keys.
 */
static int32_t uuid_binary_search(const uint8_t* keys, uint32_t count,
                                   const uint8_t uuid[16]) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(keys + mid * ARTIST_ATLAS_UUID_SIZE, uuid, ARTIST_ATLAS_UUID_SIZE);
        if (cmp == 0) return (int32_t)mid;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

int32_t artist_atlas_reader_lookup(const artist_atlas_reader_t* reader,
                                    const uint8_t uuid[16]) {
    if (!reader || !reader->header) return -1;
    return uuid_binary_search(reader->uuid_keys, reader->header->art_count, uuid);
}

bool artist_atlas_reader_is_no_art(const artist_atlas_reader_t* reader,
                                    const uint8_t uuid[16]) {
    if (!reader || !reader->header || reader->header->no_art_count == 0) return false;
    return uuid_binary_search(reader->no_art_uuids, reader->header->no_art_count, uuid) >= 0;
}

const uint8_t* artist_atlas_reader_get_pixels(const artist_atlas_reader_t* reader,
                                               int32_t index) {
    if (!reader || !reader->pixel_data || index < 0 ||
        (uint32_t)index >= reader->header->art_count)
        return NULL;
    return reader->pixel_data + (uint32_t)index * reader->pixel_stride;
}

uint32_t artist_atlas_reader_get_thumb_size(const artist_atlas_reader_t* reader) {
    return reader && reader->header ? reader->header->thumb_size : 0;
}

uint32_t artist_atlas_reader_get_pixel_stride(const artist_atlas_reader_t* reader) {
    return reader ? reader->pixel_stride : 0;
}

uint8_t artist_atlas_reader_get_channels(const artist_atlas_reader_t* reader) {
    return reader && reader->header ? reader->header->channels : 0;
}

uint32_t artist_atlas_reader_get_art_count(const artist_atlas_reader_t* reader) {
    return reader && reader->header ? reader->header->art_count : 0;
}

uint32_t artist_atlas_reader_get_no_art_count(const artist_atlas_reader_t* reader) {
    return reader && reader->header ? reader->header->no_art_count : 0;
}
