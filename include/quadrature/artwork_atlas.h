/**
 * Quadrature Artwork Atlas
 *
 * Binary format for packed album artwork thumbnails.
 * Provides O(log n) lookup via binary search on sorted album_id index.
 *
 * File Format:
 * ┌──────────────────────────────────────────────────────────────┐
 * │ HEADER (32 bytes)                                            │
 * ├──────────────────────────────────────────────────────────────┤
 * │   magic:     "QDRA" (4 bytes)                                │
 * │   version:   uint32_t = 1                                    │
 * │   count:     uint32_t (number of entries)                    │
 * │   flags:     uint32_t (reserved)                             │
 * │   thumb_size: uint32_t (48)                                  │
 * │   reserved:  12 bytes                                        │
 * ├──────────────────────────────────────────────────────────────┤
 * │ INDEX (16 bytes × count, sorted by album_id)                 │
 * ├──────────────────────────────────────────────────────────────┤
 * │   album_id:  int64_t                                         │
 * │   offset:    uint32_t (from start of DATA section)           │
 * │   size:      uint32_t (PNG blob size in bytes)               │
 * ├──────────────────────────────────────────────────────────────┤
 * │ DATA (variable, PNG blobs concatenated)                      │
 * └──────────────────────────────────────────────────────────────┘
 */

#ifndef QUADRATURE_ARTWORK_ATLAS_H
#define QUADRATURE_ARTWORK_ATLAS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic bytes identifying atlas file */
#define ARTWORK_ATLAS_MAGIC "QDRA"
#define ARTWORK_ATLAS_MAGIC_SIZE 4

/* Current format version */
#define ARTWORK_ATLAS_VERSION 1

/* Default thumbnail size */
#define ARTWORK_ATLAS_THUMB_SIZE 48

/**
 * Atlas file header (32 bytes, fixed size)
 */
typedef struct __attribute__((packed)) {
    char magic[4];          /* "QDRA" */
    uint32_t version;       /* Format version (currently 1) */
    uint32_t count;         /* Number of entries in index */
    uint32_t flags;         /* Reserved for future use */
    uint32_t thumb_size;    /* Thumbnail size in pixels (48) */
    uint8_t reserved[12];   /* Reserved for future use */
} artwork_atlas_header_t;

/**
 * Index entry (16 bytes, fixed size)
 * Sorted by album_id for binary search
 */
typedef struct __attribute__((packed)) {
    int64_t album_id;       /* Album identifier (database primary key) */
    uint32_t offset;        /* Byte offset from start of DATA section */
    uint32_t size;          /* Size of PNG blob in bytes */
} artwork_atlas_entry_t;

/* Verify struct sizes at compile time */
_Static_assert(sizeof(artwork_atlas_header_t) == 32, "Header must be 32 bytes");
_Static_assert(sizeof(artwork_atlas_entry_t) == 16, "Entry must be 16 bytes");

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_ARTWORK_ATLAS_H */
