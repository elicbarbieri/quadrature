/**
 * Library Cache Implementation
 *
 * Foundation layer for all library data access. Uses flat arrays indexed by
 * entity ID for O(1) lookups. Background warming thread pages all data into
 * cache after startup. Two DB connections: db_ui (main thread) and db_warm
 * (warming thread) for zero contention via SQLite WAL.
 */

#define G_LOG_DOMAIN "quadrature"

#include "quadrature/quadrature_library.h"
#include "quadrature/quadrature_database.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdatomic.h>
#include <glib.h>

#define WARM_PAGE_SIZE 1000

// =============================================================================
// Internal Structure
// =============================================================================

struct library_cache {
    // Entity arrays (indexed by ID — O(1) lookup)
    library_artist_info_t** artists;
    size_t artists_capacity;
    library_album_info_t** albums;
    size_t albums_capacity;
    library_track_info_t** tracks;
    size_t tracks_capacity;

    // Relationship arrays (indexed by entity ID)
    GArray**    album_tracks;           // album_tracks[album_id] -> GArray<int64_t>
    size_t      album_tracks_capacity;
    GPtrArray** track_artists;          // track_artists[track_id] -> GPtrArray<library_track_artist_t*>
    size_t      track_artists_capacity;
    GPtrArray** artist_albums;          // artist_albums[artist_id] -> GPtrArray<library_album_info_t*>
    size_t      artist_albums_capacity;

    // "Appears on" arrays
    GPtrArray** artist_appearances;
    size_t      artist_appearances_capacity;
    GPtrArray** artist_appearance_tracks;
    size_t      artist_appearance_tracks_capacity;

    // Cached GPtrArray results for get_tracks_by_album (to fix memory leak)
    GPtrArray** album_tracks_ptrs;     // album_tracks_ptrs[album_id] -> GPtrArray<library_track_info_t*>
    size_t      album_tracks_ptrs_capacity;

    // Sorted list caches (NULL if not loaded)
    GPtrArray* all_artists;
    GPtrArray* all_albums;
    library_sort_t artists_sort;
    library_sort_t albums_sort;

    // Warming thread state
    quadrature_db_t* db_warm;
    GThread*         warm_thread;
    atomic_int       warm_cancel;
    atomic_int       warm_state;

    library_cache_ready_cb ready_cb;
    void*            ready_cb_data;

    GMutex lock;
    quadrature_db_t* db;               // UI readonly connection (main thread only)
    char* db_path;
    char* music_base;
};

// =============================================================================
// Memory Management Helpers
// =============================================================================

static void free_artist_info(library_artist_info_t* info) {
    if (!info) return;
    g_free(info->name);
    g_free(info);
}

static void free_album_info(library_album_info_t* info) {
    if (!info) return;
    g_free(info->title);
    g_free(info->artist_name);
    g_free(info->path);
    g_free(info->genres);
    g_free(info);
}

static void free_track_info(library_track_info_t* info) {
    if (!info) return;
    g_free(info->path);
    g_free(info->title);
    g_free(info->artist_name);
    g_free(info->artist_display);
    g_free(info->album_title);
    g_free(info->genre);
    g_free(info);
}

static void free_track_artist(gpointer data) {
    library_track_artist_t* artist = (library_track_artist_t*)data;
    if (!artist) return;
    g_free(artist->name);
    g_free(artist);
}

// =============================================================================
// Flat Array Helpers
// =============================================================================

static inline library_artist_info_t* cache_get_artist(library_cache_t* cache, int64_t id) {
    if (id <= 0 || (size_t)id >= cache->artists_capacity) return NULL;
    return cache->artists[id];
}

static inline library_album_info_t* cache_get_album(library_cache_t* cache, int64_t id) {
    if (id <= 0 || (size_t)id >= cache->albums_capacity) return NULL;
    return cache->albums[id];
}

static inline library_track_info_t* cache_get_track(library_cache_t* cache, int64_t id) {
    if (id <= 0 || (size_t)id >= cache->tracks_capacity) return NULL;
    return cache->tracks[id];
}

static inline void cache_set_artist(library_cache_t* cache, int64_t id, library_artist_info_t* info) {
    if (id <= 0 || (size_t)id >= cache->artists_capacity) return;
    cache->artists[id] = info;
}

static inline void cache_set_album(library_cache_t* cache, int64_t id, library_album_info_t* info) {
    if (id <= 0 || (size_t)id >= cache->albums_capacity) return;
    cache->albums[id] = info;
}

static inline void cache_set_track(library_cache_t* cache, int64_t id, library_track_info_t* info) {
    if (id <= 0 || (size_t)id >= cache->tracks_capacity) return;
    cache->tracks[id] = info;
}

// =============================================================================
// Internal Helpers
// =============================================================================

// Prefetch a file into kernel page cache
static void prefetch_file(const char* path) {
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

// Fetch track info from database and convert to cache format
static library_track_info_t* fetch_track_from_db(quadrature_db_t* db, int64_t track_id) {
    db_track_t* db_track = NULL;
    quadrature_result_t res = db_get_track(db, track_id, &db_track);
    if (res != QUADRATURE_OK || !db_track) {
        return NULL;
    }

    library_track_info_t* info = g_new0(library_track_info_t, 1);
    info->track_id = db_track->id;
    info->album_id = db_track->album_id;
    info->artist_id = db_track->artist_id;
    info->path = g_strdup(db_track->path);
    info->title = g_strdup(db_track->title);
    info->artist_name = g_strdup(db_track->artist);
    info->artist_display = db_track->artist_display ? g_strdup(db_track->artist_display) : NULL;
    info->album_title = g_strdup(db_track->album);
    info->genre = db_track->genre ? g_strdup(db_track->genre) : NULL;
    info->duration_ms = db_track->duration_ms;
    info->track_num = db_track->track_num;
    info->disc_num = db_track->disc_num;
    info->year = db_track->year;

    db_track_free(db_track);
    return info;
}

// Cache track_artists for a track (used by warming and on-demand)
static void cache_track_artists_for_track(library_cache_t* cache, quadrature_db_t* db, int64_t track_id) {
    if (track_id <= 0 || (size_t)track_id >= cache->track_artists_capacity) return;
    if (cache->track_artists[track_id]) return;  // already cached

    db_track_artist_t* db_artists = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_track_artists(db, track_id, &db_artists, &count);
    if (res != QUADRATURE_OK || !db_artists || count == 0) {
        if (db_artists) db_track_artists_free(db_artists, count);
        return;
    }

    GPtrArray* result = g_ptr_array_new_with_free_func(free_track_artist);
    for (size_t i = 0; i < count; i++) {
        library_track_artist_t* artist = g_new0(library_track_artist_t, 1);
        artist->artist_id = db_artists[i].artist_id;
        artist->name = g_strdup(db_artists[i].name);
        artist->role = (db_artists[i].role == ARTIST_ROLE_PRIMARY)
                       ? LIBRARY_ARTIST_ROLE_PRIMARY
                       : LIBRARY_ARTIST_ROLE_FEATURING;
        artist->position = db_artists[i].position;
        g_ptr_array_add(result, artist);
    }
    db_track_artists_free(db_artists, count);

    cache->track_artists[track_id] = result;
}

// Prefetch all track IDs for an album (for navigation)
static void load_album_tracks(library_cache_t* cache, quadrature_db_t* db, int64_t album_id) {
    if (album_id <= 0 || (size_t)album_id >= cache->album_tracks_capacity) return;
    if (cache->album_tracks[album_id]) return;

    db_track_t* tracks = NULL;
    size_t count = 0;

    quadrature_result_t res = db_get_tracks_by_album(db, album_id, &tracks, &count);
    if (res != QUADRATURE_OK || !tracks || count == 0) {
        return;
    }

    GArray* track_ids = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), count);

    for (size_t i = 0; i < count; i++) {
        g_array_append_val(track_ids, tracks[i].id);
    }

    cache->album_tracks[album_id] = track_ids;

    db_tracks_free(tracks, count);
}

