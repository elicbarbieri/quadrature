/**
 * Quadrature Library Cache API
 *
 * Foundation layer for the entire application providing:
 * - Entity caching (artists, albums, tracks)
 * - O(1) track navigation (next/prev within album)
 * - Prefetch API for kernel page cache hints
 * - Search result caching
 *
 * Memory ownership: Cache owns all data. Callers get const pointers and must
 * not free them. Data remains valid until cache is cleared or destroyed.
 *
 * Thread safety: All operations are protected by internal mutex.
 */

#ifndef QUADRATURE_LIBRARY_H
#define QUADRATURE_LIBRARY_H

#include "quadrature.h"
#include "quadrature_database.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

typedef struct quadrature_db quadrature_db_t;

/* =============================================================================
 * Entity Info Types (cache owns all strings)
 * ============================================================================= */

typedef struct {
    int64_t artist_id;
    char* name;
    uint32_t album_count;
    uint32_t track_count;
    uint32_t total_duration_ms;
} library_artist_info_t;

typedef struct {
    int64_t album_id;
    int64_t artist_id;
    char* title;
    char* artist_name;
    char* path;              /* Relative path to album directory */
    char* genres;            /* Comma-separated distinct genres, or NULL */
    uint16_t year;
    uint16_t track_count;
    uint16_t disc_count;
    uint32_t total_duration_ms;
} library_album_info_t;

typedef struct {
    int64_t track_id;
    int64_t album_id;
    int64_t artist_id;       /* Primary artist ID (position 0) */
    char* path;              /* Full file path for decoding */
    char* title;
    char* artist_name;       /* Primary artist name */
    char* artist_display;    /* Formatted: "Artist A feat. Artist B" (or NULL if same as artist_name) */
    char* album_title;
    char* genre;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
} library_track_info_t;

/* Track artist credit (for UI artist buttons) */
typedef enum {
    LIBRARY_ARTIST_ROLE_PRIMARY = 0,     /* Track artist */
    LIBRARY_ARTIST_ROLE_FEATURING = 1,   /* Featured/guest artist */
} library_artist_role_t;

typedef struct {
    int64_t artist_id;
    char* name;
    library_artist_role_t role;
    int position;
} library_track_artist_t;

/* =============================================================================
 * Search Types
 * ============================================================================= */

typedef enum {
    LIBRARY_SEARCH_FILTER_ALL,
    LIBRARY_SEARCH_FILTER_ARTISTS,
    LIBRARY_SEARCH_FILTER_ALBUMS,
    LIBRARY_SEARCH_FILTER_TRACKS,
} library_search_filter_t;

typedef struct {
    GPtrArray* artists;      /* library_artist_info_t* */
    GPtrArray* albums;       /* library_album_info_t* */
    GPtrArray* tracks;       /* library_track_info_t* */
    size_t total_artists;    /* Total matches (may exceed array size) */
    size_t total_albums;
    size_t total_tracks;
} library_search_results_t;

/* =============================================================================
 * Sort Options
 * ============================================================================= */

typedef enum {
    LIBRARY_SORT_NAME_ASC,
    LIBRARY_SORT_NAME_DESC,
    LIBRARY_SORT_YEAR_ASC,
    LIBRARY_SORT_YEAR_DESC,
    LIBRARY_SORT_ARTIST_ASC, /* Sort by artist name ascending */
    LIBRARY_SORT_RECENT,     /* Recently added (by ID descending) */
    LIBRARY_SORT_TRACK_NUM,  /* For tracks within album */
} library_sort_t;

/* =============================================================================
 * Opaque Cache Handle
 * ============================================================================= */

typedef struct library_cache library_cache_t;

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * Create a new library cache.
 *
 * Opens the database in read-only mode internally. The cache owns
 * this connection and closes it on destroy.
 *
 * @param db_path Path to the SQLite database file
 * @param music_base Base path for resolving full file paths
 * @param out Output pointer for created cache
 * @return QUADRATURE_OK on success
 */
quadrature_result_t library_cache_create(const char* db_path,
                                         const char* music_base,
                                         library_cache_t** out);

