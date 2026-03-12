/**
 * Quadrature Library Cache API
 *
 * Foundation layer for the entire application providing:
 * - Entity caching (artists, albums, tracks)
 * - O(1) track navigation (next/prev within album)
 * - Prefetch API for kernel page cache hints
 * - Search result caching
 * - Global ID encoding for multi-library support
 *
 * Memory ownership: Cache owns all data. Callers get const pointers and must
 * not free them. Data remains valid until cache is cleared or destroyed.
 *
 * Thread safety: All operations are protected by internal mutex.
 */

#ifndef QUADRATURE_LIBRARY_H
#define QUADRATURE_LIBRARY_H

#include "quadrature.h"
#include "database.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Library Mask (Multi-Library Filtering)
 *
 * Bitmask where bit N = library N is enabled. LIBRARY_MASK_ALL = no filter.
 * Supports up to 32 libraries.
 * ============================================================================= */

#define LIBRARY_MASK_ALL  UINT32_MAX

/**
 * Compute new mask after a left-click toggle on a library pill.
 *
 * @param current_mask  Current library_mask before GTK flips the button.
 * @param lib_idx       Index of the clicked library (0–31).
 * @param now_active    Button state AFTER GTK toggled it (TRUE = was OFF, now ON).
 * @return              New mask (never 0 — falls back to LIBRARY_MASK_ALL).
 */
static inline uint32_t library_mask_after_toggle(uint32_t current_mask,
                                                  int lib_idx,
                                                  gboolean now_active) {
    uint32_t result;

    if (now_active) {
        /* Button was OFF → add to active set */
        result = current_mask | (1u << lib_idx);
    } else {
        /* Button was ON → check if all were on before this click */
        uint32_t pre_click_mask = current_mask | (1u << lib_idx);
        if (pre_click_mask == LIBRARY_MASK_ALL) {
            /* All were on before click → solo this library */
            result = 1u << lib_idx;
        } else {
            /* Remove this library from active set */
            result = current_mask & ~(1u << lib_idx);
        }
    }

    return (result == 0) ? LIBRARY_MASK_ALL : result;
}

/* =============================================================================
 * Global ID Encoding (Multi-Library Support)
 *
 * Bit-packed global entity IDs:
 *   Bits 63-48: library index (0-based, 16 bits)
 *   Bits 47-0:  local entity ID within that library (48 bits)
 *
 * Library 0 global IDs are identical to their local IDs (upper 16 bits = 0),
 * providing full backward compatibility for single-library use.
 * ============================================================================= */

/** Encode a (library index, local id) pair into a global entity ID. */
#define LIBRARY_MAKE_GLOBAL_ID(lib_idx, local_id) \
    (((int64_t)((lib_idx) & 0xFFFF) << 48) | \
     ((int64_t)(local_id) & INT64_C(0x0000FFFFFFFFFFFF)))

/** Extract the 0-based library index from a global entity ID. */
#define LIBRARY_GLOBAL_ID_LIB(id) \
    ((int)(((int64_t)(id) >> 48) & 0xFFFF))

/** Extract the local entity ID from a global entity ID. */
#define LIBRARY_GLOBAL_ID_LOCAL(id) \
    ((int64_t)((id) & INT64_C(0x0000FFFFFFFFFFFF)))

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

typedef struct quadrature_db quadrature_db_t;
typedef struct quadrature_meta_db quadrature_meta_db_t;
typedef struct quadrature_bios_db quadrature_bios_db_t;

/* =============================================================================
 * Entity Info Types (cache owns all strings)
 * ============================================================================= */

typedef struct {
    int64_t artist_id;       /* Global ID: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id) */
    char* name;
    char* musicbrainz_id;    /* MBID for cross-library merging; NULL if unknown */
    uint32_t album_count;
    uint32_t track_count;
    int library_index;       /* Source library (0-based); -1 if merged across libraries */
    int64_t *merged_source_ids;  /* NULL if single-library; GLOBAL artist IDs from each source */
    int merged_source_count;
} library_artist_info_t;

