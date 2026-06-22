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
 * finishes warming, build_mbid_indices() builds sorted arrays for O(log n)
 * cross-library MBID resolution, then fires the ready callback.
 * Slots keep only their own data — deduplication happens at query time.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/library.h"
#include "quadrature/database.h"
#include "quadrature/metadata.h"

#include <sqlite3.h>
#include <stdlib.h>

/* library_mask_after_toggle / library_mask_solo live in library_mask.c. */

/* Forward declarations for helpers defined after the warming thread.
 * library_mask_is_multi_library / library_build_corrected_query are exported
 * via internal.h for sibling search files. */
static void build_search_vocab_slot(struct library_cache *cache, int slot_idx);
static const char *correct_token(struct library_cache *cache, const char *token);
static void build_mbid_indices(struct library_cache *cache);

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdatomic.h>
#include <glib.h>

/* LibrarySlot and struct library_cache live in internal.h so that
 * cache_search.c and library_search.c can walk them directly. */

/* =============================================================================
 * Memory Management Helpers
 *
 * All entity structs are allocated via g_atomic_rc_box so they can be shared
 * between old and new cache slots during COW refresh.  The clear_*() functions
 * free owned strings/arrays but NOT the struct itself — g_atomic_rc_box
 * handles the final deallocation when the refcount drops to zero.
 * ============================================================================= */

static void
clear_artist_info(gpointer ptr)
{
    library_artist_info_t *info = ptr;
    g_free(info->name);
    g_free(info->musicbrainz_id);
}

static void
release_artist_info(library_artist_info_t *info)
{
    if (!info)
        return;
    g_atomic_rc_box_release_full(info, clear_artist_info);
}

static void
clear_album_info(gpointer ptr)
{
    library_album_info_t *info = ptr;
    g_free(info->title);
    g_free(info->artist_name);
    g_free(info->path);
    g_free(info->genres);
    g_free(info->musicbrainz_release_id);
    g_free(info->musicbrainz_release_group_id);
}

static void
release_album_info(library_album_info_t *info)
{
    if (!info)
        return;
    g_atomic_rc_box_release_full(info, clear_album_info);
}

static void
clear_track_info(gpointer ptr)
{
    library_track_info_t *info = ptr;
    g_free(info->path);
    g_free(info->title);
    g_free(info->artist_display);
    g_free(info->album_title);
    g_free(info->genre);
}

static void
release_track_info(library_track_info_t *info)
{
    if (!info)
        return;
    g_atomic_rc_box_release_full(info, clear_track_info);
}

static void
clear_track_artist(gpointer ptr)
{
    library_track_artist_t *artist = ptr;
    g_free(artist->name);
    g_free(artist->join_phrase);
}

static void
release_track_artist(gpointer data)
{
    if (!data)
        return;
    g_atomic_rc_box_release_full(data, clear_track_artist);
}

/* slot_get_artist/album/track live in internal.h (inline, shared with
 * cache_search.c and library_search.c).
 *
 * slot_set_* always *replaces*: if an entity already lives at local_id it is
 * released before the new one is installed. This is what makes the warming
 * callbacks authoritative under COW refresh (see LIBRARY_CACHE.md
 * → "COW Refresh Invariants" → I2). */

static inline void
slot_set_artist(LibrarySlot *slot, int64_t local_id, library_artist_info_t *info)
{
    if (local_id <= 0 || (size_t)local_id >= slot->artists_capacity)
        return;
    library_artist_info_t *old = slot->artists[local_id];
    slot->artists[local_id] = info;
    if (old)
        release_artist_info(old);
}

static inline void
slot_set_album(LibrarySlot *slot, int64_t local_id, library_album_info_t *info)
{
    if (local_id <= 0 || (size_t)local_id >= slot->albums_capacity)
        return;
    library_album_info_t *old = slot->albums[local_id];
    slot->albums[local_id] = info;
    if (old)
        release_album_info(old);
}

static inline void
slot_set_track(LibrarySlot *slot, int64_t local_id, library_track_info_t *info)
{
    if (local_id <= 0 || (size_t)local_id >= slot->tracks_capacity)
        return;
    library_track_info_t *old = slot->tracks[local_id];
    slot->tracks[local_id] = info;
    if (old)
        release_track_info(old);
}

/* =============================================================================
 * Bitmap → Slot Lookup
 * ============================================================================= */

/** Look up slot by bitmap_index. Lock-free for reads. Returns NULL if invalid. */
static inline LibrarySlot *
bitmap_to_slot(library_cache_t *cache, int bitmap_index)
{
    if (bitmap_index < 0 || bitmap_index >= cache->bitmap_capacity)
        return NULL;
    return g_atomic_pointer_get(&cache->bitmap_map[bitmap_index]);
}

/* =============================================================================
 * Global ID Decode Helper — Lock-Free Bitmap Lookup
 * ============================================================================= */

static LibrarySlot *
decode_slot(library_cache_t *cache, int64_t global_id, int64_t *local_id_out)
{
    int bitmap_idx = LIBRARY_GLOBAL_ID_LIB(global_id);
    int64_t local_id = LIBRARY_GLOBAL_ID_LOCAL(global_id);
    if (bitmap_idx < 0 || bitmap_idx >= cache->bitmap_capacity)
        return NULL;
    LibrarySlot *slot = g_atomic_pointer_get(&cache->bitmap_map[bitmap_idx]);
    if (!slot)
        return NULL;
    if (local_id_out)
        *local_id_out = local_id;
    return slot;
}

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

static void
prefetch_file(const char *path)
{
    if (!path)
        return;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;

    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
#ifdef POSIX_FADV_WILLNEED
        posix_fadvise(fd, 0, st.st_size, POSIX_FADV_WILLNEED);
#endif
    }
    close(fd);
}

static char *
resolve_track_path(const char *music_base, const char *album_rel_path, const char *track_rel_path)
{
    if (!track_rel_path)
        return NULL;
    g_assert(track_rel_path[0] != '/');

    char *album_base = g_build_filename(
        music_base ? music_base : "", album_rel_path ? album_rel_path : "", NULL);
    char *full_path = g_canonicalize_filename(track_rel_path, album_base);
    g_free(album_base);
    return full_path;
}

/* =============================================================================
 * Background Prefetch Thread
 * ============================================================================= */

static gpointer
prefetch_thread_func(gpointer data)
{
    library_cache_t *cache = data;
    while (!atomic_load(&cache->prefetch_shutdown)) {
        char *path = g_async_queue_timeout_pop(cache->prefetch_queue, 100 * 1000); /* 100ms */
        if (!path)
            continue;
        prefetch_file(path);
        g_free(path);
    }
    /* Drain remaining items */
    char *path;
    while ((path = g_async_queue_try_pop(cache->prefetch_queue)))
        g_free(path);
    return NULL;
}

/* Convert library_sort_t to db_sort_t for DB queries */
static db_sort_t
library_sort_to_db_sort(library_sort_t sort)
{
    switch (sort) {
    case LIBRARY_SORT_NAME_ASC:
        return DB_SORT_NAME_ASC;
    case LIBRARY_SORT_NAME_DESC:
        return DB_SORT_NAME_DESC;
    case LIBRARY_SORT_YEAR_ASC:
        return DB_SORT_YEAR_ASC;
    case LIBRARY_SORT_YEAR_DESC:
        return DB_SORT_YEAR_DESC;
    case LIBRARY_SORT_ARTIST_ASC:
        return DB_SORT_ARTIST_ASC;
    case LIBRARY_SORT_RECENT:
        return DB_SORT_RECENT;
    default:
        return DB_SORT_NAME_ASC;
    }
}

/* =============================================================================
 * Slot Array Freeing / Allocating
 * ============================================================================= */

static void
free_slot_arrays(LibrarySlot *slot)
{
    if (slot->artists) {
        for (size_t i = 0; i < slot->artists_capacity; i++)
            if (slot->artists[i])
                release_artist_info(slot->artists[i]);
        g_free(slot->artists);
        slot->artists = NULL;
    }
    slot->artists_capacity = 0;

    if (slot->albums) {
        for (size_t i = 0; i < slot->albums_capacity; i++)
            if (slot->albums[i])
                release_album_info(slot->albums[i]);
        g_free(slot->albums);
        slot->albums = NULL;
    }
    slot->albums_capacity = 0;

    if (slot->tracks) {
        for (size_t i = 0; i < slot->tracks_capacity; i++)
            if (slot->tracks[i])
                release_track_info(slot->tracks[i]);
        g_free(slot->tracks);
        slot->tracks = NULL;
    }
    slot->tracks_capacity = 0;

    if (slot->album_tracks) {
        for (size_t i = 0; i < slot->album_tracks_capacity; i++)
            if (slot->album_tracks[i])
                g_array_unref(slot->album_tracks[i]);
        g_free(slot->album_tracks);
        slot->album_tracks = NULL;
    }
    slot->album_tracks_capacity = 0;

    if (slot->track_artists) {
        for (size_t i = 0; i < slot->track_artists_capacity; i++)
            if (slot->track_artists[i])
                g_ptr_array_unref(slot->track_artists[i]);
        g_free(slot->track_artists);
        slot->track_artists = NULL;
    }
    slot->track_artists_capacity = 0;

    if (slot->artist_albums) {
        for (size_t i = 0; i < slot->artist_albums_capacity; i++)
            if (slot->artist_albums[i])
                g_ptr_array_unref(slot->artist_albums[i]);
        g_free(slot->artist_albums);
        slot->artist_albums = NULL;
    }
    slot->artist_albums_capacity = 0;

    if (slot->artist_appearances) {
        for (size_t i = 0; i < slot->artist_appearances_capacity; i++)
            if (slot->artist_appearances[i])
                g_ptr_array_unref(slot->artist_appearances[i]);
        g_free(slot->artist_appearances);
        slot->artist_appearances = NULL;
    }
    slot->artist_appearances_capacity = 0;

    if (slot->artist_appearance_tracks) {
        for (size_t i = 0; i < slot->artist_appearance_tracks_capacity; i++)
            if (slot->artist_appearance_tracks[i])
                g_ptr_array_unref(slot->artist_appearance_tracks[i]);
        g_free(slot->artist_appearance_tracks);
        slot->artist_appearance_tracks = NULL;
    }
    slot->artist_appearance_tracks_capacity = 0;

    if (slot->album_tracks_ptrs) {
        for (size_t i = 0; i < slot->album_tracks_ptrs_capacity; i++)
            if (slot->album_tracks_ptrs[i])
                g_ptr_array_unref(slot->album_tracks_ptrs[i]);
        g_free(slot->album_tracks_ptrs);
        slot->album_tracks_ptrs = NULL;
    }
    slot->album_tracks_ptrs_capacity = 0;
}

