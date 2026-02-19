/**
 * Library Cache Implementation
 *
 * Multi-library foundation layer for all library data access.  Each library
 * root occupies a LibrarySlot with its own flat arrays indexed by LOCAL entity
 * ID.  All IDs exposed to callers are GLOBAL IDs encoded via
 * LIBRARY_MAKE_GLOBAL_ID(lib_idx, local_id).  Library 0 global IDs are
 * identical to their local DB IDs, preserving full backward compatibility for
 * single-library use.
 *
 * Background warming: each slot gets its own warming thread.  After a slot
 * finishes warming, rebuild_merged_artists() merges MBID-matched artists
 * across warmed slots, then fires the ready callback on the main thread.
 */

#define G_LOG_DOMAIN "quadrature"

#include "quadrature/library.h"
#include "quadrature/database.h"

#include <sqlite3.h>
#include <stdlib.h>

/* Forward declarations for helpers defined after the warming thread */
static void build_search_vocab_slot(struct library_cache *cache, int slot_idx);
static const char *correct_token(struct library_cache *cache, const char *token);
static char *build_corrected_query(struct library_cache *cache, const char *query);
static void rebuild_merged_artists(struct library_cache *cache);

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdatomic.h>
#include <glib.h>

#define WARM_PAGE_SIZE 1000

/* =============================================================================
 * LibrarySlot — per-library state
 * ============================================================================= */

typedef struct library_cache LibraryCachePriv;  /* avoid forward-decl loop */

typedef struct {
    int    lib_idx;         /* 0-based; used for LIBRARY_MAKE_GLOBAL_ID */
    char  *db_path;
    char  *music_base;
    char  *display_name;

    /* Entity arrays (indexed by LOCAL id — O(1) lookup) */
    library_artist_info_t **artists;
    size_t                  artists_capacity;
    library_album_info_t  **albums;
    size_t                  albums_capacity;
    library_track_info_t  **tracks;
    size_t                  tracks_capacity;

    /* Relationship arrays (indexed by LOCAL id) */
    GArray    **album_tracks;           /* album_tracks[local_album_id] → GArray<int64_t global track ids> */
    size_t      album_tracks_capacity;
    GPtrArray **track_artists;          /* track_artists[local_track_id] → GPtrArray<library_track_artist_t*> */
    size_t      track_artists_capacity;
    GPtrArray **artist_albums;          /* artist_albums[local_artist_id] → GPtrArray<library_album_info_t*> */
    size_t      artist_albums_capacity;
    GPtrArray **artist_appearances;
    size_t      artist_appearances_capacity;
    GPtrArray **artist_appearance_tracks;
    size_t      artist_appearance_tracks_capacity;
    GPtrArray **album_tracks_ptrs;      /* cached GPtrArray<library_track_info_t*> results */
    size_t      album_tracks_ptrs_capacity;

    /* DB connections */
    quadrature_db_t *db;        /* UI readonly — main thread only */
    quadrature_db_t *db_warm;   /* warming thread only */

    /* Per-slot warming state */
    GThread    *warm_thread;
    atomic_int  warm_cancel;
    atomic_int  warm_state;     /* LIBRARY_CACHE_IDLE / WARMING / READY */

    /* Backpointer */
    LibraryCachePriv *cache;
} LibrarySlot;

/* =============================================================================
 * struct library_cache
 * ============================================================================= */

struct library_cache {
    LibrarySlot  *slots;
    int           slot_count;

    /* Merged search vocabulary */
    char        **search_vocab;
    size_t        search_vocab_count;

    library_cache_ready_cb ready_cb;
    void                  *ready_cb_data;

    GMutex lock;  /* Covers all slot arrays + merged lists */
};

/* =============================================================================
 * Memory Management Helpers
 * ============================================================================= */

static void free_artist_info(library_artist_info_t *info) {
    if (!info) return;
    g_free(info->name);
    g_free(info->musicbrainz_id);
    g_free(info->merged_source_ids);
    g_free(info);
}

static void free_album_info(library_album_info_t *info) {
    if (!info) return;
    g_free(info->title);
    g_free(info->artist_name);
    g_free(info->path);
    g_free(info->genres);
    g_free(info->musicbrainz_release_id);
    g_free(info);
}

static void free_track_info(library_track_info_t *info) {
    if (!info) return;
    g_free(info->path);
    g_free(info->title);
    g_free(info->artist_display);
    g_free(info->album_title);
    g_free(info->genre);
    g_free(info);
}

static void free_track_artist(gpointer data) {
    library_track_artist_t *artist = (library_track_artist_t *)data;
    if (!artist) return;
    g_free(artist->name);
    g_free(artist->join_phrase);
    g_free(artist);
}

/* =============================================================================
 * Slot Flat-Array Helpers
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

static inline void slot_set_artist(LibrarySlot *slot, int64_t local_id, library_artist_info_t *info) {
    if (local_id <= 0 || (size_t)local_id >= slot->artists_capacity) return;
    slot->artists[local_id] = info;
}

static inline void slot_set_album(LibrarySlot *slot, int64_t local_id, library_album_info_t *info) {
    if (local_id <= 0 || (size_t)local_id >= slot->albums_capacity) return;
    slot->albums[local_id] = info;
}

static inline void slot_set_track(LibrarySlot *slot, int64_t local_id, library_track_info_t *info) {
    if (local_id <= 0 || (size_t)local_id >= slot->tracks_capacity) return;
    slot->tracks[local_id] = info;
}

/* =============================================================================
 * Global ID Decode Helper
 * ============================================================================= */

static LibrarySlot *decode_slot(library_cache_t *cache, int64_t global_id, int64_t *local_id_out) {
    int lib_idx = LIBRARY_GLOBAL_ID_LIB(global_id);
    int64_t local_id = LIBRARY_GLOBAL_ID_LOCAL(global_id);
    if (lib_idx < 0 || lib_idx >= cache->slot_count) return NULL;
    if (local_id_out) *local_id_out = local_id;
    return &cache->slots[lib_idx];
}

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

static void prefetch_file(const char *path) {
    if (!path) return;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
#ifdef POSIX_FADV_WILLNEED
        posix_fadvise(fd, 0, st.st_size, POSIX_FADV_WILLNEED);
#endif
    }
    close(fd);
}

static char *resolve_track_path(const char *music_base,
                                const char *album_rel_path,
                                const char *track_rel_path) {
    if (!track_rel_path) return NULL;
    g_assert(track_rel_path[0] != '/');

    char *album_base = g_build_filename(music_base ? music_base : "",
                                        album_rel_path ? album_rel_path : "", NULL);
    char *full_path = g_canonicalize_filename(track_rel_path, album_base);
    g_free(album_base);
    return full_path;
}

/* Fetch track from DB and convert to cache format.
 * The returned info uses LOCAL (unencoded) IDs; caller must encode to global
 * if needed. Stores the DB's compact relative path (not resolved). */
static library_track_info_t *fetch_track_from_db(quadrature_db_t *db, int64_t local_track_id) {
    db_track_t *db_track = NULL;
    quadrature_result_t res = db_get_track(db, local_track_id, &db_track);
    if (res != QUADRATURE_OK || !db_track) return NULL;

    library_track_info_t *info = g_new0(library_track_info_t, 1);
    info->track_id  = db_track->id;          /* caller encodes to global */
    info->album_id  = db_track->album_id;    /* caller encodes to global */
    info->artist_id = db_track->artist_id;   /* caller encodes to global */
    info->path      = g_strdup(db_track->path ? db_track->path : "");
    info->title     = g_strdup(db_track->title);
    info->artist_display = g_strdup(db_track->artist_display ? db_track->artist_display : db_track->artist);
    info->album_title   = g_strdup(db_track->album);
    info->genre         = db_track->genre ? g_strdup(db_track->genre) : NULL;
    info->duration_ms   = db_track->duration_ms;
    info->track_num     = db_track->track_num;
    info->disc_num      = db_track->disc_num;
    info->year          = db_track->year;

    db_track_free(db_track);
    return info;
}

/* Cache track_artists for a track (local ids; called with lock held). */
static void cache_track_artists_for_slot_track(LibrarySlot *slot, int64_t local_track_id) {
    if (local_track_id <= 0 || (size_t)local_track_id >= slot->track_artists_capacity) return;
    if (slot->track_artists[local_track_id]) return;

    db_track_artist_t *db_artists = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_track_artists(slot->db_warm ? slot->db_warm : slot->db,
                                                   local_track_id, &db_artists, &count);
    if (res != QUADRATURE_OK || !db_artists || count == 0) {
        if (db_artists) db_track_artists_free(db_artists, count);
        return;
    }

    GPtrArray *result = g_ptr_array_new_with_free_func(free_track_artist);
    for (size_t i = 0; i < count; i++) {
        library_track_artist_t *artist = g_new0(library_track_artist_t, 1);
        /* Encode artist_id to global */
        artist->artist_id   = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_artists[i].artist_id);
        artist->name        = g_strdup(db_artists[i].name);
        artist->join_phrase = g_strdup(db_artists[i].join_phrase
                                       ? db_artists[i].join_phrase : "");
        artist->role     = (db_artists[i].position == 0)
                           ? LIBRARY_ARTIST_ROLE_PRIMARY
                           : LIBRARY_ARTIST_ROLE_FEATURING;
        artist->position = db_artists[i].position;
        g_ptr_array_add(result, artist);
    }
    db_track_artists_free(db_artists, count);

    slot->track_artists[local_track_id] = result;
}

