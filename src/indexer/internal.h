#ifndef INDEXER_INTERNAL_H
#define INDEXER_INTERNAL_H

/**
 * Internal header for indexer implementation.
 * Contains all private types and shared constants.
 */

#include "quadrature/quadrature.h"
#include "quadrature/database.h"
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
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
    char* path;              // Full path (owned, must free)
    char* title;             // Track title (owned, may be NULL)
    char* artist;            // Artist name (owned, may be NULL)
    char* album_artist;      // Album artist (owned, may be NULL — from ALBUMARTIST tag)
    char* album;             // Album title (owned, may be NULL)
    char* genre;             // Genre (owned, may be NULL)
    char* mb_release_id;         // MUSICBRAINZ_ALBUMID tag (owned, may be NULL)
    char* mb_release_group_id;   // MUSICBRAINZ_RELEASEGROUPID tag (owned, may be NULL)
    char* mb_artist_id;          // MUSICBRAINZ_ARTISTID tag (owned, may be NULL)
    char* mb_album_artist_id;    // MUSICBRAINZ_ALBUMARTISTID tag (owned, may be NULL)
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;       // From disc/discnumber tag (0 = unset)
    uint16_t year;
    int64_t mtime;           // File modification time (filled by caller)
    int64_t size;            // File size in bytes (filled by caller)
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
// Utils: Artist Tag Splitting
// =============================================================================

/**
 * One parsed credit from an ARTIST tag.
 * name and join_phrase are heap-allocated; call artist_credits_free() when done.
 */
typedef struct {
    char* name;        /* Artist name, e.g. "ADULT." or "Dorit Chrysler" */
    char* join_phrase; /* Connector placed after this name: " feat. ", " & ", "" for last */
} artist_credit_t;

/**
 * Parse a raw ARTIST tag into individual credits by splitting on common
 * feat./ft./& delimiters (case-insensitive).
 *
 * Always returns at least 1 credit (the whole string if no delimiter found).
 * join_phrase is normalised: " feat " → " feat. ", " ft " → " ft. ".
 *
 * @param tag   Raw ARTIST tag value (non-NULL).
 * @param out   Output array of credits (owned by caller).
 * @return      Number of credits written to *out.
 */
size_t parse_artist_tag(const char* tag, artist_credit_t** out);

/** Free credits returned by parse_artist_tag. */
void artist_credits_free(artist_credit_t* credits, size_t count);

/**
 * Extract featuring artists from a title string.
 * Matches (feat. ...), [feat. ...], (ft. ...), [ft. ...], (featuring ...) patterns.
 * Case-insensitive.
 *
 * @param title      Input title (not modified)
 * @param clean_out  Output: title with featuring group removed (g_free when done)
 * @param feat_out   Output: featuring artist string (g_free when done)
 * @return true if a featuring group was found and extracted
 */
bool title_extract_featuring(const char* title, char** clean_out, char** feat_out);

/**
 * Detect which artist delimiter (if any) is used across album tracks.
 * Checks ';' first (fewer false positives), then '/'.
 * A delimiter is confirmed when 2+ tags have different suffixes after it
 * (e.g., "A/B" and "A/C" → '/' is a delimiter; all "AC/DC" → not).
 *
 * @param artist_tags  Array of raw ARTIST tag strings (may contain NULLs)
 * @param count        Number of elements
 * @return Delimiter character (';' or '/'), or '\0' if none detected
 */
char detect_artist_delimiter(const char* const* artist_tags, size_t count);

// =============================================================================
// Utils: Audio File Detection & Metadata Extraction
// =============================================================================

extern const char* AUDIO_EXTENSIONS[];

bool is_audio_file(const char* path);
quadrature_result_t extract_audio_metadata(const char* path, index_item_t* out);

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
// Phase 2 Producer-Consumer Types
// =============================================================================

/**
 * Per-track FFmpeg extraction output (no DB IDs).
 * Built by worker threads, consumed by the DB writer thread.
 */
