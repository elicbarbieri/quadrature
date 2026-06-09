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

#define LIBRARY_MASK_ALL UINT32_MAX

/* Sentinel passed to library_cache_get_albums / _get_artists `num_results`
 * parameter to request every matching library version. */
#define LIBRARY_RESULTS_ALL (-1)

/** Toggle a single library bit. Never returns 0 — falls back to LIBRARY_MASK_ALL. */
uint32_t library_mask_after_toggle(uint32_t current_mask, int lib_idx);

/** Solo: enable only this library, disable all others. */
uint32_t library_mask_solo(int lib_idx);

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
    (((int64_t)((lib_idx) & 0xFFFF) << 48) | ((int64_t)(local_id) & INT64_C(0x0000FFFFFFFFFFFF)))

/** Extract the bitmap index (stable library ID) from a global entity ID. */
#define LIBRARY_GLOBAL_ID_LIB(id) ((int)(((int64_t)(id) >> 48) & 0xFFFF))

/** Extract the local entity ID from a global entity ID. */
#define LIBRARY_GLOBAL_ID_LOCAL(id) ((int64_t)((id) & INT64_C(0x0000FFFFFFFFFFFF)))

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
    int64_t artist_id; /* Global ID: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id) */
    char *name;
    char *musicbrainz_id; /* MBID for cross-library merging; NULL if unknown */
    uint32_t album_count; /* Per-slot count (set during warming) */
    uint32_t track_count; /* Per-slot count (set during warming) */
    int library_index;    /* Source library (0-based bitmap index) */
} library_artist_info_t;

typedef struct {
    int64_t album_id;       /* Global ID: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id) */
    int64_t artist_id;      /* Global ID */
    int64_t first_track_id; /* Global ID of first track (disc 1, track 1); 0 if unknown */
    char *title;
    char *artist_name;
    char *path;   /* Relative path to album directory */
    char *genres; /* Comma-separated distinct genres, or NULL */
    uint16_t year;
    uint16_t track_count;
    char *musicbrainz_release_id;       /* Album MBID; NULL if unknown */
    char *musicbrainz_release_group_id; /* Release group MBID; NULL if unknown */
    int library_index;                  /* Source library (0-based bitmap index) */
} library_album_info_t;

typedef struct {
    int64_t track_id;  /* Global ID: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id) */
    int64_t album_id;  /* Global ID */
    int64_t artist_id; /* Global ID of primary artist (position 0) */
    char *path; /* Relative path within album dir (resolve via library_cache_resolve_track_path) */
    char *title;
    char *
        artist_display; /* Display name: "Artist A feat. Artist B", or just primary name. Always non-NULL. */
    char *album_title;
    char *genre;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
    int library_index; /* Source library (0-based) */
} library_track_info_t;

/* Track artist credit (for UI artist buttons) */
typedef enum {
    LIBRARY_ARTIST_ROLE_PRIMARY = 0,   /* Track artist */
    LIBRARY_ARTIST_ROLE_FEATURING = 1, /* Featured/guest artist */
} library_artist_role_t;

typedef struct {
    int64_t artist_id;
    char *name;                 /* Canonical artist name */
    char *join_phrase;          /* Connector to next artist: " feat. ", " & ", "" for last */
    library_artist_role_t role; /* Derived from position: 0=PRIMARY, >0=FEATURING */
    int position;
} library_track_artist_t;

/* =============================================================================
 * Search Types
 * ============================================================================= */

/* FTS search types (library_search_filter_t, library_search_results_t) and
 * library_cache_search / library_credit_search live in library_search.h.
 * Kept separate so UI views that only touch cache entity data don't pull in
 * the search query surface. */

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
 * COW Refresh Changeset
 *
 * Carries the set of DB rowids that have been mutated since the last cache
 * warming (or previous refresh). Produced by the indexer's ChangeTracker,
 * consumed by library_cache_refresh_slot() to drive SEED skipping.
 *
 * See docs/architecture/LIBRARY_CACHE.md → "COW Refresh Invariants" for the
 * correctness contract — in short: the changeset is never load-bearing.
 * Passing NULL means "unknown, rebuild everything from DB" and is always safe.
 *
 * All int64_t arrays are LOCAL rowids (not global IDs).
 * ============================================================================= */