typedef struct {
    int64_t album_id;        /* Global ID: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id) */
    int64_t artist_id;       /* Global ID */
    char* title;
    char* artist_name;
    char* path;              /* Relative path to album directory */
    char* genres;            /* Comma-separated distinct genres, or NULL */
    uint16_t year;
    uint16_t track_count;
    char* musicbrainz_release_id; /* Album MBID; NULL if unknown */
    int library_index;       /* Source library (0-based); -1 if merged across libraries */
    int64_t *merged_source_ids;  /* NULL if single-library; GLOBAL album IDs from each source */
    int merged_source_count;
} library_album_info_t;

typedef struct {
    int64_t track_id;        /* Global ID: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id) */
    int64_t album_id;        /* Global ID */
    int64_t artist_id;       /* Global ID of primary artist (position 0) */
    char* path;              /* Relative path within album dir (resolve via library_cache_resolve_track_path) */
    char* title;
    char* artist_display;    /* Display name: "Artist A feat. Artist B", or just primary name. Always non-NULL. */
    char* album_title;
    char* genre;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
    int library_index;       /* Source library (0-based) */
} library_track_info_t;

/* Track artist credit (for UI artist buttons) */
typedef enum {
    LIBRARY_ARTIST_ROLE_PRIMARY = 0,     /* Track artist */
    LIBRARY_ARTIST_ROLE_FEATURING = 1,   /* Featured/guest artist */
} library_artist_role_t;

typedef struct {
    int64_t artist_id;
    char* name;          /* Canonical artist name */
    char* join_phrase;   /* Connector to next artist: " feat. ", " & ", "" for last */
    library_artist_role_t role; /* Derived from position: 0=PRIMARY, >0=FEATURING */
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
 * Source descriptor for one library slot in a multi-library cache.
 */
typedef struct {
    const char *db_path;       /* Path to quadrature.sqlite for this library */
    const char *music_base;    /* Root directory for resolving relative file paths */
    const char *display_name;  /* Human-readable name; NULL = use basename(music_base) */
} library_cache_source_t;

/**
 * Create a multi-library cache.
 *
 * Each source describes one library slot. Slots are assigned indices 0..N-1.
 * Entity IDs exposed by all accessor functions are GLOBAL IDs encoded with
 * LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id). Library 0 global IDs are
 * identical to their local DB IDs (backward compatible).
 *
 * @param sources Array of library source descriptors (NULL if source_count == 0)
 * @param source_count Number of sources (0 = empty cache, slots added later)
 * @param out Output pointer for created cache
 * @return QUADRATURE_OK on success
 */
quadrature_result_t library_cache_create_multi(const library_cache_source_t *sources,
                                                int source_count,
                                                library_cache_t **out);

/**
 * Create a new library cache (single-library convenience wrapper).
 *
 * Equivalent to library_cache_create_multi() with source_count=1.
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

/**
 * Add a new library slot to an existing cache.
 *
 * Cancels all warming threads (realloc may move the slots array), initializes
 * the new slot from the source descriptor, and returns its 0-based index.
 * Caller should call library_cache_warm_slot() afterward to populate it.
 *
 * @param cache Library cache
 * @param source Source descriptor for the new library
 * @return New slot index on success, -1 on failure
 */
int library_cache_add_slot(library_cache_t *cache,
                           const library_cache_source_t *source);

/**
 * Remove a library slot and shift remaining slots down.
 *
 * Cancels all warming threads, destroys the target slot, shifts higher slots
 * down by one (clearing their cached entities since baked-in global IDs change),
 * and rebuilds cross-library artist merging.
 *
 * After removal, shifted slots are in IDLE state and must be rewarmed
 * (e.g. via library_cache_start_warming()).
 *
 * @param cache Library cache
 * @param lib_idx 0-based slot index to remove
 * @return QUADRATURE_OK on success
 */
quadrature_result_t library_cache_remove_slot(library_cache_t *cache, int lib_idx);

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

/**
 * Resolve the full absolute path for a track.
 * Combines music_base + album_path + track_relative_path.
 * Caller must g_free() the returned string.
 *
 * @param cache Library cache
 * @param track_id Track ID
 * @return Absolute path (caller-owned), or NULL if track not found
 */
char *library_cache_resolve_track_path(library_cache_t *cache, int64_t track_id);

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
                                               const db_search_opts_t* filters,
                                               uint32_t library_mask);