typedef struct {
    char* rel_path;        // Track path relative to album dir (owned)
    char* title;           // From tag or filename fallback (owned, may be NULL)
    char* artist_tag;      // Raw artist tag string for parse_artist_tag (owned, may be NULL)
    char* genre;           // Genre tag (owned, may be NULL)
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
    int64_t mtime;         // From stat()
} extracted_track_t;

/**
 * Per-album FFmpeg extraction output, pushed to GAsyncQueue by workers.
 * The DB writer thread pops these, writes to SQLite, and populates results.
 */
typedef struct {
    // Album-level
    char* dir_path;             // Full album directory path (owned)
    char* album_rel_path;       // Relative to library_root (owned)
    char* folder_name;          // Last path component / album title (owned)
    char* album_artist;         // Best album artist name (owned)
    char* album_mb_release_id;       // MUSICBRAINZ_ALBUMID tag (owned, may be NULL)
    char* album_mb_release_group_id; // MUSICBRAINZ_RELEASEGROUPID tag (owned, may be NULL)
    char* album_mb_artist_id;        // MUSICBRAINZ_ALBUMARTISTID tag (owned, may be NULL)
    int64_t dir_mtime;          // For finalize phase
    int64_t dir_size;           // Size fingerprint for two-factor delta detection
    bool mb_resolved;           // Cached from scan phase

    // Tracks
    extracted_track_t* tracks;
    size_t track_count;

    // Slot index into processed_album_t results array
    size_t result_index;
} metadata_result_t;

void extracted_track_free(extracted_track_t* track);
void metadata_result_free(metadata_result_t* result);

// =============================================================================
// Library Validation
// =============================================================================

/* Forward declaration from indexer.c */
typedef struct indexer indexer_t;

/**
 * Log an indexer error to the database.
 * Called from validation and indexer functions to record errors.
 *
 * @param idx Indexer context
 * @param path File or directory path where error occurred
 * @param fmt Printf-style format string
 */
void log_indexer_error(indexer_t* idx, const char* path, const char* fmt, ...);

/**
 * Validate track numbering for an album.
 *
 * Automatically detects continuous vs per-disc numbering patterns:
 *   - Per-disc:    Each disc resets to track 1 (e.g., Disc 1=[1-12], Disc 2=[1-10])
 *   - Continuous:  Track numbers increment globally (e.g., Disc 1=[1-12], Disc 2=[13-22])
 *
 * Logs errors via log_indexer_error() if gaps are found in the track sequence.
 *
 * @param idx Indexer context (for error logging)
 * @param mr Album metadata with extracted tracks
 */
void validate_album_track_numbering(indexer_t* idx, const metadata_result_t* mr);

// =============================================================================
// Artwork
// =============================================================================

/**
 * Find the best cover image file in an album directory.
 * Priority: cover > folder > front > albumart (.jpg/.jpeg/.png/.webp).
 * @param album_dir  Directory to search
 * @param art_path   Output buffer for the found path
 * @param path_size  Size of art_path buffer
 * @return QUADRATURE_OK with path written, or QUADRATURE_ERROR_FILE_NOT_FOUND
 */
quadrature_result_t artwork_find(const char* album_dir, char* art_path, size_t path_size);

/**
 * Return raw image bytes for the best artwork in an album directory.
 * Tries named cover files first, then falls back to embedded art in audio files.
 * On success, *data_out is g_malloc'd — caller must g_free().
 */
quadrature_result_t artwork_find_bytes(const char* album_dir,
                                       uint8_t** data_out, size_t* size_out);

// =============================================================================
// Artwork Atlas Format
//
// Binary format for packed album artwork thumbnails.
// Provides O(log n) lookup via binary search on sorted album_id index.
// =============================================================================

/* Magic bytes identifying atlas file */
#define ARTWORK_ATLAS_MAGIC "QDRA"
#define ARTWORK_ATLAS_MAGIC_SIZE 4

/* Current format version (v3 adds no_art section) */
#define ARTWORK_ATLAS_VERSION 3

/* Default thumbnail size and channels */
#define ARTWORK_ATLAS_THUMB_SIZE 48
#define ARTWORK_ATLAS_CHANNELS 3