/* Allocate all per-slot entity arrays. Prefer db_warm to avoid main-thread contention. */
static void
allocate_slot_arrays(LibrarySlot *slot)
{
    quadrature_db_t *db = slot->db_warm ? slot->db_warm : slot->db;
    int64_t max_artist = 0, max_album = 0, max_track = 0;
    db_get_max_ids(db, &max_artist, &max_album, &max_track);

    slot->artists_capacity = (size_t)(max_artist + 1);
    slot->albums_capacity = (size_t)(max_album + 1);
    slot->tracks_capacity = (size_t)(max_track + 1);

    slot->artists = g_new0(library_artist_info_t *, slot->artists_capacity);
    slot->albums = g_new0(library_album_info_t *, slot->albums_capacity);
    slot->tracks = g_new0(library_track_info_t *, slot->tracks_capacity);

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

static void
cancel_and_join_slot_warming(LibrarySlot *slot)
{
    if (slot->warm_thread) {
        atomic_store(&slot->warm_cancel, 1);
        g_thread_join(slot->warm_thread);
        slot->warm_thread = NULL;
        atomic_store(&slot->warm_cancel, 0);
    }
}

/* Sort tracks by (disc_num, track_num) for album display order */
static gint
cmp_track_disc_num(gconstpointer a, gconstpointer b)
{
    const library_track_info_t *ta = *(const library_track_info_t *const *)a;
    const library_track_info_t *tb = *(const library_track_info_t *const *)b;
    if (ta->disc_num != tb->disc_num)
        return ta->disc_num - tb->disc_num;
    return ta->track_num - tb->track_num;
}

/* Swap all entity/relationship array pointers and capacities between two slots.
 * Used by COW refresh: install freshly-built shadow arrays into the live slot
 * and leave the old arrays in `b` for draining. db/state fields are untouched. */
static void
swap_slot_arrays(LibrarySlot *a, LibrarySlot *b)
{
#define SWAP_FIELD(f)               \
    do {                            \
        __typeof__(a->f) _t = a->f; \
        a->f = b->f;                \
        b->f = _t;                  \
    } while (0)
    SWAP_FIELD(artists);
    SWAP_FIELD(artists_capacity);
    SWAP_FIELD(albums);
    SWAP_FIELD(albums_capacity);
    SWAP_FIELD(tracks);
    SWAP_FIELD(tracks_capacity);
    SWAP_FIELD(track_artists);
    SWAP_FIELD(track_artists_capacity);
    SWAP_FIELD(album_tracks);
    SWAP_FIELD(album_tracks_capacity);
    SWAP_FIELD(artist_albums);
    SWAP_FIELD(artist_albums_capacity);
    SWAP_FIELD(artist_appearances);
    SWAP_FIELD(artist_appearances_capacity);
    SWAP_FIELD(artist_appearance_tracks);
    SWAP_FIELD(artist_appearance_tracks_capacity);
    SWAP_FIELD(album_tracks_ptrs);
    SWAP_FIELD(album_tracks_ptrs_capacity);
#undef SWAP_FIELD
}

/* Compute relationship arrays and derived aggregates from the already-populated
 * entity/track_artist arrays of a slot. Shared by initial warming and COW
 * refresh; both call it on freshly g_new0()'d relationship arrays, so the
 * defensive resets (album_count/track_count, prior genres) are no-ops on the
 * warm path. If out_appearances is non-NULL it receives the count of distinct
 * (artist, album) "appears on" links added (for logging). */
static void
rebuild_slot_relationships(LibrarySlot *s, size_t *out_appearances)
{
    size_t n_appearances = 0;

    /* Pass A: albums → artist_albums */
    for (size_t local_album_id = 1; local_album_id < s->albums_capacity; local_album_id++) {
        library_album_info_t *album = slot_get_album(s, (int64_t)local_album_id);
        if (!album)
            continue;
        int64_t local_aid = LIBRARY_GLOBAL_ID_LOCAL(album->artist_id);
        if (local_aid <= 0 || (size_t)local_aid >= s->artist_albums_capacity)
            continue;
        if (!s->artist_albums[local_aid])
            s->artist_albums[local_aid] = g_ptr_array_new();
        g_ptr_array_add(s->artist_albums[local_aid], album);
    }

    /* Pass B: reset + set album_count on each artist */
    for (size_t aid = 1; aid < s->artists_capacity; aid++) {
        library_artist_info_t *a = slot_get_artist(s, (int64_t)aid);
        if (!a)
            continue;
        a->album_count = 0;
        a->track_count = 0;
        if (aid < s->artist_albums_capacity) {
            if (!s->artist_albums[aid])
                s->artist_albums[aid] = g_ptr_array_new();
            a->album_count = s->artist_albums[aid]->len;
        }
    }

    /* Pass C: track_artists → "Appears on" + artist track_count */
    for (size_t tid = 1; tid < s->tracks_capacity; tid++) {
        library_track_info_t *track = slot_get_track(s, (int64_t)tid);
        if (!track)
            continue;
        if ((size_t)tid >= s->track_artists_capacity || !s->track_artists[tid])
            continue;

        GPtrArray *ta = s->track_artists[tid];
        int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
        library_album_info_t *album = slot_get_album(s, local_album_id);
        int64_t album_local_artist = album ? LIBRARY_GLOBAL_ID_LOCAL(album->artist_id) : -1;

        for (guint j = 0; j < ta->len; j++) {
            library_track_artist_t *credit = g_ptr_array_index(ta, j);
            int64_t credit_local_aid = LIBRARY_GLOBAL_ID_LOCAL(credit->artist_id);

            library_artist_info_t *a
                = (credit_local_aid > 0 && (size_t)credit_local_aid < s->artists_capacity)
                      ? slot_get_artist(s, credit_local_aid)
                      : NULL;
            if (a)
                a->track_count++;

            if (!album || credit_local_aid == album_local_artist)
                continue;
            if (credit_local_aid <= 0 || (size_t)credit_local_aid >= s->artist_appearances_capacity)
                continue;

            if (!s->artist_appearances[credit_local_aid])
                s->artist_appearances[credit_local_aid] = g_ptr_array_new();

            GPtrArray *app_albums = s->artist_appearances[credit_local_aid];
            gboolean found = FALSE;
            for (guint k = 0; k < app_albums->len; k++) {
                if (g_ptr_array_index(app_albums, k) == album) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) {
                g_ptr_array_add(app_albums, album);
                n_appearances++;
            }

            if ((size_t)credit_local_aid < s->artist_appearance_tracks_capacity) {
                if (!s->artist_appearance_tracks[credit_local_aid])
                    s->artist_appearance_tracks[credit_local_aid] = g_ptr_array_new();
                g_ptr_array_add(s->artist_appearance_tracks[credit_local_aid], track);
            }
        }
    }

    /* Pass D: album_tracks_ptrs, sorted; propagate first_track_id + genres */
    for (size_t aid = 1; aid < s->album_tracks_capacity; aid++) {
        GArray *track_ids = s->album_tracks[aid];
        if (!track_ids || track_ids->len == 0)
            continue;
        if (aid >= s->album_tracks_ptrs_capacity)
            continue;
        if (s->album_tracks_ptrs[aid])
            continue;

        GPtrArray *ptrs = g_ptr_array_sized_new(track_ids->len);
        for (guint i = 0; i < track_ids->len; i++) {
            int64_t global_tid = g_array_index(track_ids, int64_t, i);
            int64_t local_tid = LIBRARY_GLOBAL_ID_LOCAL(global_tid);
            library_track_info_t *track = slot_get_track(s, local_tid);
            if (track)
                g_ptr_array_add(ptrs, track);
        }
        g_ptr_array_sort(ptrs, cmp_track_disc_num);
        s->album_tracks_ptrs[aid] = ptrs;

        library_album_info_t *album = slot_get_album(s, (int64_t)aid);
        if (!album)
            continue;
        album->track_count = (uint16_t)ptrs->len;
        if (ptrs->len > 0) {
            library_track_info_t *first = g_ptr_array_index(ptrs, 0);
            album->first_track_id = first->track_id;
        }

        /* Collect distinct lowercase genres */
        g_free(album->genres);
        album->genres = NULL;
        GHashTable *genre_set = NULL;
        for (guint gi = 0; gi < ptrs->len; gi++) {
            library_track_info_t *t = g_ptr_array_index(ptrs, gi);
            if (!t->genre || !t->genre[0])
                continue;
            if (!genre_set)
                genre_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            g_hash_table_add(genre_set, g_ascii_strdown(t->genre, -1));
        }
        if (genre_set) {
            GString *gs = g_string_new(NULL);
            GHashTableIter iter;
            gpointer key;
            g_hash_table_iter_init(&iter, genre_set);
            while (g_hash_table_iter_next(&iter, &key, NULL)) {
                if (gs->len > 0)
                    g_string_append_c(gs, ';');
                g_string_append(gs, (const char *)key);
            }
            album->genres = g_string_free(gs, FALSE);
            g_hash_table_destroy(genre_set);
        }
    }

    if (out_appearances)
        *out_appearances = n_appearances;
}

/* ── Phase 1-2 streaming callbacks ─────────────────────────────────────── */

static bool
on_warm_artist(const db_artist_t *a, void *data)
{
    LibrarySlot *slot = data;
    if (atomic_load(&slot->warm_cancel))
        return false;

    /* Callback is authoritative: always install fresh, replacing any prior
     * occupant. See LIBRARY_CACHE.md → "COW Refresh Invariants" → I2. */
    library_artist_info_t *info = g_atomic_rc_box_alloc0(sizeof(library_artist_info_t));
    info->artist_id = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, a->id);
    info->library_index = slot->bitmap_index;
    info->name = g_strdup(a->name);
    info->musicbrainz_id = a->musicbrainz_id ? g_strdup(a->musicbrainz_id) : NULL;
    /* album_count, track_count left 0 — computed in Phase 4 */
    slot_set_artist(slot, a->id, info);
    return true;
}

static bool
on_warm_album(const db_album_t *a, void *data)
{
    LibrarySlot *slot = data;
    if (atomic_load(&slot->warm_cancel))
        return false;

    /* Authoritative — replaces any prior occupant (see I2). */
    library_album_info_t *info = g_atomic_rc_box_alloc0(sizeof(library_album_info_t));
    info->album_id = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, a->id);
    info->artist_id = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, a->artist_id);
    info->library_index = slot->bitmap_index;
    info->title = g_strdup(a->title);
    info->path = g_strdup(a->path ? a->path : "");
    info->year = a->year;
    info->musicbrainz_release_id
        = a->musicbrainz_release_id ? g_strdup(a->musicbrainz_release_id) : NULL;
    info->musicbrainz_release_group_id
        = a->musicbrainz_release_group_id ? g_strdup(a->musicbrainz_release_group_id) : NULL;
    /* Resolve artist_name from Phase 1 data */
    library_artist_info_t *artist = slot_get_artist(slot, a->artist_id);
    info->artist_name = g_strdup(artist ? artist->name : "Unknown Artist");
    /* track_count=0, genres=NULL — computed in Phase 4 */
    slot_set_album(slot, a->id, info);
    return true;
}

/* ── Phase 3 streaming callback ───────────────────────────────────────── */

typedef struct {
    LibrarySlot *slot;
    size_t loaded;
} WarmTracksCtx;

static bool
on_warm_track(const db_track_lean_t *t, void *data)
{
    WarmTracksCtx *ctx = data;
    LibrarySlot *slot = ctx->slot;

    if (atomic_load(&slot->warm_cancel))
        return false;

    int64_t local_tid = t->id;
    int64_t global_tid = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, local_tid);

    /* Append to album_tracks — tracks arrive in rowid order, not album order,
     * so we lazily init per-album arrays and sort in Phase 4. */
    if (t->album_id > 0 && (size_t)t->album_id < slot->album_tracks_capacity) {
        if (!slot->album_tracks[t->album_id])
            slot->album_tracks[t->album_id] = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), 16);
        g_array_append_val(slot->album_tracks[t->album_id], global_tid);
    }

    /* Authoritative — replaces any prior occupant (see I2). */
    library_track_info_t *info = g_atomic_rc_box_alloc0(sizeof(library_track_info_t));
    info->track_id = global_tid;
    info->album_id = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, t->album_id);
    info->library_index = slot->bitmap_index;
    info->path = g_strdup(t->path ? t->path : "");
    info->title = g_strdup(t->title);
    info->genre = t->genre ? g_strdup(t->genre) : NULL;
    info->duration_ms = t->duration_ms;
    info->track_num = t->track_num;
    info->disc_num = t->disc_num;
    info->year = t->year;

    /* Resolve from already-loaded Phase 1-2 data (no JOINs in track query) */
    info->artist_display = t->artist_display ? g_strdup(t->artist_display) : NULL;
    library_album_info_t *album = slot_get_album(slot, t->album_id);
    info->album_title = g_strdup(album ? album->title : "Unknown Album");

    /* artist_id set to 0 here — resolved in Phase 3.5 for position=0 entry */
    slot_set_track(slot, local_tid, info);
    ctx->loaded++;

    return true;
}

/* Fired on main thread after a slot finishes warming */
static gboolean
warming_complete_idle(gpointer data)
{
    library_cache_t *cache = (library_cache_t *)data;
    if (cache->ready_cb) {
        cache->ready_cb(cache->ready_cb_data);
    }
    return G_SOURCE_REMOVE;
}

/* ── Phase 3.5 streaming callback ─────────────────────────────────────── */

typedef struct {
    LibrarySlot *slot;
    int64_t prev_tid;
    GPtrArray *cur_list;
} BulkTrackArtistCtx;

static void
bulk_ta_flush(BulkTrackArtistCtx *ctx)
{
    if (ctx->cur_list && ctx->prev_tid > 0
        && (size_t)ctx->prev_tid < ctx->slot->track_artists_capacity) {
        /* Authoritative: release any prior list for this track before install. */
        GPtrArray *old = ctx->slot->track_artists[ctx->prev_tid];
        ctx->slot->track_artists[ctx->prev_tid] = ctx->cur_list;
        ctx->cur_list = NULL;
        if (old)
            g_ptr_array_unref(old);
    } else if (ctx->cur_list) {
        g_ptr_array_unref(ctx->cur_list);
        ctx->cur_list = NULL;
    }
}

static bool
on_bulk_track_artist(
    int64_t track_id, int64_t artist_id, const char *join_phrase, int position, void *user_data)
{
    BulkTrackArtistCtx *c = user_data;
    if (track_id != c->prev_tid) {
        bulk_ta_flush(c);
        c->prev_tid = track_id;
        c->cur_list = g_ptr_array_new_with_free_func(release_track_artist);
    }
    if (!c->cur_list)
        return true;

    /* Resolve artist name from Phase 1 data (no JOIN in query) */
    library_artist_info_t *artist = slot_get_artist(c->slot, artist_id);

    library_track_artist_t *ta = g_atomic_rc_box_alloc0(sizeof(library_track_artist_t));
    ta->artist_id = LIBRARY_MAKE_GLOBAL_ID(c->slot->bitmap_index, artist_id);
    ta->name = g_strdup(artist ? artist->name : "Unknown Artist");
    ta->join_phrase = g_strdup(join_phrase);
    ta->role = (position == 0) ? LIBRARY_ARTIST_ROLE_PRIMARY : LIBRARY_ARTIST_ROLE_FEATURING;
    ta->position = position;
    g_ptr_array_add(c->cur_list, ta);

    /* Set track's primary artist_id and artist_display (both 0/NULL from Phase 3) */
    if (position == 0) {
        library_track_info_t *track = slot_get_track(c->slot, track_id);
        if (track) {
            if (track->artist_id == 0)
                track->artist_id = LIBRARY_MAKE_GLOBAL_ID(c->slot->bitmap_index, artist_id);
            if (!track->artist_display)
                track->artist_display = g_strdup(artist ? artist->name : "Unknown Artist");
        }
    }

    return true;
}

