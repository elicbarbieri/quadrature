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
#include "library.h"
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

/* Max filesystem path length used across indexer public/internal APIs. */
#define INDEXER_PATH_MAX 4096

/* =============================================================================
 * Progress Information
 * ============================================================================= */

/**
 * Indexing phases for progress display.
 */
typedef enum {
    INDEXER_PHASE_SCANNING,    /* Fast directory walk, build work queue */
    INDEXER_PHASE_METADATA,    /* Parallel metadata extraction (FFmpeg tag read only) */
    INDEXER_PHASE_ARTWORK,     /* Parallel artwork processing, atlas write */
    INDEXER_PHASE_FINGERPRINT, /* Parallel Chromaprint + AcoustID fingerprinting */
    INDEXER_PHASE_RESOLVE,     /* Batched MusicBrainz Postgres resolution */
    INDEXER_PHASE_ARTIST_ART,  /* Fetch artist images from fanart.tv */
    INDEXER_PHASE_ARTIST_BIO,  /* Fetch artist bios from Wikipedia */
    INDEXER_PHASE_FINALIZE,    /* WAL checkpoint */
    INDEXER_PHASE_COMPLETE,    /* Done */
    INDEXER_PHASE_COUNT        /* Number of phases (not a real phase) */
} indexer_phase_t;

typedef struct {
    size_t files_total;
    size_t files_processed;
    size_t files_new;
    size_t files_unchanged;
    size_t dirs_scanned;
    size_t error_count;       /* Errors logged during this scan */
    double progress;          /* 0.0 to 1.0 */
    const char *current_path; /* Currently processing (read-only, do not free) */

    /* Phase tracking */
    indexer_phase_t phase;

    /* Artwork progress */
    size_t albums_total;     /* Albums needing artwork */
    size_t albums_processed; /* Albums with artwork done */

    /* Fingerprint progress (overlaps with resolve phase) */
    size_t fingerprint_total;     /* Albums needing fingerprinting */
    size_t fingerprint_processed; /* Albums fingerprinted so far */

    /* Per-phase start times (set by indexer, used by UI for rate/ETA).
     * Indexed by indexer_phase_t.  Zero if phase was never entered. */
    int64_t phase_start_times[INDEXER_PHASE_COUNT];

    /* Path to the new atlas file written during this scan.
     * Set on INDEXER_ARTWORK_READY and INDEXER_COMPLETED.
     * Empty string if artwork processing was disabled or no atlas was written. */
    char atlas_path[INDEXER_PATH_MAX];

    /* Artist art progress (Phase 7) */
    size_t artist_art_total;
    size_t artist_art_processed;
    size_t artist_art_downloaded;

    /* Artist bio progress (Phase 8) */
    size_t artist_bio_total;
    size_t artist_bio_processed;
    size_t artist_bio_fetched;

    /* Service connectivity errors (set by Phase 5+6, checked by UI on completion) */
    bool mb_pg_error;    /* MusicBrainz PG database unreachable */
    bool acoustid_error; /* AcoustID index or PG unreachable */
    bool fanart_error;   /* fanart.tv API key invalid or service unreachable */

    /* Per-phase wall-clock durations in ms (populated on INDEXER_COMPLETED).
     * Indexed by indexer_phase_t. Zero if phase was skipped. */
    uint32_t phase_duration_ms[INDEXER_PHASE_COUNT];

    /* Phase 2 throughput */
    float metadata_albums_per_sec; /* albums_processed / metadata_duration */

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
    INDEXER_LIBRARY_UPDATED, /* SQLite metadata changed -- reload library cache */
    INDEXER_ARTWORK_UPDATED, /* Artwork atlas changed -- reload atlas texture */
    INDEXER_COMPLETED,       /* All phases done (terminal; no cache operations) */
    INDEXER_CANCELLED,
    INDEXER_ERROR
} indexer_event_t;

/* =============================================================================
 * Callback
 * ============================================================================= */

/**
 * Indexer event callback.
 *
 * @param event      Which event fired.
 * @param progress   Snapshot of indexer state at emit time (always non-NULL).
 * @param changeset  For INDEXER_LIBRARY_UPDATED: set of DB rows mutated since
 *                   the previous LIBRARY_UPDATED (ownership stays with the
 *                   indexer; valid for the duration of the call only).
 *                   NULL for all other events.
 * @param user_data  Cookie from indexer_config_t.
 */