/**
 * Atlas file header (32 bytes, fixed size)
 *
 * v3 layout:
 *   [Header 32B]
 *   [album_ids: int64_t[count]]           sorted art-entry IDs
 *   [pixels: uint8_t[count][pixel_stride]] dense RGB pixel data
 *   [no_art_count: uint32_t]              number of known-no-artwork entries
 *   [no_art_ids: int64_t[no_art_count]]   sorted album IDs with no artwork
 *   [CRC32: uint32_t]
 *
 * pixel_stride = thumb_size * thumb_size * channels
 * v2 files (no no_art section) are accepted by the reader.
 */
typedef struct __attribute__((packed)) {
    char magic[4];          /* "QDRA" */
    uint32_t version;       /* Format version (3) */
    uint32_t count;         /* Number of art entries (with pixel data) */
    uint32_t no_art_count;  /* Number of known-no-artwork entries (v2: was flags=0) */
    uint32_t thumb_size;    /* Thumbnail size in pixels (48) */
    uint8_t channels;       /* Color channels (3 = RGB) */
    uint8_t reserved[11];   /* Reserved for future use */
} artwork_atlas_header_t;

_Static_assert(sizeof(artwork_atlas_header_t) == 32, "Header must be 32 bytes");

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
 * Load all entries from the existing atlas on disk into the builder.
 * Art entries go into the pixel array; no-art entries go into the no_art list.
 * Handles both v2 (detects placeholders via memcmp) and v3 (reads no_art section).
 * Sets dirty=false. Call after create(), before process_album().
 */
quadrature_result_t artwork_atlas_builder_load_existing(artwork_atlas_builder_t* builder);

/**
 * Record an album as having no artwork (known-no-art sentinel).
 * Thread-safe. Stored in the no_art section of the v3 atlas.
 */
quadrature_result_t artwork_atlas_builder_add_no_art(artwork_atlas_builder_t* builder,
                                                      int64_t album_id);

/**
 * Sweep the no_art list: remove albums that now have a fanart cover on disk.
 * Call after load_existing() + set_fanart_covers_dir() + set_album_rg_map().
 * Returns the number of albums promoted. If promoted_ids is non-NULL, appends
 * the promoted album IDs to it (caller owns the GArray).
 */
size_t artwork_atlas_builder_sweep_no_art(artwork_atlas_builder_t* builder,
                                          GArray* promoted_ids);


/**
 * Set a directory of fanart.tv-sourced album cover images to use as a
 * fallback when no local or embedded artwork is found.
 * Files are named {release_group_id}.jpg (MusicBrainz UUIDs).
 * Must be called before process_album().
 */
void artwork_atlas_builder_set_fanart_covers_dir(artwork_atlas_builder_t* builder,
                                                  const char* dir);

/**
 * Register album_id → release_group_id mappings so the fanart cover
 * fallback can find cached covers keyed by release group UUID.
 * The builder takes ownership of the hash table.
 */
void artwork_atlas_builder_set_album_rg_map(artwork_atlas_builder_t* builder,
                                             GHashTable* album_id_to_rg_id);

/**
 * Process a single album's artwork (thread-safe).
 * Can be called concurrently from multiple worker threads.
 * If no artwork is found, stores a placeholder and sets *used_fallback = true.
 *
 * @param builder The atlas builder handle
 * @param album_id Database album ID
 * @param album_dir Directory containing the album
 * @param used_fallback Out: set to true if placeholder was stored (may be NULL)
 * @return QUADRATURE_OK on success (including placeholder), error on real failure
 */
quadrature_result_t artwork_atlas_process_album(artwork_atlas_builder_t* builder,
                                                 int64_t album_id,
                                                 const char* album_dir,
                                                 bool *used_fallback);

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
 * Add pre-generated raw pixel data to the atlas (for preserved entries).
 * pixel_data must be exactly pixel_stride bytes (thumb_size * thumb_size * channels).
 */
quadrature_result_t artwork_atlas_add_cached_pixels(artwork_atlas_builder_t* builder,
                                                     int64_t album_id,
                                                     const void* pixel_data,
                                                     size_t pixel_size);

/**
 * Per-album profiling stats accumulated by the builder (thread-safe reads).
 */