static gpointer
slot_warming_thread_func(gpointer data)
{
    LibrarySlot *slot = (LibrarySlot *)data;
    library_cache_t *cache = slot->cache;

    gint64 wall_start = g_get_monotonic_time();
    gint64 sql_us = 0; /* microseconds spent in SQLite */
    gint64 phase_start;
    size_t n_artists = 0, n_albums = 0, n_tracks = 0, n_appearances = 0;

    /* Single read transaction for all SQL phases — consistent snapshot + avoids
     * per-query WAL overhead. */
    db_begin_read(slot->db_warm);

    /* ── Phase 1: Stream all artists (pure rowid scan) ──────────────────── */
    phase_start = g_get_monotonic_time();
    {
        gint64 t0 = g_get_monotonic_time();
        db_iter_all_artists(slot->db_warm, on_warm_artist, slot);
        sql_us += g_get_monotonic_time() - t0;
        for (size_t aid = 1; aid < slot->artists_capacity; aid++)
            if (slot_get_artist(slot, (int64_t)aid))
                n_artists++;
    }
    gint64 phase1_us = g_get_monotonic_time() - phase_start;

    if (atomic_load(&slot->warm_cancel))
        goto done;

    /* ── Phase 2: Stream all albums (pure rowid scan) ───────────────────── */
    phase_start = g_get_monotonic_time();
    {
        gint64 t0 = g_get_monotonic_time();
        db_iter_all_albums(slot->db_warm, on_warm_album, slot);
        sql_us += g_get_monotonic_time() - t0;
        for (size_t aid = 1; aid < slot->albums_capacity; aid++)
            if (slot_get_album(slot, (int64_t)aid))
                n_albums++;
    }
    gint64 phase2_us = g_get_monotonic_time() - phase_start;

    if (atomic_load(&slot->warm_cancel))
        goto done;

    /* ── Phase 3: Stream all tracks (pure rowid scan, no index) ─────────── */
    phase_start = g_get_monotonic_time();
    {
        WarmTracksCtx ctx = { .slot = slot, .loaded = 0 };

        gint64 t0 = g_get_monotonic_time();
        db_iter_all_tracks(slot->db_warm, on_warm_track, &ctx);
        sql_us += g_get_monotonic_time() - t0;

        n_tracks = ctx.loaded;
    }
    gint64 phase3_us = g_get_monotonic_time() - phase_start;

    if (atomic_load(&slot->warm_cancel))
        goto done;

    /* ── Phase 3.5: Stream track artists ────────────────────────────────── */
    phase_start = g_get_monotonic_time();
    {
        BulkTrackArtistCtx ta_ctx = { .slot = slot, .prev_tid = -1, .cur_list = NULL };
        gint64 t0 = g_get_monotonic_time();
        db_iter_all_track_artists(slot->db_warm, on_bulk_track_artist, &ta_ctx);
        sql_us += g_get_monotonic_time() - t0;
        bulk_ta_flush(&ta_ctx);
    }
    gint64 phase35_us = g_get_monotonic_time() - phase_start;

    /* End read transaction — all SQL done */
    db_end_read(slot->db_warm);

    if (atomic_load(&slot->warm_cancel))
        goto done;

    /* ── Phase 4: Compute relationships and derived aggregates ───────────
     * Merged loops to minimize cache pressure on million-entry arrays. */
    phase_start = g_get_monotonic_time();
    rebuild_slot_relationships(slot, &n_appearances);
    gint64 phase4_us = g_get_monotonic_time() - phase_start;

    /* ── Phase 5: Build search vocabulary ────────────────────────────────── */
    phase_start = g_get_monotonic_time();
    build_search_vocab_slot(cache, slot->lib_idx);
    gint64 phase5_us = g_get_monotonic_time() - phase_start;

    /* ── Phase 6: Merge cross-library artists and signal completion ───────
     * No lock: warming thread is exclusive writer to its slot. Merge functions
     * read other slots (READY = immutable). The ready_cb fires via g_idle_add
     * after this, so main-thread readers see the completed merge. */
    phase_start = g_get_monotonic_time();
    build_mbid_indices(cache);
    gint64 phase6_us = g_get_monotonic_time() - phase_start;

    atomic_store(&slot->warm_state, LIBRARY_CACHE_READY);

    if (cache->ready_cb) {
        g_idle_add(warming_complete_idle, cache);
    }

    {
        gint64 wall_us = g_get_monotonic_time() - wall_start;
        double wall_ms = wall_us / 1000.0;
        double sql_pct = wall_us > 0 ? (sql_us * 100.0 / wall_us) : 0;
        const char *name = slot->display_name ? slot->display_name : slot->music_base;

        g_message("library cache [%s] warmed in %.0fms — "
                  "%zu artists, %zu albums, %zu tracks, %zu appearances  "
                  "(%.0f%% sql)",
                  name ? name : "?",
                  wall_ms,
                  n_artists,
                  n_albums,
                  n_tracks,
                  n_appearances,
                  sql_pct);
        g_debug("  timing: artists %.1fms | albums %.1fms | tracks %.1fms | "
                "credits %.1fms | relations %.1fms | vocab %.1fms | merge %.1fms",
                phase1_us / 1000.0,
                phase2_us / 1000.0,
                phase3_us / 1000.0,
                phase35_us / 1000.0,
                phase4_us / 1000.0,
                phase5_us / 1000.0,
                phase6_us / 1000.0);
    }
    return NULL;

done:
    db_end_read(slot->db_warm);
    g_info("cache warming [slot %d]: cancelled", slot->lib_idx);
    return NULL;
}

/* =============================================================================
 * MBID Resolution Indices
 *
 * Sorted arrays for O(log n) cross-library artist/album resolution.
 * Built after each slot warms.  Query functions use these at runtime to
 * collect data from all slots sharing a MusicBrainz ID.
 *
 * Slots keep ONLY their own data — no entity mutation, no combined arrays,
 * no skip sets.  Deduplication happens at query time.
 * ============================================================================= */

static void
free_mbid_artist_index(library_cache_t *cache)
{
    for (size_t i = 0; i < cache->mbid_artist_index.count; i++)
        g_free(cache->mbid_artist_index.entries[i].global_ids);
    g_free(cache->mbid_artist_index.entries);
    cache->mbid_artist_index.entries = NULL;
    cache->mbid_artist_index.count = 0;
}

static void
free_mbrid_album_index(library_cache_t *cache)
{
    for (size_t i = 0; i < cache->mbrid_album_index.count; i++)
        g_free(cache->mbrid_album_index.entries[i].global_ids);
    g_free(cache->mbrid_album_index.entries);
    cache->mbrid_album_index.entries = NULL;
    cache->mbrid_album_index.count = 0;
}

static int
mbid_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct mbid_artist_entry *)a)->mbid,
                  ((const struct mbid_artist_entry *)b)->mbid);
}

static int
mbrid_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct mbrid_album_entry *)a)->mbrid,
                  ((const struct mbrid_album_entry *)b)->mbrid);
}

/** Binary search for an artist MBID. Returns entry or NULL. */
static const struct mbid_artist_entry *
mbid_artist_lookup(const library_cache_t *cache, const char *mbid)
{
    if (!mbid || !cache->mbid_artist_index.entries)
        return NULL;
    struct mbid_artist_entry key;
    memcpy(key.mbid, mbid, 37);
    return bsearch(&key,
                   cache->mbid_artist_index.entries,
                   cache->mbid_artist_index.count,
                   sizeof(struct mbid_artist_entry),
                   mbid_entry_cmp);
}

/** Binary search for an album MBRID. Returns entry or NULL. */
static const struct mbrid_album_entry *
mbrid_album_lookup(const library_cache_t *cache, const char *mbrid)
{
    if (!mbrid || !cache->mbrid_album_index.entries)
        return NULL;
    struct mbrid_album_entry key;
    memcpy(key.mbrid, mbrid, 37);
    return bsearch(&key,
                   cache->mbrid_album_index.entries,
                   cache->mbrid_album_index.count,
                   sizeof(struct mbrid_album_entry),
                   mbrid_entry_cmp);
}

/* ── Public MBID-aware library enumeration ─────────────────────────────── */

int
library_cache_get_artist_libraries(library_cache_t *cache,
                                   int64_t artist_global_id,
                                   int *out_libs,
                                   int max_libs)
{
    if (!cache || !out_libs || max_libs <= 0)
        return 0;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, artist_global_id, &local_id);
    if (!slot)
        return 0;

    library_artist_info_t *a = slot_get_artist(slot, local_id);
    if (!a)
        return 0;

    if (a->musicbrainz_id) {
        const struct mbid_artist_entry *entry = mbid_artist_lookup(cache, a->musicbrainz_id);
        if (entry) {
            int n = 0;
            for (uint8_t i = 0; i < entry->count && n < max_libs; i++) {
                int lib = LIBRARY_GLOBAL_ID_LIB(entry->global_ids[i]);
                /* Dedup — same lib can appear if index is stale */
                bool dup = false;
                for (int j = 0; j < n; j++)
                    if (out_libs[j] == lib) {
                        dup = true;
                        break;
                    }
                if (!dup)
                    out_libs[n++] = lib;
            }
            return n;
        }
    }

    /* Fallback: source library only */
    out_libs[0] = a->library_index;
    return 1;
}

int
library_cache_get_album_libraries(library_cache_t *cache,
                                  int64_t album_global_id,
                                  int *out_libs,
                                  int max_libs)
{
    if (!cache || !out_libs || max_libs <= 0)
        return 0;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, album_global_id, &local_id);
    if (!slot)
        return 0;

    library_album_info_t *a = slot_get_album(slot, local_id);
    if (!a)
        return 0;

    if (a->musicbrainz_release_group_id) {
        const struct mbrid_album_entry *entry
            = mbrid_album_lookup(cache, a->musicbrainz_release_group_id);
        if (entry) {
            int n = 0;
            for (uint8_t i = 0; i < entry->count && n < max_libs; i++) {
                int lib = LIBRARY_GLOBAL_ID_LIB(entry->global_ids[i]);
                bool dup = false;
                for (int j = 0; j < n; j++)
                    if (out_libs[j] == lib) {
                        dup = true;
                        break;
                    }
                if (!dup)
                    out_libs[n++] = lib;
            }
            return n;
        }
    }

    /* Fallback: source library only */
    out_libs[0] = a->library_index;
    return 1;
}

