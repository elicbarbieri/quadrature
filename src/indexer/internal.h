#ifndef INDEXER_INTERNAL_H
#define INDEXER_INTERNAL_H

/**
 * Internal header for indexer implementation.
 * Contains all private types and shared constants.
 */

#include "quadrature/core/types.h"
#include "quadrature/database/database.h"
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
    char* album;        // Album title (owned, may be NULL)
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t year;
    int64_t mtime;      // File modification time (filled by caller)
    int64_t size;       // File size in bytes (filled by caller)
} index_item_t;

// =============================================================================
// Extended Metadata (for folder album context and metadata popup)
// =============================================================================

typedef struct {
    // Basic metadata (same as index_item_t)
    char* path;
    char* title;
    char* artist;
    char* album;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
    int64_t mtime;
    int64_t size;

    // Extended metadata
    char* album_artist;      // ALBUMARTIST tag
    char* genre;
    char* comment;
    char* encoder;
    bool compilation;        // COMPILATION or TCMP tag
    uint16_t disc_total;     // Total disc count (from "N/M" format)
    uint16_t track_total;    // Total track count (from "N/M" format)

    // Audio format info
    int32_t bitrate;         // kbps
    int32_t sample_rate;     // Hz
    int32_t channels;
    char* codec;             // e.g., "FLAC", "MP3", "AAC"

    // Flags
    bool has_embedded_art;

    // Raw metadata as JSON (for Copy JSON feature)
    char* raw_json;
} extended_metadata_t;

// Free extended_metadata_t and its members
void extended_metadata_free(extended_metadata_t* meta);

// Extract extended metadata from audio file
quadrature_result_t extract_extended_metadata(const char* path, extended_metadata_t* out);

// =============================================================================
// Folder Album Context (for directory-first album grouping)
// =============================================================================

// Opaque handle - defined in folder_album.c
typedef struct folder_album_context folder_album_context_t;

// Create a new folder album context for the given directory
folder_album_context_t* folder_album_context_new(const char* dir_path);

// Add track metadata to the context (call for each track in directory)
void folder_album_context_add_track(folder_album_context_t* ctx,
                                     const extended_metadata_t* meta);

// Finalize the context - determines album title, artist, compilation status
void folder_album_finalize(folder_album_context_t* ctx);

// Free the context
void folder_album_context_free(folder_album_context_t* ctx);

// Accessors (call after folder_album_finalize)
const char* folder_album_get_directory_path(const folder_album_context_t* ctx);
const char* folder_album_get_folder_name(const folder_album_context_t* ctx);
const char* folder_album_get_title(const folder_album_context_t* ctx);
const char* folder_album_get_artist(const folder_album_context_t* ctx);
bool folder_album_is_compilation(const folder_album_context_t* ctx);
uint16_t folder_album_get_year(const folder_album_context_t* ctx);
size_t folder_album_get_track_count(const folder_album_context_t* ctx);
size_t folder_album_get_unique_artist_count(const folder_album_context_t* ctx);

// Validation helpers - check if track metadata differs from folder album
bool folder_album_track_has_album_mismatch(const folder_album_context_t* ctx,
                                            const extended_metadata_t* meta);
bool folder_album_track_has_artist_inconsistency(const folder_album_context_t* ctx,
                                                  const extended_metadata_t* meta);

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