typedef struct {
    int64_t find_ns;        /* Total time in find_and_load_artwork */
    int64_t resize_ns;      /* Total time in vips resize (file or buffer path) */
    size_t fallback_count;  /* Albums with no artwork (used fallback) */
} artwork_atlas_profile_t;

void artwork_atlas_builder_get_profile(artwork_atlas_builder_t* builder,
                                        artwork_atlas_profile_t* out);

// =============================================================================
// Atlas Reader (mmap-backed, zero-copy)
//
// Single authoritative reader for album artwork atlases.
// Uses mmap(MAP_PRIVATE) for zero-copy binary search + pixel access.
// All pointers valid until reader is closed.  Thread-safe for concurrent reads
// when externally serialised against close/reopen (e.g. artwork_manager's atlas_lock).
// =============================================================================

typedef struct artwork_atlas_reader artwork_atlas_reader_t;

artwork_atlas_reader_t* artwork_atlas_reader_open(const char* path);
void artwork_atlas_reader_close(artwork_atlas_reader_t* reader);

/** Binary search for album_id.  Returns index (≥0) or -1 if not found.  O(log n). */
int32_t artwork_atlas_reader_lookup(const artwork_atlas_reader_t* reader, int64_t album_id);

size_t artwork_atlas_reader_get_count(const artwork_atlas_reader_t* reader);
uint32_t artwork_atlas_reader_get_pixel_stride(const artwork_atlas_reader_t* reader);
uint32_t artwork_atlas_reader_get_thumb_size(const artwork_atlas_reader_t* reader);
uint8_t artwork_atlas_reader_get_channels(const artwork_atlas_reader_t* reader);
int64_t artwork_atlas_reader_get_album_id_at(const artwork_atlas_reader_t* reader, size_t index);

/** Zero-copy pointer to raw pixel data at index (into mmap). Valid until reader is closed. */
const uint8_t* artwork_atlas_reader_get_pixels_at(const artwork_atlas_reader_t* reader, size_t index);

/** Check if an album is in the known-no-artwork set (O(log n) binary search). */
bool artwork_atlas_reader_has_no_art(const artwork_atlas_reader_t* reader, int64_t album_id);

uint32_t artwork_atlas_reader_get_no_art_count(const artwork_atlas_reader_t* reader);

/** Total mmap'd file size in bytes (for stats/diagnostics). */
size_t artwork_atlas_reader_get_file_size(const artwork_atlas_reader_t* reader);

// =============================================================================
// Global Artist Atlas (UUID-keyed, shared across libraries)
//
// Binary format for artist thumbnails keyed by MusicBrainz UUID.
// Single global file at ~/.local/share/quadrature/atlas/artists.atlas
// with flock()-based write serialization via artists.atlas.lock.
//
// Layout:
//   [Header 32B]
//   [uuid_keys: uint8_t[art_count][16]]          sorted 16-byte binary UUIDs
//   [pixels: uint8_t[art_count][pixel_stride]]    dense RGB pixel data
//   [no_art_count: uint32_t]                      number of known-no-artwork entries
//   [no_art_uuids: uint8_t[no_art_count][16]]     sorted 16-byte binary UUIDs
// =============================================================================

#define ARTIST_ATLAS_MAGIC "QDAR"
#define ARTIST_ATLAS_MAGIC_SIZE 4
#define ARTIST_ATLAS_VERSION 1
#define ARTIST_ATLAS_UUID_SIZE 16

/**
 * Global artist atlas header (32 bytes, fixed size).
 * Same size as artwork_atlas_header_t but different magic + semantics.
 */
typedef struct __attribute__((packed)) {
    char magic[4];          /* "QDAR" */
    uint32_t version;       /* 1 */
    uint32_t art_count;     /* Number of entries with artwork */
    uint32_t no_art_count;  /* Number of known-no-artwork entries */
    uint32_t thumb_size;    /* Thumbnail size in pixels */
    uint8_t channels;       /* Color channels (3 = RGB) */
    uint8_t reserved[11];   /* Pad to 32 bytes */
} artist_atlas_header_t;

_Static_assert(sizeof(artist_atlas_header_t) == 32, "Artist atlas header must be 32 bytes");