int
library_cache_get_track_libraries(library_cache_t *cache,
                                  int64_t track_global_id,
                                  int *out_libs,
                                  int max_libs)
{
    if (!cache || !out_libs || max_libs <= 0)
        return 0;

    int64_t local_track_id;
    LibrarySlot *src_slot = decode_slot(cache, track_global_id, &local_track_id);
    if (!src_slot)
        return 0;

    library_track_info_t *t = slot_get_track(src_slot, local_track_id);
    if (!t)
        return 0;

    /* Resolve the source album to get the release-group MBID. */
    int64_t src_local_album = LIBRARY_GLOBAL_ID_LOCAL(t->album_id);
    library_album_info_t *src_album = slot_get_album(src_slot, src_local_album);

    if (src_album && src_album->musicbrainz_release_group_id
        && src_album->musicbrainz_release_group_id[0]) {
        const struct mbrid_album_entry *entry
            = mbrid_album_lookup(cache, src_album->musicbrainz_release_group_id);
        if (entry) {
            int n = 0;
            uint16_t want_disc = t->disc_num;
            uint16_t want_track = t->track_num;

            for (uint8_t i = 0; i < entry->count && n < max_libs; i++) {
                int64_t global_album_id = entry->global_ids[i];
                int64_t local_album_id;
                LibrarySlot *slot = decode_slot(cache, global_album_id, &local_album_id);
                if (!slot)
                    continue;
                if (local_album_id <= 0
                    || (size_t)local_album_id >= slot->album_tracks_ptrs_capacity)
                    continue;
                const GPtrArray *tracks = slot->album_tracks_ptrs[local_album_id];
                if (!tracks)
                    continue;

                /* Strict (disc, track) match within this release. */
                bool found = false;
                for (guint k = 0; k < tracks->len; k++) {
                    const library_track_info_t *ti = g_ptr_array_index(tracks, k);
                    if (ti->disc_num == want_disc && ti->track_num == want_track) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    continue;

                int lib = slot->bitmap_index;
                bool dup = false;
                for (int j = 0; j < n; j++)
                    if (out_libs[j] == lib) {
                        dup = true;
                        break;
                    }
                if (!dup)
                    out_libs[n++] = lib;
            }
            if (n > 0)
                return n;
        }
    }

    /* Fallback: source library only */
    out_libs[0] = t->library_index;
    return 1;
}

/**
 * Build both MBID indices from scratch.  Walks all slots, collects
 * MBID → global_id pairs, flattens into sorted arrays.
 */
static void
build_mbid_indices(library_cache_t *cache)
{
    free_mbid_artist_index(cache);
    free_mbrid_album_index(cache);

    /* ── Artist index ── */
    {
        /* Collect: MBID string → GArray of uint32 global_ids */
        GHashTable *map
            = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)g_array_unref);
        for (int i = 0; i < cache->slot_count; i++) {
            LibrarySlot *slot = &cache->slots[i];
            for (size_t aid = 1; aid < slot->artists_capacity; aid++) {
                library_artist_info_t *a = slot_get_artist(slot, (int64_t)aid);
                if (!a || !a->musicbrainz_id)
                    continue;
                GArray *ids = g_hash_table_lookup(map, a->musicbrainz_id);
                if (!ids) {
                    ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
                    g_hash_table_insert(map, a->musicbrainz_id, ids);
                }
                int64_t gid = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, aid);
                g_array_append_val(ids, gid);
            }
        }

        /* Flatten to sorted array */
        size_t n = g_hash_table_size(map);
        struct mbid_artist_entry *entries = g_new(struct mbid_artist_entry, n);
        GHashTableIter iter;
        gpointer k, v;
        size_t idx = 0;
        g_hash_table_iter_init(&iter, map);
        while (g_hash_table_iter_next(&iter, &k, &v)) {
            GArray *ids = v;
            struct mbid_artist_entry *e = &entries[idx++];
            g_strlcpy(e->mbid, (const char *)k, sizeof(e->mbid));
            e->count = (uint8_t)(ids->len < 255 ? ids->len : 255);
            e->global_ids = g_new(int64_t, e->count);
            memcpy(e->global_ids, ids->data, e->count * sizeof(int64_t));
        }
        g_hash_table_destroy(map);
        qsort(entries, n, sizeof(struct mbid_artist_entry), mbid_entry_cmp);
        cache->mbid_artist_index.entries = entries;
        cache->mbid_artist_index.count = n;
    }

    /* ── Album index ── */
    {
        GHashTable *map
            = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)g_array_unref);
        for (int i = 0; i < cache->slot_count; i++) {
            LibrarySlot *slot = &cache->slots[i];
            for (size_t aid = 1; aid < slot->albums_capacity; aid++) {
                library_album_info_t *a = slot_get_album(slot, (int64_t)aid);
                if (!a || !a->musicbrainz_release_group_id || !a->musicbrainz_release_group_id[0])
                    continue;
                GArray *ids = g_hash_table_lookup(map, a->musicbrainz_release_group_id);
                if (!ids) {
                    ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
                    g_hash_table_insert(map, a->musicbrainz_release_group_id, ids);
                }
                int64_t gid = LIBRARY_MAKE_GLOBAL_ID(slot->bitmap_index, aid);
                g_array_append_val(ids, gid);
            }
        }

        size_t n = g_hash_table_size(map);
        struct mbrid_album_entry *entries = g_new(struct mbrid_album_entry, n);
        GHashTableIter iter;
        gpointer k, v;
        size_t idx = 0;
        g_hash_table_iter_init(&iter, map);
        while (g_hash_table_iter_next(&iter, &k, &v)) {
            GArray *ids = v;
            struct mbrid_album_entry *e = &entries[idx++];
            g_strlcpy(e->mbrid, (const char *)k, sizeof(e->mbrid));
            e->count = (uint8_t)(ids->len < 255 ? ids->len : 255);
            e->global_ids = g_new(int64_t, e->count);
            memcpy(e->global_ids, ids->data, e->count * sizeof(int64_t));
        }
        g_hash_table_destroy(map);
        qsort(entries, n, sizeof(struct mbrid_album_entry), mbrid_entry_cmp);
        cache->mbrid_album_index.entries = entries;
        cache->mbrid_album_index.count = n;
    }
}

/* =============================================================================
 * Search Vocabulary (Levenshtein typo correction)
 * ============================================================================= */

static int
levenshtein(const char *a, const char *b, int max_dist)
{
    int la = (int)strlen(a);
    int lb = (int)strlen(b);

    if (abs(la - lb) > max_dist)
        return max_dist + 1;

    int *prev = g_alloca((lb + 1) * sizeof(int));
    int *curr = g_alloca((lb + 1) * sizeof(int));

    for (int j = 0; j <= lb; j++)
        prev[j] = j;

    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        int row_min = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (g_ascii_tolower(a[i - 1]) == g_ascii_tolower(b[j - 1])) ? 0 : 1;
            curr[j] = MIN(MIN(prev[j] + 1, curr[j - 1] + 1), prev[j - 1] + cost);
            if (curr[j] < row_min)
                row_min = curr[j];
        }
        if (row_min > max_dist)
            return max_dist + 1;
        int *tmp = prev;
        prev = curr;
        curr = tmp;
    }
    return prev[lb];
}

static const char *
correct_token(library_cache_t *cache, const char *token)
{
    if (!cache->search_vocab || cache->search_vocab_count == 0)
        return token;

    int tok_len = (int)strlen(token);
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
    size_t end = MIN(lo + 500, cache->search_vocab_count);

    const char *best = token;
    int best_dist = max_dist + 1;

    for (size_t i = start; i < end; i++) {
        int d = levenshtein(token, cache->search_vocab[i], max_dist);
        if (d < best_dist) {
            best_dist = d;
            best = cache->search_vocab[i];
            if (d == 0)
                break;
        }
    }

    return (best_dist <= max_dist) ? best : token;
}

