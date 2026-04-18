/**
 * Quadrature Metadata DB — MusicBrainz Recording Relations
 *
 * A separate per-library SQLite database (quadrature-metadata.sqlite) that
 * stores artist-recording relationships fetched from MusicBrainz: producers,
 * remixers, vocalists, engineers, and all other l_artist_recording link types.
 *
 * Written exclusively by Phase 4 (mb_resolver) after a successful album
 * resolution. Read on-demand by the UI detail view — the DB is opened,
 * queried, and closed immediately to keep RAM at zero between accesses.
 *
 * If the file does not exist (Phase 4 never ran), db_meta_open_readonly()
 * returns QUADRATURE_ERROR_FILE_NOT_FOUND. Callers must handle this
 * gracefully — show nothing, no crash.
 */

#ifndef QUADRATURE_METADATA_H
#define QUADRATURE_METADATA_H

#include "quadrature/quadrature.h"
#include "quadrature/database.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Opaque Handle
 * ============================================================================= */

typedef struct quadrature_meta_db quadrature_meta_db_t;

/* =============================================================================
 * Result Types (read path)
 * ============================================================================= */

/**
 * One row returned by db_meta_get_links().
 * All string fields are owned by the array; free with db_meta_links_free().
 */
typedef struct {
    char* link_type_gid;    /* link_type.gid (UUID string) */
    char* link_type_name;   /* e.g. "producer", "remixer", "vocal" */
    char* artist_mbid;      /* artist.gid (UUID string) */
    char* artist_name;      /* canonical artist name */
    char* artist_sort_name; /* sort name; may be NULL */
    char* artist_type;      /* "Person", "Group", etc.; may be NULL */
    char* entity0_credit;   /* credited name override; NULL = use artist_name */
    char* attributes;       /* comma-separated attribute names; NULL if none */
} db_meta_link_t;

/**
 * Release-level metadata from the releases table.
 * All string fields are owned; free with db_meta_release_free().
 */
typedef struct {
    char* release_date;     /* "YYYY-MM-DD", "YYYY-MM", or "YYYY"; may be NULL */
    char* release_type;     /* "Album", "EP", "Single", etc.; may be NULL */
    char* label;            /* Record label name; may be NULL */
    char* catalog_number;   /* Label catalog number; may be NULL */
    char* barcode;          /* UPC/barcode; may be NULL */
    char* genres;           /* Semicolon-separated curated MB genres; may be NULL */
} db_meta_release_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * Open (or create) the metadata DB at {library_root}/quadrature-metadata.sqlite.
 * Applies schema and pragmas. Used by Phase 4 writer.
 */
quadrature_result_t db_meta_open(const char* library_root,
                                  quadrature_meta_db_t** out);

/**
 * Open the metadata DB in ATTACH mode — attaches quadrature-metadata.sqlite
 * to the main DB connection as schema "meta". Writes go through the main
 * connection, so db_reconcile_album and meta writes commit atomically inside
 * the main batch transaction.
 *
 * db_meta_begin/commit become no-ops in this mode (the main db owns the
 * transaction). db_meta_close() will DETACH but leave the main connection
 * open.
 */
quadrature_result_t db_meta_open_attached(quadrature_db_t* main_db,
                                           const char* library_root,
                                           quadrature_meta_db_t** out);

/**
 * Open the metadata DB read-only. Returns QUADRATURE_ERROR_FILE_NOT_FOUND if
 * the file does not exist (Phase 4 never ran). Used by the UI.
 */
quadrature_result_t db_meta_open_readonly(const char* library_root,
                                           quadrature_meta_db_t** out);

/**
 * Close and free the metadata DB handle.
 */
void db_meta_close(quadrature_meta_db_t* db);

/* =============================================================================
 * Transactions (write path)
 * ============================================================================= */

quadrature_result_t db_meta_begin(quadrature_meta_db_t* db);
quadrature_result_t db_meta_commit(quadrature_meta_db_t* db);
quadrature_result_t db_meta_rollback(quadrature_meta_db_t* db);

/* =============================================================================
 * Write Operations (Phase 4 only)
 * ============================================================================= */

/**
 * Upsert the (release_mbid, disc_num, track_num) -> recording_mbid bridge row.
 */
quadrature_result_t db_meta_upsert_recording(quadrature_meta_db_t* db,
    const char* recording_mbid,
    const char* release_mbid,
    int disc_num,
    int track_num);

/**
 * Upsert a link_type row by its GID.
 */
quadrature_result_t db_meta_upsert_link_type(quadrature_meta_db_t* db,
    const char* link_type_gid,
    const char* name,
    const char* description);

/**
 * Upsert an artist row by its MBID.
 */
quadrature_result_t db_meta_upsert_artist(quadrature_meta_db_t* db,
    const char* artist_mbid,
    const char* name,
    const char* sort_name,
    const char* artist_type);

/**
 * Insert one recording_links row. Call db_meta_delete_recording_links() first
 * if re-resolving to avoid duplicate rows.
 */
quadrature_result_t db_meta_insert_recording_link(quadrature_meta_db_t* db,
    const char* recording_mbid,
    const char* artist_mbid,
    const char* link_type_gid,
    const char* entity0_credit,
    const char* attributes);

/**
 * Delete all recording_links rows for a recording_mbid. Call before
 * re-inserting on re-resolve to keep data fresh.
 */
quadrature_result_t db_meta_delete_recording_links(quadrature_meta_db_t* db,
    const char* recording_mbid);

/**
 * Upsert release-level metadata (date, type, label, catalog, barcode, genres).
 * Called by Phase 4 after successful resolution.
 */