// Get track without locking (for internal use when lock is already held)
static const library_track_info_t* get_track_unlocked(library_cache_t* cache,
                                                       int64_t track_id) {
    library_track_info_t* info = cache_get_track(cache, track_id);
    if (info) return info;

    // Cache miss - fetch from database (db_ui, main thread)
    g_warning("Cache MISS → DB fallback: get_track(track_id=%" G_GINT64_FORMAT ")", track_id);
    info = fetch_track_from_db(cache->db, track_id);
    if (!info) return NULL;

    cache_set_track(cache, track_id, info);

    // Load album tracks for navigation if not already done
    if (info->album_id > 0 && (size_t)info->album_id < cache->album_tracks_capacity
        && !cache->album_tracks[info->album_id]) {
        load_album_tracks(cache, cache->db, info->album_id);
    }

    return info;
}

// Convert library_sort_t to db_sort_t for DB queries
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


// =============================================================================
// Array Freeing Helpers
// =============================================================================

static void free_entity_arrays(library_cache_t* cache) {
    // Free artist info structs
    if (cache->artists) {
        for (size_t i = 0; i < cache->artists_capacity; i++) {
            if (cache->artists[i]) free_artist_info(cache->artists[i]);
        }
        g_free(cache->artists);
        cache->artists = NULL;
    }
    cache->artists_capacity = 0;

    // Free album info structs
    if (cache->albums) {
        for (size_t i = 0; i < cache->albums_capacity; i++) {
            if (cache->albums[i]) free_album_info(cache->albums[i]);
        }
        g_free(cache->albums);
        cache->albums = NULL;
    }
    cache->albums_capacity = 0;

    // Free track info structs
    if (cache->tracks) {
        for (size_t i = 0; i < cache->tracks_capacity; i++) {
            if (cache->tracks[i]) free_track_info(cache->tracks[i]);
        }
        g_free(cache->tracks);
        cache->tracks = NULL;
    }
    cache->tracks_capacity = 0;

    // Free album_tracks arrays
    if (cache->album_tracks) {
        for (size_t i = 0; i < cache->album_tracks_capacity; i++) {
            if (cache->album_tracks[i]) g_array_unref(cache->album_tracks[i]);
        }
        g_free(cache->album_tracks);
        cache->album_tracks = NULL;
    }
    cache->album_tracks_capacity = 0;

    // Free track_artists arrays
    if (cache->track_artists) {
        for (size_t i = 0; i < cache->track_artists_capacity; i++) {
            if (cache->track_artists[i]) g_ptr_array_unref(cache->track_artists[i]);
        }
        g_free(cache->track_artists);
        cache->track_artists = NULL;
    }
    cache->track_artists_capacity = 0;

    // Free artist_albums arrays
    if (cache->artist_albums) {
        for (size_t i = 0; i < cache->artist_albums_capacity; i++) {
            if (cache->artist_albums[i]) g_ptr_array_unref(cache->artist_albums[i]);
        }
        g_free(cache->artist_albums);
        cache->artist_albums = NULL;
    }
    cache->artist_albums_capacity = 0;

    // Free artist_appearances arrays
    if (cache->artist_appearances) {
        for (size_t i = 0; i < cache->artist_appearances_capacity; i++) {
            if (cache->artist_appearances[i]) g_ptr_array_unref(cache->artist_appearances[i]);
        }
        g_free(cache->artist_appearances);
        cache->artist_appearances = NULL;
    }
    cache->artist_appearances_capacity = 0;

    // Free artist_appearance_tracks arrays
    if (cache->artist_appearance_tracks) {
        for (size_t i = 0; i < cache->artist_appearance_tracks_capacity; i++) {
            if (cache->artist_appearance_tracks[i]) g_ptr_array_unref(cache->artist_appearance_tracks[i]);
        }
        g_free(cache->artist_appearance_tracks);
        cache->artist_appearance_tracks = NULL;
    }
    cache->artist_appearance_tracks_capacity = 0;

    // Free album_tracks_ptrs arrays (cached GPtrArray results)
    if (cache->album_tracks_ptrs) {
        for (size_t i = 0; i < cache->album_tracks_ptrs_capacity; i++) {
            if (cache->album_tracks_ptrs[i]) g_ptr_array_unref(cache->album_tracks_ptrs[i]);
        }
        g_free(cache->album_tracks_ptrs);
        cache->album_tracks_ptrs = NULL;
    }
    cache->album_tracks_ptrs_capacity = 0;

    // Free sorted list caches
    if (cache->all_artists) {
        g_ptr_array_unref(cache->all_artists);
        cache->all_artists = NULL;
    }
    if (cache->all_albums) {
        g_ptr_array_unref(cache->all_albums);
        cache->all_albums = NULL;
    }
}