/* Load album tracks into slot (must be called with lock held; uses slot->db). */
static void load_album_tracks_slot(LibrarySlot *slot, int64_t local_album_id) {
    if (local_album_id <= 0 || (size_t)local_album_id >= slot->album_tracks_capacity) return;
    if (slot->album_tracks[local_album_id]) return;

    db_track_t *tracks = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_tracks_by_album(slot->db, local_album_id, &tracks, &count);
    if (res != QUADRATURE_OK || !tracks || count == 0) {
        if (tracks) db_tracks_free(tracks, count);
        return;
    }

    GArray *track_ids = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), count);
    for (size_t i = 0; i < count; i++) {
        int64_t global_tid = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, tracks[i].id);
        g_array_append_val(track_ids, global_tid);
    }
    slot->album_tracks[local_album_id] = track_ids;
    db_tracks_free(tracks, count);
}

/* Convert library_sort_t to db_sort_t for DB queries */
static db_sort_t library_sort_to_db_sort(library_sort_t sort) {
    switch (sort) {
        case LIBRARY_SORT_NAME_ASC:   return DB_SORT_NAME_ASC;
        case LIBRARY_SORT_NAME_DESC:  return DB_SORT_NAME_DESC;
        case LIBRARY_SORT_YEAR_ASC:   return DB_SORT_YEAR_ASC;
        case LIBRARY_SORT_YEAR_DESC:  return DB_SORT_YEAR_DESC;
        case LIBRARY_SORT_ARTIST_ASC: return DB_SORT_ARTIST_ASC;
        case LIBRARY_SORT_RECENT:     return DB_SORT_RECENT;
        default:                      return DB_SORT_NAME_ASC;
    }
}

/* =============================================================================
 * Slot Array Freeing / Allocating
 * ============================================================================= */

static void free_slot_arrays(LibrarySlot *slot) {
    if (slot->artists) {
        for (size_t i = 0; i < slot->artists_capacity; i++)
            if (slot->artists[i]) free_artist_info(slot->artists[i]);
        g_free(slot->artists);
        slot->artists = NULL;
    }
    slot->artists_capacity = 0;

    if (slot->albums) {
        for (size_t i = 0; i < slot->albums_capacity; i++)
            if (slot->albums[i]) free_album_info(slot->albums[i]);
        g_free(slot->albums);
        slot->albums = NULL;
    }
    slot->albums_capacity = 0;

    if (slot->tracks) {
        for (size_t i = 0; i < slot->tracks_capacity; i++)
            if (slot->tracks[i]) free_track_info(slot->tracks[i]);
        g_free(slot->tracks);
        slot->tracks = NULL;
    }
    slot->tracks_capacity = 0;

    if (slot->album_tracks) {
        for (size_t i = 0; i < slot->album_tracks_capacity; i++)
            if (slot->album_tracks[i]) g_array_unref(slot->album_tracks[i]);
        g_free(slot->album_tracks);
        slot->album_tracks = NULL;
    }
    slot->album_tracks_capacity = 0;

    if (slot->track_artists) {
        for (size_t i = 0; i < slot->track_artists_capacity; i++)
            if (slot->track_artists[i]) g_ptr_array_unref(slot->track_artists[i]);
        g_free(slot->track_artists);
        slot->track_artists = NULL;
    }
    slot->track_artists_capacity = 0;

    if (slot->artist_albums) {
        for (size_t i = 0; i < slot->artist_albums_capacity; i++)
            if (slot->artist_albums[i]) g_ptr_array_unref(slot->artist_albums[i]);
        g_free(slot->artist_albums);
        slot->artist_albums = NULL;
    }
    slot->artist_albums_capacity = 0;

    if (slot->artist_appearances) {
        for (size_t i = 0; i < slot->artist_appearances_capacity; i++)
            if (slot->artist_appearances[i]) g_ptr_array_unref(slot->artist_appearances[i]);
        g_free(slot->artist_appearances);
        slot->artist_appearances = NULL;
    }
    slot->artist_appearances_capacity = 0;

    if (slot->artist_appearance_tracks) {
        for (size_t i = 0; i < slot->artist_appearance_tracks_capacity; i++)
            if (slot->artist_appearance_tracks[i]) g_ptr_array_unref(slot->artist_appearance_tracks[i]);
        g_free(slot->artist_appearance_tracks);
        slot->artist_appearance_tracks = NULL;
    }
    slot->artist_appearance_tracks_capacity = 0;

    if (slot->album_tracks_ptrs) {
        for (size_t i = 0; i < slot->album_tracks_ptrs_capacity; i++)
            if (slot->album_tracks_ptrs[i]) g_ptr_array_unref(slot->album_tracks_ptrs[i]);
        g_free(slot->album_tracks_ptrs);
        slot->album_tracks_ptrs = NULL;
    }
    slot->album_tracks_ptrs_capacity = 0;
}

/* Allocate all per-slot entity arrays using slot->db for max-ID queries. */
static void allocate_slot_arrays(LibrarySlot *slot) {
    int64_t max_artist = db_get_max_id(slot->db, "artists");
    int64_t max_album  = db_get_max_id(slot->db, "albums");
    int64_t max_track  = db_get_max_id(slot->db, "tracks");

    slot->artists_capacity = (size_t)(max_artist + 1);
    slot->albums_capacity  = (size_t)(max_album  + 1);
    slot->tracks_capacity  = (size_t)(max_track  + 1);

    slot->artists = g_new0(library_artist_info_t *, slot->artists_capacity);
    slot->albums  = g_new0(library_album_info_t *,  slot->albums_capacity);
    slot->tracks  = g_new0(library_track_info_t *,  slot->tracks_capacity);

    slot->album_tracks_capacity = slot->albums_capacity;
    slot->album_tracks = g_new0(GArray *, slot->album_tracks_capacity);

    slot->track_artists_capacity = slot->tracks_capacity;
    slot->track_artists = g_new0(GPtrArray *, slot->track_artists_capacity);

    slot->artist_albums_capacity = slot->artists_capacity;
    slot->artist_albums = g_new0(GPtrArray *, slot->artist_albums_capacity);

    slot->artist_appearances_capacity = slot->artists_capacity;
    slot->artist_appearances = g_new0(GPtrArray *, slot->artist_appearances_capacity);

    slot->artist_appearance_tracks_capacity = slot->artists_capacity;
    slot->artist_appearance_tracks = g_new0(GPtrArray *, slot->artist_appearance_tracks_capacity);

    slot->album_tracks_ptrs_capacity = slot->albums_capacity;
    slot->album_tracks_ptrs = g_new0(GPtrArray *, slot->album_tracks_ptrs_capacity);
}

/* =============================================================================
 * Warming Thread
 * ============================================================================= */

static void cancel_and_join_slot_warming(LibrarySlot *slot) {
    if (slot->warm_thread) {
        atomic_store(&slot->warm_cancel, 1);
        g_thread_join(slot->warm_thread);
        slot->warm_thread = NULL;
        atomic_store(&slot->warm_cancel, 0);
    }
}

/* Fired on main thread after a slot finishes warming */
static gboolean warming_complete_idle(gpointer data) {
    library_cache_t *cache = (library_cache_t *)data;
    if (cache->ready_cb) {
        cache->ready_cb(cache->ready_cb_data);
    }
    return G_SOURCE_REMOVE;
}