typedef struct library_cache_changeset {
    int64_t *artists; /* Mutated local artist rowids; NULL if count==0 */
    size_t artists_count;

    int64_t *albums; /* Mutated local album rowids */
    size_t albums_count;

    int64_t *tracks; /* Mutated local track rowids */
    size_t tracks_count;

    /* True if any track_artists row was mutated. track_artists is keyed by a
     * synthetic rowid (not track_id), so row-precise invalidation isn't
     * worthwhile — if set, the refresh rebuilds all track_artists arrays. */
    bool track_artists_dirty;
} library_cache_changeset_t;

/** Allocate an empty changeset (zeroed). Owns its arrays. */
library_cache_changeset_t *library_cache_changeset_new(void);

/** Free a changeset and its arrays. NULL-safe. */
void library_cache_changeset_free(library_cache_changeset_t *cs);

/** Deep-copy a changeset. Returns NULL if src is NULL. */
library_cache_changeset_t *library_cache_changeset_copy(const library_cache_changeset_t *src);

/**
 * Merge src into dst. Duplicates in the unioned arrays are removed. If either
 * has track_artists_dirty, the result has it too. src is unchanged.
 * Safe when dst is NULL (no-op) or src is NULL (no-op).
 */
void library_cache_changeset_merge(library_cache_changeset_t *dst,
                                   const library_cache_changeset_t *src);

/** True if the changeset contains no rowids and track_artists is clean. */
bool library_cache_changeset_is_empty(const library_cache_changeset_t *cs);

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * Source descriptor for one library slot in a multi-library cache.
 */
typedef struct {
    const char *db_path;      /* Path to quadrature.sqlite for this library */
    const char *music_base;   /* Root directory for resolving relative file paths */
    const char *display_name; /* Human-readable name; NULL = use basename(music_base) */
    int bitmap_index;         /* Stable library ID encoded in global entity IDs.
                                * Must be unique across sources and ≥ 0.
                                * Typically settings->libraries[i].library_index. */
} library_cache_source_t;

/**
 * Create a multi-library cache.
 *
 * Each source describes one library slot. Entity IDs exposed by all accessor
 * functions are GLOBAL IDs encoded with LIBRARY_MAKE_GLOBAL_ID(bitmap_index,
 * local_id), where bitmap_index is the stable library ID from the source
 * descriptor.  Slots are addressed internally by position but looked up
 * externally via the bitmap_index → slot mapping.
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
 * Destroy a library cache.
 *
 * @param cache Cache to destroy (NULL-safe)
 */
void library_cache_destroy(library_cache_t *cache);

/**
 * Add a new library slot to an existing cache.
 *
 * Cancels all warming threads (realloc may move the slots array), initializes
 * the new slot from the source descriptor, and registers it in the bitmap map.
 * Caller should call library_cache_warm_slot() afterward to populate it.
 *
 * @param cache Library cache
 * @param source Source descriptor (must include a unique bitmap_index)
 * @return The bitmap_index on success, -1 on failure
 */
int library_cache_add_slot(library_cache_t *cache, const library_cache_source_t *source);

/**
 * Remove a library slot by bitmap index.
 *
 * Cancels all warming threads, destroys the target slot, compacts remaining
 * slots, and rebuilds cross-library artist merging.  Global IDs for
 * remaining libraries are unchanged (they encode bitmap_index, not position).
 *
 * @param cache Library cache
 * @param bitmap_index Stable library ID to remove
 * @return QUADRATURE_OK on success
 */
quadrature_result_t library_cache_remove_slot(library_cache_t *cache, int bitmap_index);

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
const library_track_info_t *library_cache_get_track(library_cache_t *cache, int64_t track_id);

/**
 * Get album by ID (cached). Calls get_albums() and returns the first entry
 * (respects library sort order in the bitmask).
 */
const library_album_info_t *
library_cache_get_album(library_cache_t *cache, int64_t album_id, uint32_t library_mask);

/**
 * Get all library versions of an album (via release-group MBID index).
 * Returns a GPtrArray of const library_album_info_t* sorted by bitmap_index.
 * If the album has no release-group MBID, returns just the source album.
 *
 * @param cache        Library cache
 * @param album_id     Global album ID (any library version)
 * @param library_mask Bitmask of enabled libraries
 * @param num_results  Max results to return (LIBRARY_RESULTS_ALL for every version)
 * @return GPtrArray of interior pointers (caller must g_ptr_array_unref), or NULL
 */
GPtrArray *library_cache_get_albums(library_cache_t *cache,
                                    int64_t album_id,
                                    uint32_t library_mask,
                                    int num_results);

/**
 * Get artist by ID (cached). Calls get_artists() and returns the first entry
 * (respects library sort order in the bitmask).
 */
