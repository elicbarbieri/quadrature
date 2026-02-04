/**
 * Quadrature MusicBrainz Resolution API
 *
 * Two-tier local resolution via self-hosted PostgreSQL:
 * - Tier 1: File has MUSICBRAINZ_ALBUMID tag → use that release UUID directly
 * - Tier 2: No tags → cached fingerprint → local AcoustID PG → consensus vote
 *
 * All resolved metadata goes to the SQLite database.
 * Never modifies files in scanned libraries.
 */

#ifndef QUADRATURE_MUSICBRAINZ_H
#define QUADRATURE_MUSICBRAINZ_H

#include "quadrature.h"
#include "quadrature_database.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * MB Resolver API (DB-driven metadata resolution)
 * ============================================================================= */

typedef struct mb_resolver mb_resolver_t;

typedef struct {
    const char* pg_conninfo;             /* libpq connection string for self-hosted MB+AcoustID database */
    int parallelism;                     /* 0 = auto */
} mb_resolver_options_t;

typedef enum {
    MB_RESOLVE_QUERYING,       /* Finding unresolved albums from DB */
    MB_RESOLVE_FINGERPRINTING, /* Reading cached chromaprint from DB */
    MB_RESOLVE_MATCHING,       /* Local AcoustID PG lookup + release voting */
    MB_RESOLVE_FETCHING,       /* Getting full release from MB PG */
    MB_RESOLVE_WRITING,        /* Updating DB with MB data */
    MB_RESOLVE_COMPLETE
} mb_resolve_phase_t;

typedef struct {
    size_t albums_total;
    size_t albums_processed;
    size_t albums_resolved;
    size_t albums_no_match;
    size_t albums_failed;
    const char* current_album;
    mb_resolve_phase_t phase;
    double progress;
} mb_resolver_progress_t;

typedef void (*mb_resolver_progress_cb)(const mb_resolver_progress_t*, void*);

/**
 * Create resolver context.
 *
 * @param out Output pointer for created resolver
 * @param db Database handle (resolver uses its own queries)
 * @param options Configuration options
 * @param callback Progress callback (may be NULL)
 * @param user_data User data for callback
 */
quadrature_result_t mb_resolver_create(mb_resolver_t** out,
    quadrature_db_t* db,
    const mb_resolver_options_t* options,
    mb_resolver_progress_cb callback,
    void* user_data);

/**
 * Run resolver (blocking). Processes all unresolved albums in DB.
 */
quadrature_result_t mb_resolver_run(mb_resolver_t* ctx);

/**
 * Cancel in-progress resolution. Safe to call from any thread.
 */
void mb_resolver_cancel(mb_resolver_t* ctx);

/**
 * Destroy resolver context.
 */
void mb_resolver_destroy(mb_resolver_t* ctx);

/* =============================================================================
 * Tag Detection API (for checking if files have MB tags)
 * ============================================================================= */

/**
 * MusicBrainz IDs read from audio file tags.
 * Standard tags used by private trackers and MusicBrainz Picard.
 */
typedef struct {
    char* track_id;           /* MUSICBRAINZ_TRACKID */
    char* release_id;         /* MUSICBRAINZ_ALBUMID */
    char* artist_id;          /* MUSICBRAINZ_ARTISTID */
    char* album_artist_id;    /* MUSICBRAINZ_ALBUMARTISTID */
    char* release_group_id;   /* MUSICBRAINZ_RELEASEGROUPID */
    char* recording_id;       /* MUSICBRAINZ_RELEASETRACKID (alternative) */
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

/* =============================================================================
 * Fingerprint API (exposed for indexer Phase 2)
 * ============================================================================= */

typedef struct {
    char* fingerprint;    /* Base64-encoded Chromaprint fingerprint */
    int duration;         /* Duration in seconds */
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

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_MUSICBRAINZ_H */
