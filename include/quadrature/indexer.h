/**
 * Quadrature Indexer & MusicBrainz Resolution API
 *
 * Eight-phase library indexer for scanning and updating the music database:
 * - Phase 1 - SCAN:        Fast directory walk, delta detection via album mtimes
 * - Phase 2 - METADATA:    Parallel tag extraction (FFmpeg, no audio decode)
 * - Phase 3 - FINALIZE:    Batch mtime flush + WAL checkpoint
 *                           INDEXER_LIBRARY_UPDATED fires -- metadata usable, UI browsable
 * - Phase 4 - ARTWORK:     Parallel image processing, atlas write
 *                           INDEXER_ARTWORK_UPDATED fires -- album atlas available
 * - Phase 5 - FINGERPRINT: Parallel Chromaprint + AcoustID (producer)
 * - Phase 6 - RESOLVE:     Batched MusicBrainz Postgres resolution (consumer)
 *                           Phases 5+6 run concurrently via producer-consumer queue
 *                           INDEXER_LIBRARY_UPDATED fires -- MB enrichment in DB
 * - Phase 7 - ARTIST_ART:  Fetch artist images from fanart.tv
 *                           INDEXER_ARTWORK_UPDATED fires -- artist atlas updated
 * - Phase 8 - ARTIST_BIO:  Fetch artist bios from Wikipedia via Wikidata
 *                           (no signal -- bios are lazily fetched by the UI)
 *                           INDEXER_COMPLETED fires (terminal; no cache operations)
 *
 * Delta detection via albums.last_updated_at (no separate dir_mtime table).
 *
 * MusicBrainz resolution uses two-tier local resolution via self-hosted PostgreSQL:
 * - Tier 1: File has MUSICBRAINZ_ALBUMID tag -> use that release UUID directly
 * - Tier 2: No tags -> cached fingerprint -> local AcoustID PG -> consensus vote
 */

#ifndef QUADRATURE_INDEXER_H
#define QUADRATURE_INDEXER_H

#include "quadrature.h"
#include "database.h"
#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

typedef struct indexer indexer_t;

/* =============================================================================
 * Progress Information
 * ============================================================================= */

/**
 * Indexing phases for progress display.
 */
typedef enum {
    INDEXER_PHASE_SCANNING,      /* Fast directory walk, build work queue */
    INDEXER_PHASE_METADATA,      /* Parallel metadata extraction (FFmpeg tag read only) */
    INDEXER_PHASE_ARTWORK,       /* Parallel artwork processing, atlas write */
    INDEXER_PHASE_FINGERPRINT,   /* Parallel Chromaprint + AcoustID fingerprinting */
    INDEXER_PHASE_RESOLVE,       /* Batched MusicBrainz Postgres resolution */
    INDEXER_PHASE_ARTIST_ART,    /* Fetch artist images from fanart.tv */
    INDEXER_PHASE_ARTIST_BIO,    /* Fetch artist bios from Wikipedia */
    INDEXER_PHASE_FINALIZE,      /* WAL checkpoint */
    INDEXER_PHASE_COMPLETE,      /* Done */
    INDEXER_PHASE_COUNT          /* Number of phases (not a real phase) */
} indexer_phase_t;

typedef struct {
    size_t files_total;
    size_t files_processed;
    size_t files_new;
    size_t files_unchanged;
    size_t dirs_scanned;
    size_t error_count;       /* Errors logged during this scan */
    double progress;          /* 0.0 to 1.0 */
    const char* current_path; /* Currently processing (read-only, do not free) */

    /* Phase tracking */
    indexer_phase_t phase;

    /* Artwork progress */
    size_t albums_total;         /* Albums needing artwork */
    size_t albums_processed;     /* Albums with artwork done */

    /* Fingerprint progress (overlaps with resolve phase) */
    size_t fingerprint_total;       /* Albums needing fingerprinting */
    size_t fingerprint_processed;   /* Albums fingerprinted so far */

    /* Per-phase start times (set by indexer, used by UI for rate/ETA).
     * Indexed by indexer_phase_t.  Zero if phase was never entered. */
    int64_t phase_start_times[INDEXER_PHASE_COUNT];

    /* Path to the new atlas file written during this scan.
     * Set on INDEXER_ARTWORK_READY and INDEXER_COMPLETED.
     * Empty string if artwork processing was disabled or no atlas was written. */
    char atlas_path[512];

    /* Artist art progress (Phase 7) */
    size_t artist_art_total;
    size_t artist_art_processed;
    size_t artist_art_downloaded;

    /* Artist bio progress (Phase 8) */
    size_t artist_bio_total;
    size_t artist_bio_processed;
    size_t artist_bio_fetched;

    /* Service connectivity errors (set by Phase 5+6, checked by UI on completion) */
    bool mb_pg_error;       /* MusicBrainz PG database unreachable */
    bool acoustid_error;    /* AcoustID index or PG unreachable */
    bool fanart_error;      /* fanart.tv API key invalid or service unreachable */

    /* Per-phase wall-clock durations in ms (populated on INDEXER_COMPLETED).
     * Indexed by indexer_phase_t. Zero if phase was skipped. */
    uint32_t phase_duration_ms[INDEXER_PHASE_COUNT];

    /* Phase 2 throughput */
    float metadata_albums_per_sec;    /* albums_processed / metadata_duration */

    /* Phase 6 resolution stats */
    size_t mb_albums_attempted;
    size_t mb_albums_resolved;
    size_t mb_albums_no_match;
    size_t mb_albums_failed;

    /* Phase 7/8 HTTP stats */
    size_t artist_art_http_errors;
    size_t artist_bio_http_errors;
} indexer_progress_t;

