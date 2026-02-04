#ifndef INDEXER_INTERNAL_H
#define INDEXER_INTERNAL_H

/**
 * Internal header for indexer implementation.
 * Contains all private types and shared constants.
 */

#include "quadrature/quadrature.h"
#include "quadrature/quadrature_database.h"
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/stat.h>

// =============================================================================
// Constants
// =============================================================================

#define INDEXER_PATH_MAX 4096
#define INDEXER_PATH_SUFFIX_RESERVE 32

// =============================================================================
// Index Item (internal transfer structure)
// =============================================================================

typedef struct {
    char* path;         // Full path (owned, must free)
    char* title;        // Track title (owned, may be NULL)
    char* artist;       // Artist name (owned, may be NULL)
    char* album_artist; // Album artist (owned, may be NULL — from ALBUMARTIST tag)
    char* album;        // Album title (owned, may be NULL)
    char* genre;        // Genre (owned, may be NULL)
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;  // From disc/discnumber tag (0 = unset)
    uint16_t year;
    int64_t mtime;      // File modification time (filled by caller)
    int64_t size;       // File size in bytes (filled by caller)
} index_item_t;

// =============================================================================
// Directory Scan Result (single-pass enumeration)
// =============================================================================

typedef struct {
    char** files;           // Array of full file paths (audio files only)
    struct stat* stats;     // Parallel array of stat results for files
    size_t file_count;      // Number of files
    size_t file_capacity;   // Allocated capacity for files/stats arrays

    char** subdirs;         // Array of full subdir paths
    size_t subdir_count;    // Number of subdirectories
    size_t subdir_capacity; // Allocated capacity for subdirs array
} dir_scan_result_t;

// Initialize a dir_scan_result_t (zeroes all fields)
void dir_scan_result_init(dir_scan_result_t* result);

// Free all memory in a dir_scan_result_t (does not free the struct itself)
void dir_scan_result_free(dir_scan_result_t* result);

// Single-pass directory scan using d_type optimization
// Populates result with audio files and subdirectories
void dir_scan_single_pass(const char* dir_path, dir_scan_result_t* result);

// Free index_item and its members (path, title, artist, album are freed)
void index_item_free(index_item_t* item);

// =============================================================================
// Utils: Audio File Detection & Metadata Extraction
// =============================================================================

extern const char* AUDIO_EXTENSIONS[];

bool is_audio_file(const char* path);
quadrature_result_t extract_audio_metadata(const char* path, index_item_t* out);
size_t scan_audio_files_in_dir(const char* dir, char*** files_out, struct stat** stats_out);

// =============================================================================
// Utils: Disc Folder Detection (for multi-disc albums)
// =============================================================================

/**
 * Check if a directory name matches a disc folder pattern.
 * Patterns: CD1, CD 1, CD-1, Disc1, Disc 1, Disc-1, Disc One, D1, d1, etc.
 * Case-insensitive, supports numbers 1-99.
 *
 * @param dir_name Directory name (not full path)
 * @return true if the name matches a disc folder pattern
 */
bool is_disc_folder(const char* dir_name);

/**
 * Extract the disc number from a disc folder name.
 *
 * @param dir_name Directory name (not full path)
 * @return Disc number (1-99), or 0 if not a valid disc folder
 */
uint16_t get_disc_number_from_folder(const char* dir_name);

// =============================================================================
// Artwork
// =============================================================================

typedef enum {
    ART_SOURCE_NONE = 0,
    ART_SOURCE_COVER,
    ART_SOURCE_FOLDER,
    ART_SOURCE_FRONT,
    ART_SOURCE_ALBUMART,
    ART_SOURCE_EMBEDDED
} art_source_t;

quadrature_result_t artwork_find(const char* album_dir, char* art_path,
                                  size_t path_size, art_source_t* source);
quadrature_result_t artwork_extract_embedded(const char* audio_path, const char* output_path);
quadrature_result_t artwork_generate_thumbnail(const char* input_path,
                                                const char* output_path, int size);