static gpointer slot_warming_thread_func(gpointer data) {
    LibrarySlot *slot  = (LibrarySlot *)data;
    library_cache_t *cache = slot->cache;

    g_debug("cache warming [slot %d]: started", slot->lib_idx);
    gint64 start_time = g_get_monotonic_time();

    /* ── Phase 1: Page artists ────────────────────────────────────────────── */
    {
        size_t offset = 0;

        for (;;) {
            if (atomic_load(&slot->warm_cancel)) goto done;

            db_page_opts_t opts = {
                .offset = offset,
                .limit  = WARM_PAGE_SIZE,
                .sort   = DB_SORT_NAME_ASC,
            };

            db_artist_t *db_artists = NULL;
            size_t count = 0, total = 0;
            quadrature_result_t res = db_get_artists_page(slot->db_warm, &opts,
                                                          &db_artists, &count, &total);
            if (res != QUADRATURE_OK || count == 0) break;

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                int64_t local_id = db_artists[i].id;
                if (!slot_get_artist(slot, local_id)) {
                    library_artist_info_t *info = g_new0(library_artist_info_t, 1);
                    info->artist_id         = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, local_id);
                    info->library_index     = slot->lib_idx;
                    info->name              = g_strdup(db_artists[i].name);
                    info->musicbrainz_id    = g_strdup(db_artists[i].musicbrainz_id);
                    info->album_count       = (uint32_t)db_artists[i].album_count;
                    info->track_count       = (uint32_t)db_artists[i].track_count;
                    slot_set_artist(slot, local_id, info);
                }
            }
            g_mutex_unlock(&cache->lock);

            db_artists_free(db_artists, count);
            offset += count;
            if (count < WARM_PAGE_SIZE) break;
        }

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_debug("cache warming [slot %d]: phase 1 (artists) done in %.1f ms",
                slot->lib_idx, phase_elapsed / 1000.0);
    }

    /* ── Phase 2: Page albums ─────────────────────────────────────────────── */
    {
        size_t offset = 0;

        for (;;) {
            if (atomic_load(&slot->warm_cancel)) goto done;

            db_page_opts_t opts = {
                .offset = offset,
                .limit  = WARM_PAGE_SIZE,
                .sort   = DB_SORT_NAME_ASC,
            };

            db_album_t *db_albums = NULL;
            size_t count = 0, total = 0;
            quadrature_result_t res = db_get_albums_page(slot->db_warm, &opts,
                                                         &db_albums, &count, &total);
            if (res != QUADRATURE_OK || count == 0) break;

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                int64_t local_id = db_albums[i].id;
                if (!slot_get_album(slot, local_id)) {
                    library_album_info_t *info = g_new0(library_album_info_t, 1);
                    info->album_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, local_id);
                    info->artist_id     = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_albums[i].artist_id);
                    info->library_index = slot->lib_idx;
                    info->title         = g_strdup(db_albums[i].title);
                    info->artist_name   = g_strdup(db_albums[i].artist_name);
                    info->path          = g_strdup(db_albums[i].path ? db_albums[i].path : "");
                    info->genres        = db_albums[i].genres ? g_strdup(db_albums[i].genres) : NULL;
                    info->year          = db_albums[i].year;
                    info->track_count   = (uint16_t)db_albums[i].track_count;
                    info->musicbrainz_release_id = db_albums[i].musicbrainz_release_id
                                                 ? g_strdup(db_albums[i].musicbrainz_release_id) : NULL;
                    slot_set_album(slot, local_id, info);
                }
            }
            g_mutex_unlock(&cache->lock);

            db_albums_free(db_albums, count);
            offset += count;
            if (count < WARM_PAGE_SIZE) break;
        }

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_debug("cache warming [slot %d]: phase 2 (albums) done in %.1f ms",
                slot->lib_idx, phase_elapsed / 1000.0);
    }

    /* ── Phase 3: Load tracks per album ───────────────────────────────────── */
    {
        size_t tracks_loaded = 0;

        for (size_t local_album_id = 1; local_album_id < slot->albums_capacity; local_album_id++) {
            if (atomic_load(&slot->warm_cancel)) goto done;

            library_album_info_t *album = slot_get_album(slot, (int64_t)local_album_id);
            if (!album) continue;

            db_track_t *db_tracks = NULL;
            size_t count = 0;
            quadrature_result_t res = db_get_tracks_by_album(slot->db_warm,
                                                              (int64_t)local_album_id,
                                                              &db_tracks, &count);
            if (res != QUADRATURE_OK || !db_tracks || count == 0) continue;

            g_mutex_lock(&cache->lock);

            /* album_tracks stores GLOBAL track IDs */
            GArray *track_ids = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), count);

            for (size_t i = 0; i < count; i++) {
                int64_t local_tid  = db_tracks[i].id;
                int64_t global_tid = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, local_tid);
                g_array_append_val(track_ids, global_tid);

                if (!slot_get_track(slot, local_tid)) {
                    library_track_info_t *info = g_new0(library_track_info_t, 1);
                    info->track_id      = global_tid;
                    info->album_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_tracks[i].album_id);
                    info->artist_id     = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_tracks[i].artist_id);
                    info->library_index = slot->lib_idx;
                    info->path          = g_strdup(db_tracks[i].path ? db_tracks[i].path : "");
                    info->title         = g_strdup(db_tracks[i].title);
                    info->artist_display = g_strdup(db_tracks[i].artist_display
                                         ? db_tracks[i].artist_display : db_tracks[i].artist);
                    info->album_title   = g_strdup(db_tracks[i].album);
                    info->genre         = db_tracks[i].genre ? g_strdup(db_tracks[i].genre) : NULL;
                    info->duration_ms   = db_tracks[i].duration_ms;
                    info->track_num     = db_tracks[i].track_num;
                    info->disc_num      = db_tracks[i].disc_num;
                    info->year          = db_tracks[i].year;
                    slot_set_track(slot, local_tid, info);
                    tracks_loaded++;
                }
            }

            if (local_album_id < slot->album_tracks_capacity) {
                if (slot->album_tracks[local_album_id])
                    g_array_unref(slot->album_tracks[local_album_id]);
                slot->album_tracks[local_album_id] = track_ids;
            } else {
                g_array_unref(track_ids);
            }

            g_mutex_unlock(&cache->lock);

            db_tracks_free(db_tracks, count);
        }

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_debug("cache warming [slot %d]: phase 3 (tracks) done — %zu tracks in %.1f ms",
                slot->lib_idx, tracks_loaded, phase_elapsed / 1000.0);
    }

    /* ── Phase 3.5: Cache track artists ──────────────────────────────────── */
    {
        for (size_t tid = 1; tid < slot->tracks_capacity; tid++) {
            if (atomic_load(&slot->warm_cancel)) goto done;
            if (!slot_get_track(slot, (int64_t)tid)) continue;

            g_mutex_lock(&cache->lock);
            cache_track_artists_for_slot_track(slot, (int64_t)tid);
            g_mutex_unlock(&cache->lock);
        }

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_debug("cache warming [slot %d]: phase 3.5 (track artists) done in %.1f ms",
                slot->lib_idx, phase_elapsed / 1000.0);
    }

    /* ── Phase 4: Compute aggregates and relationships ────────────────────── */
    {
        g_mutex_lock(&cache->lock);

        /* Snapshot which artists already have albums loaded (by on-demand fallback) */
        GHashTable *preloaded_artists = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (size_t aid = 1; aid < slot->artist_albums_capacity; aid++) {
            if (slot->artist_albums[aid] && slot->artist_albums[aid]->len > 0)
                g_hash_table_add(preloaded_artists, GSIZE_TO_POINTER(aid));
        }

        /* Build artist_albums relationships */
        for (size_t local_album_id = 1; local_album_id < slot->albums_capacity; local_album_id++) {
            library_album_info_t *album = slot_get_album(slot, (int64_t)local_album_id);
            if (!album) continue;

            /* Decode local artist id from global artist_id stored in album */
            int64_t local_aid = LIBRARY_GLOBAL_ID_LOCAL(album->artist_id);
            if (local_aid <= 0 || (size_t)local_aid >= slot->artist_albums_capacity) continue;

            if (g_hash_table_contains(preloaded_artists, GSIZE_TO_POINTER((gsize)local_aid)))
                continue;

            if (!slot->artist_albums[local_aid])
                slot->artist_albums[local_aid] = g_ptr_array_new();
            g_ptr_array_add(slot->artist_albums[local_aid], album);
        }
        g_hash_table_destroy(preloaded_artists);

        /* Ensure all cached artists have an entry */
        for (size_t aid = 1; aid < slot->artists_capacity; aid++) {
            if (slot_get_artist(slot, (int64_t)aid) &&
                aid < slot->artist_albums_capacity && !slot->artist_albums[aid]) {
                slot->artist_albums[aid] = g_ptr_array_new();
            }
        }

        /* Snapshot which artists already have appearances loaded */
        GHashTable *preloaded_appearances = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (size_t aid = 1; aid < slot->artist_appearances_capacity; aid++) {
            if (slot->artist_appearances[aid] && slot->artist_appearances[aid]->len > 0)
                g_hash_table_add(preloaded_appearances, GSIZE_TO_POINTER(aid));
        }

        /* Compute artist aggregates and "appears on" from tracks */
        for (size_t tid = 1; tid < slot->tracks_capacity; tid++) {
            library_track_info_t *track = slot_get_track(slot, (int64_t)tid);
            if (!track) continue;

            /* "Appears on" — check track_artists for non-primary artists */
            if ((size_t)tid < slot->track_artists_capacity && slot->track_artists[tid]) {
                GPtrArray *ta = slot->track_artists[tid];
                int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
                library_album_info_t *album = slot_get_album(slot, local_album_id);
                if (!album) continue;

                int64_t album_local_artist = LIBRARY_GLOBAL_ID_LOCAL(album->artist_id);

                for (guint j = 0; j < ta->len; j++) {
                    library_track_artist_t *credit = g_ptr_array_index(ta, j);
                    int64_t credit_local_aid = LIBRARY_GLOBAL_ID_LOCAL(credit->artist_id);

                    if (credit_local_aid == album_local_artist) continue;
                    if (credit_local_aid <= 0 ||
                        (size_t)credit_local_aid >= slot->artist_appearances_capacity) continue;

                    if (g_hash_table_contains(preloaded_appearances,
                                              GSIZE_TO_POINTER((gsize)credit_local_aid)))
                        continue;

                    if (!slot->artist_appearances[credit_local_aid])
                        slot->artist_appearances[credit_local_aid] = g_ptr_array_new();

                    GPtrArray *app_albums = slot->artist_appearances[credit_local_aid];
                    gboolean found = FALSE;
                    for (guint k = 0; k < app_albums->len; k++) {
                        if (g_ptr_array_index(app_albums, k) == album) {
                            found = TRUE;
                            break;
                        }
                    }
                    if (!found) g_ptr_array_add(app_albums, album);
                }
            }
        }
        g_hash_table_destroy(preloaded_appearances);

        g_mutex_unlock(&cache->lock);

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_debug("cache warming [slot %d]: phase 4 (aggregates) done in %.1f ms",
                slot->lib_idx, phase_elapsed / 1000.0);
    }

    /* ── Phase 5: Build search vocabulary ────────────────────────────────── */
    build_search_vocab_slot(cache, slot->lib_idx);

    /* ── Phase 6: Merge cross-library artists and signal completion ─────── */
    g_mutex_lock(&cache->lock);
    rebuild_merged_artists(cache);
    g_mutex_unlock(&cache->lock);

    atomic_store(&slot->warm_state, LIBRARY_CACHE_READY);

    if (cache->ready_cb) {
        g_idle_add(warming_complete_idle, cache);
    }

    {
        gint64 total_elapsed = g_get_monotonic_time() - start_time;
        size_t n_artists = 0, n_albums = 0, n_tracks = 0;
        for (size_t i = 1; i < slot->artists_capacity; i++)
            if (slot_get_artist(slot, (int64_t)i)) n_artists++;
        for (size_t i = 1; i < slot->albums_capacity; i++)
            if (slot_get_album(slot, (int64_t)i)) n_albums++;
        for (size_t i = 1; i < slot->tracks_capacity; i++)
            if (slot_get_track(slot, (int64_t)i)) n_tracks++;
        g_message("cache warming [slot %d]: ready — %zu artists, %zu albums, %zu tracks in %.1f ms",
                  slot->lib_idx, n_artists, n_albums, n_tracks, total_elapsed / 1000.0);
    }
    return NULL;