/* =============================================================================
 * Event Types
 * ============================================================================= */

typedef enum {
    INDEXER_STARTED,
    INDEXER_PROGRESS,
    INDEXER_LIBRARY_UPDATED,  /* SQLite metadata changed -- reload library cache */
    INDEXER_ARTWORK_UPDATED,  /* Artwork atlas changed -- reload atlas texture */
    INDEXER_COMPLETED,        /* All phases done (terminal; no cache operations) */
    INDEXER_CANCELLED,
    INDEXER_ERROR
} indexer_event_t;

/* =============================================================================
 * Callback
 * ============================================================================= */

typedef void (*indexer_callback_t)(indexer_event_t event,
                                   const indexer_progress_t* progress,
                                   void* user_data);

/* =============================================================================
 * Configuration
 * ============================================================================= */

typedef struct {
    int thread_count;         /* 0 = auto (num_cpus) */
    bool process_artwork;
    int art_size;             /* Thumbnail size, default 48 */
    indexer_callback_t callback;
    void* user_data;

    /* MusicBrainz (Phase 4) */
    bool mb_resolve;          /* Run MusicBrainz resolver in Phase 4 (requires pg_conninfo) */
    const char* pg_conninfo;  /* libpq conninfo for MB+AcoustID PG database */

    /* MusicBrainz Solr search (for fuzzy text search — diacritics, Unicode) */
    const char* mb_solr_url;          /* Solr base URL, e.g. "http://host:8983" (NULL = skip text search) */

    /* AcoustID fingerprinting (Phase 5) */
    const char* acoustid_pg_conninfo;  /* libpq conninfo for AcoustID PG (NULL = skip fingerprinting) */
    const char* acoustid_index_url;    /* acoustid-index HTTP URL, e.g. "http://host:8081" (NULL = skip) */

    /* Artist art (Phase 7) — skipped entirely when false */
    bool fetch_artist_art;
    const char* fanart_api_key;  /* fanart.tv personal API key (NULL = skip fanart.tv download) */

    /* Cross-library artist art reuse: other library roots whose
     * artwork/artists/{mbid}/ dirs can be checked before fetching. */
    const char* const* other_library_roots;
    size_t other_library_roots_count;

    /* Artist bios (Phase 8) — skipped entirely when false */
    bool fetch_artist_bios;
} indexer_config_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

quadrature_result_t indexer_create(indexer_t** out, const indexer_config_t* config);
void indexer_destroy(indexer_t* indexer);

/* =============================================================================
 * Operations
 * ============================================================================= */

/**
 * Start scanning a library root. Non-blocking - returns immediately.
 *
 * The indexer opens (creating if needed) {library_root}/quadrature.sqlite
 * in the worker thread and closes it on completion. Artwork is written to
 * {library_root}/artwork/.
 *
 * @param indexer Indexer instance
 * @param library_root Absolute path to the library root directory (music files)
 * @param data_root Where to store DB + artwork (NULL = same as library_root)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t indexer_scan(indexer_t* indexer,
                                 const char* library_root,
                                 const char* data_root);

/**
 * Cancel current scan.
 *
 * @param indexer Indexer instance
 */
void indexer_cancel(indexer_t* indexer);

/**
 * Check if indexer is running.
 *
 * @param indexer Indexer instance
 * @return true if running
 */
bool indexer_is_running(const indexer_t* indexer);

/**
 * Wait for current scan to complete.
 *
 * @param indexer Indexer instance
 */
void indexer_wait(indexer_t* indexer);

/**
 * Get current progress (thread-safe copy).
 *
 * @param indexer Indexer instance
 * @param progress Output progress struct
 */
void indexer_get_progress(indexer_t* indexer, indexer_progress_t* progress);

/* =============================================================================
 * MB Resolver API (DB-driven metadata resolution)
 * ============================================================================= */

typedef struct mb_resolver mb_resolver_t;

typedef struct {
    const char* pg_conninfo;            /* libpq connection string for self-hosted MusicBrainz database */
    const char* acoustid_pg_conninfo;  /* libpq connection string for acoustid database (optional) */
    const char* acoustid_index_url;    /* URL for acoustid-index HTTP service, e.g. "http://host:8081" (optional) */
    const char* mb_solr_url;           /* MusicBrainz Solr base URL, e.g. "http://host:8983" (optional) */
    int parallelism;                    /* 0 = auto */
    const char* library_root;          /* Absolute path to library root; required for fingerprinting */
} mb_resolver_options_t;

typedef enum {
    MB_RESOLVE_QUERYING,       /* Finding unresolved albums from DB */
    MB_RESOLVE_FINGERPRINTING, /* Generating Chromaprint fingerprints from audio files on-demand */
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
    size_t fingerprint_total;      /* Albums needing fingerprinting (no MUSICBRAINZ_ALBUMID tag) */
    size_t fingerprint_processed;  /* Albums fingerprinted so far */
    mb_resolve_phase_t phase;
    double progress;
    bool acoustid_error;    /* AcoustID index/PG pool creation failed */
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
 * Fingerprint API
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

#endif /* QUADRATURE_INDEXER_H */
