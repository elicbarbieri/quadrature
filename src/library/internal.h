/**
 * Private shared state for src/library/ (.c files).
 *
 * Owned by library_cache.c; cache_search.c and library_search.c consume it
 * for FTS and credit-search queries. Never include from outside src/library/.
 *
 * Exposes only what the sibling search files need:
 *   - LibrarySlot layout (to iterate entity arrays by local id)
 *   - library_cache layout (slots + slot_count + vocab table)
 *   - slot_get_* fast accessors
 *   - mask_is_multi_library / build_corrected_query helpers
 *
 * Everything else (warming thread, MBID indices, prefetch) stays file-static
 * in library_cache.c.
 */

#ifndef QUADRATURE_LIBRARY_INTERNAL_H
#define QUADRATURE_LIBRARY_INTERNAL_H

#include "quadrature/library.h"
#include "quadrature/database.h"
#include "quadrature/metadata.h"

#include <glib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* =============================================================================
 * LibrarySlot — per-library state (owned by library_cache.c)
 * ============================================================================= */

typedef struct library_cache LibraryCachePriv;  /* avoid forward-decl loop */

typedef struct {
    int    lib_idx;
    int    bitmap_index;
    char  *db_path;
    char  *music_base;
    char  *display_name;

    library_artist_info_t **artists;
    size_t                  artists_capacity;
    library_album_info_t  **albums;
    size_t                  albums_capacity;
    library_track_info_t  **tracks;
    size_t                  tracks_capacity;

    GArray    **album_tracks;
    size_t      album_tracks_capacity;
    GPtrArray **track_artists;
    size_t      track_artists_capacity;
    GPtrArray **artist_albums;
    size_t      artist_albums_capacity;
    GPtrArray **artist_appearances;
    size_t      artist_appearances_capacity;
    GPtrArray **artist_appearance_tracks;
    size_t      artist_appearance_tracks_capacity;
    GPtrArray **album_tracks_ptrs;
    size_t      album_tracks_ptrs_capacity;

    quadrature_db_t *db;
    quadrature_db_t *db_warm;

    quadrature_meta_db_t *meta_db;
    quadrature_bios_db_t *bios_db;

    GThread    *warm_thread;
    atomic_int  warm_cancel;
    atomic_int  warm_state;

    atomic_bool available;

    LibraryCachePriv *cache;
} LibrarySlot;

/* =============================================================================
 * struct library_cache — only the fields needed outside library_cache.c
 * The remaining fields (MBID indices, prefetch, lock, ready_cb) live as file-
 * scope additions via the struct's full definition in library_cache.c. We
 * replicate the full struct here because C requires a complete type to access
 * any member.
 * ============================================================================= */

struct mbid_artist_entry {
    char mbid[37];
    int64_t *global_ids;
    uint8_t count;
};

struct mbrid_album_entry {
    char mbrid[37];
    int64_t *global_ids;
    uint8_t count;
};

struct library_cache {
    LibrarySlot  *slots;
    int           slot_count;

    LibrarySlot **bitmap_map;
    int           bitmap_capacity;

    char        **search_vocab;
    size_t        search_vocab_count;

    library_cache_ready_cb ready_cb;
    void                  *ready_cb_data;

    GMutex lock;

    struct {
        struct mbid_artist_entry *entries;
        size_t count;
    } mbid_artist_index;

    struct {
        struct mbrid_album_entry *entries;
        size_t count;
    } mbrid_album_index;

    GAsyncQueue *prefetch_queue;
    GThread     *prefetch_thread;
    atomic_int   prefetch_shutdown;
};

/* =============================================================================
 * Flat-array accessors (inline; safe across TUs)
 * ============================================================================= */

static inline library_artist_info_t *slot_get_artist(LibrarySlot *slot, int64_t local_id) {
    if (local_id <= 0 || (size_t)local_id >= slot->artists_capacity) return NULL;
    return slot->artists[local_id];
}

static inline library_album_info_t *slot_get_album(LibrarySlot *slot, int64_t local_id) {
    if (local_id <= 0 || (size_t)local_id >= slot->albums_capacity) return NULL;
    return slot->albums[local_id];
}

static inline library_track_info_t *slot_get_track(LibrarySlot *slot, int64_t local_id) {
    if (local_id <= 0 || (size_t)local_id >= slot->tracks_capacity) return NULL;
    return slot->tracks[local_id];
}

/* =============================================================================
 * Helpers exposed from library_cache.c to sibling search files
 * ============================================================================= */

/** True when the mask selects ≥2 available slots (controls dedup tables). */
bool library_mask_is_multi_library(library_cache_t *cache, uint32_t mask);

/** Best-effort single-token corrections using the cache's merged vocabulary.
 *  Caller owns the returned string (g_free). NULL on allocation failure. */
char *library_build_corrected_query(library_cache_t *cache, const char *query);

#endif /* QUADRATURE_LIBRARY_INTERNAL_H */