const library_artist_info_t *
library_cache_get_artist(library_cache_t *cache, int64_t artist_id, uint32_t library_mask);

/**
 * Get all library versions of an artist (via MusicBrainz artist ID index).
 * Returns a GPtrArray of const library_artist_info_t* sorted by bitmap_index.
 * If the artist has no MBID, returns just the source artist.
 *
 * @param cache        Library cache
 * @param artist_id    Global artist ID (any library version)
 * @param library_mask Bitmask of enabled libraries
 * @param num_results  Max results to return (LIBRARY_RESULTS_ALL for every version)
 * @return GPtrArray of interior pointers (caller must g_ptr_array_unref), or NULL
 */
GPtrArray *library_cache_get_artists(library_cache_t *cache,
                                     int64_t artist_id,
                                     uint32_t library_mask,
                                     int num_results);

/**
 * Get all artists for a track (cached).
 * Returns array of artist credits ordered by position.
 *
 * @param cache Library cache
 * @param track_id Track ID
 * @return GPtrArray of library_track_artist_t* (owned by cache) or NULL if not found
 */
const GPtrArray *library_cache_get_track_artists(library_cache_t *cache, int64_t track_id);

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
int64_t library_cache_get_next_track_id(library_cache_t *cache, int64_t current_track_id);

/**
 * Get previous track ID in album.
 *
 * @param cache Library cache
 * @param current_track_id Current track ID
 * @return Previous track ID, or 0 if current track is first in album
 */
int64_t library_cache_get_prev_track_id(library_cache_t *cache, int64_t current_track_id);

/* =============================================================================
 * List Queries
 * ============================================================================= */

/**
 * Get tracks by album (ordered by disc_num, track_num).
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual track pointers inside are cache-owned (do not free them).
 *
 * @param cache Library cache
 * @param album_id Album ID
 * @param library_mask Bitmask of enabled libraries (LIBRARY_MASK_ALL = all)
 * @return GPtrArray of library_track_info_t* (caller owns array, cache owns items) or NULL
 */
GPtrArray *
library_cache_get_tracks_by_album(library_cache_t *cache, int64_t album_id, uint32_t library_mask);

/**
 * Get albums by artist.
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual album pointers inside are cache-owned (do not free them).
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @param library_mask Bitmask of enabled libraries (LIBRARY_MASK_ALL = all)
 * @return GPtrArray of library_album_info_t* (caller owns array, cache owns items) or NULL
 */
GPtrArray *library_cache_get_albums_by_artist(library_cache_t *cache,
                                              int64_t artist_id,
                                              uint32_t library_mask);

/* =============================================================================
 * "Appears On" Queries (Featured Artist Detection)
 * ============================================================================= */

/**
 * Get albums where artist appears as track artist but not album artist.
 * Used for "Appears On" section in artist detail views.
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual album pointers inside are cache-owned (do not free them).
 *
 * Example: If artist "Norah Jones" has tracks on "Ray Charles - Genius Loves Company",
 * that album would appear in Norah Jones's "Appears On" list.
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @param library_mask Bitmask of enabled libraries (LIBRARY_MASK_ALL = all)
 * @return GPtrArray of library_album_info_t* (caller owns array, cache owns items) or NULL
 */
GPtrArray *library_cache_get_artist_appearances(library_cache_t *cache,
                                                int64_t artist_id,
                                                uint32_t library_mask);

/**
 * Get tracks where artist appears on albums by other artists.
 * Returns the specific tracks this artist contributed to other artists' albums.
 * Sorted by album title, disc number, track number.
 * Returns a caller-owned GPtrArray (caller must g_ptr_array_unref).
 * Individual track pointers inside are cache-owned (do not free them).
 *
 * @param cache Library cache
 * @param artist_id Artist ID
 * @param library_mask Bitmask of enabled libraries (LIBRARY_MASK_ALL = all)
 * @return GPtrArray of library_track_info_t* (caller owns array, cache owns items) or NULL
 */
GPtrArray *library_cache_get_artist_appearance_tracks(library_cache_t *cache,
                                                      int64_t artist_id,
                                                      uint32_t library_mask);

/**
 * Get merged artist counts across libraries, computed on demand.
 *
 * @param album_count  Own albums (MBRID-deduped across libraries)
 * @param appearance_count  Tracks on OTHER artists' albums (excludes own albums,
 *                          deduped by MBRID+disc+track across libraries)
 */