char *
library_build_corrected_query(library_cache_t *cache, const char *query)
{
    if (!query || !*query)
        return NULL;

    GString *out = g_string_new(NULL);
    gchar **tokens = g_strsplit_set(query, " \t\n\r", -1);

    for (int i = 0; tokens[i]; i++) {
        const gchar *tok = tokens[i];
        if (!tok || !*tok || strlen(tok) < 2)
            continue;

        const char *corrected = correct_token(cache, tok);
        if (out->len > 0)
            g_string_append_c(out, ' ');
        g_string_append(out, corrected);
    }

    g_strfreev(tokens);

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static int
vocab_term_cmp(const void *a, const void *b)
{
    return g_ascii_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

/* Build/merge search vocabulary from one slot's db_warm connection.
 * Merges results into cache->search_vocab (under cache->lock). */
static void
build_search_vocab_slot(library_cache_t *cache, int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= cache->slot_count)
        return;
    LibrarySlot *slot = &cache->slots[slot_idx];

    const char *db_path_str = db_path(slot->db_warm);
    if (!db_path_str)
        return;

    sqlite3 *raw_db = NULL;
    int rc = sqlite3_open_v2(db_path_str, &raw_db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK)
        return;
    sqlite3_exec(raw_db, "PRAGMA query_only = ON;", NULL, NULL, NULL);

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Seed with existing vocab so we merge rather than replace.
     * No lock: vocab pointer is only swapped below and by other warming
     * threads' Phase 5. Worst case: stale read → duplicate term → harmless. */
    for (size_t i = 0; i < cache->search_vocab_count; i++) {
        if (cache->search_vocab[i] && !g_hash_table_contains(seen, cache->search_vocab[i]))
            g_hash_table_insert(seen, g_strdup(cache->search_vocab[i]), NULL);
    }

    static const char *vocab_sql[] = { "SELECT DISTINCT term FROM fts5vocab('tracks_fts', 'row')",
                                       "SELECT DISTINCT term FROM fts5vocab('artists_fts', 'row')",
                                       "SELECT DISTINCT term FROM fts5vocab('albums_fts', 'row')",
                                       NULL };

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

    GList *keys = g_hash_table_get_keys(seen);
    size_t count = (size_t)g_list_length(keys);
    char **vocab = g_new(char *, count);

    size_t idx = 0;
    for (GList *l = keys; l; l = l->next)
        vocab[idx++] = g_strdup((const char *)l->data);

    g_list_free(keys);
    g_hash_table_destroy(seen);

    qsort(vocab, count, sizeof(char *), vocab_term_cmp);

    /* Swap vocab — old freed, new assigned. Main-thread reads happen after
     * ready_cb fires (via g_idle_add), which is after warming completes. */
    if (cache->search_vocab) {
        for (size_t i = 0; i < cache->search_vocab_count; i++)
            g_free(cache->search_vocab[i]);
        g_free(cache->search_vocab);
    }
    cache->search_vocab = vocab;
    cache->search_vocab_count = count;
}

/* =============================================================================
 * Slot Initialisation / Teardown
 * ============================================================================= */

/* Open DB connections and allocate arrays for one slot. */
static quadrature_result_t
init_slot(LibrarySlot *slot,
          int lib_idx,
          int bitmap_index,
          const char *db_path_str,
          const char *music_base,
          const char *display_name,
          library_cache_t *cache)
{
    slot->lib_idx = lib_idx;
    slot->bitmap_index = bitmap_index;
    slot->db_path = g_strdup(db_path_str);
    slot->music_base = music_base ? g_strdup(music_base) : NULL;
    slot->display_name = display_name ? g_strdup(display_name) : NULL;
    slot->cache = cache;

    atomic_init(&slot->warm_cancel, 0);
    atomic_init(&slot->warm_state, LIBRARY_CACHE_IDLE);
    atomic_init(&slot->available, true);

    quadrature_result_t res = db_open(db_path_str, true, &slot->db);
    if (res != QUADRATURE_OK) {
        /* DB doesn't exist yet (first run / post-db-clean). Cache is valid but
         * queries return empty until ensure_slot_db_open() succeeds after indexing. */
        slot->db = NULL;
        slot->db_warm = NULL;
        return QUADRATURE_OK;
    }

    res = db_open(db_path_str, true, &slot->db_warm);
    if (res != QUADRATURE_OK) {
        db_close(slot->db);
        slot->db = NULL;
        slot->db_warm = NULL;
        return QUADRATURE_OK;
    }

    /* Open auxiliary DBs (optional — NULL if file doesn't exist) */
    char *data_root = g_path_get_dirname(db_path_str);
    if (db_meta_open_readonly(data_root, &slot->meta_db) != QUADRATURE_OK)
        slot->meta_db = NULL;
    if (db_bios_open_readonly(data_root, &slot->bios_db) != QUADRATURE_OK)
        slot->bios_db = NULL;
    g_free(data_root);

    allocate_slot_arrays(slot);
    return QUADRATURE_OK;
}

static void
destroy_slot_internals(LibrarySlot *slot)
{
    cancel_and_join_slot_warming(slot);

    free_slot_arrays(slot);

    if (slot->meta_db) {
        db_meta_close(slot->meta_db);
        slot->meta_db = NULL;
    }
    if (slot->bios_db) {
        db_bios_close(slot->bios_db);
        slot->bios_db = NULL;
    }
    if (slot->db_warm) {
        db_close(slot->db_warm);
        slot->db_warm = NULL;
    }
    if (slot->db) {
        db_close(slot->db);
        slot->db = NULL;
    }

    g_clear_pointer(&slot->db_path, g_free);
    g_clear_pointer(&slot->music_base, g_free);
    g_clear_pointer(&slot->display_name, g_free);
}

/* Open both DB connections for a slot whose DB was absent at init time.
 * Must be called with cache->lock held (or before warming starts).
 * No-op if connections are already open. */
static void
ensure_slot_db_open(LibrarySlot *slot)
{
    if (slot->db)
        return; /* Already open */

    if (db_open(slot->db_path, true, &slot->db) != QUADRATURE_OK) {
        slot->db = NULL;
        return;
    }
    if (db_open(slot->db_path, true, &slot->db_warm) != QUADRATURE_OK) {
        db_close(slot->db);
        slot->db = NULL;
        slot->db_warm = NULL;
        return;
    }

    /* Re-open auxiliary DBs (indexer may have just created them) */
    if (!slot->meta_db || !slot->bios_db) {
        char *data_root = g_path_get_dirname(slot->db_path);
        if (!slot->meta_db && db_meta_open_readonly(data_root, &slot->meta_db) != QUADRATURE_OK)
            slot->meta_db = NULL;
        if (!slot->bios_db && db_bios_open_readonly(data_root, &slot->bios_db) != QUADRATURE_OK)
            slot->bios_db = NULL;
        g_free(data_root);
    }

    allocate_slot_arrays(slot);
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

quadrature_result_t
library_cache_create_multi(const library_cache_source_t *sources,
                           int source_count,
                           library_cache_t **out)
{
    if (!out || source_count < 0)
        return QUADRATURE_ERROR_INVALID_PARAM;
    if (source_count > 0 && !sources)
        return QUADRATURE_ERROR_INVALID_PARAM;

    library_cache_t *cache = g_new0(library_cache_t, 1);
    g_mutex_init(&cache->lock);

    cache->slot_count = source_count;
    cache->slots = g_new0(LibrarySlot, source_count);

    /* Determine bitmap capacity (max bitmap_index + 1) */
    int max_bitmap = -1;
    for (int i = 0; i < source_count; i++) {
        if (sources[i].bitmap_index > max_bitmap)
            max_bitmap = sources[i].bitmap_index;
    }
    cache->bitmap_capacity = max_bitmap + 1;
    cache->bitmap_map = g_new0(LibrarySlot *, cache->bitmap_capacity);

    for (int i = 0; i < source_count; i++) {
        quadrature_result_t res = init_slot(&cache->slots[i],
                                            i,
                                            sources[i].bitmap_index,
                                            sources[i].db_path,
                                            sources[i].music_base,
                                            sources[i].display_name,
                                            cache);
        if (res != QUADRATURE_OK) {
            /* Clean up already-initialised slots */
            for (int j = 0; j < i; j++)
                destroy_slot_internals(&cache->slots[j]);
            g_free(cache->slots);
            g_free(cache->bitmap_map);
            g_mutex_clear(&cache->lock);
            g_free(cache);
            return res;
        }
        /* Register in bitmap map */
        cache->bitmap_map[sources[i].bitmap_index] = &cache->slots[i];
    }

    /* Start background prefetch thread */
    atomic_init(&cache->prefetch_shutdown, 0);
    cache->prefetch_queue = g_async_queue_new();
    cache->prefetch_thread = g_thread_new("prefetch", prefetch_thread_func, cache);

    *out = cache;
    return QUADRATURE_OK;
}

int
library_cache_get_library_count(library_cache_t *cache)
{
    if (!cache)
        return 0;
    return cache->slot_count;
}

int
library_cache_get_bitmap_index(library_cache_t *cache, int slot_position)
{
    if (!cache || slot_position < 0 || slot_position >= cache->slot_count)
        return -1;
    return cache->slots[slot_position].bitmap_index;
}

const char *
library_cache_get_library_name(library_cache_t *cache, int bitmap_index)
{
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return NULL;
    if (slot->display_name)
        return slot->display_name;
    if (slot->music_base) {
        const char *slash = strrchr(slot->music_base, '/');
        return slash ? slash + 1 : slot->music_base;
    }
    return slot->db_path;
}

void
library_cache_set_library_name(library_cache_t *cache, int bitmap_index, const char *name)
{
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return;
    g_free(slot->display_name);
    slot->display_name = (name && name[0]) ? g_strdup(name) : NULL;
}

void
library_cache_set_available(library_cache_t *cache, int bitmap_index, gboolean available)
{
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return;
    atomic_store(&slot->available, (bool)available);
}

gboolean
library_cache_get_available(library_cache_t *cache, int bitmap_index)
{
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return FALSE;
    return (gboolean)atomic_load(&slot->available);
}

size_t
library_cache_get_slot_memory_bytes(library_cache_t *cache, int bitmap_index)
{
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return 0;
    int state = atomic_load(&slot->warm_state);
    if (state != LIBRARY_CACHE_READY && state != LIBRARY_CACHE_REFRESHING)
        return 0;

    /* Pointer arrays: capacity × sizeof(pointer) */
    size_t bytes = 0;
    bytes += slot->artists_capacity * sizeof(library_artist_info_t *);
    bytes += slot->albums_capacity * sizeof(library_album_info_t *);
    bytes += slot->tracks_capacity * sizeof(library_track_info_t *);

    /* Relationship pointer arrays */
    bytes += slot->album_tracks_capacity * sizeof(GArray *);
    bytes += slot->track_artists_capacity * sizeof(GPtrArray *);
    bytes += slot->artist_albums_capacity * sizeof(GPtrArray *);
    bytes += slot->artist_appearances_capacity * sizeof(GPtrArray *);
    bytes += slot->album_tracks_ptrs_capacity * sizeof(GPtrArray *);

    /* Estimate heap-allocated info structs (capacity is max_id+1, many slots NULL).
     * Count populated slots — iterate pointer arrays is too expensive for a perf
     * query, so use a conservative estimate: sizeof(struct) + ~80 bytes average
     * string storage per populated entity, estimated at 60% fill. */
    size_t est_artists = slot->artists_capacity * 6 / 10;
    size_t est_albums = slot->albums_capacity * 6 / 10;
    size_t est_tracks = slot->tracks_capacity * 6 / 10;

    bytes += est_artists * (sizeof(library_artist_info_t) + 80);
    bytes += est_albums * (sizeof(library_album_info_t) + 160);
    bytes += est_tracks * (sizeof(library_track_info_t) + 200);

    return bytes;
}

library_cache_dbs_t
library_cache_get_dbs(library_cache_t *cache, int bitmap_index)
{
    library_cache_dbs_t dbs = { 0 };
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (slot) {
        dbs.db = slot->db;
        dbs.meta = slot->meta_db;
        dbs.bios = slot->bios_db;
    }
    return dbs;
}

void
library_cache_destroy(library_cache_t *cache)
{
    if (!cache)
        return;

    /* Shut down prefetch thread */
    atomic_store(&cache->prefetch_shutdown, 1);
    if (cache->prefetch_thread) {
        g_thread_join(cache->prefetch_thread);
        cache->prefetch_thread = NULL;
    }
    if (cache->prefetch_queue) {
        g_async_queue_unref(cache->prefetch_queue);
        cache->prefetch_queue = NULL;
    }

    for (int i = 0; i < cache->slot_count; i++)
        destroy_slot_internals(&cache->slots[i]);

    g_free(cache->slots);
    g_free(cache->bitmap_map);

    if (cache->search_vocab) {
        for (size_t i = 0; i < cache->search_vocab_count; i++)
            g_free(cache->search_vocab[i]);
        g_free(cache->search_vocab);
    }

    free_mbid_artist_index(cache);
    free_mbrid_album_index(cache);

    g_mutex_clear(&cache->lock);
    g_free(cache);
}

/* =============================================================================
 * Cache Warming API
 * ============================================================================= */

void
library_cache_set_ready_callback(library_cache_t *cache, library_cache_ready_cb cb, void *user_data)
{
    g_assert(cache != NULL);
    cache->ready_cb = cb;
    cache->ready_cb_data = user_data;
}

void
library_cache_start_warming(library_cache_t *cache)
{
    g_assert(cache != NULL);
    for (int i = 0; i < cache->slot_count; i++)
        library_cache_warm_slot(cache, cache->slots[i].bitmap_index);
}

void
library_cache_warm_slot_blocking(library_cache_t *cache, int bitmap_index)
{
    g_assert(cache != NULL);
    library_cache_warm_slot(cache, bitmap_index);
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (slot && slot->warm_thread) {
        g_thread_join(slot->warm_thread);
        slot->warm_thread = NULL;
    }
}

void
library_cache_warm_slot(library_cache_t *cache, int bitmap_index)
{
    g_assert(cache != NULL);
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return;

    if (!slot->db_warm) {
        g_warning("library_cache_warm_slot[bitmap=%d]: db_warm is NULL, skipping", bitmap_index);
        return; /* DB not available yet; will warm after indexer creates it */
    }

    int expected = LIBRARY_CACHE_IDLE;
    if (!atomic_compare_exchange_strong(&slot->warm_state, &expected, LIBRARY_CACHE_WARMING)) {
        g_warning("library_cache_warm_slot[bitmap=%d]: warm_state not IDLE (state=%d), skipping",
                  bitmap_index,
                  atomic_load(&slot->warm_state));
        return; /* already warming or ready */
    }

    char *thread_name = g_strdup_printf("cache-warm-%d", bitmap_index);
    slot->warm_thread = g_thread_new(thread_name, slot_warming_thread_func, slot);
    g_free(thread_name);
}

void
library_cache_await_slot(library_cache_t *cache, int bitmap_index)
{
    g_assert(cache != NULL);
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (slot && slot->warm_thread) {
        g_thread_join(slot->warm_thread);
        slot->warm_thread = NULL;
    }
}

/* =============================================================================
 * Cache Management
 * ============================================================================= */

void
library_cache_clear(library_cache_t *cache)
{
    g_assert(cache != NULL);

    /* Cancel all warming threads — after join, no concurrent access. */
    for (int i = 0; i < cache->slot_count; i++)
        cancel_and_join_slot_warming(&cache->slots[i]);

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        ensure_slot_db_open(slot);
        free_slot_arrays(slot);
        if (slot->db)
            allocate_slot_arrays(slot);
        atomic_store(&slot->warm_state, LIBRARY_CACHE_IDLE);
    }
}

void
library_cache_clear_slot(library_cache_t *cache, int bitmap_index)
{
    g_assert(cache != NULL);
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return;

    cancel_and_join_slot_warming(slot);

    ensure_slot_db_open(slot);
    free_slot_arrays(slot);
    if (slot->db)
        allocate_slot_arrays(slot);
    atomic_store(&slot->warm_state, LIBRARY_CACHE_IDLE);

    build_mbid_indices(cache);
}

/* =============================================================================
 * COW Slot Refresh
 *
 * Build a new version of a slot's entity arrays by re-reading the entire
 * library from the DB, then atomically swap the new arrays into the slot.
 * UI reads against the old arrays keep working throughout — only the pointer
 * swap is visible, and it's done under cache->lock.
 *
 * There is intentionally no "seed from old slot" shortcut. Callbacks are the
 * single source of truth for entity content (see LIBRARY_CACHE.md → "COW
 * Refresh Invariants"). The `library_cache_changeset_t` from the indexer is
 * consumed at call time for observability and fast-path elision (empty
 * changeset ⇒ nothing to do), not to gate per-entity work.
 * ============================================================================= */

typedef struct {
    library_cache_t *cache;
    LibrarySlot *slot;
} CowRefreshCtx;

/* (free_shadow_arrays was removed: free_slot_arrays() drains a standalone slot
 * too, additionally NULLing the freed fields, which is harmless for a throwaway
 * shadow that is about to go out of scope.) */

static gpointer
cow_refresh_thread_func(gpointer data)
{
    CowRefreshCtx *ctx = data;
    LibrarySlot *slot = ctx->slot;
    library_cache_t *cache = ctx->cache;

    gint64 start_time = g_get_monotonic_time();

    /* Reopen the warming connection. slot->db stays untouched — UI reads
     * from it concurrently via WAL. */
    if (slot->db_warm) {
        db_close(slot->db_warm);
        slot->db_warm = NULL;
    }
    if (db_open(slot->db_path, true, &slot->db_warm) != QUADRATURE_OK) {
        g_warning("COW refresh [bitmap=%d]: failed to reopen db_warm", slot->bitmap_index);
        goto done;
    }

    /* Auxiliary DBs may have been created by the indexer after the initial
     * warm (meta/bios exist only after Phase 6/8 write to them). */
    if (!slot->meta_db || !slot->bios_db) {
        char *data_root = g_path_get_dirname(slot->db_path);
        if (!slot->meta_db && db_meta_open_readonly(data_root, &slot->meta_db) != QUADRATURE_OK)
            slot->meta_db = NULL;
        if (!slot->bios_db && db_bios_open_readonly(data_root, &slot->bios_db) != QUADRATURE_OK)
            slot->bios_db = NULL;
        g_free(data_root);
    }

    /* Shadow slot holds the new arrays while DELTA runs. Shares bitmap_index
     * and warm_cancel with the real slot so callbacks work unchanged. */
    LibrarySlot shadow = { 0 };
    shadow.bitmap_index = slot->bitmap_index;

    /* Borrow db_warm so allocate_slot_arrays() sizes from the right connection;
     * shadow never owns it, so clear it before the array lifetime begins. */
    shadow.db_warm = slot->db_warm;
    allocate_slot_arrays(&shadow);
    shadow.db_warm = NULL;

    if (atomic_load(&slot->warm_cancel))
        goto cancel;

    /* ── DELTA: re-read the entire library from DB into shadow arrays. */
    {
        db_begin_read(slot->db_warm);

        db_iter_all_artists(slot->db_warm, on_warm_artist, &shadow);
        if (atomic_load(&slot->warm_cancel)) {
            db_end_read(slot->db_warm);
            goto cancel;
        }

        db_iter_all_albums(slot->db_warm, on_warm_album, &shadow);
        if (atomic_load(&slot->warm_cancel)) {
            db_end_read(slot->db_warm);
            goto cancel;
        }

        WarmTracksCtx tctx = { .slot = &shadow, .loaded = 0 };
        db_iter_all_tracks(slot->db_warm, on_warm_track, &tctx);

        BulkTrackArtistCtx ta_ctx = { .slot = &shadow, .prev_tid = -1, .cur_list = NULL };
        db_iter_all_track_artists(slot->db_warm, on_bulk_track_artist, &ta_ctx);
        bulk_ta_flush(&ta_ctx);

        db_end_read(slot->db_warm);
    }

    if (atomic_load(&slot->warm_cancel))
        goto cancel;

    /* ── REBUILD: relationship arrays and derived aggregates on the shadow. */
    rebuild_slot_relationships(&shadow, NULL);

    if (atomic_load(&slot->warm_cancel))
        goto cancel;

    /* ── SWAP + DRAIN under cache->lock (invariant I6). */
    {
        g_mutex_lock(&cache->lock);

        /* Install freshly-built shadow arrays into the live slot; the old arrays
         * land back in `shadow` for draining after the lock is released. */
        swap_slot_arrays(slot, &shadow);

        /* Rebuild MBID indices under the same lock (I6). */
        build_mbid_indices(cache);

        g_mutex_unlock(&cache->lock);

        /* Search vocab reads db_warm, not slot arrays — safe outside lock. */
        build_search_vocab_slot(cache, slot->lib_idx);

        atomic_store(&slot->warm_state, LIBRARY_CACHE_READY);

        if (cache->ready_cb)
            g_idle_add(warming_complete_idle, cache);

        {
            gint64 elapsed = g_get_monotonic_time() - start_time;
            size_t n_artists = 0, n_albums = 0, n_tracks = 0;
            for (size_t i = 1; i < slot->artists_capacity; i++)
                if (slot_get_artist(slot, (int64_t)i))
                    n_artists++;
            for (size_t i = 1; i < slot->albums_capacity; i++)
                if (slot_get_album(slot, (int64_t)i))
                    n_albums++;
            for (size_t i = 1; i < slot->tracks_capacity; i++)
                if (slot_get_track(slot, (int64_t)i))
                    n_tracks++;
            g_message(
                "COW refresh [bitmap=%d]: ready — %zu artists, %zu albums, %zu tracks in %.1f ms",
                slot->bitmap_index,
                n_artists,
                n_albums,
                n_tracks,
                elapsed / 1000.0);
        }

        /* DRAIN old arrays (now held by shadow) outside the lock. */
        free_slot_arrays(&shadow);

        goto done;
    }

cancel:
    free_slot_arrays(&shadow);

done:
    g_free(ctx);
    return NULL;
}

void
library_cache_refresh_slot(library_cache_t *cache,
                           int bitmap_index,
                           const library_cache_changeset_t *changes)
{
    g_assert(cache != NULL);
    LibrarySlot *slot = bitmap_to_slot(cache, bitmap_index);
    if (!slot)
        return;

    /* Empty changeset ⇒ nothing mutated since last warming; refresh is a no-op.
     * NULL changeset = "don't know, rebuild" (always safe per invariant I4). */
    if (changes && library_cache_changeset_is_empty(changes)) {
        g_debug("library_cache_refresh_slot[bitmap=%d]: empty changeset — skipping", bitmap_index);
        return;
    }

    /* Cancel any in-progress warming/refresh for this slot. */
    cancel_and_join_slot_warming(slot);

    CowRefreshCtx *ctx = g_new0(CowRefreshCtx, 1);
    ctx->cache = cache;
    ctx->slot = slot;

    /* READY → REFRESHING: old data stays live, UI queries keep working.
     * Also allow from IDLE (slot was cleared but not yet warmed). */
    int expected = LIBRARY_CACHE_READY;
    if (!atomic_compare_exchange_strong(&slot->warm_state, &expected, LIBRARY_CACHE_REFRESHING)) {
        expected = LIBRARY_CACHE_IDLE;
        if (!atomic_compare_exchange_strong(
                &slot->warm_state, &expected, LIBRARY_CACHE_REFRESHING)) {
            g_warning("library_cache_refresh_slot[bitmap=%d]: unexpected state %d",
                      bitmap_index,
                      atomic_load(&slot->warm_state));
            g_free(ctx);
            return;
        }
    }

    char *thread_name = g_strdup_printf("cow-refresh-%d", bitmap_index);
    slot->warm_thread = g_thread_new(thread_name, cow_refresh_thread_func, ctx);
    g_free(thread_name);
}

/* =============================================================================
 * library_cache_changeset_t — owned, mutable list of rowids (public API)
 * ============================================================================= */

library_cache_changeset_t *
library_cache_changeset_new(void)
{
    return g_new0(library_cache_changeset_t, 1);
}

void
library_cache_changeset_free(library_cache_changeset_t *cs)
{
    if (!cs)
        return;
    g_free(cs->artists);
    g_free(cs->albums);
    g_free(cs->tracks);
    g_free(cs);
}

/* Deep-copy an int64 array (may be NULL when count == 0). */
static int64_t *
copy_int64_array(const int64_t *src, size_t count)
{
    if (count == 0 || !src)
        return NULL;
    int64_t *dst = g_new(int64_t, count);
    memcpy(dst, src, count * sizeof(int64_t));
    return dst;
}

library_cache_changeset_t *
library_cache_changeset_copy(const library_cache_changeset_t *src)
{
    if (!src)
        return NULL;
    library_cache_changeset_t *dst = library_cache_changeset_new();
    dst->artists = copy_int64_array(src->artists, src->artists_count);
    dst->artists_count = src->artists_count;
    dst->albums = copy_int64_array(src->albums, src->albums_count);
    dst->albums_count = src->albums_count;
    dst->tracks = copy_int64_array(src->tracks, src->tracks_count);
    dst->tracks_count = src->tracks_count;
    dst->track_artists_dirty = src->track_artists_dirty;
    return dst;
}

/* Merge two int64 arrays into a newly-allocated dedup'd array.
 * The merged output is not sorted (order-independent for our callers). */
static int64_t *
merge_int64_arrays(
    const int64_t *a, size_t a_count, const int64_t *b, size_t b_count, size_t *out_count)
{
    if (a_count == 0 && b_count == 0) {
        *out_count = 0;
        return NULL;
    }

    /* Hash set dedup */
    GHashTable *set = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    for (size_t i = 0; i < a_count; i++) {
        if (!g_hash_table_contains(set, &a[i])) {
            int64_t *k = g_new(int64_t, 1);
            *k = a[i];
            g_hash_table_add(set, k);
        }
    }
    for (size_t i = 0; i < b_count; i++) {
        if (!g_hash_table_contains(set, &b[i])) {
            int64_t *k = g_new(int64_t, 1);
            *k = b[i];
            g_hash_table_add(set, k);
        }
    }
    guint n = g_hash_table_size(set);
    int64_t *arr = n ? g_new(int64_t, n) : NULL;
    GHashTableIter iter;
    gpointer key;
    guint i = 0;
    g_hash_table_iter_init(&iter, set);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        arr[i++] = *(int64_t *)key;
    g_hash_table_destroy(set);
    *out_count = n;
    return arr;
}

void
library_cache_changeset_merge(library_cache_changeset_t *dst, const library_cache_changeset_t *src)
{
    if (!dst || !src)
        return;

    size_t n;
    int64_t *merged;

    merged = merge_int64_arrays(
        dst->artists, dst->artists_count, src->artists, src->artists_count, &n);
    g_free(dst->artists);
    dst->artists = merged;
    dst->artists_count = n;

    merged = merge_int64_arrays(dst->albums, dst->albums_count, src->albums, src->albums_count, &n);
    g_free(dst->albums);
    dst->albums = merged;
    dst->albums_count = n;

    merged = merge_int64_arrays(dst->tracks, dst->tracks_count, src->tracks, src->tracks_count, &n);
    g_free(dst->tracks);
    dst->tracks = merged;
    dst->tracks_count = n;

    if (src->track_artists_dirty)
        dst->track_artists_dirty = true;
}

bool
library_cache_changeset_is_empty(const library_cache_changeset_t *cs)
{
    if (!cs)
        return true;
    return cs->artists_count == 0 && cs->albums_count == 0 && cs->tracks_count == 0
           && !cs->track_artists_dirty;
}

/* =============================================================================
 * Dynamic Slot Management
 * ============================================================================= */

int
library_cache_add_slot(library_cache_t *cache, const library_cache_source_t *source)
{
    g_assert(cache != NULL);
    g_assert(source != NULL);
    g_assert(source->bitmap_index >= 0);

    /* Cancel all warming threads — g_realloc may move the slots array,
     * invalidating any LibrarySlot* held by warming threads.
     * After join, no concurrent access — no lock needed. */
    for (int i = 0; i < cache->slot_count; i++)
        cancel_and_join_slot_warming(&cache->slots[i]);

    int new_idx = cache->slot_count;
    cache->slots = g_realloc(cache->slots, sizeof(LibrarySlot) * (size_t)(new_idx + 1));
    memset(&cache->slots[new_idx], 0, sizeof(LibrarySlot));

    /* Fix backpointers after realloc (warming threads are stopped, but
     * backpointers must be consistent before any new warming starts). */
    for (int i = 0; i < new_idx; i++)
        cache->slots[i].cache = cache;

    quadrature_result_t res = init_slot(&cache->slots[new_idx],
                                        new_idx,
                                        source->bitmap_index,
                                        source->db_path,
                                        source->music_base,
                                        source->display_name,
                                        cache);
    if (res != QUADRATURE_OK)
        return -1;

    cache->slot_count = new_idx + 1;

    /* Grow bitmap map if needed and register the new slot */
    int bi = source->bitmap_index;
    if (bi >= cache->bitmap_capacity) {
        int new_cap = bi + 1;
        cache->bitmap_map = g_realloc(cache->bitmap_map, sizeof(LibrarySlot *) * (size_t)new_cap);
        for (int i = cache->bitmap_capacity; i < new_cap; i++)
            cache->bitmap_map[i] = NULL;
        cache->bitmap_capacity = new_cap;
    }
    /* Update ALL bitmap_map pointers (realloc may have moved slots array) */
    for (int i = 0; i < cache->slot_count; i++)
        g_atomic_pointer_set(&cache->bitmap_map[cache->slots[i].bitmap_index], &cache->slots[i]);

    return bi;
}

quadrature_result_t
library_cache_remove_slot(library_cache_t *cache, int bitmap_index)
{
    g_assert(cache != NULL);
    LibrarySlot *target = bitmap_to_slot(cache, bitmap_index);
    if (!target)
        return QUADRATURE_ERROR_INVALID_PARAM;

    int slot_pos = target->lib_idx;

    /* Cancel ALL warming/refresh threads — realloc may move the slots array.
     * After join, no concurrent access — no lock needed. */
    for (int i = 0; i < cache->slot_count; i++)
        cancel_and_join_slot_warming(&cache->slots[i]);

    /* Clear bitmap map entry for the removed library */
    g_atomic_pointer_set(&cache->bitmap_map[bitmap_index], NULL);

    destroy_slot_internals(&cache->slots[slot_pos]);

    /* Shift remaining slots down */
    int remaining = cache->slot_count - slot_pos - 1;
    if (remaining > 0) {
        memmove(&cache->slots[slot_pos],
                &cache->slots[slot_pos + 1],
                sizeof(LibrarySlot) * (size_t)remaining);
    }
    cache->slot_count--;

    /* Update lib_idx (position) and backpointers for shifted slots.
     * bitmap_index is stable — no need to touch it or the entities.
     * But we do need to re-register shifted slots in bitmap_map since
     * their addresses changed. */
    for (int i = slot_pos; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        slot->lib_idx = i;
        slot->cache = cache;
        g_atomic_pointer_set(&cache->bitmap_map[slot->bitmap_index], slot);
    }

    /* Fix backpointers for un-shifted slots too (defensive). */
    for (int i = 0; i < slot_pos; i++)
        cache->slots[i].cache = cache;

    build_mbid_indices(cache);

    return QUADRATURE_OK;
}

/* =============================================================================
 * Entity Lookups — warming-only, no DB fallback
 * ============================================================================= */

/* =============================================================================
 * Entity Getters — pure array lookups, no DB fallback, no locks.
 * After warming, all entities are pre-populated in slot arrays.
 * ============================================================================= */

const library_track_info_t *
library_cache_get_track(library_cache_t *cache, int64_t track_id)
{
    if (!cache || track_id <= 0)
        return NULL;
    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, track_id, &local_id);
    if (!slot)
        return NULL;
    return slot_get_track(slot, local_id);
}