done:
    g_info("cache warming [slot %d]: cancelled", slot->lib_idx);
    return NULL;
}

/* =============================================================================
 * Cross-Library Artist Merging (must be called with cache->lock held)
 * ============================================================================= */

static void rebuild_merged_artists(library_cache_t *cache) {
    /* MBID → representative artist pointer.  No key/value free needed: both
     * keys and values are pointers into the slot arrays (cache owns them). */
    GHashTable *mbid_to_rep = g_hash_table_new(g_str_hash, g_str_equal);

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        for (size_t aid = 1; aid < slot->artists_capacity; aid++) {
            library_artist_info_t *a = slot_get_artist(slot, (int64_t)aid);
            if (!a) continue;

            /* Reset any merge state from a previous rebuild */
            g_free(a->merged_source_ids);
            a->merged_source_ids  = NULL;
            a->merged_source_count = 0;
            a->library_index      = slot->lib_idx;

            if (!a->musicbrainz_id)
                continue;

            library_artist_info_t *rep =
                g_hash_table_lookup(mbid_to_rep, a->musicbrainz_id);
            if (!rep) {
                g_hash_table_insert(mbid_to_rep, a->musicbrainz_id, a);
            } else {
                /* Merge into representative: accumulate counts */
                rep->track_count       += a->track_count;
                rep->album_count       += a->album_count;
                rep->merged_source_count++;
                rep->merged_source_ids = g_realloc(rep->merged_source_ids,
                    (size_t)rep->merged_source_count * sizeof(int64_t));
                rep->merged_source_ids[rep->merged_source_count - 1] = a->artist_id;
                rep->library_index = -1;

                /* Invalidate the representative's cached album list so the next
                 * library_cache_get_albums_by_artist() call rebuilds it with all
                 * merged sources included. */
                int64_t rep_local = LIBRARY_GLOBAL_ID_LOCAL(rep->artist_id);
                int     rep_lib   = LIBRARY_GLOBAL_ID_LIB(rep->artist_id);
                if (rep_lib >= 0 && rep_lib < cache->slot_count) {
                    LibrarySlot *rep_slot = &cache->slots[rep_lib];
                    if (rep_local > 0 &&
                        (size_t)rep_local < rep_slot->artist_albums_capacity &&
                        rep_slot->artist_albums[rep_local]) {
                        g_ptr_array_unref(rep_slot->artist_albums[rep_local]);
                        rep_slot->artist_albums[rep_local] = NULL;
                    }
                    if (rep_local > 0 &&
                        (size_t)rep_local < rep_slot->artist_appearances_capacity &&
                        rep_slot->artist_appearances[rep_local]) {
                        g_ptr_array_unref(rep_slot->artist_appearances[rep_local]);
                        rep_slot->artist_appearances[rep_local] = NULL;
                    }
                }
            }
        }
    }

    g_hash_table_destroy(mbid_to_rep);
}

/* =============================================================================
 * Search Vocabulary (Levenshtein typo correction)
 * ============================================================================= */

static int levenshtein(const char *a, const char *b, int max_dist) {
    int la = (int)strlen(a);
    int lb = (int)strlen(b);

    if (abs(la - lb) > max_dist) return max_dist + 1;

    int *prev = g_alloca((lb + 1) * sizeof(int));
    int *curr = g_alloca((lb + 1) * sizeof(int));

    for (int j = 0; j <= lb; j++) prev[j] = j;

    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        int row_min = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (g_ascii_tolower(a[i-1]) == g_ascii_tolower(b[j-1])) ? 0 : 1;
            curr[j] = MIN(MIN(prev[j] + 1, curr[j-1] + 1), prev[j-1] + cost);
            if (curr[j] < row_min) row_min = curr[j];
        }
        if (row_min > max_dist) return max_dist + 1;
        int *tmp = prev; prev = curr; curr = tmp;
    }
    return prev[lb];
}

static const char *correct_token(library_cache_t *cache, const char *token) {
    if (!cache->search_vocab || cache->search_vocab_count == 0) return token;

    int tok_len  = (int)strlen(token);
    int max_dist = (tok_len <= 3) ? 1 : 2;

    size_t lo = 0, hi = cache->search_vocab_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_ascii_strcasecmp(cache->search_vocab[mid], token) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }

    size_t start = (lo > 500) ? lo - 500 : 0;
    size_t end   = MIN(lo + 500, cache->search_vocab_count);

    const char *best      = token;
    int         best_dist = max_dist + 1;

    for (size_t i = start; i < end; i++) {
        int d = levenshtein(token, cache->search_vocab[i], max_dist);
        if (d < best_dist) {
            best_dist = d;
            best = cache->search_vocab[i];
            if (d == 0) break;
        }
    }

    return (best_dist <= max_dist) ? best : token;
}

static char *build_corrected_query(library_cache_t *cache, const char *query) {
    if (!query || !*query) return NULL;

    GString *out = g_string_new(NULL);
    gchar **tokens = g_strsplit_set(query, " \t\n\r", -1);

    for (int i = 0; tokens[i]; i++) {
        const gchar *tok = tokens[i];
        if (!tok || !*tok || strlen(tok) < 2) continue;

        const char *corrected = correct_token(cache, tok);
        if (out->len > 0) g_string_append_c(out, ' ');
        g_string_append(out, corrected);
    }

    g_strfreev(tokens);

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static int vocab_term_cmp(const void *a, const void *b) {
    return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}

/* Build/merge search vocabulary from one slot's db_warm connection.
 * Merges results into cache->search_vocab (under cache->lock). */
static void build_search_vocab_slot(library_cache_t *cache, int slot_idx) {
    if (slot_idx < 0 || slot_idx >= cache->slot_count) return;
    LibrarySlot *slot = &cache->slots[slot_idx];

    const char *db_path_str = db_path(slot->db_warm);
    if (!db_path_str) return;

    sqlite3 *raw_db = NULL;
    int rc = sqlite3_open_v2(db_path_str, &raw_db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) return;
    sqlite3_exec(raw_db, "PRAGMA query_only = ON;", NULL, NULL, NULL);

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Seed with existing vocab so we merge rather than replace */
    g_mutex_lock(&cache->lock);
    for (size_t i = 0; i < cache->search_vocab_count; i++) {
        if (cache->search_vocab[i] &&
            !g_hash_table_contains(seen, cache->search_vocab[i]))
            g_hash_table_insert(seen, g_strdup(cache->search_vocab[i]), NULL);
    }
    g_mutex_unlock(&cache->lock);

    static const char *vocab_sql[] = {
        "SELECT DISTINCT term FROM fts5vocab('tracks_fts', 'row')",
        "SELECT DISTINCT term FROM fts5vocab('artists_fts', 'row')",
        "SELECT DISTINCT term FROM fts5vocab('albums_fts', 'row')",
        NULL
    };

    for (int qi = 0; vocab_sql[qi]; qi++) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(raw_db, vocab_sql[qi], -1, &stmt, NULL) != SQLITE_OK)
            continue;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *term = (const char *)sqlite3_column_text(stmt, 0);
            if (term && *term && !g_hash_table_contains(seen, term))
                g_hash_table_insert(seen, g_strdup(term), NULL);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(raw_db);

    GList *keys  = g_hash_table_get_keys(seen);
    size_t count = (size_t)g_list_length(keys);
    char **vocab = g_new(char *, count);

    size_t idx = 0;
    for (GList *l = keys; l; l = l->next)
        vocab[idx++] = g_strdup((const char *)l->data);

    g_list_free(keys);
    g_hash_table_destroy(seen);

    qsort(vocab, count, sizeof(char *), vocab_term_cmp);

    g_mutex_lock(&cache->lock);
    if (cache->search_vocab) {
        for (size_t i = 0; i < cache->search_vocab_count; i++)
            g_free(cache->search_vocab[i]);
        g_free(cache->search_vocab);
    }
    cache->search_vocab       = vocab;
    cache->search_vocab_count = count;
    g_mutex_unlock(&cache->lock);

    g_debug("cache [slot %d]: search vocab built/merged, %zu unique terms",
            slot_idx, count);
}