/**
 * Opaque handle for building/rewriting the global artist atlas.
 * NOT thread-safe — callers must serialize via flock() on the lockfile.
 */
typedef struct artist_atlas_builder artist_atlas_builder_t;

/**
 * Create a new artist atlas builder.
 *
 * @param atlas_path  Final path (e.g. ~/.local/share/quadrature/atlas/artists.atlas)
 * @param thumb_size  Thumbnail size in pixels
 * @param out         Output pointer for builder handle
 */
quadrature_result_t artist_atlas_builder_create(const char* atlas_path,
                                                 int thumb_size,
                                                 artist_atlas_builder_t** out);

/**
 * Seed the builder with all existing entries from the current atlas on disk.
 * Call this after create() and before adding new entries, so that a rewrite
 * preserves other libraries' contributions.
 */
quadrature_result_t artist_atlas_builder_load_existing(artist_atlas_builder_t* builder);

/**
 * Add an artist with artwork (raw pixel data).
 * uuid must be exactly 16 bytes (binary form).
 * pixel_data must be exactly pixel_stride bytes.
 * If UUID already exists, the entry is updated (last writer wins).
 */
quadrature_result_t artist_atlas_builder_add_art(artist_atlas_builder_t* builder,
                                                  const uint8_t uuid[16],
                                                  const void* pixel_data,
                                                  size_t pixel_size);

/**
 * Mark an artist as having no artwork (known-no-art sentinel).
 * uuid must be exactly 16 bytes (binary form).
 */
quadrature_result_t artist_atlas_builder_add_no_art(artist_atlas_builder_t* builder,
                                                     const uint8_t uuid[16]);

/**
 * Finish building and write the atlas atomically (temp file + rename).
 */
quadrature_result_t artist_atlas_builder_finish(artist_atlas_builder_t* builder);

/**
 * Destroy the builder and free resources.
 */
void artist_atlas_builder_destroy(artist_atlas_builder_t* builder);

// =============================================================================
// Global Artist Atlas Reader (mmapped, for display-time lookups)
// =============================================================================

typedef struct artist_atlas_reader artist_atlas_reader_t;

/**
 * Open and mmap a global artist atlas file.
 * Returns NULL if file doesn't exist or is invalid.
 */
artist_atlas_reader_t* artist_atlas_reader_open(const char* path);

/**
 * Close the reader and unmap the file.
 */
void artist_atlas_reader_close(artist_atlas_reader_t* reader);

/**
 * Look up an artist by binary UUID (16 bytes).
 * Returns index into pixel data, or -1 if not found.
 */
int32_t artist_atlas_reader_lookup(const artist_atlas_reader_t* reader,
                                    const uint8_t uuid[16]);

/**
 * Check if an artist is in the known-no-artwork set.
 */
bool artist_atlas_reader_is_no_art(const artist_atlas_reader_t* reader,
                                    const uint8_t uuid[16]);

/**
 * Get raw pixel data at the given index (zero-copy into mmap).
 * Returns pointer valid until reader is closed. Caller must NOT free.
 * Returns NULL if index is out of range.
 */
const uint8_t* artist_atlas_reader_get_pixels(const artist_atlas_reader_t* reader,
                                               int32_t index);

/** Get the thumbnail size, pixel stride, and channel count. */
uint32_t artist_atlas_reader_get_thumb_size(const artist_atlas_reader_t* reader);
uint32_t artist_atlas_reader_get_pixel_stride(const artist_atlas_reader_t* reader);
uint8_t artist_atlas_reader_get_channels(const artist_atlas_reader_t* reader);

/**
 * Get the number of artwork entries.
 */
uint32_t artist_atlas_reader_get_art_count(const artist_atlas_reader_t* reader);

/**
 * Get the number of no-art entries.
 */
uint32_t artist_atlas_reader_get_no_art_count(const artist_atlas_reader_t* reader);

// =============================================================================
// CRC32 (atlas file integrity)
// =============================================================================

#define ATLAS_CRC32_SIZE 4

