#ifndef QUADRATURE_MB_ENRICHMENT_H
#define QUADRATURE_MB_ENRICHMENT_H

#include "quadrature/core/types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MusicBrainz Enrichment API
 *
 * Enriches local music files with authoritative MusicBrainz metadata:
 * - Fingerprints tracks using AcoustID/Chromaprint
 * - Matches to MusicBrainz releases
 * - Writes standard MUSICBRAINZ_* tags to audio files
 * - Downloads album artwork from Cover Art Archive
 *
 * Tracks that already have MUSICBRAINZ_TRACKID tags (e.g., from private
 * trackers like RED/OPS) are skipped unless --force is used.
 */

// Forward declarations
typedef struct mb_enrichment_ctx mb_enrichment_ctx_t;

// =============================================================================
// Configuration
// =============================================================================

typedef struct {
    bool force;              // Re-enrich even if MUSICBRAINZ_TRACKID exists
    bool dry_run;            // Don't write anything, just report what would change
    bool embed_artwork;      // Embed album art in audio files
    bool download_artwork;   // Download cover.jpg to album folder
    int parallelism;         // Number of parallel operations (0 = auto)
    const char* acoustid_api_key;  // AcoustID API key (required)
} mb_enrichment_options_t;

// =============================================================================
// Progress Information
// =============================================================================

typedef enum {
    MB_PHASE_DISCOVERING,    // Finding albums to enrich
    MB_PHASE_FINGERPRINTING, // Generating audio fingerprints
    MB_PHASE_MATCHING,       // Querying AcoustID/MusicBrainz
    MB_PHASE_TAGGING,        // Writing metadata to files
    MB_PHASE_ARTWORK,        // Downloading/writing artwork
    MB_PHASE_COMPLETE        // Done
} mb_enrichment_phase_t;

typedef struct {
    size_t albums_total;
    size_t albums_processed;
    size_t albums_matched;
    size_t albums_skipped;    // Already had MB tags
    size_t albums_failed;
    size_t tracks_tagged;     // Total tracks with tags written
    const char* current_album;   // Currently processing (read-only)
    mb_enrichment_phase_t phase;
    double progress;             // 0.0 to 1.0
} mb_enrichment_progress_t;

// =============================================================================
// Callback
// =============================================================================

typedef void (*mb_enrichment_progress_cb)(
    const mb_enrichment_progress_t* progress,
    void* user_data
);

// =============================================================================
// Lifecycle
// =============================================================================

/**
 * Create enrichment context.
 *
 * @param out Output pointer for created context
 * @param options Configuration options
 * @param callback Progress callback (may be NULL)
 * @param user_data User data for callback
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_enrichment_create(
    mb_enrichment_ctx_t** out,
    const mb_enrichment_options_t* options,
    mb_enrichment_progress_cb callback,
    void* user_data
);

/**
 * Destroy enrichment context.
 *
 * @param ctx Context to destroy (may be NULL)
 */
void mb_enrichment_destroy(mb_enrichment_ctx_t* ctx);

// =============================================================================
// Operations
// =============================================================================

/**
 * Run enrichment on a path (blocking).
 *
 * Scans the path for albums, checks for existing MUSICBRAINZ_TRACKID tags,
 * fingerprints untagged tracks, matches to MusicBrainz, and writes tags.
 *
 * @param ctx Enrichment context
 * @param path Path to enrich (file or directory)
 * @return QUADRATURE_OK on success, QUADRATURE_ERROR_CANCELLED if cancelled
 */
quadrature_result_t mb_enrichment_run(
    mb_enrichment_ctx_t* ctx,
    const char* path
);

/**
 * Cancel in-progress enrichment.
 *
 * Safe to call from any thread. The current operation will complete,
 * then enrichment will stop and mb_enrichment_run() will return.
 *
 * @param ctx Enrichment context
 */
void mb_enrichment_cancel(mb_enrichment_ctx_t* ctx);

/**
 * Check if enrichment is running.
 *
 * @param ctx Enrichment context
 * @return true if enrichment is in progress
 */
bool mb_enrichment_is_running(const mb_enrichment_ctx_t* ctx);

// =============================================================================
// Tag Detection API (for checking if files need enrichment)
// =============================================================================

/**
 * MusicBrainz IDs read from audio file tags.
 * Standard tags used by private trackers and MusicBrainz Picard.
 */
typedef struct {
    char* track_id;           // MUSICBRAINZ_TRACKID
    char* release_id;         // MUSICBRAINZ_ALBUMID
    char* artist_id;          // MUSICBRAINZ_ARTISTID
    char* album_artist_id;    // MUSICBRAINZ_ALBUMARTISTID
    char* release_group_id;   // MUSICBRAINZ_RELEASEGROUPID
    char* recording_id;       // MUSICBRAINZ_RELEASETRACKID (alternative)
} mb_tags_t;

/**
 * Read MusicBrainz tags from an audio file.
 *
 * @param audio_path Path to audio file
 * @param tags Output tags (caller must call mb_tags_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_tags_read(const char* audio_path, mb_tags_t* tags);

/**
 * Check if an audio file has MusicBrainz tags.
 *
 * @param audio_path Path to audio file
 * @return true if MUSICBRAINZ_TRACKID or MUSICBRAINZ_RELEASETRACKID exists
 */
bool mb_tags_exist(const char* audio_path);

/**
 * Free tags structure contents.
 *
 * @param tags Tags to free (does not free the struct itself)
 */
void mb_tags_free(mb_tags_t* tags);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_MB_ENRICHMENT_H