/* =============================================================================
 * Slot Initialisation / Teardown
 * ============================================================================= */

/* Open DB connections and allocate arrays for one slot. */
static quadrature_result_t init_slot(LibrarySlot *slot,
                                     int lib_idx,
                                     const char *db_path_str,
                                     const char *music_base,
                                     const char *display_name,
                                     library_cache_t *cache) {
    slot->lib_idx     = lib_idx;
    slot->db_path     = g_strdup(db_path_str);
    slot->music_base  = music_base  ? g_strdup(music_base)  : NULL;
    slot->display_name = display_name ? g_strdup(display_name) : NULL;
    slot->cache       = cache;

    atomic_init(&slot->warm_cancel, 0);
    atomic_init(&slot->warm_state, LIBRARY_CACHE_IDLE);

    quadrature_result_t res = db_open_readonly(db_path_str, &slot->db);
    if (res != QUADRATURE_OK) {
        /* DB doesn't exist yet (first run / post-db-clean). Cache is valid but
         * queries return empty until ensure_slot_db_open() succeeds after indexing. */
        slot->db      = NULL;
        slot->db_warm = NULL;
        return QUADRATURE_OK;
    }

    res = db_open_readonly(db_path_str, &slot->db_warm);
    if (res != QUADRATURE_OK) {
        db_close(slot->db);
        slot->db      = NULL;
        slot->db_warm = NULL;
        return QUADRATURE_OK;
    }

    allocate_slot_arrays(slot);
    return QUADRATURE_OK;
}

static void destroy_slot_internals(LibrarySlot *slot) {
    cancel_and_join_slot_warming(slot);

    free_slot_arrays(slot);

    if (slot->db_warm) { db_close(slot->db_warm); slot->db_warm = NULL; }
    if (slot->db)      { db_close(slot->db);      slot->db      = NULL; }

    g_free(slot->db_path);    slot->db_path    = NULL;
    g_free(slot->music_base); slot->music_base  = NULL;
    g_free(slot->display_name); slot->display_name = NULL;
}

/* Open both DB connections for a slot whose DB was absent at init time.
 * Must be called with cache->lock held (or before warming starts).
 * No-op if connections are already open. */
static void ensure_slot_db_open(LibrarySlot *slot) {
    if (slot->db) return;  /* Already open */

    if (db_open_readonly(slot->db_path, &slot->db) != QUADRATURE_OK) {
        slot->db = NULL;
        return;
    }
    if (db_open_readonly(slot->db_path, &slot->db_warm) != QUADRATURE_OK) {
        db_close(slot->db);
        slot->db      = NULL;
        slot->db_warm = NULL;
        return;
    }
    allocate_slot_arrays(slot);
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

quadrature_result_t library_cache_create_multi(const library_cache_source_t *sources,
                                                int source_count,
                                                library_cache_t **out) {
    if (!sources || source_count <= 0 || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    library_cache_t *cache = g_new0(library_cache_t, 1);
    g_mutex_init(&cache->lock);

    cache->slot_count  = source_count;
    cache->slots       = g_new0(LibrarySlot, source_count);

    for (int i = 0; i < source_count; i++) {
        quadrature_result_t res = init_slot(&cache->slots[i],
                                            i,
                                            sources[i].db_path,
                                            sources[i].music_base,
                                            sources[i].display_name,
                                            cache);
        if (res != QUADRATURE_OK) {
            /* Clean up already-initialised slots */
            for (int j = 0; j < i; j++)
                destroy_slot_internals(&cache->slots[j]);
            g_free(cache->slots);
            g_mutex_clear(&cache->lock);
            g_free(cache);
            return res;
        }
    }

    *out = cache;
    return QUADRATURE_OK;
}

quadrature_result_t library_cache_create(const char *db_path_str,
                                         const char *music_base,
                                         library_cache_t **out) {
    if (!db_path_str || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    library_cache_source_t src = {
        .db_path      = db_path_str,
        .music_base   = music_base,
        .display_name = NULL,
    };
    return library_cache_create_multi(&src, 1, out);
}

int library_cache_get_library_count(library_cache_t *cache) {
    if (!cache) return 0;
    return cache->slot_count;
}

const char *library_cache_get_library_name(library_cache_t *cache, int library_index) {
    if (!cache || library_index < 0 || library_index >= cache->slot_count) return NULL;
    LibrarySlot *slot = &cache->slots[library_index];
    if (slot->display_name) return slot->display_name;
    if (slot->music_base) {
        const char *slash = strrchr(slot->music_base, '/');
        return slash ? slash + 1 : slot->music_base;
    }
    return slot->db_path;
}

size_t library_cache_get_slot_memory_bytes(library_cache_t *cache, int library_index) {
    if (!cache || library_index < 0 || library_index >= cache->slot_count) return 0;
    LibrarySlot *slot = &cache->slots[library_index];
    if (atomic_load(&slot->warm_state) != LIBRARY_CACHE_READY) return 0;

    /* Pointer arrays: capacity × sizeof(pointer) */
    size_t bytes = 0;
    bytes += slot->artists_capacity * sizeof(library_artist_info_t *);
    bytes += slot->albums_capacity  * sizeof(library_album_info_t *);
    bytes += slot->tracks_capacity  * sizeof(library_track_info_t *);

    /* Relationship pointer arrays */
    bytes += slot->album_tracks_capacity       * sizeof(GArray *);
    bytes += slot->track_artists_capacity      * sizeof(GPtrArray *);
    bytes += slot->artist_albums_capacity      * sizeof(GPtrArray *);
    bytes += slot->artist_appearances_capacity * sizeof(GPtrArray *);
    bytes += slot->album_tracks_ptrs_capacity  * sizeof(GPtrArray *);

    /* Estimate heap-allocated info structs (capacity is max_id+1, many slots NULL).
     * Count populated slots — iterate pointer arrays is too expensive for a perf
     * query, so use a conservative estimate: sizeof(struct) + ~80 bytes average
     * string storage per populated entity, estimated at 60% fill. */
    size_t est_artists = slot->artists_capacity * 6 / 10;
    size_t est_albums  = slot->albums_capacity  * 6 / 10;
    size_t est_tracks  = slot->tracks_capacity  * 6 / 10;

    bytes += est_artists * (sizeof(library_artist_info_t) + 80);
    bytes += est_albums  * (sizeof(library_album_info_t) + 160);
    bytes += est_tracks  * (sizeof(library_track_info_t) + 200);

    return bytes;
}

void library_cache_destroy(library_cache_t *cache) {
    if (!cache) return;

    for (int i = 0; i < cache->slot_count; i++)
        destroy_slot_internals(&cache->slots[i]);

    g_free(cache->slots);

    if (cache->search_vocab) {
        for (size_t i = 0; i < cache->search_vocab_count; i++)
            g_free(cache->search_vocab[i]);
        g_free(cache->search_vocab);
    }

    g_mutex_clear(&cache->lock);
    g_free(cache);
}

/* =============================================================================
 * Cache Warming API
 * ============================================================================= */

void library_cache_set_ready_callback(library_cache_t *cache,
                                       library_cache_ready_cb cb, void *user_data) {
    g_assert(cache != NULL);
    cache->ready_cb      = cb;
    cache->ready_cb_data = user_data;
}

void library_cache_start_warming(library_cache_t *cache) {
    g_assert(cache != NULL);
    for (int i = 0; i < cache->slot_count; i++)
        library_cache_warm_slot(cache, i);
}

void library_cache_warm_slot(library_cache_t *cache, int lib_idx) {
    g_assert(cache != NULL);
    if (lib_idx < 0 || lib_idx >= cache->slot_count) return;

    LibrarySlot *slot = &cache->slots[lib_idx];

    if (!slot->db_warm) {
        g_warning("library_cache_warm_slot[%d]: db_warm is NULL, skipping", lib_idx);
        return;  /* DB not available yet; will warm after indexer creates it */
    }

    int expected = LIBRARY_CACHE_IDLE;
    if (!atomic_compare_exchange_strong(&slot->warm_state, &expected, LIBRARY_CACHE_WARMING)) {
        g_warning("library_cache_warm_slot[%d]: warm_state not IDLE (state=%d), skipping",
                  lib_idx, atomic_load(&slot->warm_state));
        return;  /* already warming or ready */
    }

    char *thread_name = g_strdup_printf("cache-warm-%d", lib_idx);
    slot->warm_thread = g_thread_new(thread_name, slot_warming_thread_func, slot);
    g_free(thread_name);
}

/* =============================================================================
 * Cache Management
 * ============================================================================= */

void library_cache_clear(library_cache_t *cache) {
    g_assert(cache != NULL);

    /* Cancel all warming threads */
    for (int i = 0; i < cache->slot_count; i++)
        cancel_and_join_slot_warming(&cache->slots[i]);

    g_mutex_lock(&cache->lock);

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        ensure_slot_db_open(slot);          /* open DB if indexer just created it */
        free_slot_arrays(slot);
        if (slot->db) allocate_slot_arrays(slot);  /* only if DB is open */
        atomic_store(&slot->warm_state, LIBRARY_CACHE_IDLE);
    }

    g_mutex_unlock(&cache->lock);
}

void library_cache_clear_slot(library_cache_t *cache, int lib_idx) {
    g_assert(cache != NULL);
    if (lib_idx < 0 || lib_idx >= cache->slot_count) return;

    LibrarySlot *slot = &cache->slots[lib_idx];
    cancel_and_join_slot_warming(slot);

    g_mutex_lock(&cache->lock);
    ensure_slot_db_open(slot);          /* open DB if indexer just created it */
    free_slot_arrays(slot);
    if (slot->db) allocate_slot_arrays(slot);  /* only if DB is open */
    atomic_store(&slot->warm_state, LIBRARY_CACHE_IDLE);

    /* Rebuild merged lists to remove this slot's stale pointers */
    rebuild_merged_artists(cache);
    g_mutex_unlock(&cache->lock);
}

/* =============================================================================
 * On-demand DB fallbacks (called with lock held, use slot->db)
 * ============================================================================= */

/* Construct a library_album_info_t from a db_album_t, encoding global IDs. */
static library_album_info_t *album_info_from_db_album(LibrarySlot *slot,
                                                       const db_album_t *db_album) {
    library_album_info_t *info = g_new0(library_album_info_t, 1);
    info->album_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_album->id);
    info->artist_id     = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_album->artist_id);
    info->library_index = slot->lib_idx;
    info->title         = g_strdup(db_album->title);
    info->artist_name   = g_strdup(db_album->artist_name);
    info->path          = g_strdup(db_album->path ? db_album->path : "");
    info->genres        = db_album->genres ? g_strdup(db_album->genres) : NULL;
    info->year          = db_album->year;
    info->track_count   = (uint16_t)db_album->track_count;
    info->musicbrainz_release_id = db_album->musicbrainz_release_id
                                 ? g_strdup(db_album->musicbrainz_release_id) : NULL;
    return info;
}