/**
 * Incremental CRC32 (IEEE 802.3 polynomial, same as zlib/gzip).
 * Usage:
 *   uint32_t crc = ATLAS_CRC32_INIT;
 *   crc = atlas_crc32_update(crc, buf1, len1);
 *   crc = atlas_crc32_update(crc, buf2, len2);
 *   uint32_t final = atlas_crc32_final(crc);
 */
#define ATLAS_CRC32_INIT 0xFFFFFFFF
#define atlas_crc32_final(crc) ((crc) ^ 0xFFFFFFFF)

static inline uint32_t atlas_crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool table_init = false;
    if (!table_init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_init = true;
    }
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

/** One-shot CRC32 (convenience wrapper around incremental API). */
static inline uint32_t atlas_crc32(const uint8_t* data, size_t len) {
    return atlas_crc32_final(atlas_crc32_update(ATLAS_CRC32_INIT, data, len));
}

// =============================================================================
// UUID Helpers
// =============================================================================

/**
 * Parse a MusicBrainz UUID string (36 chars, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")
 * into 16-byte binary form. Returns true on success.
 */
bool mbid_parse(const char* str, uint8_t out[16]);

/**
 * Format a 16-byte binary UUID into a 36-char string (plus NUL terminator).
 * out must be at least 37 bytes.
 */
void mbid_format(const uint8_t uuid[16], char out[37]);

// =============================================================================
// Artist Art (fanart.tv)
// =============================================================================

typedef struct {
    const char* personal_api_key;  // User-supplied personal key (client_key); NULL = no personal key
    const char* artwork_dir;       // {library_root}/artwork/ (for cached artist image files)
    quadrature_db_t* db;
    atomic_int* cancel_flag;
    int rate_limit_ms;             // Default: 500ms = 2 req/s
    int art_thumb_size;            // Thumbnail size for atlas (default: 48)

    // Global artist atlas paths (shared across libraries)
    const char* atlas_path;        // ~/.local/share/quadrature/atlas/artists.atlas
    const char* atlas_lock_path;   // ~/.local/share/quadrature/atlas/artists.atlas.lock

    // Cross-library art reuse: artwork dirs from other libraries to check
    // before hitting fanart.tv. NULL-terminated or use count.
    const char* const* other_artwork_dirs;
    size_t other_artwork_dirs_count;

    // Telemetry: incremented on each HTTP error (owned by caller, may be NULL)
    atomic_size_t* http_errors;

    // Album cover art from fanart.tv (piggyback on artist API responses)
    // When set, albums missing from the album atlas that have a release_group_id
    // will have their covers fetched from the same fanart.tv response.
    const char* album_artwork_dir;   // {data_root}/artwork/ (for album atlas files)
    int album_thumb_size;            // Album thumbnail size (typically 48)
} artist_art_config_t;

typedef struct {
    size_t total;
    size_t processed;
    size_t downloaded;
    size_t skipped;
    size_t no_images;
    size_t errors;
    size_t album_covers;    // Album covers downloaded from fanart.tv responses
} artist_art_progress_t;

typedef void (*artist_art_progress_cb)(const artist_art_progress_t*, void*);

quadrature_result_t artist_art_fetch_all(const artist_art_config_t* config,
                                          artist_art_progress_cb cb, void* user_data);

// =============================================================================
// Artist Bios (Wikipedia via Wikidata)
// =============================================================================

typedef struct {
    quadrature_db_t* db;           // Main library DB (for artist MBID list)
    const char* library_root;      // Library root path (for metadata DB)
    atomic_int* cancel_flag;
    int rate_limit_ms;             // Default: 500ms

    // Telemetry: incremented on each HTTP error (owned by caller, may be NULL)
    atomic_size_t* http_errors;
} artist_bio_config_t;

typedef struct {
    size_t total;
    size_t processed;
    size_t fetched;
    size_t skipped;
    size_t no_bio;
} artist_bio_progress_t;

typedef void (*artist_bio_progress_cb)(const artist_bio_progress_t*, void*);

quadrature_result_t artist_bio_fetch_all(const artist_bio_config_t* config,
                                          artist_bio_progress_cb cb, void* user_data);

#endif // INDEXER_INTERNAL_H