static void allocate_entity_arrays(library_cache_t* cache) {
    int64_t max_artist = db_get_max_id(cache->db, "artists");
    int64_t max_album  = db_get_max_id(cache->db, "albums");
    int64_t max_track  = db_get_max_id(cache->db, "tracks");

    cache->artists_capacity = (size_t)(max_artist + 1);
    cache->albums_capacity  = (size_t)(max_album + 1);
    cache->tracks_capacity  = (size_t)(max_track + 1);

    cache->artists = g_new0(library_artist_info_t*, cache->artists_capacity);
    cache->albums  = g_new0(library_album_info_t*, cache->albums_capacity);
    cache->tracks  = g_new0(library_track_info_t*, cache->tracks_capacity);

    cache->album_tracks_capacity = cache->albums_capacity;
    cache->album_tracks = g_new0(GArray*, cache->album_tracks_capacity);

    cache->track_artists_capacity = cache->tracks_capacity;
    cache->track_artists = g_new0(GPtrArray*, cache->track_artists_capacity);

    cache->artist_albums_capacity = cache->artists_capacity;
    cache->artist_albums = g_new0(GPtrArray*, cache->artist_albums_capacity);

    cache->artist_appearances_capacity = cache->artists_capacity;
    cache->artist_appearances = g_new0(GPtrArray*, cache->artist_appearances_capacity);

    cache->artist_appearance_tracks_capacity = cache->artists_capacity;
    cache->artist_appearance_tracks = g_new0(GPtrArray*, cache->artist_appearance_tracks_capacity);

    cache->album_tracks_ptrs_capacity = cache->albums_capacity;
    cache->album_tracks_ptrs = g_new0(GPtrArray*, cache->album_tracks_ptrs_capacity);
}

// =============================================================================
// Warming Thread
// =============================================================================

static void cancel_and_join_warming(library_cache_t* cache) {
    if (cache->warm_thread) {
        atomic_store(&cache->warm_cancel, 1);
        g_thread_join(cache->warm_thread);
        cache->warm_thread = NULL;
        atomic_store(&cache->warm_cancel, 0);
    }
}

static gboolean warming_complete_idle(gpointer data) {
    library_cache_t* cache = (library_cache_t*)data;
    if (cache->ready_cb) {
        cache->ready_cb(cache->ready_cb_data);
    }
    return G_SOURCE_REMOVE;
}