typedef void (*indexer_callback_t)(indexer_event_t event,
                                   const indexer_progress_t *progress,
                                   const library_cache_changeset_t *changeset,
                                   void *user_data);

/* =============================================================================
 * Configuration
 * ============================================================================= */

typedef struct {
    int thread_count; /* 0 = auto (num_cpus) */
    bool process_artwork;
    int art_size; /* Thumbnail size, default 48 */
    indexer_callback_t callback;
    void *user_data;

    /* MusicBrainz (Phase 4) */
    bool mb_resolve;         /* Run MusicBrainz resolver in Phase 4 (requires pg_conninfo) */
    const char *pg_conninfo; /* libpq conninfo for MB+AcoustID PG database */

    /* MusicBrainz Solr search (for fuzzy text search — diacritics, Unicode) */
    const char *mb_solr_url; /* Solr base URL, e.g. "http://host:8983" (NULL = skip text search) */

    /* AcoustID fingerprinting (Phase 5) */
    const char
        *acoustid_pg_conninfo; /* libpq conninfo for AcoustID PG (NULL = skip fingerprinting) */
    const char
        *acoustid_index_url; /* acoustid-index HTTP URL, e.g. "http://host:8081" (NULL = skip) */
    /* HTTP backend uses a bundled AcoustID application key
     * (QUADRATURE_BUNDLED_ACOUSTID_KEY in mb_http_backend.c). No user-facing
     * AcoustID config is needed for fingerprint LOOKUP — the application key
     * identifies Quadrature itself, not the user. */

    /* Artist art (Phase 7) — skipped entirely when false */
    bool fetch_artist_art;
    const char *fanart_api_key; /* fanart.tv personal API key (NULL = skip fanart.tv download) */

    /* Cross-library artist art reuse: other library roots whose
     * artwork/artists/{mbid}/ dirs can be checked before fetching. */
    const char *const *other_library_roots;
    size_t other_library_roots_count;

    /* Artist bios (Phase 8) — skipped entirely when false */
    bool fetch_artist_bios;
} indexer_config_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

quadrature_result_t indexer_create(indexer_t **out, const indexer_config_t *config);
void indexer_destroy(indexer_t *indexer);

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
quadrature_result_t
indexer_scan(indexer_t *indexer, const char *library_root, const char *data_root);

/**
 * Cancel current scan.
 *
 * @param indexer Indexer instance
 */
void indexer_cancel(indexer_t *indexer);

/**
 * Wait for current scan to complete.
 *
 * @param indexer Indexer instance
 */
void indexer_wait(indexer_t *indexer);

/**
 * Get current progress (thread-safe copy).
 *
 * @param indexer Indexer instance
 * @param progress Output progress struct
 */
void indexer_get_progress(indexer_t *indexer, indexer_progress_t *progress);

/* =============================================================================
 * MB Resolver API (DB-driven metadata resolution)
 * ============================================================================= */

typedef struct mb_resolver mb_resolver_t;

typedef struct {
    /* Backend selection: if pg_conninfo is non-empty, the resolver builds
     * a `pg://` backend (self-hosted MB mirror). Otherwise it builds an
     * `mb+http://` backend that talks to public musicbrainz.org and
     * api.acoustid.org. The HTTP backend is strict-under the published
     * rate limits (≤0.91 req/sec MB, ≤2.86 req/sec AcoustID) and uses
     * the bundled QUADRATURE_BUNDLED_ACOUSTID_KEY for fingerprint lookups. */
    const char *pg_conninfo;          /* libpq conninfo (PG path); NULL/empty → use HTTP */
    const char *acoustid_pg_conninfo; /* libpq conninfo for acoustid DB (PG path; optional) */
    const char *acoustid_index_url;   /* acoustid-index HTTP URL (PG path; optional) */
    const char *mb_solr_url;          /* Solr URL (PG path; optional) */
    int parallelism;                  /* 0 = auto */
    const char *
        library_root; /* Absolute path to library root (music files); required for fingerprinting */
    const char *data_root; /* Absolute path to data root (DB + meta); NULL = same as library_root */
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
    size_t fingerprint_total;     /* Albums needing fingerprinting (no MUSICBRAINZ_ALBUMID tag) */
    size_t fingerprint_processed; /* Albums fingerprinted so far */
    mb_resolve_phase_t phase;
    double progress;
    bool acoustid_error; /* AcoustID index/PG pool creation failed */
} mb_resolver_progress_t;