void library_cache_get_merged_artist_counts(library_cache_t *cache,
                                            int64_t artist_id,
                                            uint32_t library_mask,
                                            uint32_t *album_count,
                                            uint32_t *appearance_count);

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
GPtrArray *library_cache_get_artists_filtered(library_cache_t *cache,
                                              library_sort_t sort,
                                              const char *search_text,
                                              const db_search_opts_t *filters,
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
GPtrArray *library_cache_get_albums_filtered(library_cache_t *cache,
                                             library_sort_t sort,
                                             const char *search_text,
                                             const db_search_opts_t *filters,
                                             uint32_t library_mask);

/* Search API (library_cache_search, library_credit_search) is declared in
 * library_search.h. */

/* =============================================================================
 * Prefetch API (Kernel Page Cache Hints)
 * ============================================================================= */

/**
 * Prefetch audio files into kernel page cache.
 * Resolves track_ids -> file paths, prefetches each file.
 * Called by AudioCache for visible search results / track lists.
 *
 * @param cache Library cache
 * @param track_ids Array of track IDs
 * @param count Number of track IDs
 */
void
library_cache_prefetch_audio_files(library_cache_t *cache, const int64_t *track_ids, size_t count);

/* =============================================================================
 * Cache Warming
 * ============================================================================= */

typedef enum {
    LIBRARY_CACHE_IDLE = 0,
    LIBRARY_CACHE_WARMING = 1,
    LIBRARY_CACHE_READY = 2,
    LIBRARY_CACHE_REFRESHING = 3, /* READY + COW refresh in progress; old data still live */
} library_cache_state_t;

/**
 * Callback invoked on the main thread (via g_idle_add) when warming completes.
 */
typedef void (*library_cache_ready_cb)(void *user_data);

/**
 * Set the callback for when cache warming completes.
 */
void library_cache_set_ready_callback(library_cache_t *cache,
                                      library_cache_ready_cb cb,
                                      void *user_data);

/**
 * Start background warming thread for ALL idle library slots.
 * Safe to call multiple times -- individual slots are no-ops if already
 * warming or ready.
 */
void library_cache_start_warming(library_cache_t *cache);

/**
 * Warm a specific slot and block until it is fully populated.
 * For testing and CLI tools that need synchronous cache population.
 * Other slots remain unaffected — the UI can display data from any
 * slot that has reached READY independently.
 */
void library_cache_warm_slot_blocking(library_cache_t *cache, int bitmap_index);

/**
 * Warm a specific library slot. No-op if slot is already warming or ready.
 * The ready callback fires on the main thread when this slot's warming
 * completes.
 *
 * @param bitmap_index  Stable library ID
 */
void library_cache_warm_slot(library_cache_t *cache, int bitmap_index);

/**
 * Block until a slot's active background thread (warming or COW refresh)
 * completes. No-op if no thread is running. Does NOT cancel the thread —
 * just waits for natural completion.
 *
 * Useful for tests and CLI tools that need synchronous refresh.
 *
 * @param bitmap_index  Stable library ID
 */
void library_cache_await_slot(library_cache_t *cache, int bitmap_index);

/* =============================================================================
 * Cache Management
 * ============================================================================= */

/**
 * Clear all cached data across ALL slots and cancel all warming threads.
 * Joins warming threads before clearing.
 *
 * @param cache Library cache
 */
void library_cache_clear(library_cache_t *cache);

/**
 * Clear a specific library slot and reset it to IDLE.
 * Cancels any active warming thread for this slot.
 * Cross-library artist merging is rebuilt to remove the cleared slot's
 * stale merge state.
 *
 * @param bitmap_index  Stable library ID
 */
void library_cache_clear_slot(library_cache_t *cache, int bitmap_index);

/**
 * COW refresh: build a new version of a library slot from its DB, sharing
 * unchanged entities with the old slot via atomic refcounting.
 *
 * Flow:
 *   1. SEED — new slot copies all entity pointers from old slot (rc bump)
 *   2. DELTA — re-read changed albums/tracks/artists from DB, replace in new
 *   3. REBUILD — reconstruct relationship arrays in new slot
 *   4. SWAP — atomic bitmap_map pointer update
 *   5. DRAIN — release old slot (shared entities survive, stale ones freed)
 *
 * The ready callback fires on the main thread when complete (same as warming).
 *
 * @param cache Library cache
 * @param bitmap_index Stable library ID to refresh
 * @param changes Changeset from the indexer (NULL = full rebuild, always safe).
 *                The callee does NOT take ownership; it reads the arrays and
 *                they may be freed as soon as the call returns.
 */
void library_cache_refresh_slot(library_cache_t *cache,
                                int bitmap_index,
                                const library_cache_changeset_t *changes);

/* =============================================================================
 * Multi-Library Accessors
 * ============================================================================= */

/**
 * Get the number of library slots in this cache.
 * Returns the actual slot_count set at creation time.
 */
int library_cache_get_library_count(library_cache_t *cache);

/**
 * Get the bitmap_index (stable library ID) for a slot at a given position.
 * Useful for iterating: for (int i = 0; i < count; i++) { int bi = ..._get_bitmap_index(cache, i); }
 *
 * @param slot_position 0-based position in the internal slots array
 * @return bitmap_index, or -1 if out of range
 */
int library_cache_get_bitmap_index(library_cache_t *cache, int slot_position);

/**
 * Estimate memory bytes used by a single library slot's entity arrays.
 * O(1) computation from pre-tracked capacity × sizeof — suitable for
 * performance dashboard polling at 1-10Hz.
 *
 * @param cache Library cache
 * @param bitmap_index Stable library ID
 * @return Estimated bytes, or 0 if index invalid or slot is IDLE
 */
size_t library_cache_get_slot_memory_bytes(library_cache_t *cache, int bitmap_index);

/**
 * Readonly DB handles for a library slot. All pointers are cache-owned —
 * caller must NOT close them. Any field may be NULL if the DB doesn't exist.
 */
typedef struct {
    quadrature_db_t *db;
    quadrature_meta_db_t *meta;
    quadrature_bios_db_t *bios;
} library_cache_dbs_t;

/**
 * Get all cached readonly DB handles for a library slot in one lookup.
 * Returns zeroed struct if bitmap_index is invalid.
 */
library_cache_dbs_t library_cache_get_dbs(library_cache_t *cache, int bitmap_index);

/**
 * Get the display name for a library slot.
 * Returns the configured display name, or the basename of the library path
 * if none was configured.
 *
 * @param cache Library cache
 * @param bitmap_index Stable library ID
 * @return Static string owned by the cache; do not free. NULL if index invalid.
 */
const char *library_cache_get_library_name(library_cache_t *cache, int bitmap_index);

/**
 * Get all library bitmap indices where an artist appears (via MBID index).
 * Falls back to the source library if the artist has no MBID.
 *
 * @param out_libs Caller-owned array to receive library indices
 * @param max_libs Capacity of out_libs
 * @return Number of library indices written (0 if entity not found)
 */
int library_cache_get_artist_libraries(library_cache_t *cache,
                                       int64_t artist_global_id,
                                       int *out_libs,
                                       int max_libs);

/**
 * Get all library bitmap indices where an album appears (via MBID index,
 * keyed on release-group ID for cross-edition dedup).
 * Falls back to the source library if the album has no release-group MBID.
 *
 * @param out_libs Caller-owned array to receive library indices
 * @param max_libs Capacity of out_libs
 * @return Number of library indices written (0 if entity not found)
 */
int library_cache_get_album_libraries(library_cache_t *cache,
                                      int64_t album_global_id,
                                      int *out_libs,
                                      int max_libs);

/**
 * Get all library bitmap indices that contain the "same" track as the given
 * one — identified by (release_group MBID, disc_num, track_num). Requires the
 * track's album to have a resolved musicbrainz_release_group_id; otherwise
 * falls back to the source library only.
 *
 * Strict match: a library is included only if a track with the matching
 * (disc, track) tuple actually exists there — a release in the same release
 * group missing the track (e.g. deluxe bonus cut) will not be badged.
 *
 * @return Number of library indices written (0 if entity not found)
 */
int library_cache_get_track_libraries(library_cache_t *cache,
                                      int64_t track_global_id,
                                      int *out_libs,
                                      int max_libs);

/**
 * Update the display name for a library slot.
 *
 * Replaces the current display_name. Pass NULL to revert to basename fallback.
 * The cache takes ownership of a copy of @p name.
 *
 * @param cache  Library cache
 * @param bitmap_index  Stable library ID
 * @param name  New display name, or NULL
 */
void library_cache_set_library_name(library_cache_t *cache, int bitmap_index, const char *name);

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
void library_cache_set_available(library_cache_t *cache, int bitmap_index, gboolean available);

/**
 * Get availability for a library slot.
 * Thread-safe (atomic load).
 */
gboolean library_cache_get_available(library_cache_t *cache, int bitmap_index);

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_LIBRARY_H */