/* Sort helper: compare album info pointers by library_index (ascending). */
static int
cmp_album_by_lib(gconstpointer a, gconstpointer b)
{
    const library_album_info_t *aa = *(const library_album_info_t *const *)a;
    const library_album_info_t *bb = *(const library_album_info_t *const *)b;
    return aa->library_index - bb->library_index;
}

/* Sort helper: compare artist info pointers by library_index (ascending). */
static int
cmp_artist_by_lib(gconstpointer a, gconstpointer b)
{
    const library_artist_info_t *aa = *(const library_artist_info_t *const *)a;
    const library_artist_info_t *bb = *(const library_artist_info_t *const *)b;
    return aa->library_index - bb->library_index;
}

/* Typed wrappers so the generic version-collector below takes a uniform getter
 * without function-pointer type punning. */
static gpointer
slot_get_album_p(LibrarySlot *slot, int64_t local_id)
{
    return slot_get_album(slot, local_id);
}
static gpointer
slot_get_artist_p(LibrarySlot *slot, int64_t local_id)
{
    return slot_get_artist(slot, local_id);
}

/* Shared body of library_cache_get_albums/get_artists. The caller resolves the
 * source entity and its MBID-index entry (NULL when absent or in single-result
 * mode); this walks the cross-library entry, mask-filters, sorts by library and
 * truncates — or returns just the source when there is no entry. */