typedef void (*mb_resolver_progress_cb)(const mb_resolver_progress_t *, void *);

/**
 * Create resolver context.
 *
 * @param out Output pointer for created resolver
 * @param db Database handle (resolver uses its own queries)
 * @param options Configuration options
 * @param callback Progress callback (may be NULL)
 * @param user_data User data for callback
 */
quadrature_result_t mb_resolver_create(mb_resolver_t **out,
                                       quadrature_db_t *db,
                                       const mb_resolver_options_t *options,
                                       mb_resolver_progress_cb callback,
                                       void *user_data);

/**
 * Run resolver (blocking). Processes all unresolved albums in DB.
 */
quadrature_result_t mb_resolver_run(mb_resolver_t *ctx);

/**
 * Destroy resolver context.
 */
void mb_resolver_destroy(mb_resolver_t *ctx);

/* =============================================================================
 * Fingerprint API
 * ============================================================================= */

typedef struct {
    char *fingerprint; /* Base64-encoded Chromaprint fingerprint */
    int duration;      /* Duration in seconds */
} mb_fingerprint_t;

/**
 * Generate audio fingerprint for a file.
 *
 * @param audio_path Path to audio file
 * @param fingerprint Output fingerprint (caller must free with mb_fingerprint_free)
 * @return QUADRATURE_OK on success
 */
quadrature_result_t mb_fingerprint_generate(const char *audio_path, mb_fingerprint_t *fingerprint);

/**
 * Free fingerprint data.
 */
void mb_fingerprint_free(mb_fingerprint_t *fp);

/* ============================================================================
 * Album State Reconciler — the single writer for album/track updates
 *
 * Producers (Phase 2 metadata extraction, Phase 6 MusicBrainz resolver) build
 * a `desired_album_state_t` describing what the album SHOULD look like.
 * `db_reconcile_album()` loads current DB state, diffs against desired, and
 * emits only the field-level UPDATE/INSERT/DELETE operations needed to
 * converge. Must be called inside an active transaction.
 * ========================================================================== */

typedef enum {
    RECONCILE_SOURCE_TAGS = 1, /* Phase 2: audio file tags (FFmpeg) */
    RECONCILE_SOURCE_MB = 2,   /* Phase 6: MusicBrainz resolver */
} reconcile_source_t;

typedef enum {
    RECONCILE_CONFIDENCE_NONE = 0,
    RECONCILE_CONFIDENCE_FUZZY = 1, /* title + duration similarity (Pass 2) */
    RECONCILE_CONFIDENCE_EXACT = 2, /* (disc,track) match (Pass 1) */
} reconcile_confidence_t;

typedef struct {
    int64_t artist_id;
    const char *name;
    const char *join_phrase; /* "", " feat. ", " & " */
    int position;
} desired_track_artist_t;

typedef enum {
    DESIRED_TRACK_TITLE = 1 << 0,
    DESIRED_TRACK_NUM = 1 << 1,
    DESIRED_TRACK_DISC = 1 << 2,
    DESIRED_TRACK_DURATION = 1 << 3,
    DESIRED_TRACK_YEAR = 1 << 4,
    DESIRED_TRACK_GENRE = 1 << 5,       /* replace */
    DESIRED_TRACK_GENRE_MERGE = 1 << 6, /* merge into existing */
    DESIRED_TRACK_MTIME = 1 << 7,
    DESIRED_TRACK_ARTISTS = 1 << 8,
} desired_track_field_t;

typedef struct {
    const char *path;        /* identity — album-relative; required */
    uint32_t present_fields; /* desired_track_field_t bitmask */

    const char *title;
    uint16_t track_num;
    uint16_t disc_num;
    uint32_t duration_ms;
    uint16_t year;
    const char *genre;
    int64_t mtime;

    const desired_track_artist_t *artists;
    size_t artist_count;

    reconcile_confidence_t position_confidence;
} desired_track_t;