/**
 * Destroy a library cache.
 *
 * @param cache Cache to destroy (NULL-safe)
 */
void library_cache_destroy(library_cache_t* cache);

/* =============================================================================
 * Entity Getters (Single Item)
 * ============================================================================= */

/**
 * Get track by ID (cached).
 * Also triggers album track prefetch for navigation.
 *
 * @param cache Library cache
 * @param track_id Track ID
 * @return Track info (owned by cache) or NULL if not found
 */
const library_track_info_t* library_cache_get_track(library_cache_t* cache,
                                                     int64_t track_id);

/**
 * Get album by ID (cached).
 *
 * @param cache Library cache
 * @param album_id Album ID
 * @return Album info (owned by cache) or NULL if not found
 */
const library_album_info_t* library_cache_get_album(library_cache_t* cache,
                                                     int64_t album_id);

/**
 * Get artist by ID (cached).
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @return Artist info (owned by cache) or NULL if not found
 */
const library_artist_info_t* library_cache_get_artist(library_cache_t* cache,
                                                       int64_t artist_id);

/**
 * Get all artists for a track (cached).
 * Returns array of artist credits ordered by position.
 *
 * @param cache Library cache
 * @param track_id Track ID
 * @return GPtrArray of library_track_artist_t* (owned by cache) or NULL if not found
 */
const GPtrArray* library_cache_get_track_artists(library_cache_t* cache,
                                                   int64_t track_id);

/* =============================================================================
 * Track Navigation (Instant Resolution)
 * ============================================================================= */

/**
 * Get next track ID in album.
 * Handles multi-disc albums correctly (disc 1 track N -> disc 2 track 1).
 *
 * @param cache Library cache
 * @param current_track_id Current track ID
 * @return Next track ID, or 0 if current track is last in album
 */
int64_t library_cache_get_next_track_id(library_cache_t* cache,
                                        int64_t current_track_id);

/**
 * Get previous track ID in album.
 *
 * @param cache Library cache
 * @param current_track_id Current track ID
 * @return Previous track ID, or 0 if current track is first in album
 */
int64_t library_cache_get_prev_track_id(library_cache_t* cache,
                                        int64_t current_track_id);

/* =============================================================================
 * List Queries
 * ============================================================================= */

/**
 * Get tracks by album (ordered by disc_num, track_num).
 *
 * @param cache Library cache
 * @param album_id Album ID
 * @return GPtrArray of library_track_info_t* (owned by cache) or NULL
 */
const GPtrArray* library_cache_get_tracks_by_album(library_cache_t* cache,
                                                    int64_t album_id);

/**
 * Get albums by artist.
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @return GPtrArray of library_album_info_t* (owned by cache) or NULL
 */
const GPtrArray* library_cache_get_albums_by_artist(library_cache_t* cache,
                                                     int64_t artist_id);

/* =============================================================================
 * "Appears On" Queries (Featured Artist Detection)
 * ============================================================================= */

/**
 * Get albums where artist appears as track artist but not album artist.
 * Used for "Appears On" section in artist detail views.
 *
 * Example: If artist "Norah Jones" has tracks on "Ray Charles - Genius Loves Company",
 * that album would appear in Norah Jones's "Appears On" list.
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @return GPtrArray of library_album_info_t* (owned by cache) or NULL
 */
const GPtrArray* library_cache_get_artist_appearances(library_cache_t* cache,
                                                       int64_t artist_id);

/**
 * Get tracks where artist appears on albums by other artists.
 * Returns the specific tracks this artist contributed to other artists' albums.
 * Sorted by album title, disc number, track number.
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @return GPtrArray of library_track_info_t* (owned by cache) or NULL
 */
const GPtrArray* library_cache_get_artist_appearance_tracks(library_cache_t* cache,
                                                             int64_t artist_id);