/* Look up album in slot; if missing, construct from db_album and insert. */
static library_album_info_t *ensure_album_in_slot(LibrarySlot *slot,
                                                    const db_album_t *db_album) {
    library_album_info_t *info = slot_get_album(slot, db_album->id);
    if (!info) {
        info = album_info_from_db_album(slot, db_album);
        slot_set_album(slot, db_album->id, info);
    }
    return info;
}

static library_track_info_t *get_track_unlocked(LibrarySlot *slot,
                                                 int64_t local_track_id) {
    library_track_info_t *info = slot_get_track(slot, local_track_id);
    if (info) return info;

    /* Fetch raw track then encode IDs */
    info = fetch_track_from_db(slot->db, local_track_id);
    if (!info) return NULL;

    info->track_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, info->track_id);
    info->album_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, info->album_id);
    info->artist_id     = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, info->artist_id);
    info->library_index = slot->lib_idx;

    slot_set_track(slot, local_track_id, info);

    /* Load album tracks for navigation */
    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(info->album_id);
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity
        && !slot->album_tracks[local_album_id]) {
        load_album_tracks_slot(slot, local_album_id);
    }

    return info;
}

static library_album_info_t *get_album_unlocked(LibrarySlot *slot,
                                                 int64_t local_album_id) {
    library_album_info_t *info = slot_get_album(slot, local_album_id);
    if (info) return info;

    db_album_t *db_album = NULL;
    quadrature_result_t res = db_get_album_by_id(slot->db, local_album_id, &db_album);
    if (res != QUADRATURE_OK || !db_album) return NULL;

    info = album_info_from_db_album(slot, db_album);
    slot_set_album(slot, local_album_id, info);
    db_albums_free(db_album, 1);
    return info;
}

static library_artist_info_t *get_artist_unlocked(LibrarySlot *slot,
                                                   int64_t local_artist_id) {
    library_artist_info_t *info = slot_get_artist(slot, local_artist_id);
    if (info) return info;

    db_artist_t *artist = NULL;
    quadrature_result_t res = db_get_artist_by_id(slot->db, local_artist_id, &artist);

    if (res == QUADRATURE_OK && artist) {
        info = g_new0(library_artist_info_t, 1);
        info->artist_id         = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, local_artist_id);
        info->library_index     = slot->lib_idx;
        info->name              = g_strdup(artist->name);
        info->musicbrainz_id    = g_strdup(artist->musicbrainz_id);
        info->album_count       = (uint32_t)artist->album_count;
        info->track_count       = (uint32_t)artist->track_count;

        slot_set_artist(slot, local_artist_id, info);
        db_artists_free(artist, 1);
    }

    return info;
}

/* =============================================================================
 * Entity Getters
 * ============================================================================= */

const library_track_info_t *library_cache_get_track(library_cache_t *cache,
                                                     int64_t track_id) {
    if (!cache || track_id <= 0) return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, track_id, &local_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);
    const library_track_info_t *result = get_track_unlocked(slot, local_id);
    g_mutex_unlock(&cache->lock);

    return result;
}

const library_album_info_t *library_cache_get_album(library_cache_t *cache,
                                                     int64_t album_id) {
    if (!cache || album_id <= 0) return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, album_id, &local_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);
    library_album_info_t *info = get_album_unlocked(slot, local_id);
    g_mutex_unlock(&cache->lock);

    return info;
}

const library_artist_info_t *library_cache_get_artist(library_cache_t *cache,
                                                       int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, artist_id, &local_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);
    library_artist_info_t *info = get_artist_unlocked(slot, local_id);
    g_mutex_unlock(&cache->lock);

    return info;
}

char *library_cache_resolve_track_path(library_cache_t *cache, int64_t track_id) {
    if (!cache || track_id <= 0) return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, track_id, &local_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);
    library_track_info_t *track = get_track_unlocked(slot, local_id);
    if (!track) { g_mutex_unlock(&cache->lock); return NULL; }

    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
    library_album_info_t *album = get_album_unlocked(slot, local_album_id);
    const char *album_path = album ? album->path : NULL;

    char *full_path = resolve_track_path(slot->music_base, album_path, track->path);
    g_mutex_unlock(&cache->lock);
    return full_path;
}

const GPtrArray *library_cache_get_track_artists(library_cache_t *cache,
                                                  int64_t track_id) {
    if (!cache || track_id <= 0) return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, track_id, &local_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);

    if (local_id > 0 && (size_t)local_id < slot->track_artists_capacity
        && slot->track_artists[local_id]) {
        GPtrArray *result = slot->track_artists[local_id];
        g_mutex_unlock(&cache->lock);
        return result;
    }

    /* Cache miss — use slot->db for on-demand fetch */
    quadrature_db_t *saved_warm = slot->db_warm;
    slot->db_warm = NULL;  /* force helper to use slot->db */
    cache_track_artists_for_slot_track(slot, local_id);
    slot->db_warm = saved_warm;

    GPtrArray *result = NULL;
    if (local_id > 0 && (size_t)local_id < slot->track_artists_capacity)
        result = slot->track_artists[local_id];

    g_mutex_unlock(&cache->lock);
    return result;
}

/* =============================================================================
 * Track Navigation
 * ============================================================================= */

int64_t library_cache_get_next_track_id(library_cache_t *cache,
                                        int64_t current_track_id) {
    if (!cache || current_track_id <= 0) return 0;

    int64_t local_track_id;
    LibrarySlot *slot = decode_slot(cache, current_track_id, &local_track_id);
    if (!slot) return 0;

    g_mutex_lock(&cache->lock);

    const library_track_info_t *info = get_track_unlocked(slot, local_track_id);
    if (!info) { g_mutex_unlock(&cache->lock); return 0; }

    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(info->album_id);
    GArray *album_tracks   = NULL;
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity)
        album_tracks = slot->album_tracks[local_album_id];

    if (!album_tracks || album_tracks->len == 0) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    /* album_tracks stores GLOBAL track IDs */
    for (guint i = 0; i < album_tracks->len; i++) {
        int64_t gtid = g_array_index(album_tracks, int64_t, i);
        if (gtid == current_track_id) {
            if (i + 1 < album_tracks->len) {
                int64_t next_id = g_array_index(album_tracks, int64_t, i + 1);
                g_mutex_unlock(&cache->lock);
                return next_id;
            }
            break;
        }
    }

    g_mutex_unlock(&cache->lock);
    return 0;
}

int64_t library_cache_get_prev_track_id(library_cache_t *cache,
                                        int64_t current_track_id) {
    if (!cache || current_track_id <= 0) return 0;

    int64_t local_track_id;
    LibrarySlot *slot = decode_slot(cache, current_track_id, &local_track_id);
    if (!slot) return 0;

    g_mutex_lock(&cache->lock);

    const library_track_info_t *info = get_track_unlocked(slot, local_track_id);
    if (!info) { g_mutex_unlock(&cache->lock); return 0; }

    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(info->album_id);
    GArray *album_tracks   = NULL;
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity)
        album_tracks = slot->album_tracks[local_album_id];

    if (!album_tracks || album_tracks->len == 0) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    for (guint i = 0; i < album_tracks->len; i++) {
        int64_t gtid = g_array_index(album_tracks, int64_t, i);
        if (gtid == current_track_id) {
            if (i > 0) {
                int64_t prev_id = g_array_index(album_tracks, int64_t, i - 1);
                g_mutex_unlock(&cache->lock);
                return prev_id;
            }
            break;
        }
    }

    g_mutex_unlock(&cache->lock);
    return 0;
}

/* =============================================================================
 * List Queries
 * ============================================================================= */

