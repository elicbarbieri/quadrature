/**
 * library_credit_search — shared cross-library credit-search implementation.
 *
 * See include/quadrature/library_search.h for semantics. This file is the ONLY
 * place that (a) traverses per-library meta DBs for credit matches and
 * (b) collapses cross-library duplicates — both production UI and integration
 * tests call through here.
 *
 * Dedup keys:
 *   tracks: "<RGID>|<disc>|<track>" when the album has an RGID; otherwise the
 *           raw album_id (fallback; pre-Phase-6 albums without MBIDs can't be
 *           matched across libraries by any positional key).
 *   albums: "<RGID>" when present; otherwise raw album_id.
 *
 * Thread model:
 *   Safe to call from any thread. Performs no GTK/UI work. library_cache
 *   accessors are used for cache pointers only long enough to extract
 *   int64_t IDs and MBID strings (dup'd into the result); no interior
 *   pointers are retained across the call boundary, so callers may use the
 *   result after cache refresh.
 */

#define G_LOG_DOMAIN "quadrature"

#include "quadrature/library_search.h"
#include "quadrature/database.h"
#include "quadrature/metadata.h"

#include <string.h>

/* =============================================================================
 * library_credit_info_t lifecycle
 * ============================================================================= */

static library_credit_info_t *
credit_info_new(const char *artist_name, const char *artist_mbid)
{
    library_credit_info_t *info = g_new0(library_credit_info_t, 1);
    info->roles = g_ptr_array_new_with_free_func(g_free);
    info->artist_name = g_strdup(artist_name ? artist_name : "");
    info->artist_mbid = g_strdup(artist_mbid ? artist_mbid : "");
    return info;
}

static void
credit_info_free(gpointer data)
{
    library_credit_info_t *info = data;
    if (!info)
        return;
    g_ptr_array_unref(info->roles);
    g_free(info->artist_name);
    g_free(info->artist_mbid);
    g_free(info);
}

/* Add `role` to `info->roles` if not already present (linear scan — the number
 * of distinct roles per track is tiny, hash-set overhead isn't worth it). */
static void
credit_info_add_role(library_credit_info_t *info, const char *role)
{
    if (!role || !role[0])
        return;
    for (guint i = 0; i < info->roles->len; i++) {
        if (g_strcmp0(g_ptr_array_index(info->roles, i), role) == 0)
            return;
    }
    g_ptr_array_add(info->roles, g_strdup(role));
}

static void
credit_info_merge(library_credit_info_t *dst, const library_credit_info_t *src)
{
    for (guint i = 0; i < src->roles->len; i++)
        credit_info_add_role(dst, g_ptr_array_index(src->roles, i));
}

/* Format a user-facing role label from a db_meta_artist_credit_t. Mirrors
 * the behavior that used to live in build_credit_track_set. */
static char *
format_role(const db_meta_artist_credit_t *c)
{
    const char *base = (c->attributes && c->attributes[0])
                           ? c->attributes
                           : (c->link_type_name ? c->link_type_name : "Credit");
    char *role = g_strdup(base);
    if (role[0])
        role[0] = g_ascii_toupper(role[0]);
    return role;
}

/* =============================================================================
 * Phase 1: gather per-library hits, keyed by global track_id
 * ============================================================================= */

/* Append a packed "mbid\tname\ttype" meta-artist entry, deduped by MBID. */
static void
append_meta_artist(GPtrArray *meta_artists,
                   GHashTable *seen_mbids,
                   const db_meta_artist_search_result_t *a)
{
    if (!a->artist_mbid)
        return;
    if (g_hash_table_contains(seen_mbids, a->artist_mbid))
        return;
    g_hash_table_add(seen_mbids, g_strdup(a->artist_mbid));
    g_ptr_array_add(meta_artists,
                    g_strdup_printf("%s\t%s\t%s",
                                    a->artist_mbid,
                                    a->name ? a->name : "",
                                    a->artist_type ? a->artist_type : ""));
}

/* Walk one library's meta DB and populate `track_set` (global_id → info) and
 * `meta_artists` / `seen_meta_mbids`. */
static void
gather_library(library_cache_t *cache,
               int bitmap_index,
               const char *credit_text,
               const char *role_gid,
               GHashTable *track_set,
               GPtrArray *meta_artists,
               GHashTable *seen_meta_mbids)
{
    library_cache_dbs_t dbs = library_cache_get_dbs(cache, bitmap_index);
    if (!dbs.meta || !dbs.db)
        return;

    db_meta_artist_search_result_t *artists = NULL;
    size_t artist_count = 0;
    if (db_meta_search_artists(dbs.meta, credit_text, 50, &artists, &artist_count) != QUADRATURE_OK)
        return;
    if (artist_count == 0) {
        db_meta_artist_search_results_free(artists, artist_count);
        return;
    }

    for (size_t ai = 0; ai < artist_count; ai++) {
        append_meta_artist(meta_artists, seen_meta_mbids, &artists[ai]);

        db_meta_artist_credit_t *credits = NULL;
        size_t credit_count = 0;
        if (db_meta_get_credits_by_artist(
                dbs.meta, artists[ai].artist_mbid, role_gid, &credits, &credit_count)
                != QUADRATURE_OK
            || credit_count == 0) {
            db_meta_artist_credits_free(credits, credit_count);
            continue;
        }

        db_track_position_t *pos = g_new0(db_track_position_t, credit_count);
        int64_t *local_ids = g_new0(int64_t, credit_count);
        for (size_t ci = 0; ci < credit_count; ci++) {
            pos[ci].release_mbid = credits[ci].release_mbid;
            pos[ci].disc_num = credits[ci].disc_num;
            pos[ci].track_num = credits[ci].track_num;
        }
        db_resolve_track_positions_batch(dbs.db, pos, credit_count, local_ids);

        for (size_t ci = 0; ci < credit_count; ci++) {
            if (local_ids[ci] == 0)
                continue;
            int64_t global_id = LIBRARY_MAKE_GLOBAL_ID(bitmap_index, local_ids[ci]);

            int64_t *key_lookup = &global_id;
            library_credit_info_t *info = g_hash_table_lookup(track_set, key_lookup);
            if (!info) {
                info = credit_info_new(artists[ai].name, artists[ai].artist_mbid);
                int64_t *k = g_new(int64_t, 1);
                *k = global_id;
                g_hash_table_insert(track_set, k, info);
            }
            char *role = format_role(&credits[ci]);
            credit_info_add_role(info, role);
            g_free(role);
        }

        g_free(pos);
        g_free(local_ids);
        db_meta_artist_credits_free(credits, credit_count);
    }

    db_meta_artist_search_results_free(artists, artist_count);
}