/**
 * Get artists matching filters (queries DB for IDs, resolves from cache).
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual artist pointers inside are cache-owned (do not free them).
 * 
 * Pass NULL for search_text and filters to get all artists (replaces get_artists).
 *
 * @param cache Library cache
 * @param sort Sort order
 * @param search_text Text to match artist name (NULL = no text filter)
 * @param filters Genre/year filter options (NULL = no genre/year filter)
 * @return GPtrArray of library_artist_info_t* (caller owns array, cache owns items)
 */
GPtrArray* library_cache_get_artists_filtered(library_cache_t* cache,
                                               library_sort_t sort,
                                               const char* search_text,
                                               const db_search_opts_t* filters);

/**
 * Get albums matching filters (queries DB for IDs, resolves from cache).
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual album pointers inside are cache-owned (do not free them).
 * 
 * Pass NULL for search_text and filters to get all albums (replaces get_albums).
 *
 * @param cache Library cache
 * @param sort Sort order
 * @param search_text Text to match album title or artist name (NULL = no text filter)
 * @param filters Genre/year filter options (NULL = no genre/year filter)
 * @return GPtrArray of library_album_info_t* (caller owns array, cache owns items)
 */
GPtrArray* library_cache_get_albums_filtered(library_cache_t* cache,
                                              library_sort_t sort,
                                              const char* search_text,
                                              const db_search_opts_t* filters);

/* =============================================================================
 * Search
 * ============================================================================= */

/**
 * Search across artists, albums, and tracks.
 * Caller owns the returned results and must free with library_search_results_free().
 *
 * @param cache Library cache
 * @param query Search query
 * @param filter Search filter (all, artists only, albums only, tracks only)
 * @param limit Max results per type (0 = unlimited)
 * @param opts Optional genre/year filter options (NULL = no extra filters)
 * @return Search results (caller-owned) or NULL on error
 */
library_search_results_t* library_cache_search(library_cache_t* cache,
                                                const char* query,
                                                library_search_filter_t filter,
                                                size_t limit,
                                                const db_search_opts_t* opts);

/**
 * Free search results returned by library_cache_search().
 */
void library_search_results_free(library_search_results_t* results);

/* =============================================================================
 * Prefetch API (Kernel Page Cache Hints)
 * ============================================================================= */

/**
 * Prefetch full-size album artwork into kernel page cache.
 * Resolves album_id -> album path, tries artwork files in discovery order.
 * Called by ArtworkManager when user clicks album row.
 *
 * @param cache Library cache
 * @param album_id Album ID
 */
void library_cache_prefetch_fullsize_artwork(library_cache_t* cache,
                                             int64_t album_id);

/**
 * Prefetch audio files into kernel page cache.
 * Resolves track_ids -> file paths, prefetches each file.
 * Called by AudioCache for visible search results / track lists.
 *
 * @param cache Library cache
 * @param track_ids Array of track IDs
 * @param count Number of track IDs
 */
void library_cache_prefetch_audio_files(library_cache_t* cache,
                                        const int64_t* track_ids,
                                        size_t count);

/* =============================================================================
 * Cache Warming
 * ============================================================================= */

typedef enum {
    LIBRARY_CACHE_IDLE = 0,
    LIBRARY_CACHE_WARMING = 1,
    LIBRARY_CACHE_READY = 2,
} library_cache_state_t;

/**
 * Callback invoked on the main thread (via g_idle_add) when warming completes.
 */
typedef void (*library_cache_ready_cb)(void* user_data);

/**
 * Set the callback for when cache warming completes.
 */
void library_cache_set_ready_callback(library_cache_t* cache,
                                       library_cache_ready_cb cb, void* user_data);

/**
 * Start background warming thread. Pages all entities into cache.
 * Safe to call multiple times — no-op if already warming or ready.
 */
void library_cache_start_warming(library_cache_t* cache);

/**
 * Get current warming state.
 */
library_cache_state_t library_cache_get_state(library_cache_t* cache);

/* =============================================================================
 * Cache Management
 * ============================================================================= */

/**
 * Clear all cached data and cancel warming.
 * Joins warming thread before clearing.
 *
 * @param cache Library cache
 */
void library_cache_clear(library_cache_t* cache);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_LIBRARY_H */