/**
 * Get albums matching filters (queries DB for IDs, resolves from cache).
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual album pointers inside are cache-owned (do not free them).
 * Cross-library albums with the same musicbrainz_release_id are deduplicated.
 *
 * @param cache Library cache
 * @param sort Sort order
 * @param search_text Text to match album title or artist name (NULL = no text filter)
 * @param filters Genre/year filter options (NULL = no genre/year filter)
 * @param library_mask Bitmask of enabled libraries (LIBRARY_MASK_ALL = all)
 * @return GPtrArray of library_album_info_t* (caller owns array, cache owns items)
 */
GPtrArray* library_cache_get_albums_filtered(library_cache_t* cache,
                                              library_sort_t sort,
                                              const char* search_text,
                                              const db_search_opts_t* filters,
                                              uint32_t library_mask);

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
                                                const db_search_opts_t* opts,
                                                uint32_t library_mask);

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
 * Start background warming thread for ALL idle library slots.
 * Safe to call multiple times -- individual slots are no-ops if already
 * warming or ready.
 */
void library_cache_start_warming(library_cache_t* cache);

/**
 * Warm a specific library slot. No-op if slot is already warming or ready.
 * The ready callback fires on the main thread when this slot's warming
 * completes.
 *
 * @param lib_idx  0-based library slot index
 */
void library_cache_warm_slot(library_cache_t *cache, int lib_idx);

/* =============================================================================
 * Cache Management
 * ============================================================================= */

/**
 * Clear all cached data across ALL slots and cancel all warming threads.
 * Joins warming threads before clearing.
 *
 * @param cache Library cache
 */
void library_cache_clear(library_cache_t* cache);

/**
 * Clear a specific library slot and reset it to IDLE.
 * Cancels any active warming thread for this slot.
 * Cross-library artist merging is rebuilt to remove the cleared slot's
 * stale merge state.
 *
 * @param lib_idx  0-based library slot index
 */
void library_cache_clear_slot(library_cache_t *cache, int lib_idx);

/* =============================================================================
 * Multi-Library Accessors
 * ============================================================================= */

/**
 * Get the number of library slots in this cache.
 * Returns the actual slot_count set at creation time.
 */
int library_cache_get_library_count(library_cache_t* cache);

/**
 * Estimate memory bytes used by a single library slot's entity arrays.
 * O(1) computation from pre-tracked capacity × sizeof — suitable for
 * performance dashboard polling at 1-10Hz.
 *
 * @param cache Library cache
 * @param library_index Slot index (0-based)
 * @return Estimated bytes, or 0 if index invalid or slot is IDLE
 */
size_t library_cache_get_slot_memory_bytes(library_cache_t* cache, int library_index);

/**
 * Get cached readonly DB handles for a library slot.
 * Returns NULL if index is invalid or the DB file doesn't exist.
 * Caller must NOT close the returned handle.
 */
quadrature_meta_db_t *library_cache_get_meta_db(library_cache_t *cache, int lib_idx);
quadrature_bios_db_t *library_cache_get_bios_db(library_cache_t *cache, int lib_idx);
quadrature_db_t *library_cache_get_db(library_cache_t *cache, int lib_idx);

/**
 * Get the display name for a library slot.
 * Returns the configured display name, or the basename of the library path
 * if none was configured.
 *
 * @param cache Library cache
 * @param library_index Slot index (0-based)
 * @return Static string owned by the cache; do not free. NULL if index invalid.
 */
const char* library_cache_get_library_name(library_cache_t* cache, int library_index);

/**
 * Update the display name for a library slot.
 *
 * Replaces the current display_name. Pass NULL to revert to basename fallback.
 * The cache takes ownership of a copy of @p name.
 *
 * @param cache  Library cache
 * @param lib_idx  Slot index (0-based)
 * @param name  New display name, or NULL
 */
void library_cache_set_library_name(library_cache_t* cache, int lib_idx, const char* name);

/**
 * Set availability for a library slot.
 *
 * When FALSE, filtered queries (artists, albums, search) skip this slot
 * entirely — its entities vanish from the UI.  Single-entity getters
 * (get_track, get_album, get_artist) still work for in-flight operations
 * like currently-playing tracks.
 *
 * Thread-safe (atomic store).
 */
void library_cache_set_available(library_cache_t* cache, int lib_idx, gboolean available);

/**
 * Get availability for a library slot.
 * Thread-safe (atomic load).
 */
gboolean library_cache_get_available(library_cache_t* cache, int lib_idx);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_LIBRARY_H */
