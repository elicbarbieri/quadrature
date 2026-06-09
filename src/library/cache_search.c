/**
 * cache_search — FTS over the library cache's per-slot main DBs.
 *
 * library_cache_search is the UI-facing query that lights up the search bar:
 * artists, albums, tracks matching a free-text query. Each slot's quadrature.sqlite
 * holds the FTS index; this file loops slots, intersects with the requested
 * library_mask, runs the per-type queries, and dedupes cross-library by MBID
 * (artist MBID, album RGID, track (RGID|disc|track)). Fallback spelling
 * correction taps the cache's merged vocabulary.
 *
 * No credit-meta DB access here — that's library_search.c's domain.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/library_search.h"
#include "quadrature/database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t
run_search_queries(library_cache_t *cache,
                   const char *query,
                   library_search_filter_t filter,
                   size_t limit,
                   const db_search_opts_t *opts,
                   uint32_t library_mask,
                   library_search_results_t *results)
{
    size_t total = 0;
    bool multi = library_mask_is_multi_library(cache, library_mask);
    GHashTable *seen_artist_mbids = multi ? g_hash_table_new(g_str_hash, g_str_equal) : NULL;
    GHashTable *seen_album_mbrids = multi ? g_hash_table_new(g_str_hash, g_str_equal) : NULL;
    GHashTable *seen_track_keys
        = multi ? g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL) : NULL;

    for (int slot_idx = 0; slot_idx < cache->slot_count; slot_idx++) {
        LibrarySlot *slot = &cache->slots[slot_idx];
        if (!(library_mask & (1u << slot->bitmap_index)))
            continue;
        if (!slot->db)
            continue;
        if (!atomic_load(&slot->available))
            continue;

        if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_ARTISTS) {
            db_id_query_opts_t id_opts = {
                .search_text = query,
                .filters = opts,
                .sort = DB_SORT_NAME_ASC,
            };
            int64_t *ids = NULL;
            size_t count = 0;
            db_get_entity_ids_filtered(slot->db, DB_ENTITY_ARTIST, &id_opts, &ids, &count);

            for (size_t i = 0; i < count; i++) {
                library_artist_info_t *info = slot_get_artist(slot, ids[i]);
                if (!info)
                    continue;
                if (seen_artist_mbids && info->musicbrainz_id) {
                    if (g_hash_table_contains(seen_artist_mbids, info->musicbrainz_id))
                        continue;
                    g_hash_table_add(seen_artist_mbids, info->musicbrainz_id);
                }
                g_ptr_array_add(results->artists, info);
            }
            g_free(ids);
        }

        if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_ALBUMS) {
            db_id_query_opts_t id_opts = {
                .search_text = query,
                .filters = opts,
                .sort = DB_SORT_NAME_ASC,
            };
            int64_t *ids = NULL;
            size_t count = 0;
            db_get_entity_ids_filtered(slot->db, DB_ENTITY_ALBUM, &id_opts, &ids, &count);

            for (size_t i = 0; i < count; i++) {
                library_album_info_t *info = slot_get_album(slot, ids[i]);
                if (!info)
                    continue;
                if (seen_album_mbrids && info->musicbrainz_release_group_id
                    && info->musicbrainz_release_group_id[0]) {
                    if (g_hash_table_contains(seen_album_mbrids,
                                              info->musicbrainz_release_group_id))
                        continue;
                    g_hash_table_add(seen_album_mbrids, info->musicbrainz_release_group_id);
                }
                g_ptr_array_add(results->albums, info);
            }
            g_free(ids);
        }

        if (filter == LIBRARY_SEARCH_FILTER_ALL || filter == LIBRARY_SEARCH_FILTER_TRACKS) {
            db_id_query_opts_t id_opts = {
                .search_text = query,
                .filters = opts,
                .limit = limit > 0 ? limit : 100,
            };
            int64_t *ids = NULL;
            size_t count = 0;
            db_get_entity_ids_filtered(slot->db, DB_ENTITY_TRACK, &id_opts, &ids, &count);

            for (size_t i = 0; i < count; i++) {
                library_track_info_t *info = slot_get_track(slot, ids[i]);
                if (!info)
                    continue;
                if (seen_track_keys) {
                    int64_t local_album = LIBRARY_GLOBAL_ID_LOCAL(info->album_id);
                    library_album_info_t *alb = slot_get_album(slot, local_album);
                    if (alb && alb->musicbrainz_release_group_id
                        && alb->musicbrainz_release_group_id[0]) {
                        char key[128];
                        snprintf(key,
                                 sizeof(key),
                                 "%s:%u:%u",
                                 alb->musicbrainz_release_group_id,
                                 info->disc_num,
                                 info->track_num);
                        if (g_hash_table_contains(seen_track_keys, key))
                            continue;
                        g_hash_table_add(seen_track_keys, g_strdup(key));
                    }
                }
                g_ptr_array_add(results->tracks, info);
            }
            g_free(ids);
        }
    }

    if (seen_artist_mbids)
        g_hash_table_destroy(seen_artist_mbids);
    if (seen_album_mbrids)
        g_hash_table_destroy(seen_album_mbrids);
    if (seen_track_keys)
        g_hash_table_destroy(seen_track_keys);

    /* Enforce final limits across all slots */
    size_t artist_max = (filter == LIBRARY_SEARCH_FILTER_ALL) ? (limit > 0 ? limit : 3)
                                                              : (limit > 0 ? limit : 100);
    size_t album_max = (filter == LIBRARY_SEARCH_FILTER_ALL) ? (limit > 0 ? limit : 4)
                                                             : (limit > 0 ? limit : 100);
    size_t track_max = (filter == LIBRARY_SEARCH_FILTER_ALL) ? (limit > 0 ? limit : 8)
                                                             : (limit > 0 ? limit : 100);
    if (results->artists->len > artist_max)
        g_ptr_array_set_size(results->artists, artist_max);
    if (results->albums->len > album_max)
        g_ptr_array_set_size(results->albums, album_max);
    if (results->tracks->len > track_max)
        g_ptr_array_set_size(results->tracks, track_max);

    total = results->artists->len + results->albums->len + results->tracks->len;
    return total;
}

library_search_results_t *
library_cache_search(library_cache_t *cache,
                     const char *query,
                     library_search_filter_t filter,
                     size_t limit,
                     const db_search_opts_t *opts,
                     uint32_t library_mask)
{
    if (!cache || !query)
        return NULL;

    library_search_results_t *results = g_new0(library_search_results_t, 1);
    results->artists = g_ptr_array_new();
    results->albums = g_ptr_array_new();
    results->tracks = g_ptr_array_new();

    size_t found = run_search_queries(cache, query, filter, limit, opts, library_mask, results);

    if (found == 0 && strlen(query) >= 2 && cache->search_vocab_count > 0) {
        char *corrected = library_build_corrected_query(cache, query);
        if (corrected && g_strcmp0(corrected, query) != 0) {
            g_debug("cache search: no results for '%s', trying corrected '%s'", query, corrected);
            found
                = run_search_queries(cache, corrected, filter, limit, opts, library_mask, results);
        }
        g_free(corrected);
    }

    results->total_artists = results->artists->len;
    results->total_albums = results->albums->len;
    results->total_tracks = results->tracks->len;

    return results;
}

void
library_search_results_free(library_search_results_t *results)
{
    if (!results)
        return;
    if (results->artists)
        g_ptr_array_unref(results->artists);
    if (results->albums)
        g_ptr_array_unref(results->albums);
    if (results->tracks)
        g_ptr_array_unref(results->tracks);
    g_free(results);
}