static gpointer warming_thread_func(gpointer data) {
    library_cache_t* cache = (library_cache_t*)data;

    g_info("cache warming: started");
    gint64 start_time = g_get_monotonic_time();

    // Phase 1: Page artists
    {
        size_t offset = 0;
        GPtrArray* artist_list = g_ptr_array_new();

        for (;;) {
            if (atomic_load(&cache->warm_cancel)) goto done;

            db_page_opts_t opts = {
                .offset = offset,
                .limit = WARM_PAGE_SIZE,
                .sort = DB_SORT_NAME_ASC,
            };

            db_artist_t* db_artists = NULL;
            size_t count = 0, total = 0;
            quadrature_result_t res = db_get_artists_page(cache->db_warm, &opts, &db_artists, &count, &total);
            if (res != QUADRATURE_OK || count == 0) break;

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                int64_t id = db_artists[i].id;
                if (!cache_get_artist(cache, id)) {
                    library_artist_info_t* info = g_new0(library_artist_info_t, 1);
                    info->artist_id = id;
                    info->name = g_strdup(db_artists[i].name);
                    info->album_count = (uint32_t)db_artists[i].album_count;
                    info->track_count = (uint32_t)db_artists[i].track_count;
                    info->total_duration_ms = 0;
                    cache_set_artist(cache, id, info);
                }
                g_ptr_array_add(artist_list, cache_get_artist(cache, id));
            }
            g_mutex_unlock(&cache->lock);

            db_artists_free(db_artists, count);
            offset += count;
            if (count < WARM_PAGE_SIZE) break;
        }

        g_mutex_lock(&cache->lock);
        if (!cache->all_artists) {
            // Sync fallback hasn't run — install our list
            cache->all_artists = artist_list;
            cache->artists_sort = LIBRARY_SORT_NAME_ASC;
        } else {
            // Sync fallback already built the list — discard ours
            g_ptr_array_unref(artist_list);
        }
        g_mutex_unlock(&cache->lock);

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_info("cache warming: phase 1 (artists) done in %.1f ms",
               phase_elapsed / 1000.0);
    }

    // Phase 2: Page albums
    {
        size_t offset = 0;
        GPtrArray* album_list = g_ptr_array_new();

        for (;;) {
            if (atomic_load(&cache->warm_cancel)) goto done;

            db_page_opts_t opts = {
                .offset = offset,
                .limit = WARM_PAGE_SIZE,
                .sort = DB_SORT_NAME_ASC,
            };

            db_album_t* db_albums = NULL;
            size_t count = 0, total = 0;
            quadrature_result_t res = db_get_albums_page(cache->db_warm, &opts, &db_albums, &count, &total);
            if (res != QUADRATURE_OK || count == 0) break;

            g_mutex_lock(&cache->lock);
            for (size_t i = 0; i < count; i++) {
                int64_t id = db_albums[i].id;
                if (!cache_get_album(cache, id)) {
                    library_album_info_t* info = g_new0(library_album_info_t, 1);
                    info->album_id = id;
                    info->artist_id = db_albums[i].artist_id;
                    info->title = g_strdup(db_albums[i].title);
                    info->artist_name = g_strdup(db_albums[i].artist_name);
                    info->path = g_strdup("");
                    info->genres = db_albums[i].genres ? g_strdup(db_albums[i].genres) : NULL;
                    info->year = db_albums[i].year;
                    info->track_count = (uint16_t)db_albums[i].track_count;
                    info->disc_count = 1;
                    info->total_duration_ms = 0;
                    cache_set_album(cache, id, info);
                }
                g_ptr_array_add(album_list, cache_get_album(cache, id));
            }
            g_mutex_unlock(&cache->lock);

            db_albums_free(db_albums, count);
            offset += count;
            if (count < WARM_PAGE_SIZE) break;
        }

        g_mutex_lock(&cache->lock);
        if (!cache->all_albums) {
            cache->all_albums = album_list;
            cache->albums_sort = LIBRARY_SORT_NAME_ASC;
        } else {
            g_ptr_array_unref(album_list);
        }
        g_mutex_unlock(&cache->lock);

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_info("cache warming: phase 2 (albums) done in %.1f ms",
               phase_elapsed / 1000.0);
    }

    // Phase 3: Load tracks per album
    {
        size_t tracks_loaded = 0;

        for (size_t album_id = 1; album_id < cache->albums_capacity; album_id++) {
            if (atomic_load(&cache->warm_cancel)) goto done;

            library_album_info_t* album = cache_get_album(cache, (int64_t)album_id);
            if (!album) continue;

            // Load tracks for this album
            db_track_t* db_tracks = NULL;
            size_t count = 0;
            quadrature_result_t res = db_get_tracks_by_album(cache->db_warm, (int64_t)album_id, &db_tracks, &count);
            if (res != QUADRATURE_OK || !db_tracks || count == 0) continue;

            g_mutex_lock(&cache->lock);

            GArray* track_ids = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), count);
            uint16_t max_disc = 0;
            uint32_t total_duration = 0;

            for (size_t i = 0; i < count; i++) {
                int64_t tid = db_tracks[i].id;
                g_array_append_val(track_ids, tid);

                if (!cache_get_track(cache, tid)) {
                    library_track_info_t* info = g_new0(library_track_info_t, 1);
                    info->track_id = tid;
                    info->album_id = db_tracks[i].album_id;
                    info->artist_id = db_tracks[i].artist_id;
                    info->path = g_strdup(db_tracks[i].path);
                    info->title = g_strdup(db_tracks[i].title);
                    info->artist_name = g_strdup(db_tracks[i].artist);
                    info->artist_display = db_tracks[i].artist_display ? g_strdup(db_tracks[i].artist_display) : NULL;
                    info->album_title = g_strdup(db_tracks[i].album);
                    info->genre = db_tracks[i].genre ? g_strdup(db_tracks[i].genre) : NULL;
                    info->duration_ms = db_tracks[i].duration_ms;
                    info->track_num = db_tracks[i].track_num;
                    info->disc_num = db_tracks[i].disc_num;
                    info->year = db_tracks[i].year;
                    cache_set_track(cache, tid, info);
                    tracks_loaded++;
                }

                library_track_info_t* track = cache_get_track(cache, tid);
                if (track) {
                    total_duration += track->duration_ms;
                    if (track->disc_num > max_disc) max_disc = track->disc_num;
                }
            }

            // Update album aggregates
            album->total_duration_ms = total_duration;
            album->disc_count = max_disc > 0 ? max_disc : 1;
            album->track_count = (uint16_t)count;

            // Store album track IDs
            if (album_id < cache->album_tracks_capacity) {
                if (cache->album_tracks[album_id])
                    g_array_unref(cache->album_tracks[album_id]);
                cache->album_tracks[album_id] = track_ids;
            } else {
                g_array_unref(track_ids);
            }

            g_mutex_unlock(&cache->lock);

            db_tracks_free(db_tracks, count);
        }

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_info("cache warming: phase 3 (tracks) done — %zu tracks in %.1f ms",
               tracks_loaded, phase_elapsed / 1000.0);
    }

    // Cache track_artists for "appears on" feature
    {
        for (size_t tid = 1; tid < cache->tracks_capacity; tid++) {
            if (atomic_load(&cache->warm_cancel)) goto done;
            library_track_info_t* track = cache_get_track(cache, (int64_t)tid);
            if (!track) continue;

            g_mutex_lock(&cache->lock);
            cache_track_artists_for_track(cache, cache->db_warm, (int64_t)tid);
            g_mutex_unlock(&cache->lock);
        }

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_info("cache warming: phase 3.5 (track artists) done in %.1f ms", phase_elapsed / 1000.0);
    }

    // Phase 4: Compute aggregates and relationships (in-memory, no DB)
    {
        g_mutex_lock(&cache->lock);

        // Snapshot which artists already have albums loaded (by on-demand fallback)
        GHashTable* preloaded_artists = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (size_t aid = 1; aid < cache->artist_albums_capacity; aid++) {
            if (cache->artist_albums[aid] && cache->artist_albums[aid]->len > 0) {
                g_hash_table_add(preloaded_artists, GSIZE_TO_POINTER(aid));
            }
        }

        // Build artist_albums relationships and compute artist aggregates
        for (size_t album_id = 1; album_id < cache->albums_capacity; album_id++) {
            library_album_info_t* album = cache_get_album(cache, (int64_t)album_id);
            if (!album) continue;

            int64_t aid = album->artist_id;
            if (aid <= 0 || (size_t)aid >= cache->artist_albums_capacity) continue;

            // Skip artists already populated by on-demand fallback
            if (g_hash_table_contains(preloaded_artists, GSIZE_TO_POINTER((gsize)aid)))
                continue;

            if (!cache->artist_albums[aid]) {
                cache->artist_albums[aid] = g_ptr_array_new();
            }
            g_ptr_array_add(cache->artist_albums[aid], album);
        }
        g_hash_table_destroy(preloaded_artists);

        // Ensure all cached artists have an artist_albums entry (empty for featuring-only artists)
        for (size_t aid = 1; aid < cache->artists_capacity; aid++) {
            if (cache_get_artist(cache, (int64_t)aid) &&
                aid < cache->artist_albums_capacity && !cache->artist_albums[aid]) {
                cache->artist_albums[aid] = g_ptr_array_new();
            }
        }

        // Snapshot which artists already have appearances loaded (by on-demand fallback)
        GHashTable* preloaded_appearances = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (size_t aid = 1; aid < cache->artist_appearances_capacity; aid++) {
            if (cache->artist_appearances[aid] && cache->artist_appearances[aid]->len > 0) {
                g_hash_table_add(preloaded_appearances, GSIZE_TO_POINTER(aid));
            }
        }

        // Compute artist aggregates from tracks
        for (size_t tid = 1; tid < cache->tracks_capacity; tid++) {
            library_track_info_t* track = cache_get_track(cache, (int64_t)tid);
            if (!track) continue;

            int64_t aid = track->artist_id;
            library_artist_info_t* artist = cache_get_artist(cache, aid);
            if (artist) {
                artist->total_duration_ms += track->duration_ms;
            }

            // Check for "appears on" — track_artists may have additional artists
            if ((size_t)tid < cache->track_artists_capacity && cache->track_artists[tid]) {
                GPtrArray* ta = cache->track_artists[tid];
                library_album_info_t* album = cache_get_album(cache, track->album_id);
                if (!album) continue;

                for (guint j = 0; j < ta->len; j++) {
                    library_track_artist_t* credit = g_ptr_array_index(ta, j);
                    int64_t credit_aid = credit->artist_id;

                    // Skip if this artist IS the album artist
                    if (credit_aid == album->artist_id) continue;
                    if (credit_aid <= 0 || (size_t)credit_aid >= cache->artist_appearances_capacity) continue;

                    // Skip artists already populated by on-demand fallback
                    if (g_hash_table_contains(preloaded_appearances, GSIZE_TO_POINTER((gsize)credit_aid)))
                        continue;

                    // Add to appearances (unique albums)
                    if (!cache->artist_appearances[credit_aid]) {
                        cache->artist_appearances[credit_aid] = g_ptr_array_new();
                    }
                    // Check if album already in list
                    GPtrArray* app_albums = cache->artist_appearances[credit_aid];
                    gboolean found = FALSE;
                    for (guint k = 0; k < app_albums->len; k++) {
                        if (g_ptr_array_index(app_albums, k) == album) {
                            found = TRUE;
                            break;
                        }
                    }
                    if (!found) g_ptr_array_add(app_albums, album);

                    // Add to appearance tracks
                    if (!cache->artist_appearance_tracks[credit_aid]) {
                        cache->artist_appearance_tracks[credit_aid] = g_ptr_array_new();
                    }
                    g_ptr_array_add(cache->artist_appearance_tracks[credit_aid], track);
                }
            }
        }
        g_hash_table_destroy(preloaded_appearances);

        g_mutex_unlock(&cache->lock);

        gint64 phase_elapsed = g_get_monotonic_time() - start_time;
        g_info("cache warming: phase 4 (aggregates) done in %.1f ms", phase_elapsed / 1000.0);
    }

    // Phase 5: Signal completion
    atomic_store(&cache->warm_state, LIBRARY_CACHE_READY);
    if (cache->ready_cb) {
        g_idle_add(warming_complete_idle, cache);
    }

    {
        gint64 total_elapsed = g_get_monotonic_time() - start_time;
        g_info("cache warming: complete in %.1f ms", total_elapsed / 1000.0);
    }
    return NULL;