quadrature_result_t db_meta_upsert_release(quadrature_meta_db_t* db,
    const char* release_mbid,
    const char* release_date,
    const char* release_type,
    const char* label,
    const char* catalog_number,
    const char* barcode,
    const char* genres);

/* =============================================================================
 * Read Operations (UI)
 * ============================================================================= */

/**
 * Look up recording_mbid by (release_mbid, disc_num, track_num).
 * On success, *out is a g_malloc'd string the caller must g_free().
 * Returns QUADRATURE_ERROR_FILE_NOT_FOUND if no row exists.
 */
quadrature_result_t db_meta_get_recording_mbid(quadrature_meta_db_t* db,
    const char* release_mbid,
    int disc_num,
    int track_num,
    char** out);

/**
 * Fetch all recording_links rows for a recording_mbid.
 * *out is a heap-allocated array of *count elements; free with db_meta_links_free().
 * Returns QUADRATURE_OK with count=0 if no links exist.
 */
quadrature_result_t db_meta_get_links(quadrature_meta_db_t* db,
    const char* recording_mbid,
    db_meta_link_t** out,
    size_t* count);

/**
 * Fetch recording_mbids linked to a given artist, optionally filtered by
 * link_type_gid (pass NULL for all link types).
 * *out_recording_mbids is a NULL-terminated array of g_malloc'd strings;
 * caller must g_free each element and g_free the array.
 */
quadrature_result_t db_meta_get_recordings_by_artist(quadrature_meta_db_t* db,
    const char* artist_mbid,
    const char* link_type_gid_filter,
    char*** out_recording_mbids,
    size_t* count);

/**
 * Free a db_meta_link_t array returned by db_meta_get_links().
 */
void db_meta_links_free(db_meta_link_t* links, size_t count);

/**
 * Fetch release-level metadata by MBID.
 * On success, *out is a heap-allocated struct; free with db_meta_release_free().
 * Returns QUADRATURE_ERROR_FILE_NOT_FOUND if no row exists.
 */
quadrature_result_t db_meta_get_release(quadrature_meta_db_t* db,
    const char* release_mbid,
    db_meta_release_t** out);

/**
 * Free a db_meta_release_t returned by db_meta_get_release().
 */
void db_meta_release_free(db_meta_release_t* release);

/* =============================================================================
 * Credit Bridge Queries (artist-centric, for navigation & search)
 * ============================================================================= */

/**
 * One credit row for a given artist across all their recording links.
 * Provides positional data to resolve back to main DB tracks.
 * Free array with db_meta_artist_credits_free().
 */
typedef struct {
    char *release_mbid;
    int disc_num;
    int track_num;
    char *link_type_name;   /* "producer", "vocal", "instrument" */
    char *attributes;       /* "guitar", "bass", etc. (NULL if none) */
} db_meta_artist_credit_t;

/**
 * Fetch all recording credits for a given artist, optionally filtered by
 * link_type_gid (pass NULL for all roles).
 * Returns positional tuples that can be resolved to main DB track_ids.
 */
quadrature_result_t db_meta_get_credits_by_artist(
    quadrature_meta_db_t *db, const char *artist_mbid,
    const char *link_type_gid_filter,
    db_meta_artist_credit_t **out, size_t *count);

void db_meta_artist_credits_free(db_meta_artist_credit_t *credits, size_t count);

/**
 * One result row from db_meta_search_artists().
 * Free array with db_meta_artist_search_results_free().
 */
typedef struct {
    char *artist_mbid;
    char *name;
    char *sort_name;
    char *artist_type;  /* "Person", "Group", etc. */
} db_meta_artist_search_result_t;

/**
 * Search metadata artists by name using LIKE '%query%' COLLATE NOCASE.
 * The artists table has <5K rows — no FTS needed.
 */
quadrature_result_t db_meta_search_artists(
    quadrature_meta_db_t *db, const char *query,
    size_t limit, db_meta_artist_search_result_t **out, size_t *count);

void db_meta_artist_search_results_free(db_meta_artist_search_result_t *results, size_t count);

/* =============================================================================
 * Maintenance
 * ============================================================================= */

/**
 * Issue a WAL checkpoint (PASSIVE mode). Call before closing after a write session.
 */
quadrature_result_t db_meta_checkpoint(quadrature_meta_db_t* db);

/* =============================================================================
 * Bios DB — Artist biographies (separate from metadata DB)
 *
 * Stored in {library_root}/quadrature-bios.sqlite so that deleting the
 * metadata DB (to force MusicBrainz re-resolve) does not destroy bios.
 * On first open, auto-migrates existing bios from the metadata DB.
 * ============================================================================= */

typedef struct quadrature_bios_db quadrature_bios_db_t;

quadrature_result_t db_bios_open(const char* library_root, quadrature_bios_db_t** out);
quadrature_result_t db_bios_open_readonly(const char* library_root, quadrature_bios_db_t** out);
void db_bios_close(quadrature_bios_db_t* db);
quadrature_result_t db_bios_begin(quadrature_bios_db_t* db);
quadrature_result_t db_bios_commit(quadrature_bios_db_t* db);
quadrature_result_t db_bios_checkpoint(quadrature_bios_db_t* db);
quadrature_result_t db_bios_upsert(quadrature_bios_db_t* db, const char* artist_mbid,
                                    const char* bio_text, const char* wiki_url);
quadrature_result_t db_bios_get(quadrature_bios_db_t* db, const char* artist_mbid,
                                 char** bio_text_out, char** wiki_url_out);
quadrature_result_t db_bios_exists(quadrature_bios_db_t* db, const char* artist_mbid,
                                    bool* exists_out);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_METADATA_H */