typedef enum {
    DESIRED_ALBUM_TITLE = 1 << 0,
    DESIRED_ALBUM_ARTIST_ID = 1 << 1,
    DESIRED_ALBUM_COMPILATION = 1 << 2,
    DESIRED_ALBUM_YEAR = 1 << 3,
    DESIRED_ALBUM_MB_RELEASE_ID = 1 << 4,
    DESIRED_ALBUM_MB_RELEASE_GROUP = 1 << 5,
    DESIRED_ALBUM_MB_STATUS = 1 << 6,
    DESIRED_ALBUM_MB_RESOLVED_AT = 1 << 7,
    DESIRED_ALBUM_PATH = 1 << 8,
} desired_album_field_t;

typedef struct {
    reconcile_source_t source;
    uint32_t present_fields;

    const char *path;
    const char *title;
    int64_t artist_id;
    bool is_compilation;
    uint16_t year;
    const char *musicbrainz_release_id;
    const char *musicbrainz_release_group_id;
    int mb_status;
    int64_t mb_resolved_at;

    const desired_track_t *tracks;
    size_t track_count;
} desired_album_state_t;

typedef struct {
    /* Delete any existing DB track whose path is NOT in desired.tracks. */
    bool prune_missing_tracks;

    /* Minimum confidence required to overwrite track_num / disc_num from MB. */
    reconcile_confidence_t mb_position_min_confidence;

    /* When TAGS-sourced and album is already MB_STATUS_RESOLVED, skip
     * title/artist/MB-field writes (user edits + MB are authoritative). */
    bool respect_user_edits;
} reconcile_policy_t;

extern const reconcile_policy_t RECONCILE_POLICY_MB;   /* Phase 6 */
extern const reconcile_policy_t RECONCILE_POLICY_TAGS; /* Phase 2 */

typedef struct {
    int album_fields_changed;
    int tracks_inserted;
    int tracks_updated;
    int tracks_deleted;
    int track_titles_changed;
    int track_positions_changed;
    int track_artists_changed;
    int track_genres_changed;
    bool fts_synced;
} reconcile_summary_t;

/* Create a new album row (or return an existing one) by folder path.
 * All MB fields default to 0/NULL. Producers call this, then db_reconcile_album. */
quadrature_result_t db_create_or_get_album_by_path(quadrature_db_t *db,
                                                   const char *path,
                                                   const char *title,
                                                   int64_t artist_id,
                                                   uint16_t year,
                                                   int64_t *album_id_out);

/* Reconcile a batch of albums' state to match their desired states. Must run
 * inside a txn. `album_ids` and `desireds` are parallel arrays of length
 * `count`. Pass count=1 for single-album reconciliation — there is no separate
 * single-album API.
 *
 * Implementation: three bulk SELECTs via json_each(?) load current album,
 * track, and track_artist state for the entire batch, then per-album
 * diff-and-update uses cached prepared statements. Per-album failures
 * (missing album row) are skipped; the remainder of the batch still commits.
 *
 * summaries_out may be NULL, or point to a parallel array of `count` entries. */
quadrature_result_t db_reconcile_albums(quadrature_db_t *db,
                                        const int64_t *album_ids,
                                        const desired_album_state_t *desireds,
                                        size_t count,
                                        const reconcile_policy_t *policy,
                                        reconcile_summary_t *summaries_out);

/* Delete an album and all its tracks (orphan sweep). Must run inside a txn. */
quadrature_result_t db_delete_album(quadrature_db_t *db, int64_t album_id);

/* Status-only convenience: reconcile mb_status + mb_resolved_at with
 * RECONCILE_POLICY_MB. Used by the MB resolver to record FAILED / NO_MATCH
 * outcomes without building a full desired_album_state_t. Reconciler writes
 * only differing fields, so this is ~1 SELECT + 0-1 UPDATE. */
quadrature_result_t db_reconcile_album_mb_status(quadrature_db_t *db,
                                                 int64_t album_id,
                                                 int mb_status,
                                                 int64_t mb_resolved_at);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_INDEXER_H */
