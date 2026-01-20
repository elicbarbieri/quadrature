#ifndef MUSICBRAINZ_INTERNAL_H
#define MUSICBRAINZ_INTERNAL_H

/**
 * Internal header for MusicBrainz enrichment implementation.
 * Contains all private types and shared constants.
 */

#include "quadrature/core/types.h"
#include "quadrature/musicbrainz/mb_enrichment.h"
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Constants
// =============================================================================

#define MB_PATH_MAX 4096

// Rate limits (requests per second)
#define ACOUSTID_RATE_LIMIT 3.0
#define MUSICBRAINZ_RATE_LIMIT 1.0
#define COVERART_RATE_LIMIT 1.0

// API endpoints
#define ACOUSTID_API_URL "https://api.acoustid.org/v2/lookup"
#define MUSICBRAINZ_API_URL "https://musicbrainz.org/ws/2"
#define COVERART_API_URL "https://coverartarchive.org"

// User agent (required by MusicBrainz)
#define MB_USER_AGENT "Quadrature/0.1.0 (https://github.com/quadrature/quadrature)"

// Fingerprinting
#define MB_FINGERPRINT_DURATION 120  // Seconds of audio to fingerprint
#define MB_MIN_TRACKS_FOR_MATCH 3    // Minimum tracks to fingerprint per album
#define MB_MATCH_CONFIDENCE 0.80     // 80% of tracks must match same release

// =============================================================================
// Standard MusicBrainz Tag Names
// =============================================================================

// These are the standard tag names used by MusicBrainz Picard and private trackers
#define MB_TAG_TRACKID "MUSICBRAINZ_TRACKID"
#define MB_TAG_ALBUMID "MUSICBRAINZ_ALBUMID"
#define MB_TAG_ARTISTID "MUSICBRAINZ_ARTISTID"
#define MB_TAG_ALBUMARTISTID "MUSICBRAINZ_ALBUMARTISTID"
#define MB_TAG_RELEASEGROUPID "MUSICBRAINZ_RELEASEGROUPID"
#define MB_TAG_RELEASETRACKID "MUSICBRAINZ_RELEASETRACKID"

// =============================================================================
// HTTP Client (mb_client.c)
// =============================================================================

typedef struct mb_http_client mb_http_client_t;

/**
 * Rate limiter types.
 */
typedef enum {
    MB_RATE_ACOUSTID,
    MB_RATE_MUSICBRAINZ,
    MB_RATE_COVERART
} mb_rate_type_t;

/**
 * Create HTTP client with rate limiting.
 */
quadrature_result_t mb_http_client_create(mb_http_client_t** out);

/**
 * Destroy HTTP client.
 */
void mb_http_client_destroy(mb_http_client_t* client);

/**
 * Perform GET request with rate limiting.
 *
 * @param client HTTP client
 * @param url Full URL to request
 * @param rate_type Which rate limiter to use
 * @param response_out Output buffer for response (caller must g_free)
 * @param response_len_out Output for response length
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_http_get(mb_http_client_t* client,
                                 const char* url,
                                 mb_rate_type_t rate_type,
                                 char** response_out,
                                 size_t* response_len_out);

/**
 * Download binary data (for artwork).
 *
 * @param client HTTP client
 * @param url Full URL to download
 * @param rate_type Which rate limiter to use
 * @param data_out Output buffer for data (caller must g_free)
 * @param data_len_out Output for data length
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_http_download(mb_http_client_t* client,
                                      const char* url,
                                      mb_rate_type_t rate_type,
                                      uint8_t** data_out,
                                      size_t* data_len_out);

// =============================================================================
// Fingerprinting (mb_fingerprint.c)
// =============================================================================

typedef struct {
    char* fingerprint;    // Base64-encoded Chromaprint fingerprint
    int duration;         // Duration in seconds
} mb_fingerprint_t;

/**
 * Generate audio fingerprint for a file.
 *
 * @param audio_path Path to audio file
 * @param fingerprint Output fingerprint (caller must free with mb_fingerprint_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_fingerprint_generate(const char* audio_path,
                                             mb_fingerprint_t* fingerprint);

/**
 * Free fingerprint data.
 */
void mb_fingerprint_free(mb_fingerprint_t* fp);

// =============================================================================
// AcoustID Lookup (mb_acoustid.c)
// =============================================================================

typedef struct {
    char* recording_id;   // MusicBrainz recording ID
    char* release_id;     // MusicBrainz release ID (may be NULL)
    float score;          // Match confidence (0.0-1.0)
} mb_acoustid_result_t;

typedef struct {
    mb_acoustid_result_t* results;
    size_t count;
} mb_acoustid_response_t;

