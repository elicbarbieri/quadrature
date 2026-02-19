#ifndef MUSICBRAINZ_INTERNAL_H
#define MUSICBRAINZ_INTERNAL_H

/**
 * Internal header for MusicBrainz resolution implementation.
 * Contains all private types and shared constants.
 */

#include "quadrature/quadrature.h"
#include "quadrature/indexer.h"
#include "quadrature/metadata.h"
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Constants
// =============================================================================

#define MB_PATH_MAX 4096

// Fingerprinting
#define MB_FINGERPRINT_DURATION 120  // Seconds of audio to fingerprint
#define MB_FINGERPRINT_TRACKS 4      // Max tracks to fingerprint per album (with early exit)
#define MB_MATCH_CONFIDENCE 0.80     // 80% of tracks must match same release

// Batch resolution
#define MB_BATCH_SIZE 50             // Albums per PG round-trip

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
 * Look up fingerprint via acoustid-index HTTP service + acoustid/MB PostgreSQL.
 *
 * Flow: decode fingerprint → POST hashes to acoustid-index HTTP → track_ids
 *       → acoustid PG (track_id → recording MBID)
 *       → MB PG (recording MBID → release UUID)
 *
 * @param mb_client    PostgreSQL client connected to MusicBrainz database
 * @param acoustid_client PostgreSQL client connected to acoustid database (may be NULL to skip)
 * @param acoustid_index_url  Base URL for acoustid-index, e.g. "http://192.168.1.220:8081" (may be NULL to skip)
 * @param fingerprint  Fingerprint to look up
 * @param response     Output response (caller must free with mb_acoustid_response_free)
 * @return QUADRATURE_OK on success (empty response is not an error)
 */
quadrature_result_t mb_acoustid_lookup(mb_pg_client_t* mb_client,
                                        mb_pg_client_t* acoustid_client,
                                        const char* acoustid_index_url,
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
    char* id;             // MusicBrainz ID (gid)
    char* name;           // Canonical artist name (from artist.name)
    char* credited_name;  // Name as credited on this recording (from artist_credit_name.name);
                          // may differ from canonical (e.g. "The Weeknd" vs "Weeknd")
    char* sort_name;      // Sort name (from artist.sort_name)
    char* joinphrase;     // Connector to next artist: " feat. ", " & ", "" for last
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
    char* label;                 // Record label name
    char* catalog_number;        // Label catalog number (from release_label.catalog_number)
    char* barcode;               // UPC/barcode
    char* type;                  // e.g., "Album", "EP", "Single"
    char* genres;                // Semicolon-separated curated MB genres (from release_group_tag)

    mb_artist_t* artists;        // Album artists
    size_t artist_count;

    mb_recording_t* recordings;  // Tracks
    size_t recording_count;
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
 * Set the PostgreSQL search_path to the given schema name.
 * Call immediately after mb_pg_client_create for schema-namespaced databases.
 */
void mb_pg_set_schema(mb_pg_client_t* client, const char* schema);

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
// Recording Link Rows (mb_postgres.c — returned by mb_fetch_all_batch)
// =============================================================================

/**
 * One row from the l_artist_recording batch query.
 */
typedef struct {
    char* recording_mbid;   /* recording.gid */
    char* link_type_gid;    /* link_type.gid */
    char* link_type_name;   /* link_type.name, e.g. "producer" */
    char* link_type_desc;   /* link_type.description; may be NULL */
    char* artist_mbid;      /* artist.gid */
    char* artist_name;      /* artist.name */
    char* artist_sort_name; /* artist.sort_name */
    char* artist_type;      /* artist_type.name; may be NULL */
    char* entity0_credit;   /* l_artist_recording.entity0_credit; may be NULL */
    char* attributes;       /* comma-separated lav.name values; may be NULL */
} mb_recording_link_row_t;

// =============================================================================
// Batch PG Fetch (mb_postgres.c)
// =============================================================================

/**
 * Format UUID strings into a PostgreSQL array literal: {uuid1,uuid2,...}
 * Caller must g_free() the result.
 */
char* mb_format_uuid_array(const char** uuids, size_t count);

/**
 * Install the session-local pg_temp.mb_batch_fetch() function.
 * Must be called once per PG connection (after mb_pg_set_schema).
 */
quadrature_result_t mb_pg_install_batch_function(mb_pg_client_t* client);

/**
 * Consolidated batch fetch: releases + recordings + album/track artists + links.
 * Single PG round-trip via pg_temp.mb_batch_fetch().
 *
 * @param client       MusicBrainz PG client (must have batch function installed)
 * @param release_ids  Array of release UUID strings
 * @param count        Number of release_ids
 * @param out_releases GHashTable<release_gid_string, mb_release_t*> (caller destroys)
 * @param out_links    GHashTable<release_gid_string, GPtrArray<mb_recording_link_row_t*>>
 *                     (caller destroys; always populated, may be empty)
 */
quadrature_result_t mb_fetch_all_batch(mb_pg_client_t* client,
    const char** release_ids, size_t count,
    GHashTable** out_releases, GHashTable** out_links);

// =============================================================================
// PG Connection Pool (for fingerprint workers)
// =============================================================================

typedef struct {
    mb_pg_client_t** mb_conns;
    mb_pg_client_t** acoustid_conns;
    size_t count;
    volatile gint next_slot;
} mb_pg_pool_t;

quadrature_result_t mb_pg_pool_create(const char* mb_conninfo,
    const char* acoustid_conninfo, size_t count, mb_pg_pool_t** out);
void mb_pg_pool_destroy(mb_pg_pool_t* pool);

#endif // MUSICBRAINZ_INTERNAL_H