done:
    g_info("cache warming: cancelled");
    return NULL;
}

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t library_cache_create(const char* db_path,
                                         const char* music_base,
                                         library_cache_t** out) {
    if (!db_path || !out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Open UI readonly connection
    quadrature_db_t* db = NULL;
    quadrature_result_t db_result = db_open_readonly(db_path, &db);
    if (db_result != QUADRATURE_OK) {
        return db_result;
    }

    // Open warming readonly connection
    quadrature_db_t* db_warm = NULL;
    db_result = db_open_readonly(db_path, &db_warm);
    if (db_result != QUADRATURE_OK) {
        db_close(db);
        return db_result;
    }

    library_cache_t* cache = g_new0(library_cache_t, 1);

    g_mutex_init(&cache->lock);
    cache->db = db;
    cache->db_warm = db_warm;
    cache->db_path = g_strdup(db_path);
    cache->music_base = music_base ? g_strdup(music_base) : NULL;

    atomic_init(&cache->warm_cancel, 0);
    atomic_init(&cache->warm_state, LIBRARY_CACHE_IDLE);

    // Allocate flat arrays based on current max IDs
    allocate_entity_arrays(cache);

    cache->all_artists = NULL;
    cache->all_albums = NULL;
    cache->artists_sort = LIBRARY_SORT_NAME_ASC;
    cache->albums_sort = LIBRARY_SORT_NAME_ASC;

    *out = cache;
    return QUADRATURE_OK;
}

void library_cache_destroy(library_cache_t* cache) {
    if (!cache) return;

    // Cancel and join warming thread first
    cancel_and_join_warming(cache);

    g_mutex_lock(&cache->lock);

    free_entity_arrays(cache);

    g_free(cache->music_base);
    g_free(cache->db_path);

    if (cache->db) db_close(cache->db);
    if (cache->db_warm) db_close(cache->db_warm);

    g_mutex_unlock(&cache->lock);
    g_mutex_clear(&cache->lock);

    g_free(cache);
}

// =============================================================================
// Cache Warming API
// =============================================================================

void library_cache_set_ready_callback(library_cache_t* cache,
                                       library_cache_ready_cb cb, void* user_data) {
    g_assert(cache != NULL);
    cache->ready_cb = cb;
    cache->ready_cb_data = user_data;
}

void library_cache_start_warming(library_cache_t* cache) {
    g_assert(cache != NULL);

    int expected = LIBRARY_CACHE_IDLE;
    if (!atomic_compare_exchange_strong(&cache->warm_state, &expected, LIBRARY_CACHE_WARMING)) {
        return;  // Already warming or ready
    }

    cache->warm_thread = g_thread_new("cache-warm", warming_thread_func, cache);
}

library_cache_state_t library_cache_get_state(library_cache_t* cache) {
    g_assert(cache != NULL);
    return (library_cache_state_t)atomic_load(&cache->warm_state);
}



// =============================================================================
// Entity Getters
// =============================================================================

const library_track_info_t* library_cache_get_track(library_cache_t* cache,
                                                     int64_t track_id) {
    if (!cache || track_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);
    const library_track_info_t* result = get_track_unlocked(cache, track_id);
    g_mutex_unlock(&cache->lock);

    return result;
}

// Load album from DB if not cached (must be called with lock held)
static library_album_info_t* get_album_unlocked(library_cache_t* cache, int64_t album_id) {
    library_album_info_t* info = cache_get_album(cache, album_id);
    if (info) return info;

    // Cache miss — fetch by loading tracks for this album (db_ui)
    g_warning("Cache MISS → DB fallback: get_album(album_id=%" G_GINT64_FORMAT ")", album_id);
    db_track_t* tracks = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_tracks_by_album(cache->db, album_id, &tracks, &count);

    if (res == QUADRATURE_OK && tracks && count > 0) {
        info = g_new0(library_album_info_t, 1);
        info->album_id = album_id;
        info->artist_id = tracks[0].artist_id;
        info->title = g_strdup(tracks[0].album);
        info->artist_name = g_strdup(tracks[0].artist);
        g_info("Cache: Loaded album '%s' by %s from DB", info->title, info->artist_name);
        info->year = tracks[0].year;
        info->track_count = (uint16_t)count;

        uint16_t max_disc = 0;
        uint32_t total_duration = 0;
        GHashTable* genre_set = g_hash_table_new(g_str_hash, g_str_equal);
        for (size_t i = 0; i < count; i++) {
            if (tracks[i].disc_num > max_disc) max_disc = tracks[i].disc_num;
            total_duration += tracks[i].duration_ms;
            if (tracks[i].genre && tracks[i].genre[0])
                g_hash_table_add(genre_set, tracks[i].genre);
        }
        info->disc_count = max_disc > 0 ? max_disc : 1;
        info->total_duration_ms = total_duration;

        guint genre_count = g_hash_table_size(genre_set);
        if (genre_count > 0) {
            GString* genres_str = g_string_new(NULL);
            GHashTableIter iter;
            gpointer genre_key;
            g_hash_table_iter_init(&iter, genre_set);
            gboolean first = TRUE;
            while (g_hash_table_iter_next(&iter, &genre_key, NULL)) {
                if (!first) g_string_append_c(genres_str, ';');
                g_string_append(genres_str, (const char*)genre_key);
                first = FALSE;
            }
            info->genres = g_string_free(genres_str, FALSE);
        } else {
            info->genres = NULL;
        }
        g_hash_table_destroy(genre_set);
        info->path = g_strdup("");

        cache_set_album(cache, album_id, info);
        db_tracks_free(tracks, count);
    }

    return info;
}

const library_album_info_t* library_cache_get_album(library_cache_t* cache,
                                                     int64_t album_id) {
    if (!cache || album_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);
    library_album_info_t* info = get_album_unlocked(cache, album_id);
    g_mutex_unlock(&cache->lock);

    return info;
}

// Load artist from DB if not cached (must be called with lock held)
static library_artist_info_t* get_artist_unlocked(library_cache_t* cache, int64_t artist_id) {
    library_artist_info_t* info = cache_get_artist(cache, artist_id);
    if (info) return info;

    // Cache miss — fetch via albums by artist (db_ui)
    db_album_t* albums = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_albums_by_artist(cache->db, artist_id, &albums, &count);

    if (res == QUADRATURE_OK && albums && count > 0) {
        info = g_new0(library_artist_info_t, 1);
        info->artist_id = artist_id;
        info->name = g_strdup(albums[0].artist_name);
        info->album_count = (uint32_t)count;

        uint32_t track_count = 0;
        for (size_t i = 0; i < count; i++) {
            track_count += albums[i].track_count;
        }
        info->track_count = track_count;
        info->total_duration_ms = 0;

        cache_set_artist(cache, artist_id, info);
        db_albums_free(albums, count);
    }

    return info;
}

const library_artist_info_t* library_cache_get_artist(library_cache_t* cache,
                                                       int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);
    library_artist_info_t* info = get_artist_unlocked(cache, artist_id);
    g_mutex_unlock(&cache->lock);

    return info;
}

