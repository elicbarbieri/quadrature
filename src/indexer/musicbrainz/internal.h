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
#define MB_FINGERPRINT_DURATION 30   // Seconds of audio to fingerprint (30s is sufficient for reliable identification)
#define MB_FINGERPRINT_TRACKS 4      // Max tracks to fingerprint per album (with early exit)
#define MB_MATCH_CONFIDENCE 0.80     // 80% of tracks must match same release

// Batch resolution
#define MB_BATCH_SIZE 50             // Albums per PG round-trip

// NO_MATCH retry interval (re-attempt resolution after this many seconds)
#define MB_NO_MATCH_RETRY_SECONDS (30 * 24 * 3600)  // 30 days

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
// Persistent HTTP Connection (mb_acoustid.c)
// =============================================================================

/**
 * Persistent HTTP connection to acoustid-index.
 * Reused across multiple lookups on the same worker thread.
 * Reconnects transparently on failure.
 */
typedef struct {
    int fd;
    char host[256];
    int port;
    char* url;      // Original URL, kept for reconnection
    bool alive;     // Keep-alive state from last response
} mb_http_conn_t;

mb_http_conn_t* mb_http_conn_create(const char* base_url);
void mb_http_conn_destroy(mb_http_conn_t* conn);

// =============================================================================
// AcoustID Lookup (mb_acoustid.c) — Local PostgreSQL
// =============================================================================

typedef struct {
    char* recording_id;       // MusicBrainz recording ID
    char* release_id;         // MusicBrainz release ID (may be NULL)
    char* release_group_id;   // MusicBrainz release group ID (may be NULL)
    float score;              // Match confidence (0.0-1.0)
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
 * @param http_conn    Persistent HTTP connection to acoustid-index (may be NULL to skip)
 * @param fingerprint  Fingerprint to look up
 * @param response     Output response (caller must free with mb_acoustid_response_free)
 * @return QUADRATURE_OK on success (empty response means no match found),
 *         QUADRATURE_ERROR_SERVICE_UNAVAILABLE if acoustid-index HTTP or PG is unreachable,
 *         QUADRATURE_ERROR_INVALID_PARAM for bad inputs
 */
quadrature_result_t mb_acoustid_lookup(mb_pg_client_t* mb_client,
                                        mb_pg_client_t* acoustid_client,
                                        mb_http_conn_t* http_conn,
                                        const mb_fingerprint_t* fingerprint,
                                        mb_acoustid_response_t* response);

/**
 * Free AcoustID response.
 */
void mb_acoustid_response_free(mb_acoustid_response_t* response);

/**
 * Prepare acoustid lookup SQL statements on PG connections.
 * Call once per connection pair (during pool setup).
 * After this, mb_acoustid_lookup uses PQexecPrepared for zero-parse overhead.
 */
quadrature_result_t mb_acoustid_prepare_stmts(mb_pg_client_t* mb_client,
                                               mb_pg_client_t* acoustid_client);

/**
 * Look up ISRCs via MusicBrainz PostgreSQL.
 * Returns (release_id, release_group_id) pairs in the same response type
 * as mb_acoustid_lookup, so results can be fed through the same voting logic.
 *
 * @param mb_client  PostgreSQL client connected to MusicBrainz database
 * @param isrcs      Array of ISRC strings
 * @param count      Number of ISRCs
 * @param response   Output response (caller must free with mb_acoustid_response_free)
 */
quadrature_result_t mb_isrc_lookup(mb_pg_client_t* mb_client,
                                    const char** isrcs, size_t count,
                                    mb_acoustid_response_t* response);

/**
 * Search MusicBrainz Solr for releases matching artist + album name.
 * Solr handles diacritics, Unicode normalization, and aliases via Lucene.
 * Candidates validated against PG for exact track count + total duration ±5%.
 *
 * @param mb_client   PG client for duration validation
 * @param solr_url    Solr base URL (e.g., "http://localhost:8983")
 * @param album_title Album title from audio file metadata tags
 * @param artist_name Artist name from audio file metadata tags
 * @param local_track_count  Number of tracks in local album
 * @param local_total_duration_ms  Total duration of local tracks
 * @return Allocated release_id string, or NULL. Caller must g_free().
 */
char* mb_solr_search_release(mb_pg_client_t* mb_client,
                              const char* solr_url,
                              const char* album_title,
                              const char* artist_name,
                              size_t local_track_count,
                              int64_t local_total_duration_ms);

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
 * Prepare a named statement for later execution with mb_pg_exec_prepared.
 */
quadrature_result_t mb_pg_prepare(mb_pg_client_t* client, const char* stmt_name,
                                   const char* query, int nparams);

/**
 * Execute a previously-prepared statement.
 * Returns a PGresult* (caller must PQclear).
 */
void* mb_pg_exec_prepared(mb_pg_client_t* client, const char* stmt_name,
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
    mb_http_conn_t** http_conns;       // Persistent HTTP connections to acoustid-index
    size_t count;
    volatile gint next_slot;
} mb_pg_pool_t;

/**
 * Create connection pool for fingerprint workers.
 * Each slot gets: MB PG conn, acoustid PG conn, HTTP conn to acoustid-index.
 * Prepared statements are installed on all PG connections.
 *
 * @param acoustid_index_url  acoustid-index HTTP URL (may be NULL to skip HTTP+acoustid)
 */
quadrature_result_t mb_pg_pool_create(const char* mb_conninfo,
    const char* acoustid_conninfo, const char* acoustid_index_url,
    size_t count, mb_pg_pool_t** out);
void mb_pg_pool_destroy(mb_pg_pool_t* pool);

#endif // MUSICBRAINZ_INTERNAL_H