/**
 * Look up fingerprint in AcoustID database.
 *
 * @param client HTTP client
 * @param api_key AcoustID API key
 * @param fingerprint Fingerprint to look up
 * @param response Output response (caller must free with mb_acoustid_response_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_acoustid_lookup(mb_http_client_t* client,
                                        const char* api_key,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response);

/**
 * Free AcoustID response.
 */
void mb_acoustid_response_free(mb_acoustid_response_t* response);

// =============================================================================
// MusicBrainz Parser (mb_parser.c)
// =============================================================================

typedef struct {
    char* id;             // MusicBrainz ID
    char* name;           // Artist name
    char* sort_name;      // Sort name
} mb_artist_t;

typedef struct {
    char* id;             // MusicBrainz recording ID
    char* title;          // Track title
    int position;         // Track number
    int disc_number;      // Disc number (1 if single disc)
    int duration_ms;      // Duration in milliseconds
    mb_artist_t* artists; // Track artists
    size_t artist_count;
} mb_recording_t;

typedef struct {
    char* id;                    // MusicBrainz release ID
    char* release_group_id;      // Release group ID
    char* title;                 // Album title
    char* date;                  // Release date (YYYY-MM-DD or YYYY)
    char* country;               // Release country
    char* label;                 // Record label
    char* barcode;               // UPC/barcode
    char* status;                // e.g., "Official"
    char* type;                  // e.g., "Album", "EP", "Single"

    mb_artist_t* artists;        // Album artists
    size_t artist_count;

    mb_recording_t* recordings;  // Tracks
    size_t recording_count;

    char** genres;               // Genre tags
    size_t genre_count;
} mb_release_t;

/**
 * Fetch release metadata from MusicBrainz.
 *
 * @param client HTTP client
 * @param release_id MusicBrainz release ID
 * @param release Output release (caller must free with mb_release_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_fetch_release(mb_http_client_t* client,
                                      const char* release_id,
                                      mb_release_t* release);

/**
 * Free release data.
 */
void mb_release_free(mb_release_t* release);

/**
 * Free artist data.
 */
void mb_artist_free(mb_artist_t* artist);

/**
 * Free recording data.
 */
void mb_recording_free(mb_recording_t* recording);

// =============================================================================
// Tag Writer (mb_tagger.c)
// =============================================================================

/**
 * Write MusicBrainz metadata tags to audio file.
 *
 * Writes standard tags: MUSICBRAINZ_TRACKID, MUSICBRAINZ_ALBUMID, etc.
 *
 * @param audio_path Path to audio file
 * @param release Release metadata
 * @param recording Recording metadata for this track
 * @param dry_run If true, don't actually write tags
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_write_tags(const char* audio_path,
                                   const mb_release_t* release,
                                   const mb_recording_t* recording,
                                   bool dry_run);

/**
 * Embed album artwork in audio file.
 *
 * @param audio_path Path to audio file
 * @param image_data Image data (JPEG or PNG)
 * @param image_size Image data size
 * @param dry_run If true, don't actually write
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_embed_artwork(const char* audio_path,
                                      const uint8_t* image_data,
                                      size_t image_size,
                                      bool dry_run);

// =============================================================================
// Artwork (mb_artwork.c)
// =============================================================================

typedef struct {
    uint8_t* data;        // Image data (JPEG)
    size_t size;          // Data size
    char* hash;           // SHA-256 hash (hex)
} mb_artwork_t;

/**
 * Download album artwork from Cover Art Archive.
 *
 * @param client HTTP client
 * @param release_id MusicBrainz release ID
 * @param artwork Output artwork (caller must free with mb_artwork_free)
 * @return QUADRATURE_OK on success, QUADRATURE_ERROR_FILE_NOT_FOUND if no art
 */
quadrature_result_t mb_artwork_download(mb_http_client_t* client,
                                         const char* release_id,
                                         mb_artwork_t* artwork);

/**
 * Write artwork to album directory as cover.jpg.
 *
 * @param album_path Album directory path
 * @param artwork Artwork to write
 * @param dry_run If true, don't actually write
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_artwork_write(const char* album_path,
                                      const mb_artwork_t* artwork,
                                      bool dry_run);

/**
 * Free artwork data.
 */
void mb_artwork_free(mb_artwork_t* artwork);

// =============================================================================
// Enrichment Context (mb_enrichment.c)
// =============================================================================

struct mb_enrichment_ctx {
    mb_enrichment_options_t options;
    mb_enrichment_progress_cb callback;
    void* user_data;

    mb_http_client_t* http_client;

    // Progress tracking
    mb_enrichment_progress_t progress;
    GMutex progress_mutex;

    // Cancellation
    volatile bool cancelled;

    // Work queue
    GPtrArray* album_queue;  // Array of album paths to process
};

#endif // MUSICBRAINZ_INTERNAL_H