static GPtrArray *
collect_entity_versions(library_cache_t *cache,
                        LibrarySlot *src_slot,
                        gpointer source,
                        uint32_t library_mask,
                        int num_results,
                        const int64_t *entry_ids,
                        uint8_t entry_count,
                        gpointer (*get)(LibrarySlot *, int64_t),
                        GCompareFunc cmp_by_lib)
{
    GPtrArray *results = g_ptr_array_new();

    if (entry_ids) {
        for (uint8_t i = 0; i < entry_count; i++) {
            int lib = LIBRARY_GLOBAL_ID_LIB(entry_ids[i]);
            if (!(library_mask & (1u << lib)))
                continue;
            int64_t lid;
            LibrarySlot *s = decode_slot(cache, entry_ids[i], &lid);
            if (!s)
                continue;
            gpointer e = get(s, lid);
            if (e)
                g_ptr_array_add(results, e);
        }
        g_ptr_array_sort(results, cmp_by_lib);
        if (num_results > 0 && results->len > (guint)num_results)
            g_ptr_array_set_size(results, num_results);
        return results;
    }

    /* No MBID entry or single-result mode: return source if it passes the mask. */
    if (library_mask & (1u << src_slot->bitmap_index))
        g_ptr_array_add(results, source);

    return results;
}

GPtrArray *
library_cache_get_albums(library_cache_t *cache,
                         int64_t album_id,
                         uint32_t library_mask,
                         int num_results)
{
    if (!cache || album_id <= 0)
        return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, album_id, &local_id);
    if (!slot)
        return NULL;

    library_album_info_t *source = slot_get_album(slot, local_id);
    if (!source)
        return NULL;

    const struct mbrid_album_entry *entry
        = (source->musicbrainz_release_group_id && num_results != 1)
              ? mbrid_album_lookup(cache, source->musicbrainz_release_group_id)
              : NULL;

    return collect_entity_versions(cache,
                                   slot,
                                   source,
                                   library_mask,
                                   num_results,
                                   entry ? entry->global_ids : NULL,
                                   entry ? entry->count : 0,
                                   slot_get_album_p,
                                   cmp_album_by_lib);
}

const library_album_info_t *
library_cache_get_album(library_cache_t *cache, int64_t album_id, uint32_t library_mask)
{
    GPtrArray *results = library_cache_get_albums(cache, album_id, library_mask, 1);
    if (!results || results->len == 0) {
        g_clear_pointer(&results, g_ptr_array_unref);
        return NULL;
    }
    const library_album_info_t *album = g_ptr_array_index(results, 0);
    g_ptr_array_unref(results);
    return album;
}

GPtrArray *
library_cache_get_artists(library_cache_t *cache,
                          int64_t artist_id,
                          uint32_t library_mask,
                          int num_results)
{
    if (!cache || artist_id <= 0)
        return NULL;

    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, artist_id, &local_id);
    if (!slot)
        return NULL;

    library_artist_info_t *source = slot_get_artist(slot, local_id);
    if (!source)
        return NULL;

    const struct mbid_artist_entry *entry = (source->musicbrainz_id && num_results != 1)
                                                ? mbid_artist_lookup(cache, source->musicbrainz_id)
                                                : NULL;

    return collect_entity_versions(cache,
                                   slot,
                                   source,
                                   library_mask,
                                   num_results,
                                   entry ? entry->global_ids : NULL,
                                   entry ? entry->count : 0,
                                   slot_get_artist_p,
                                   cmp_artist_by_lib);
}

const library_artist_info_t *
library_cache_get_artist(library_cache_t *cache, int64_t artist_id, uint32_t library_mask)
{
    GPtrArray *results = library_cache_get_artists(cache, artist_id, library_mask, 1);
    if (!results || results->len == 0) {
        g_clear_pointer(&results, g_ptr_array_unref);
        return NULL;
    }
    const library_artist_info_t *artist = g_ptr_array_index(results, 0);
    g_ptr_array_unref(results);
    return artist;
}

char *
library_cache_resolve_track_path(library_cache_t *cache, int64_t track_id)
{
    if (!cache || track_id <= 0)
        return NULL;
    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, track_id, &local_id);
    if (!slot)
        return NULL;

    library_track_info_t *track = slot_get_track(slot, local_id);
    if (!track)
        return NULL;

    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
    library_album_info_t *album = slot_get_album(slot, local_album_id);
    return resolve_track_path(slot->music_base, album ? album->path : NULL, track->path);
}

const GPtrArray *
library_cache_get_track_artists(library_cache_t *cache, int64_t track_id)
{
    if (!cache || track_id <= 0)
        return NULL;
    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, track_id, &local_id);
    if (!slot)
        return NULL;

    if (local_id > 0 && (size_t)local_id < slot->track_artists_capacity)
        return slot->track_artists[local_id];
    return NULL;
}

/* =============================================================================
 * Track Navigation
 * ============================================================================= */

int64_t
library_cache_get_next_track_id(library_cache_t *cache, int64_t current_track_id)
{
    if (!cache || current_track_id <= 0)
        return 0;

    int64_t local_track_id;
    LibrarySlot *slot = decode_slot(cache, current_track_id, &local_track_id);
    if (!slot)
        return 0;

    const library_track_info_t *info = slot_get_track(slot, local_track_id);
    if (!info)
        return 0;

    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(info->album_id);
    GArray *album_tracks = NULL;
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity)
        album_tracks = slot->album_tracks[local_album_id];

    if (!album_tracks || album_tracks->len == 0)
        return 0;

    for (guint i = 0; i < album_tracks->len; i++) {
        int64_t gtid = g_array_index(album_tracks, int64_t, i);
        if (gtid == current_track_id) {
            if (i + 1 < album_tracks->len)
                return g_array_index(album_tracks, int64_t, i + 1);
            break;
        }
    }
    return 0;
}

int64_t
library_cache_get_prev_track_id(library_cache_t *cache, int64_t current_track_id)
{
    if (!cache || current_track_id <= 0)
        return 0;

    int64_t local_track_id;
    LibrarySlot *slot = decode_slot(cache, current_track_id, &local_track_id);
    if (!slot)
        return 0;

    const library_track_info_t *info = slot_get_track(slot, local_track_id);
    if (!info)
        return 0;

    int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(info->album_id);
    GArray *album_tracks = NULL;
    if (local_album_id > 0 && (size_t)local_album_id < slot->album_tracks_capacity)
        album_tracks = slot->album_tracks[local_album_id];

    if (!album_tracks || album_tracks->len == 0)
        return 0;

    for (guint i = 0; i < album_tracks->len; i++) {
        int64_t gtid = g_array_index(album_tracks, int64_t, i);
        if (gtid == current_track_id) {
            if (i > 0)
                return g_array_index(album_tracks, int64_t, i - 1);
            break;
        }
    }
    return 0;
}

/* =============================================================================
 * List Queries
 * ============================================================================= */

GPtrArray *
library_cache_get_tracks_by_album(library_cache_t *cache, int64_t album_id, uint32_t library_mask)
{
    if (!cache || album_id <= 0)
        return NULL;

    int64_t local_album_id;
    LibrarySlot *slot = decode_slot(cache, album_id, &local_album_id);
    if (!slot)
        return NULL;

    /* Mask check: album lives in one library */
    if (library_mask != LIBRARY_MASK_ALL && !(library_mask & (1u << slot->bitmap_index)))
        return NULL;

    if (local_album_id <= 0 || (size_t)local_album_id >= slot->album_tracks_ptrs_capacity)
        return NULL;

    const GPtrArray *src = slot->album_tracks_ptrs[local_album_id];
    if (!src)
        return NULL;

    /* Shallow copy — caller owns array, cache owns items */
    GPtrArray *result = g_ptr_array_new();
    for (guint i = 0; i < src->len; i++)
        g_ptr_array_add(result, g_ptr_array_index(src, i));
    return result;
}

/* =============================================================================
 * Query-Time MBID Resolution
 *
 * Instead of pre-building combined relationship arrays, query functions
 * resolve artist MBID → collect from all matching slots → dedup at query time.
 * ============================================================================= */

/**
 * Collect albums from a single slot's artist_albums array into `out`.
 * If `seen_mbrids` is non-NULL, deduplicates by musicbrainz_release_id.
 */
static void
collect_slot_artist_albums(LibrarySlot *slot,
                           int64_t local_aid,
                           uint32_t library_mask,
                           GHashTable *seen_mbrids,
                           GPtrArray *out)
{
    if (local_aid <= 0 || (size_t)local_aid >= slot->artist_albums_capacity)
        return;
    GPtrArray *src = slot->artist_albums[local_aid];
    if (!src)
        return;

    for (guint i = 0; i < src->len; i++) {
        library_album_info_t *album = g_ptr_array_index(src, i);
        int lib = LIBRARY_GLOBAL_ID_LIB(album->album_id);
        if (!(library_mask & (1u << lib)))
            continue;
        if (seen_mbrids && album->musicbrainz_release_group_id
            && album->musicbrainz_release_group_id[0]) {
            if (g_hash_table_contains(seen_mbrids, album->musicbrainz_release_group_id))
                continue;
            g_hash_table_add(seen_mbrids, album->musicbrainz_release_group_id);
        }
        g_ptr_array_add(out, album);
    }
}

/**
 * Collect appearances from a single slot into `out`.
 */
static void
collect_slot_appearances(LibrarySlot *slot,
                         int64_t local_aid,
                         uint32_t library_mask,
                         GHashTable *seen_mbrids,
                         GPtrArray *out)
{
    if (local_aid <= 0 || (size_t)local_aid >= slot->artist_appearances_capacity)
        return;
    GPtrArray *src = slot->artist_appearances[local_aid];
    if (!src)
        return;

    for (guint i = 0; i < src->len; i++) {
        library_album_info_t *album = g_ptr_array_index(src, i);
        int lib = LIBRARY_GLOBAL_ID_LIB(album->album_id);
        if (!(library_mask & (1u << lib)))
            continue;
        if (seen_mbrids && album->musicbrainz_release_group_id
            && album->musicbrainz_release_group_id[0]) {
            if (g_hash_table_contains(seen_mbrids, album->musicbrainz_release_group_id))
                continue;
            g_hash_table_add(seen_mbrids, album->musicbrainz_release_group_id);
        }
        g_ptr_array_add(out, album);
    }
}

/**
 * Collect appearance tracks from a single slot into `out`.
 */
static void
collect_slot_appearance_tracks(LibrarySlot *slot,
                               int64_t local_aid,
                               uint32_t library_mask,
                               GHashTable *seen_keys,
                               GPtrArray *out)
{
    if (local_aid <= 0 || (size_t)local_aid >= slot->artist_appearance_tracks_capacity)
        return;
    GPtrArray *src = slot->artist_appearance_tracks[local_aid];
    if (!src)
        return;

    for (guint i = 0; i < src->len; i++) {
        library_track_info_t *track = g_ptr_array_index(src, i);
        int lib = LIBRARY_GLOBAL_ID_LIB(track->track_id);
        if (!(library_mask & (1u << lib)))
            continue;
        if (seen_keys) {
            int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
            library_album_info_t *album = slot_get_album(slot, local_album_id);
            if (album && album->musicbrainz_release_group_id
                && album->musicbrainz_release_group_id[0]) {
                char key[128];
                snprintf(key,
                         sizeof(key),
                         "%s:%u:%u",
                         album->musicbrainz_release_group_id,
                         track->disc_num,
                         track->track_num);
                if (g_hash_table_contains(seen_keys, key))
                    continue;
                g_hash_table_add(seen_keys, g_strdup(key));
            }
        }
        g_ptr_array_add(out, track);
    }
}

/**
 * Resolve artist_id via MBID index.  If the artist has a MusicBrainz ID,
 * returns the index entry with all global_ids across libraries.
 */