const GPtrArray *library_cache_get_tracks_by_album(library_cache_t *cache,
                                                    int64_t album_id) {
    if (!cache || album_id <= 0) return NULL;

    int64_t local_album_id;
    LibrarySlot *slot = decode_slot(cache, album_id, &local_album_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);

    /* Return cached ptr-array if available */
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_ptrs_capacity
        && slot->album_tracks_ptrs[local_album_id]) {
        GPtrArray *cached = slot->album_tracks_ptrs[local_album_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    /* Ensure album_tracks (GArray of global IDs) is populated */
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity
        && !slot->album_tracks[local_album_id]) {
        load_album_tracks_slot(slot, local_album_id);
    }

    GArray *track_ids = NULL;
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity)
        track_ids = slot->album_tracks[local_album_id];

    if (!track_ids || track_ids->len == 0) {
        g_mutex_unlock(&cache->lock);
        return NULL;
    }

    GPtrArray *result = g_ptr_array_new();
    for (guint i = 0; i < track_ids->len; i++) {
        int64_t global_tid = g_array_index(track_ids, int64_t, i);
        int64_t local_tid  = LIBRARY_GLOBAL_ID_LOCAL(global_tid);
        const library_track_info_t *track = get_track_unlocked(slot, local_tid);
        if (track) g_ptr_array_add(result, (gpointer)track);
    }

    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_ptrs_capacity)
        slot->album_tracks_ptrs[local_album_id] = result;

    g_mutex_unlock(&cache->lock);
    return result;
}

const GPtrArray *library_cache_get_albums_by_artist(library_cache_t *cache,
                                                     int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    int64_t local_artist_id;
    LibrarySlot *slot = decode_slot(cache, artist_id, &local_artist_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);

    if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_albums_capacity
        && slot->artist_albums[local_artist_id]) {
        GPtrArray *cached = slot->artist_albums[local_artist_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    db_album_t *db_albums = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_albums_by_artist(slot->db, local_artist_id,
                                                      &db_albums, &count);

    if (res != QUADRATURE_OK || !db_albums || count == 0) {
        if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_albums_capacity
            && !slot->artist_albums[local_artist_id])
            slot->artist_albums[local_artist_id] = g_ptr_array_new();
        g_mutex_unlock(&cache->lock);
        if (db_albums) db_albums_free(db_albums, count);
        return NULL;
    }

    /* MBID set for deduplication across merged sources */
    GHashTable *seen_mbids = g_hash_table_new(g_str_hash, g_str_equal);

    GPtrArray *result = g_ptr_array_new();
    for (size_t i = 0; i < count; i++) {
        library_album_info_t *ainfo = ensure_album_in_slot(slot, &db_albums[i]);
        if (ainfo->musicbrainz_release_id && ainfo->musicbrainz_release_id[0])
            g_hash_table_add(seen_mbids, ainfo->musicbrainz_release_id);
        g_ptr_array_add(result, ainfo);
    }
    db_albums_free(db_albums, count);

    /* For cross-library merged artists, also pull albums from each merged source */
    library_artist_info_t *artist_info = slot_get_artist(slot, local_artist_id);
    if (artist_info && artist_info->merged_source_count > 0) {
        for (int m = 0; m < artist_info->merged_source_count; m++) {
            int64_t src_global = artist_info->merged_source_ids[m];
            int     src_lib    = LIBRARY_GLOBAL_ID_LIB(src_global);
            int64_t src_local  = LIBRARY_GLOBAL_ID_LOCAL(src_global);
            if (src_lib < 0 || src_lib >= cache->slot_count) continue;
            LibrarySlot *src_slot = &cache->slots[src_lib];

            db_album_t *src_albums = NULL;
            size_t src_count = 0;
            db_get_albums_by_artist(src_slot->db, src_local, &src_albums, &src_count);
            if (src_albums && src_count > 0) {
                for (size_t j = 0; j < src_count; j++) {
                    library_album_info_t *ainfo = ensure_album_in_slot(src_slot, &src_albums[j]);
                    /* Dedup by MBID: skip if same release already included */
                    if (ainfo->musicbrainz_release_id && ainfo->musicbrainz_release_id[0]) {
                        if (g_hash_table_contains(seen_mbids, ainfo->musicbrainz_release_id))
                            continue;
                        g_hash_table_add(seen_mbids, ainfo->musicbrainz_release_id);
                    }
                    g_ptr_array_add(result, ainfo);
                }
                db_albums_free(src_albums, src_count);
            }
        }
    }

    g_hash_table_destroy(seen_mbids);

    if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_albums_capacity)
        slot->artist_albums[local_artist_id] = result;

    g_mutex_unlock(&cache->lock);
    return result;
}

/* =============================================================================
 * "Appears On" Queries
 * ============================================================================= */

const GPtrArray *library_cache_get_artist_appearances(library_cache_t *cache,
                                                       int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    int64_t local_artist_id;
    LibrarySlot *slot = decode_slot(cache, artist_id, &local_artist_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);

    if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_appearances_capacity
        && slot->artist_appearances[local_artist_id]) {
        GPtrArray *cached = slot->artist_appearances[local_artist_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    db_album_t *db_albums = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_artist_appearances(slot->db, local_artist_id,
                                                        &db_albums, &count);

    /* MBID set for deduplication across merged sources */
    GHashTable *seen_mbids = g_hash_table_new(g_str_hash, g_str_equal);

    GPtrArray *result = g_ptr_array_new();

    if (res == QUADRATURE_OK && db_albums && count > 0) {
        for (size_t i = 0; i < count; i++) {
            library_album_info_t *ainfo = ensure_album_in_slot(slot, &db_albums[i]);
            if (ainfo->musicbrainz_release_id && ainfo->musicbrainz_release_id[0])
                g_hash_table_add(seen_mbids, ainfo->musicbrainz_release_id);
            g_ptr_array_add(result, ainfo);
        }
        db_albums_free(db_albums, count);
    }

    /* For cross-library merged artists, also pull appearances from each merged source */
    library_artist_info_t *artist_info = slot_get_artist(slot, local_artist_id);
    if (artist_info && artist_info->merged_source_count > 0) {
        for (int m = 0; m < artist_info->merged_source_count; m++) {
            int64_t src_global = artist_info->merged_source_ids[m];
            int     src_lib    = LIBRARY_GLOBAL_ID_LIB(src_global);
            int64_t src_local  = LIBRARY_GLOBAL_ID_LOCAL(src_global);
            if (src_lib < 0 || src_lib >= cache->slot_count) continue;
            LibrarySlot *src_slot = &cache->slots[src_lib];

            db_album_t *src_albums = NULL;
            size_t src_count = 0;
            db_get_artist_appearances(src_slot->db, src_local, &src_albums, &src_count);
            if (src_albums && src_count > 0) {
                for (size_t j = 0; j < src_count; j++) {
                    library_album_info_t *ainfo = ensure_album_in_slot(src_slot, &src_albums[j]);
                    /* Dedup by MBID: skip if same release already included */
                    if (ainfo->musicbrainz_release_id && ainfo->musicbrainz_release_id[0]) {
                        if (g_hash_table_contains(seen_mbids, ainfo->musicbrainz_release_id))
                            continue;
                        g_hash_table_add(seen_mbids, ainfo->musicbrainz_release_id);
                    }
                    g_ptr_array_add(result, ainfo);
                }
                db_albums_free(src_albums, src_count);
            }
        }
    }

    g_hash_table_destroy(seen_mbids);

    if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_appearances_capacity)
        slot->artist_appearances[local_artist_id] = result;

    g_mutex_unlock(&cache->lock);
    return result;
}

const GPtrArray *library_cache_get_artist_appearance_tracks(library_cache_t *cache,
                                                             int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    int64_t local_artist_id;
    LibrarySlot *slot = decode_slot(cache, artist_id, &local_artist_id);
    if (!slot) return NULL;

    g_mutex_lock(&cache->lock);

    if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_appearance_tracks_capacity
        && slot->artist_appearance_tracks[local_artist_id]) {
        GPtrArray *cached = slot->artist_appearance_tracks[local_artist_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    db_track_t *db_tracks = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_artist_appearance_tracks(slot->db, local_artist_id,
                                                              &db_tracks, &count);

    GPtrArray *result = g_ptr_array_new();

    if (res == QUADRATURE_OK && db_tracks && count > 0) {
        for (size_t i = 0; i < count; i++) {
            int64_t local_tid = db_tracks[i].id;
            library_track_info_t *info = slot_get_track(slot, local_tid);
            if (!info) {
                info = g_new0(library_track_info_t, 1);
                info->track_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, local_tid);
                info->album_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_tracks[i].album_id);
                info->artist_id     = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, db_tracks[i].artist_id);
                info->library_index = slot->lib_idx;

                info->path = g_strdup(db_tracks[i].path ? db_tracks[i].path : "");
                info->title         = g_strdup(db_tracks[i].title);
                info->artist_display = g_strdup(db_tracks[i].artist_display
                                     ? db_tracks[i].artist_display : db_tracks[i].artist);
                info->album_title   = g_strdup(db_tracks[i].album);
                info->genre         = db_tracks[i].genre ? g_strdup(db_tracks[i].genre) : NULL;
                info->duration_ms   = db_tracks[i].duration_ms;
                info->track_num     = db_tracks[i].track_num;
                info->disc_num      = db_tracks[i].disc_num;
                info->year          = db_tracks[i].year;
                slot_set_track(slot, local_tid, info);
            }
            g_ptr_array_add(result, info);
        }
        db_tracks_free(db_tracks, count);
    }

    if (local_artist_id > 0 && (size_t)local_artist_id < slot->artist_appearance_tracks_capacity)
        slot->artist_appearance_tracks[local_artist_id] = result;

    g_mutex_unlock(&cache->lock);
    return result;
}