quadrature_result_t artwork_find_and_process(const char* album_dir, int64_t album_id,
                                              const char* cache_dir, int thumb_size,
                                              char* result_path, size_t result_size);

// =============================================================================
// Artwork Atlas Format
//
// Binary format for packed album artwork thumbnails.
// Provides O(log n) lookup via binary search on sorted album_id index.
// =============================================================================

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

// =============================================================================
// Artwork Atlas Builder
// =============================================================================

/**
 * Opaque handle for thread-safe atlas building.
 * Supports concurrent album processing from multiple worker threads.
 */
typedef struct artwork_atlas_builder artwork_atlas_builder_t;

/**
 * Create a new atlas builder.
 * Thread-safe for concurrent album processing.
 *
 * @param atlas_path Final path for the atlas file
 * @param thumb_size Thumbnail size in pixels (typically 48-300)
 * @param out Output pointer for the builder handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t artwork_atlas_builder_create(const char* atlas_path,
                                                  int thumb_size,
                                                  artwork_atlas_builder_t** out);

/**
 * Process a single album's artwork (thread-safe).
 * Can be called concurrently from multiple worker threads.
 *
 * @param builder The atlas builder handle
 * @param album_id Database album ID
 * @param album_dir Directory containing the album
 * @return QUADRATURE_OK on success, error code if no artwork found
 */
quadrature_result_t artwork_atlas_process_album(artwork_atlas_builder_t* builder,
                                                 int64_t album_id,
                                                 const char* album_dir);

/**
 * Get current progress counters (thread-safe).
 *
 * @param builder The atlas builder handle
 * @param processed_out Output for number of albums processed
 * @param errors_out Output for number of errors encountered
 */
void artwork_atlas_builder_get_progress(artwork_atlas_builder_t* builder,
                                        size_t* processed_out,
                                        size_t* errors_out);

/**
 * Check if cancellation was requested (thread-safe).
 *
 * @param builder The atlas builder handle
 * @return true if cancelled
 */
bool artwork_atlas_builder_is_cancelled(artwork_atlas_builder_t* builder);

/**
 * Request cancellation (thread-safe).
 * Workers should check is_cancelled periodically.
 *
 * @param builder The atlas builder handle
 */
void artwork_atlas_builder_cancel(artwork_atlas_builder_t* builder);

/**
 * Finish building and write the atlas file.
 * Must be called after all worker threads have finished.
 * Sorts entries by album_id and writes atomically.
 *
 * @param builder The atlas builder handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t artwork_atlas_builder_finish(artwork_atlas_builder_t* builder);

/**
 * Destroy the atlas builder and free resources.
 * Call after finish() or to abort.
 *
 * @param builder The atlas builder handle (may be NULL)
 */
void artwork_atlas_builder_destroy(artwork_atlas_builder_t* builder);

/**
 * Add pre-generated PNG data to the atlas (for cached entries).
 */
quadrature_result_t artwork_atlas_add_cached_png(artwork_atlas_builder_t* builder,
                                                  int64_t album_id,
                                                  const void* png_data,
                                                  size_t png_size);

// =============================================================================
// Atlas Reader (for loading cached entries from existing atlas)
// =============================================================================

typedef struct artwork_atlas_reader artwork_atlas_reader_t;  // Defined in artwork.c

artwork_atlas_reader_t* artwork_atlas_reader_open(const char* path);
void artwork_atlas_reader_close(artwork_atlas_reader_t* reader);
uint8_t* artwork_atlas_reader_get_png(artwork_atlas_reader_t* reader, int64_t album_id, size_t* size_out);
size_t artwork_atlas_reader_get_count(artwork_atlas_reader_t* reader);
int64_t artwork_atlas_reader_get_album_id_at(artwork_atlas_reader_t* reader, size_t index);
uint8_t* artwork_atlas_reader_get_png_at(artwork_atlas_reader_t* reader, size_t index, size_t* size_out);

#endif // INDEXER_INTERNAL_H
