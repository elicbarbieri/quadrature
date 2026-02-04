#ifndef MUSICBRAINZ_INTERNAL_H
#define MUSICBRAINZ_INTERNAL_H

/**
 * Internal header for MusicBrainz resolution implementation.
 * Contains all private types and shared constants.
 */

#include "quadrature/quadrature.h"
#include "quadrature/quadrature_musicbrainz.h"
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Constants
// =============================================================================

#define MB_PATH_MAX 4096

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
// Forward Declarations
// =============================================================================

typedef struct mb_pg_client mb_pg_client_t;

// =============================================================================
// AcoustID Lookup (mb_acoustid.c) — Local PostgreSQL
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
 * Look up fingerprint in local AcoustID PostgreSQL database.
 *
 * @param client PostgreSQL client (connected to AcoustID+MusicBrainz DB)
 * @param fingerprint Fingerprint to look up
 * @param response Output response (caller must free with mb_acoustid_response_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_acoustid_lookup(mb_pg_client_t* client,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response);

/**
 * Free AcoustID response.
 */
void mb_acoustid_response_free(mb_acoustid_response_t* response);

// =============================================================================
// MusicBrainz Data Types
// =============================================================================

typedef struct {
    char* id;             // MusicBrainz ID
    char* name;           // Artist name
    char* sort_name;      // Sort name
    char* joinphrase;     // e.g., " feat. ", " & ", "" (from artist-credit)
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

// =============================================================================
// PostgreSQL Client (mb_postgres.c)
// =============================================================================

/**
 * Create PostgreSQL client connected to a self-hosted MusicBrainz database.
 *
 * @param conninfo libpq connection string (e.g., "host=localhost dbname=musicbrainz_db")
 * @param out Output pointer for created client
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_pg_client_create(const char* conninfo, mb_pg_client_t** out);

/**
 * Destroy PostgreSQL client.
 */
void mb_pg_client_destroy(mb_pg_client_t* client);

/**
 * Execute a parameterized query against the PG client.
 * Returns a PGresult* (caller must PQclear).
 * Declared as void* to avoid leaking libpq types into this header.
 */
void* mb_pg_exec(mb_pg_client_t* client, const char* query,
                  int nparams, const char* const* params);

/**
 * Fetch release metadata from self-hosted MusicBrainz PostgreSQL.
 *
 * @param client PostgreSQL client
 * @param release_id MusicBrainz release ID (UUID string)
 * @param release Output release (caller must free with mb_release_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_fetch_release(mb_pg_client_t* client,
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

#endif // MUSICBRAINZ_INTERNAL_H