/* =============================================================================
 * Filtered Queries — iterate ALL slots' DBs, merge results
 * ============================================================================= */

GPtrArray *library_cache_get_artists_filtered(library_cache_t *cache,
                                               library_sort_t sort,
                                               const char *search_text,
                                               const db_search_opts_t *filters) {
    g_assert(cache != NULL);

    GPtrArray *result = g_ptr_array_new();

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        if (!slot->db) continue;

        db_id_query_opts_t opts = {
            .search_text = search_text,
            .filters     = filters,
            .sort        = library_sort_to_db_sort(sort),
        };

        int64_t *ids = NULL;
        size_t count = 0;
        db_get_artist_ids_filtered(slot->db, &opts, &ids, &count);

        g_mutex_lock(&cache->lock);
        for (size_t j = 0; j < count; j++) {
            int64_t local_id = ids[j];
            library_artist_info_t *info = get_artist_unlocked(slot, local_id);
            if (info) g_ptr_array_add(result, info);
        }
        g_mutex_unlock(&cache->lock);

        g_free(ids);
    }

    return result;
}

GPtrArray *library_cache_get_albums_filtered(library_cache_t *cache,
                                              library_sort_t sort,
                                              const char *search_text,
                                              const db_search_opts_t *filters) {
    g_assert(cache != NULL);

    GPtrArray *result = g_ptr_array_new();

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        if (!slot->db) continue;

        db_id_query_opts_t opts = {
            .search_text = search_text,
            .filters     = filters,
            .sort        = library_sort_to_db_sort(sort),
        };

        int64_t *ids = NULL;
        size_t count = 0;
        db_get_album_ids_filtered(slot->db, &opts, &ids, &count);

        g_mutex_lock(&cache->lock);
        for (size_t j = 0; j < count; j++) {
            int64_t local_id = ids[j];
            library_album_info_t *info = get_album_unlocked(slot, local_id);
            if (info) g_ptr_array_add(result, info);
        }
        g_mutex_unlock(&cache->lock);

        g_free(ids);
    }

    return result;
}

/* =============================================================================
 * Search
 * ============================================================================= */

static size_t run_search_queries(library_cache_t *cache, const char *query,
                                  library_search_filter_t filter, size_t limit,
                                  const db_search_opts_t *opts,
                                  library_search_results_t *results) {
    size_t total = 0;

    for (int slot_idx = 0; slot_idx < cache->slot_count; slot_idx++) {
        LibrarySlot *slot = &cache->slots[slot_idx];
        if (!slot->db) continue;

        if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_ARTISTS) {
            db_id_query_opts_t id_opts = {
                .search_text = query,
                .filters     = opts,
                .sort        = DB_SORT_NAME_ASC,
            };
            int64_t *ids = NULL;
            size_t count = 0;
            db_get_artist_ids_filtered(slot->db, &id_opts, &ids, &count);

            size_t max = (filter == LIBRARY_SEARCH_FILTER_ALL)
                         ? (limit > 0 ? limit : 5)
                         : (limit > 0 ? limit : 100);
            if (count > max) count = max;

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                library_artist_info_t *info = get_artist_unlocked(slot, ids[i]);
                if (info) g_ptr_array_add(results->artists, info);
            }
            g_mutex_unlock(&cache->lock);
            g_free(ids);
        }

        if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_ALBUMS) {
            db_id_query_opts_t id_opts = {
                .search_text = query,
                .filters     = opts,
                .sort        = DB_SORT_NAME_ASC,
            };
            int64_t *ids = NULL;
            size_t count = 0;
            db_get_album_ids_filtered(slot->db, &id_opts, &ids, &count);

            size_t max = (filter == LIBRARY_SEARCH_FILTER_ALL)
                         ? (limit > 0 ? limit : 5)
                         : (limit > 0 ? limit : 100);
            if (count > max) count = max;

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                library_album_info_t *info = slot_get_album(slot, ids[i]);
                if (info) g_ptr_array_add(results->albums, info);
            }
            g_mutex_unlock(&cache->lock);
            g_free(ids);
        }

        if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_TRACKS) {
            size_t max = (filter == LIBRARY_SEARCH_FILTER_ALL)
                         ? (limit > 0 ? limit : 10)
                         : (limit > 0 ? limit : 100);
            int64_t *ids = NULL;
            size_t count = 0;
            db_search_track_ids(slot->db, query, opts, max, &ids, &count);

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                int64_t local_tid = ids[i];
                library_track_info_t *info = slot_get_track(slot, local_tid);
                if (!info) {
                    info = fetch_track_from_db(slot->db, local_tid);
                    if (info) {
                        info->track_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, info->track_id);
                        info->album_id      = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, info->album_id);
                        info->artist_id     = LIBRARY_MAKE_GLOBAL_ID(slot->lib_idx, info->artist_id);
                        info->library_index = slot->lib_idx;
                        slot_set_track(slot, local_tid, info);
                    }
                }
                if (info) g_ptr_array_add(results->tracks, info);
            }
            g_mutex_unlock(&cache->lock);
            g_free(ids);
        }
    }

    total = results->artists->len + results->albums->len + results->tracks->len;
    return total;
}

library_search_results_t *library_cache_search(library_cache_t *cache,
                                                const char *query,
                                                library_search_filter_t filter,
                                                size_t limit,
                                                const db_search_opts_t *opts) {
    if (!cache || !query) return NULL;

    library_search_results_t *results = g_new0(library_search_results_t, 1);
    results->artists = g_ptr_array_new();
    results->albums  = g_ptr_array_new();
    results->tracks  = g_ptr_array_new();

    size_t found = run_search_queries(cache, query, filter, limit, opts, results);

    if (found == 0 && strlen(query) >= 2 && cache->search_vocab_count > 0) {
        char *corrected = build_corrected_query(cache, query);
        if (corrected && g_strcmp0(corrected, query) != 0) {
            g_debug("cache search: no results for '%s', trying corrected '%s'", query, corrected);
            found = run_search_queries(cache, corrected, filter, limit, opts, results);
        }
        g_free(corrected);
    }

    results->total_artists = results->artists->len;
    results->total_albums  = results->albums->len;
    results->total_tracks  = results->tracks->len;

    return results;
}

void library_search_results_free(library_search_results_t *results) {
    if (!results) return;
    if (results->artists) g_ptr_array_unref(results->artists);
    if (results->albums)  g_ptr_array_unref(results->albums);
    if (results->tracks)  g_ptr_array_unref(results->tracks);
    g_free(results);
}

/* =============================================================================
 * Prefetch API
 * ============================================================================= */

void library_cache_prefetch_fullsize_artwork(library_cache_t *cache,
                                             int64_t album_id) {
    if (!cache || album_id <= 0) return;

    int64_t local_album_id;
    LibrarySlot *slot = decode_slot(cache, album_id, &local_album_id);
    if (!slot) return;

    g_mutex_lock(&cache->lock);

    const library_album_info_t *album = slot_get_album(slot, local_album_id);
    if (!album) {
        g_mutex_unlock(&cache->lock);
        album = library_cache_get_album(cache, album_id);
        if (!album) return;
        g_mutex_lock(&cache->lock);
    }

    if (album->path && album->path[0]) {
        char *abs_album_path;
        if (album->path[0] == '/') {
            abs_album_path = g_strdup(album->path);
        } else if (slot->music_base) {
            abs_album_path = g_build_filename(slot->music_base, album->path, NULL);
        } else {
            abs_album_path = g_strdup(album->path);
        }

        static const char *art_names[] = {
            "art.jpg", "cover.jpg", "folder.jpg", "album.jpg", "front.jpg",
            "art.png", "cover.png", "folder.png", "album.png", "front.png",
            NULL
        };

        for (const char **name = art_names; *name; name++) {
            char *art_path = g_build_filename(abs_album_path, *name, NULL);
            if (access(art_path, R_OK) == 0) {
                prefetch_file(art_path);
                g_free(art_path);
                break;
            }
            g_free(art_path);
        }

        g_free(abs_album_path);
    }

    g_mutex_unlock(&cache->lock);
}

void library_cache_prefetch_audio_files(library_cache_t *cache,
                                        const int64_t *track_ids,
                                        size_t count) {
    if (!cache || !track_ids || count == 0) return;

    g_mutex_lock(&cache->lock);

    for (size_t i = 0; i < count; i++) {
        int64_t local_tid;
        LibrarySlot *slot = decode_slot(cache, track_ids[i], &local_tid);
        if (!slot) continue;

        const library_track_info_t *track = get_track_unlocked(slot, local_tid);
        if (track && track->path) {
            int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
            library_album_info_t *album = get_album_unlocked(slot, local_album_id);
            char *full_path = resolve_track_path(slot->music_base,
                                                  album ? album->path : NULL,
                                                  track->path);
            if (full_path) {
                prefetch_file(full_path);
                g_free(full_path);
            }
        }
    }

    g_mutex_unlock(&cache->lock);
}