const GPtrArray* library_cache_get_track_artists(library_cache_t* cache,
                                                   int64_t track_id) {
    if (!cache || track_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);

    // Check cache
    if (track_id > 0 && (size_t)track_id < cache->track_artists_capacity
        && cache->track_artists[track_id]) {
        GPtrArray* result = cache->track_artists[track_id];
        g_mutex_unlock(&cache->lock);
        return result;
    }

    // Cache miss — fetch and cache
    cache_track_artists_for_track(cache, cache->db, track_id);

    GPtrArray* result = NULL;
    if (track_id > 0 && (size_t)track_id < cache->track_artists_capacity) {
        result = cache->track_artists[track_id];
    }

    g_mutex_unlock(&cache->lock);
    return result;
}

// =============================================================================
// Track Navigation
// =============================================================================

int64_t library_cache_get_next_track_id(library_cache_t* cache,
                                        int64_t current_track_id) {
    if (!cache || current_track_id <= 0) return 0;

    g_mutex_lock(&cache->lock);

    const library_track_info_t* info = get_track_unlocked(cache, current_track_id);
    if (!info) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    int64_t album_id = info->album_id;
    GArray* album_tracks = NULL;
    if (album_id > 0 && (size_t)album_id < cache->album_tracks_capacity) {
        album_tracks = cache->album_tracks[album_id];
    }
    if (!album_tracks || album_tracks->len == 0) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    for (guint i = 0; i < album_tracks->len; i++) {
        int64_t track_id = g_array_index(album_tracks, int64_t, i);
        if (track_id == current_track_id) {
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

int64_t library_cache_get_prev_track_id(library_cache_t* cache,
                                        int64_t current_track_id) {
    if (!cache || current_track_id <= 0) return 0;

    g_mutex_lock(&cache->lock);

    const library_track_info_t* info = get_track_unlocked(cache, current_track_id);
    if (!info) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    int64_t album_id = info->album_id;
    GArray* album_tracks = NULL;
    if (album_id > 0 && (size_t)album_id < cache->album_tracks_capacity) {
        album_tracks = cache->album_tracks[album_id];
    }
    if (!album_tracks || album_tracks->len == 0) {
        g_mutex_unlock(&cache->lock);
        return 0;
    }

    for (guint i = 0; i < album_tracks->len; i++) {
        int64_t track_id = g_array_index(album_tracks, int64_t, i);
        if (track_id == current_track_id) {
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

// =============================================================================
// List Queries
// =============================================================================

const GPtrArray* library_cache_get_tracks_by_album(library_cache_t* cache,
                                                    int64_t album_id) {
    if (!cache || album_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);

    // Check if we already have a cached GPtrArray for this album
    if (album_id > 0 && (size_t)album_id < cache->album_tracks_ptrs_capacity
        && cache->album_tracks_ptrs[album_id]) {
        GPtrArray* cached = cache->album_tracks_ptrs[album_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    // First check album_tracks (GArray of IDs)
    if (album_id > 0 && (size_t)album_id < cache->album_tracks_capacity
        && !cache->album_tracks[album_id]) {
        load_album_tracks(cache, cache->db, album_id);
    }

    GArray* track_ids = NULL;
    if (album_id > 0 && (size_t)album_id < cache->album_tracks_capacity) {
        track_ids = cache->album_tracks[album_id];
    }
    if (!track_ids || track_ids->len == 0) {
        g_mutex_unlock(&cache->lock);
        return NULL;
    }

    // Build GPtrArray of track info pointers
    GPtrArray* result = g_ptr_array_new();
    for (guint i = 0; i < track_ids->len; i++) {
        int64_t track_id = g_array_index(track_ids, int64_t, i);
        const library_track_info_t* track = get_track_unlocked(cache, track_id);
        if (track) {
            g_ptr_array_add(result, (gpointer)track);
        }
    }

    // Cache the result
    if (album_id > 0 && (size_t)album_id < cache->album_tracks_ptrs_capacity) {
        cache->album_tracks_ptrs[album_id] = result;
    }

    g_mutex_unlock(&cache->lock);
    return result;
}

const GPtrArray* library_cache_get_albums_by_artist(library_cache_t* cache,
                                                     int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);

    // Check cache
    if (artist_id > 0 && (size_t)artist_id < cache->artist_albums_capacity
        && cache->artist_albums[artist_id]) {
        GPtrArray* cached = cache->artist_albums[artist_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    // Cache miss — fetch from DB
    g_warning("Cache MISS → DB fallback: get_albums_by_artist(artist_id=%" G_GINT64_FORMAT ")", artist_id);
    db_album_t* db_albums = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_albums_by_artist(cache->db, artist_id, &db_albums, &count);

    if (res != QUADRATURE_OK || !db_albums || count == 0) {
        /* Cache empty result to prevent repeated DB queries */
        if (artist_id > 0 && (size_t)artist_id < cache->artist_albums_capacity
            && !cache->artist_albums[artist_id]) {
            cache->artist_albums[artist_id] = g_ptr_array_new();
        }
        g_mutex_unlock(&cache->lock);
        if (db_albums) db_albums_free(db_albums, count);
        return NULL;
    }

    GPtrArray* result = g_ptr_array_new();
    for (size_t i = 0; i < count; i++) {
        library_album_info_t* info = cache_get_album(cache, db_albums[i].id);
        if (!info) {
            info = g_new0(library_album_info_t, 1);
            info->album_id = db_albums[i].id;
            info->artist_id = db_albums[i].artist_id;
            info->title = g_strdup(db_albums[i].title);
            info->artist_name = g_strdup(db_albums[i].artist_name);
            info->path = g_strdup("");
            info->genres = db_albums[i].genres ? g_strdup(db_albums[i].genres) : NULL;
            info->year = db_albums[i].year;
            info->track_count = (uint16_t)db_albums[i].track_count;
            info->disc_count = 1;
            info->total_duration_ms = 0;
            cache_set_album(cache, db_albums[i].id, info);
        }
        g_ptr_array_add(result, info);
    }
    db_albums_free(db_albums, count);

    if (artist_id > 0 && (size_t)artist_id < cache->artist_albums_capacity) {
        cache->artist_albums[artist_id] = result;
    }

    g_mutex_unlock(&cache->lock);
    return result;
}

// =============================================================================
// "Appears On" Queries
// =============================================================================

const GPtrArray* library_cache_get_artist_appearances(library_cache_t* cache,
                                                       int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);

    if (artist_id > 0 && (size_t)artist_id < cache->artist_appearances_capacity
        && cache->artist_appearances[artist_id]) {
        GPtrArray* cached = cache->artist_appearances[artist_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    // Cache miss — fetch from database (on-demand fallback)
    db_album_t* db_albums = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_artist_appearances(cache->db, artist_id, &db_albums, &count);

    GPtrArray* result = g_ptr_array_new();

    if (res == QUADRATURE_OK && db_albums && count > 0) {
        for (size_t i = 0; i < count; i++) {
            library_album_info_t* info = cache_get_album(cache, db_albums[i].id);
            if (!info) {
                info = g_new0(library_album_info_t, 1);
                info->album_id = db_albums[i].id;
                info->artist_id = db_albums[i].artist_id;
                info->title = g_strdup(db_albums[i].title);
                info->artist_name = g_strdup(db_albums[i].artist_name);
                info->path = g_strdup("");
                info->genres = db_albums[i].genres ? g_strdup(db_albums[i].genres) : NULL;
                info->year = db_albums[i].year;
                info->track_count = (uint16_t)db_albums[i].track_count;
                info->disc_count = 1;
                info->total_duration_ms = 0;
                cache_set_album(cache, db_albums[i].id, info);
            }
            g_ptr_array_add(result, info);
        }
        db_albums_free(db_albums, count);
    }

    if (artist_id > 0 && (size_t)artist_id < cache->artist_appearances_capacity) {
        cache->artist_appearances[artist_id] = result;
    }

    g_mutex_unlock(&cache->lock);
    return result;
}

const GPtrArray* library_cache_get_artist_appearance_tracks(library_cache_t* cache,
                                                             int64_t artist_id) {
    if (!cache || artist_id <= 0) return NULL;

    g_mutex_lock(&cache->lock);

    if (artist_id > 0 && (size_t)artist_id < cache->artist_appearance_tracks_capacity
        && cache->artist_appearance_tracks[artist_id]) {
        GPtrArray* cached = cache->artist_appearance_tracks[artist_id];
        g_mutex_unlock(&cache->lock);
        return cached;
    }

    // Cache miss — fetch from database (on-demand fallback)
    db_track_t* db_tracks = NULL;
    size_t count = 0;
    quadrature_result_t res = db_get_artist_appearance_tracks(cache->db, artist_id, &db_tracks, &count);

    GPtrArray* result = g_ptr_array_new();

    if (res == QUADRATURE_OK && db_tracks && count > 0) {
        for (size_t i = 0; i < count; i++) {
            library_track_info_t* info = cache_get_track(cache, db_tracks[i].id);
            if (!info) {
                info = g_new0(library_track_info_t, 1);
                info->track_id = db_tracks[i].id;
                info->album_id = db_tracks[i].album_id;
                info->artist_id = db_tracks[i].artist_id;
                info->path = g_strdup(db_tracks[i].path);
                info->title = g_strdup(db_tracks[i].title);
                info->artist_name = g_strdup(db_tracks[i].artist);
                info->artist_display = db_tracks[i].artist_display ? g_strdup(db_tracks[i].artist_display) : NULL;
                info->album_title = g_strdup(db_tracks[i].album);
                info->genre = db_tracks[i].genre ? g_strdup(db_tracks[i].genre) : NULL;
                info->duration_ms = db_tracks[i].duration_ms;
                info->track_num = db_tracks[i].track_num;
                info->disc_num = db_tracks[i].disc_num;
                info->year = db_tracks[i].year;
                cache_set_track(cache, db_tracks[i].id, info);
            }
            g_ptr_array_add(result, info);
        }
        db_tracks_free(db_tracks, count);
    }

    if (artist_id > 0 && (size_t)artist_id < cache->artist_appearance_tracks_capacity) {
        cache->artist_appearance_tracks[artist_id] = result;
    }

    g_mutex_unlock(&cache->lock);
    return result;
}

// =============================================================================
// Filtered Queries (ID-only SQL + cache resolution)
// =============================================================================

GPtrArray* library_cache_get_artists_filtered(library_cache_t* cache,
                                               library_sort_t sort,
                                               const char* search_text,
                                               const db_search_opts_t* filters) {
    g_assert(cache != NULL);

    db_id_query_opts_t opts = {
        .search_text = search_text,
        .filters = filters,
        .sort = library_sort_to_db_sort(sort),
    };

    int64_t* ids = NULL;
    size_t count = 0;
    db_get_artist_ids_filtered(cache->db, &opts, &ids, &count);

    GPtrArray* result = g_ptr_array_new();

    g_mutex_lock(&cache->lock);
    for (size_t i = 0; i < count; i++) {
        int64_t id = ids[i];
        library_artist_info_t* info = get_artist_unlocked(cache, id);
        if (info) {
            g_ptr_array_add(result, info);
        }
    }
    g_mutex_unlock(&cache->lock);

    g_free(ids);
    return result;
}

GPtrArray* library_cache_get_albums_filtered(library_cache_t* cache,
                                              library_sort_t sort,
                                              const char* search_text,
                                              const db_search_opts_t* filters) {
    g_assert(cache != NULL);

    db_id_query_opts_t opts = {
        .search_text = search_text,
        .filters = filters,
        .sort = library_sort_to_db_sort(sort),
    };

    int64_t* ids = NULL;
    size_t count = 0;
    db_get_album_ids_filtered(cache->db, &opts, &ids, &count);

    GPtrArray* result = g_ptr_array_new();

    g_mutex_lock(&cache->lock);
    for (size_t i = 0; i < count; i++) {
        int64_t id = ids[i];
        library_album_info_t* info = get_album_unlocked(cache, id);
        if (info) {
            g_ptr_array_add(result, info);
        }
    }
    g_mutex_unlock(&cache->lock);

    g_free(ids);
    return result;
}

// =============================================================================
// Search (ID-only + cache resolution)
// =============================================================================

library_search_results_t* library_cache_search(library_cache_t* cache,
                                                const char* query,
                                                library_search_filter_t filter,
                                                size_t limit,
                                                const db_search_opts_t* opts) {
    if (!cache || !query) return NULL;

    library_search_results_t* results = g_new0(library_search_results_t, 1);

    // Artists
    if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_ARTISTS) {
        db_id_query_opts_t id_opts = {
            .search_text = query,
            .filters = opts,
            .sort = DB_SORT_NAME_ASC,
        };

        int64_t* ids = NULL;
        size_t count = 0;
        db_get_artist_ids_filtered(cache->db, &id_opts, &ids, &count);

        size_t max = (filter == LIBRARY_SEARCH_FILTER_ALL) ? (limit > 0 ? limit : 5)
                                                            : (limit > 0 ? limit : 100);
        if (count > max) count = max;

        results->artists = g_ptr_array_new();
        g_mutex_lock(&cache->lock);
        for (size_t i = 0; i < count; i++) {
            library_artist_info_t* info = get_artist_unlocked(cache, ids[i]);
            if (info) g_ptr_array_add(results->artists, info);
        }
        g_mutex_unlock(&cache->lock);
        results->total_artists = results->artists->len;
        g_free(ids);
    } else {
        results->artists = g_ptr_array_new();
    }

    // Albums
    if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_ALBUMS) {
        db_id_query_opts_t id_opts = {
            .search_text = query,
            .filters = opts,
            .sort = DB_SORT_NAME_ASC,
        };

        // For albums, search_text matches album title (not artist name for search)
        // We use the existing album ID query which matches title OR artist
        int64_t* ids = NULL;
        size_t count = 0;
        db_get_album_ids_filtered(cache->db, &id_opts, &ids, &count);

        size_t max = (filter == LIBRARY_SEARCH_FILTER_ALL) ? (limit > 0 ? limit : 5)
                                                            : (limit > 0 ? limit : 100);
        if (count > max) count = max;

        results->albums = g_ptr_array_new();
        g_mutex_lock(&cache->lock);
        for (size_t i = 0; i < count; i++) {
            library_album_info_t* info = cache_get_album(cache, ids[i]);
            if (info) g_ptr_array_add(results->albums, info);
        }
        g_mutex_unlock(&cache->lock);
        results->total_albums = results->albums->len;
        g_free(ids);
    } else {
        results->albums = g_ptr_array_new();
    }

    // Tracks (use FTS)
    if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_TRACKS) {
        size_t max = (filter == LIBRARY_SEARCH_FILTER_ALL) ? (limit > 0 ? limit : 10)
                                                            : (limit > 0 ? limit : 100);

        int64_t* ids = NULL;
        size_t count = 0;
        db_search_track_ids(cache->db, query, opts, max, &ids, &count);

        results->tracks = g_ptr_array_new();
        g_mutex_lock(&cache->lock);
        for (size_t i = 0; i < count; i++) {
            library_track_info_t* info = cache_get_track(cache, ids[i]);
            if (!info) {
                // On-demand fetch for tracks not yet warmed
                info = fetch_track_from_db(cache->db, ids[i]);
                if (info) {
                    cache_set_track(cache, ids[i], info);
                }
            }
            if (info) g_ptr_array_add(results->tracks, info);
        }
        g_mutex_unlock(&cache->lock);
        results->total_tracks = results->tracks->len;
        g_free(ids);
    } else {
        results->tracks = g_ptr_array_new();
    }

    return results;
}

void library_search_results_free(library_search_results_t* results) {
    if (!results) return;
    // Items are cache-owned, not copies — just free the arrays
    if (results->artists) g_ptr_array_unref(results->artists);
    if (results->albums) g_ptr_array_unref(results->albums);
    if (results->tracks) g_ptr_array_unref(results->tracks);
    g_free(results);
}

// =============================================================================
// Prefetch API
// =============================================================================

void library_cache_prefetch_fullsize_artwork(library_cache_t* cache,
                                             int64_t album_id) {
    if (!cache || album_id <= 0) return;

    g_mutex_lock(&cache->lock);

    const library_album_info_t* album = cache_get_album(cache, album_id);

    if (!album) {
        g_mutex_unlock(&cache->lock);
        album = library_cache_get_album(cache, album_id);
        if (!album) return;
        g_mutex_lock(&cache->lock);
    }

    if (cache->music_base && album->path && album->path[0]) {
        char album_path[PATH_MAX];
        snprintf(album_path, sizeof(album_path), "%s/%s", cache->music_base, album->path);

        static const char* art_names[] = {
            "art.jpg", "cover.jpg", "folder.jpg", "album.jpg", "front.jpg",
            "art.png", "cover.png", "folder.png", "album.png", "front.png",
            NULL
        };

        for (const char** name = art_names; *name; name++) {
            char art_path[PATH_MAX + 32];
            snprintf(art_path, sizeof(art_path), "%s/%s", album_path, *name);
            if (access(art_path, R_OK) == 0) {
                prefetch_file(art_path);
                break;
            }
        }
    }

    g_mutex_unlock(&cache->lock);
}

void library_cache_prefetch_audio_files(library_cache_t* cache,
                                        const int64_t* track_ids,
                                        size_t count) {
    if (!cache || !track_ids || count == 0) return;

    g_mutex_lock(&cache->lock);

    for (size_t i = 0; i < count; i++) {
        const library_track_info_t* track = get_track_unlocked(cache, track_ids[i]);
        if (track && track->path) {
            prefetch_file(track->path);
        }
    }

    g_mutex_unlock(&cache->lock);
}

// =============================================================================
// Cache Management
// =============================================================================

void library_cache_clear(library_cache_t* cache) {
    g_assert(cache != NULL);

    // Cancel and join warming thread before clearing
    cancel_and_join_warming(cache);

    g_mutex_lock(&cache->lock);

    free_entity_arrays(cache);

    // Re-allocate fresh arrays
    allocate_entity_arrays(cache);

    // Reset warming state
    atomic_store(&cache->warm_state, LIBRARY_CACHE_IDLE);

    g_mutex_unlock(&cache->lock);
}
