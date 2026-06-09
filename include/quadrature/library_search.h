/**
 * library_search — read-side queries over a warm library_cache.
 *
 * Two flavors live here:
 *
 *   1. FTS search (library_cache_search)
 *      Cache-vocab + per-library DB FTS over artists / albums / tracks.
 *      Results dedupe cross-library by artist MBID, album RGID, and
 *      (RGID + disc + track) for tracks.
 *
 *   2. Credit search (library_credit_search)
 *      Walks each library's metadata DB (quadrature-metadata.sqlite) to
 *      resolve credits matching a text query, translates to local track
 *      IDs via the positional bridge, then dedupes cross-library by
 *      (RGID + disc + track). Production UI paths and integration tests
 *      all route through this single entry point so dedup lives in one place.
 *
 * Both APIs are thread-safe read-only queries over the cache; callers own
 * the returned structs and free them with the provided destructors.
 */

#ifndef QUADRATURE_LIBRARY_SEARCH_H
#define QUADRATURE_LIBRARY_SEARCH_H

#include <glib.h>
#include <stdint.h>

#include "library.h"

/* =============================================================================
 * FTS search
 * ============================================================================= */

typedef enum {
    LIBRARY_SEARCH_FILTER_ALL,
    LIBRARY_SEARCH_FILTER_ARTISTS,
    LIBRARY_SEARCH_FILTER_ALBUMS,
    LIBRARY_SEARCH_FILTER_TRACKS,
} library_search_filter_t;

typedef struct {
    GPtrArray *artists; /* library_artist_info_t*, borrowed from cache */
    GPtrArray *albums;  /* library_album_info_t*,  borrowed from cache */
    GPtrArray *tracks;  /* library_track_info_t*,  borrowed from cache */
    size_t total_artists;
    size_t total_albums;
    size_t total_tracks;
} library_search_results_t;

/**
 * Search artists, albums, tracks by name/title.
 *
 * @param cache        Warm cache.
 * @param query        User text. NULL short-circuits to empty results.
 * @param filter       Which entity types to return.
 * @param limit        Max per type. 0 = implementation-defined defaults
 *                     (3/4/8 for FILTER_ALL; 100 for single-type filters).
 * @param opts         Optional genre/year filters (NULL = none).
 * @param library_mask Bitmask of libraries to include.
 * @return Caller-owned results; free with library_search_results_free().
 */
library_search_results_t *library_cache_search(library_cache_t *cache,
                                               const char *query,
                                               library_search_filter_t filter,
                                               size_t limit,
                                               const db_search_opts_t *opts,
                                               uint32_t library_mask);

void library_search_results_free(library_search_results_t *results);

/* =============================================================================
 * Credit search
 * ============================================================================= */

typedef struct {
    GPtrArray *roles;  /* char *, one per unique role label            */
    char *artist_name; /* MusicBrainz artist name that produced match  */
    char *artist_mbid; /* MusicBrainz artist MBID                      */
} library_credit_info_t;

typedef struct {
    /* Global track_ids (int64_t) that survived cross-library dedup.
     * Order is insertion order from the first library that produced each one. */
    GArray *track_ids;

    /* Global album_ids (int64_t) derived from surviving tracks,
     * deduped by release-group MBID. */
    GArray *album_ids;

    /* Surviving-track_id → library_credit_info_t. Roles from all libraries
     * that produced the same recording are merged into the survivor. */
    GHashTable *credit_info;

    /* Unique meta artists matched across all libraries as
     * packed "mbid\tname\ttype" strings (owned). */
    GPtrArray *meta_artists;
} library_credit_search_result_t;

/**
 * Credit search across all libraries matching `library_mask`.
 *
 * @param cache        Warm cache.
 * @param credit_text  Text to match against metadata artist names
 *                     (NULL or empty → empty result).
 * @param role_gid     Optional link-type GID filter (NULL = any role).
 * @param library_mask Bitmask of libraries to include.
 * @return Newly-allocated result; never NULL.
 *         Free with library_credit_search_result_free().
 */
library_credit_search_result_t *library_credit_search(library_cache_t *cache,
                                                      const char *credit_text,
                                                      const char *role_gid,
                                                      uint32_t library_mask);

void library_credit_search_result_free(library_credit_search_result_t *r);

#endif /* QUADRATURE_LIBRARY_SEARCH_H */