static const struct mbid_artist_entry *
resolve_artist_mbid(library_cache_t *cache, int64_t artist_id)
{
    int64_t local_id;
    LibrarySlot *slot = decode_slot(cache, artist_id, &local_id);
    if (!slot)
        return NULL;
    library_artist_info_t *a = slot_get_artist(slot, local_id);
    if (!a || !a->musicbrainz_id)
        return NULL;
    return mbid_artist_lookup(cache, a->musicbrainz_id);
}

/**
 * Check if the mask selects multiple active libraries (for this cache).
 * When only one library is active, no cross-library dedup is needed.
 */
bool
library_mask_is_multi_library(library_cache_t *cache, uint32_t mask)
{
    int active = 0;
    for (int i = 0; i < cache->slot_count && active < 2; i++) {
        if (mask & (1u << cache->slots[i].bitmap_index))
            active++;
    }
    return active > 1;
}

/* Per-slot collector: append an artist's matching entities from one slot into
 * `out`, optionally deduping via `seen`. The three concrete collectors above
 * all share this shape. */
typedef void (*slot_collect_cb)(
    LibrarySlot *slot, int64_t local_aid, uint32_t library_mask, GHashTable *seen, GPtrArray *out);

/* Shared fan-out for the per-artist cross-library queries. Resolves the MBID
 * index entry, allocates a dedup table only when multiple libraries are active
 * (owning string keys when seen_owns_keys is set), invokes `collect` for every
 * matching slot, and returns the result (NULL when empty). */
static GPtrArray *
gather_artist_slots(library_cache_t *cache,
                    int64_t artist_id,
                    uint32_t library_mask,
                    slot_collect_cb collect,
                    bool seen_owns_keys)
{
    if (!cache || artist_id <= 0 || library_mask == 0)
        return NULL;

    const struct mbid_artist_entry *entry = resolve_artist_mbid(cache, artist_id);
    bool need_dedup = entry && library_mask_is_multi_library(cache, library_mask);
    GHashTable *seen = !need_dedup ? NULL
                       : seen_owns_keys
                           ? g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL)
                           : g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *result = g_ptr_array_new();

    if (entry) {
        for (uint8_t i = 0; i < entry->count; i++) {
            int64_t local;
            LibrarySlot *slot = decode_slot(cache, entry->global_ids[i], &local);
            if (slot)
                collect(slot, local, library_mask, seen, result);
        }
    } else {
        int64_t local;
        LibrarySlot *slot = decode_slot(cache, artist_id, &local);
        if (slot)
            collect(slot, local, library_mask, NULL, result);
    }

    if (seen)
        g_hash_table_destroy(seen);
    if (result->len == 0) {
        g_ptr_array_unref(result);
        return NULL;
    }
    return result;
}

GPtrArray *
library_cache_get_albums_by_artist(library_cache_t *cache, int64_t artist_id, uint32_t library_mask)
{
    return gather_artist_slots(cache, artist_id, library_mask, collect_slot_artist_albums, false);
}

/* =============================================================================
 * "Appears On" Queries
 * ============================================================================= */

GPtrArray *
library_cache_get_artist_appearances(library_cache_t *cache,
                                     int64_t artist_id,
                                     uint32_t library_mask)
{
    return gather_artist_slots(cache, artist_id, library_mask, collect_slot_appearances, false);
}

GPtrArray *
library_cache_get_artist_appearance_tracks(library_cache_t *cache,
                                           int64_t artist_id,
                                           uint32_t library_mask)
{
    return gather_artist_slots(
        cache, artist_id, library_mask, collect_slot_appearance_tracks, true);
}

/* =============================================================================
 * Merged Artist Counts (computed on demand)
 * ============================================================================= */

void
library_cache_get_merged_artist_counts(library_cache_t *cache,
                                       int64_t artist_id,
                                       uint32_t library_mask,
                                       uint32_t *album_count,
                                       uint32_t *appearance_count)
{
    if (album_count)
        *album_count = 0;
    if (appearance_count)
        *appearance_count = 0;
    if (!cache || artist_id <= 0 || library_mask == 0)
        return;

    const struct mbid_artist_entry *entry = resolve_artist_mbid(cache, artist_id);
    bool multi = entry && library_mask_is_multi_library(cache, library_mask);

    /* ── Album count (MBRID-deduped) ── */
    if (album_count) {
        GHashTable *seen = multi ? g_hash_table_new(g_str_hash, g_str_equal) : NULL;
        uint32_t count = 0;

        if (entry) {
            for (uint8_t i = 0; i < entry->count; i++) {
                int64_t local;
                LibrarySlot *slot = decode_slot(cache, entry->global_ids[i], &local);
                if (!slot)
                    continue;
                if (local <= 0 || (size_t)local >= slot->artist_albums_capacity)
                    continue;
                GPtrArray *albums = slot->artist_albums[local];
                if (!albums)
                    continue;
                for (guint j = 0; j < albums->len; j++) {
                    library_album_info_t *a = g_ptr_array_index(albums, j);
                    int lib = LIBRARY_GLOBAL_ID_LIB(a->album_id);
                    if (!(library_mask & (1u << lib)))
                        continue;
                    if (seen && a->musicbrainz_release_group_id
                        && a->musicbrainz_release_group_id[0]) {
                        if (g_hash_table_contains(seen, a->musicbrainz_release_group_id))
                            continue;
                        g_hash_table_add(seen, a->musicbrainz_release_group_id);
                    }
                    count++;
                }
            }
        } else {
            int64_t local;
            LibrarySlot *slot = decode_slot(cache, artist_id, &local);
            if (slot && local > 0 && (size_t)local < slot->artist_albums_capacity) {
                GPtrArray *albums = slot->artist_albums[local];
                if (albums) {
                    for (guint j = 0; j < albums->len; j++) {
                        library_album_info_t *a = g_ptr_array_index(albums, j);
                        int lib = LIBRARY_GLOBAL_ID_LIB(a->album_id);
                        if (library_mask & (1u << lib))
                            count++;
                    }
                }
            }
        }
        if (seen)
            g_hash_table_destroy(seen);
        *album_count = count;
    }

    /* ── Appearance count: tracks on OTHER artists' albums ──
     * artist_appearance_tracks already excludes the artist's own albums
     * (filtered during warming at line ~737: skip album_local_artist).
     * Dedup by "mbrid:disc:track" across libraries. */
    if (appearance_count) {
        GHashTable *seen
            = multi ? g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL) : NULL;
        uint32_t count = 0;

        if (entry) {
            for (uint8_t i = 0; i < entry->count; i++) {
                int64_t local;
                LibrarySlot *slot = decode_slot(cache, entry->global_ids[i], &local);
                if (!slot)
                    continue;
                if (local <= 0 || (size_t)local >= slot->artist_appearance_tracks_capacity)
                    continue;
                GPtrArray *tracks = slot->artist_appearance_tracks[local];
                if (!tracks)
                    continue;
                for (guint j = 0; j < tracks->len; j++) {
                    library_track_info_t *t = g_ptr_array_index(tracks, j);
                    int lib = LIBRARY_GLOBAL_ID_LIB(t->track_id);
                    if (!(library_mask & (1u << lib)))
                        continue;
                    if (seen) {
                        int64_t la = LIBRARY_GLOBAL_ID_LOCAL(t->album_id);
                        library_album_info_t *alb = slot_get_album(slot, la);
                        if (alb && alb->musicbrainz_release_group_id
                            && alb->musicbrainz_release_group_id[0]) {
                            char key[128];
                            snprintf(key,
                                     sizeof(key),
                                     "%s:%u:%u",
                                     alb->musicbrainz_release_group_id,
                                     t->disc_num,
                                     t->track_num);
                            if (g_hash_table_contains(seen, key))
                                continue;
                            g_hash_table_add(seen, g_strdup(key));
                        }
                    }
                    count++;
                }
            }
        } else {
            int64_t local;
            LibrarySlot *slot = decode_slot(cache, artist_id, &local);
            if (slot && local > 0 && (size_t)local < slot->artist_appearance_tracks_capacity) {
                GPtrArray *tracks = slot->artist_appearance_tracks[local];
                if (tracks) {
                    for (guint j = 0; j < tracks->len; j++) {
                        library_track_info_t *t = g_ptr_array_index(tracks, j);
                        int lib = LIBRARY_GLOBAL_ID_LIB(t->track_id);
                        if (library_mask & (1u << lib))
                            count++;
                    }
                }
            }
        }
        if (seen)
            g_hash_table_destroy(seen);
        *appearance_count = count;
    }
}

/* =============================================================================
 * Filtered Queries — iterate ALL slots' DBs, merge results
 * ============================================================================= */

GPtrArray *
library_cache_get_artists_filtered(library_cache_t *cache,
                                   library_sort_t sort,
                                   const char *search_text,
                                   const db_search_opts_t *filters,
                                   uint32_t library_mask)
{
    g_assert(cache != NULL);

    GPtrArray *result = g_ptr_array_new();
    /* Dedup MBID artists when showing all libraries (each MBID appears once) */
    GHashTable *seen_mbids = library_mask_is_multi_library(cache, library_mask)
                                 ? g_hash_table_new(g_str_hash, g_str_equal)
                                 : NULL;

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        if (!(library_mask & (1u << slot->bitmap_index)))
            continue;
        if (!slot->db)
            continue;
        if (!atomic_load(&slot->available))
            continue;

        db_id_query_opts_t opts = {
            .search_text = search_text,
            .filters = filters,
            .sort = library_sort_to_db_sort(sort),
        };

        int64_t *ids = NULL;
        size_t count = 0;
        db_get_entity_ids_filtered(slot->db, DB_ENTITY_ARTIST, &opts, &ids, &count);

        for (size_t j = 0; j < count; j++) {
            int64_t local_id = ids[j];
            library_artist_info_t *info = slot_get_artist(slot, local_id);
            if (!info)
                continue;
            /* MBID dedup: when viewing all libraries, each MBID artist
             * appears once (first slot encountered wins). */
            if (seen_mbids && info->musicbrainz_id) {
                if (g_hash_table_contains(seen_mbids, info->musicbrainz_id))
                    continue;
                g_hash_table_add(seen_mbids, info->musicbrainz_id);
            }
            g_ptr_array_add(result, info);
        }

        g_free(ids);
    }

    if (seen_mbids)
        g_hash_table_destroy(seen_mbids);
    return result;
}

GPtrArray *
library_cache_get_albums_filtered(library_cache_t *cache,
                                  library_sort_t sort,
                                  const char *search_text,
                                  const db_search_opts_t *filters,
                                  uint32_t library_mask)
{
    g_assert(cache != NULL);

    GPtrArray *result = g_ptr_array_new();
    GHashTable *seen_mbrids = library_mask_is_multi_library(cache, library_mask)
                                  ? g_hash_table_new(g_str_hash, g_str_equal)
                                  : NULL;

    for (int i = 0; i < cache->slot_count; i++) {
        LibrarySlot *slot = &cache->slots[i];
        if (!(library_mask & (1u << slot->bitmap_index)))
            continue;
        if (!slot->db)
            continue;
        if (!atomic_load(&slot->available))
            continue;

        db_id_query_opts_t opts = {
            .search_text = search_text,
            .filters = filters,
            .sort = library_sort_to_db_sort(sort),
        };

        int64_t *ids = NULL;
        size_t count = 0;
        db_get_entity_ids_filtered(slot->db, DB_ENTITY_ALBUM, &opts, &ids, &count);

        for (size_t j = 0; j < count; j++) {
            int64_t local_id = ids[j];
            library_album_info_t *info = slot_get_album(slot, local_id);
            if (!info)
                continue;
            if (seen_mbrids && info->musicbrainz_release_group_id
                && info->musicbrainz_release_group_id[0]) {
                if (g_hash_table_contains(seen_mbrids, info->musicbrainz_release_group_id))
                    continue;
                g_hash_table_add(seen_mbrids, info->musicbrainz_release_group_id);
            }
            g_ptr_array_add(result, info);
        }

        g_free(ids);
    }

    if (seen_mbrids)
        g_hash_table_destroy(seen_mbrids);
    return result;
}

/* =============================================================================
 * Search
 * ============================================================================= */

/* library_cache_search / library_search_results_free now live in cache_search.c. */

/* =============================================================================
 * Prefetch API
 * ============================================================================= */

void
library_cache_prefetch_audio_files(library_cache_t *cache, const int64_t *track_ids, size_t count)
{
    if (!cache || !track_ids || count == 0)
        return;

    for (size_t i = 0; i < count; i++) {
        int64_t local_tid;
        LibrarySlot *slot = decode_slot(cache, track_ids[i], &local_tid);
        if (!slot)
            continue;

        const library_track_info_t *track = slot_get_track(slot, local_tid);
        if (track && track->path) {
            int64_t local_album_id = LIBRARY_GLOBAL_ID_LOCAL(track->album_id);
            library_album_info_t *album = slot_get_album(slot, local_album_id);
            char *full_path
                = resolve_track_path(slot->music_base, album ? album->path : NULL, track->path);
            if (full_path)
                g_async_queue_push(cache->prefetch_queue, full_path);
        }
    }
}