/* =============================================================================
 * Phase 2: cross-library dedup by (RGID|disc|track) for tracks, RGID for albums
 * ============================================================================= */

/* Build the per-track dedup key. Falls back to a synthetic key for albums
 * without an RGID so each still surfaces once (can't be merged across
 * libraries, but also shouldn't collide). */
static char *
track_dedup_key(const library_track_info_t *track, const library_album_info_t *album)
{
    if (album && album->musicbrainz_release_group_id && album->musicbrainz_release_group_id[0]) {
        return g_strdup_printf(
            "%s|%d|%d", album->musicbrainz_release_group_id, track->disc_num, track->track_num);
    }
    return g_strdup_printf("@%" G_GINT64_FORMAT, track->album_id);
}

static char *
album_dedup_key(const library_album_info_t *album)
{
    if (album && album->musicbrainz_release_group_id && album->musicbrainz_release_group_id[0]) {
        return g_strdup(album->musicbrainz_release_group_id);
    }
    return g_strdup_printf("@%" G_GINT64_FORMAT, album ? album->album_id : 0);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

library_credit_search_result_t *
library_credit_search(library_cache_t *cache,
                      const char *credit_text,
                      const char *role_gid,
                      uint32_t library_mask)
{
    g_assert(cache != NULL);

    library_credit_search_result_t *r = g_new0(library_credit_search_result_t, 1);
    r->track_ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
    r->album_ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
    r->credit_info = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, credit_info_free);
    r->meta_artists = g_ptr_array_new_with_free_func(g_free);

    if (!credit_text || !credit_text[0])
        return r;

    /* Phase 1: raw hits keyed by global track_id. */
    GHashTable *raw_hits
        = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, credit_info_free);
    GHashTable *seen_meta_mbids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    int lib_count = library_cache_get_library_count(cache);
    for (int li = 0; li < lib_count; li++) {
        int bi = library_cache_get_bitmap_index(cache, li);
        if (!(library_mask & (1u << bi)))
            continue;
        if (!library_cache_get_available(cache, bi))
            continue;
        gather_library(
            cache, bi, credit_text, role_gid, raw_hits, r->meta_artists, seen_meta_mbids);
    }
    g_hash_table_destroy(seen_meta_mbids);

    /* Phase 2: dedup by (RGID|disc|track). Track duplicates are merged into
     * the surviving credit_info — roles from every library that produced
     * the same recording accumulate on the survivor. */
    GHashTable *surviving_by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_album_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, raw_hits);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        int64_t global_id = *(int64_t *)k;
        library_credit_info_t *incoming = v;

        const library_track_info_t *track = library_cache_get_track(cache, global_id);
        if (!track)
            continue;
        const library_album_info_t *album
            = library_cache_get_album(cache, track->album_id, library_mask);

        char *key = track_dedup_key(track, album);
        int64_t *survivor_id = (int64_t *)g_hash_table_lookup(surviving_by_key, key);

        if (survivor_id) {
            /* Dup — merge roles into survivor. */
            library_credit_info_t *survivor_info = g_hash_table_lookup(r->credit_info, survivor_id);
            if (survivor_info)
                credit_info_merge(survivor_info, incoming);
            g_free(key);
            continue;
        }

        /* Survivor. Steal the (key, info) pair from raw_hits and hand the info
         * to r->credit_info. raw_hits's key allocation is reused as the
         * credit_info key; no leak. */
        int64_t *tid = g_new(int64_t, 1);
        *tid = global_id;
        g_hash_table_insert(surviving_by_key, key, tid);
        g_hash_table_iter_steal(&iter);
        g_hash_table_insert(r->credit_info, k, incoming);
        g_array_append_val(r->track_ids, global_id);

        if (album) {
            char *akey = album_dedup_key(album);
            if (!g_hash_table_contains(seen_album_keys, akey)) {
                g_hash_table_add(seen_album_keys, akey);
                int64_t album_id = album->album_id;
                g_array_append_val(r->album_ids, album_id);
            } else {
                g_free(akey);
            }
        }
    }

    g_hash_table_destroy(surviving_by_key);
    g_hash_table_destroy(seen_album_keys);
    g_hash_table_destroy(raw_hits);
    return r;
}

void
library_credit_search_result_free(library_credit_search_result_t *r)
{
    if (!r)
        return;
    if (r->track_ids)
        g_array_unref(r->track_ids);
    if (r->album_ids)
        g_array_unref(r->album_ids);
    if (r->credit_info)
        g_hash_table_unref(r->credit_info);
    if (r->meta_artists)
        g_ptr_array_unref(r->meta_artists);
    g_free(r);
}
