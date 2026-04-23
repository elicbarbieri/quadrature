#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

// Forward declarations for helpers used before their definition
static char* build_fts_query(const char* input);

// =============================================================================
// Track Read Operations
// =============================================================================

quadrature_result_t db_get_track(quadrature_db_t* db, int64_t id, db_track_t** out) {
    if (!db || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt* stmt = db->read_track_by_id;
    db_lock(db);
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, id);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        db_track_t* track = calloc(1, sizeof(db_track_t));
        if (!track) {
            sqlite3_reset(stmt);
            db_unlock(db);
            return QUADRATURE_ERROR_OUT_OF_MEMORY;
        }

        track->id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        const char* album = (const char*)sqlite3_column_text(stmt, 3);
        const char* path = (const char*)sqlite3_column_text(stmt, 4);
        track->title = title ? strdup(title) : strdup("Unknown");
        track->artist = artist ? strdup(artist) : strdup("Unknown Artist");
        const char* artist_display = (const char*)sqlite3_column_text(stmt, 13);
        track->artist_display = artist_display ? strdup(artist_display) : NULL;
        track->album = album ? strdup(album) : strdup("Unknown Album");
        track->path = path ? strdup(path) : strdup("");
        track->duration_ms = sqlite3_column_int(stmt, 5);
        track->track_num = sqlite3_column_int(stmt, 6);
        track->disc_num = sqlite3_column_int(stmt, 7);
        if (track->disc_num == 0) track->disc_num = 1;
        track->year = sqlite3_column_int(stmt, 8);
        track->album_id = sqlite3_column_int64(stmt, 9);
        track->artist_id = sqlite3_column_int64(stmt, 10);
        const char* genre = (const char*)sqlite3_column_text(stmt, 11);
        track->genre = genre ? strdup(genre) : NULL;
        const char* album_path = (const char*)sqlite3_column_text(stmt, 12);
        track->album_path = album_path ? strdup(album_path) : NULL;

        *out = track;
        res = QUADRATURE_OK;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return res;
}

// =============================================================================
// Artist/Album Read Operations
// =============================================================================

quadrature_result_t db_get_album_by_id(quadrature_db_t* db, int64_t album_id, db_album_t** out) {
    if (!db || !out) return QUADRATURE_ERROR_INVALID_PARAM;
    *out = NULL;

    db_lock(db);

    sqlite3_stmt* stmt = db->read_album_by_id;
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, album_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        db_album_t* a = calloc(1, sizeof(db_album_t));
        a->id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        a->title = title ? strdup(title) : strdup("Unknown Album");
        a->artist_name = artist ? strdup(artist) : strdup("Unknown Artist");
        a->artist_id = sqlite3_column_int64(stmt, 3);
        a->year = sqlite3_column_int(stmt, 4);
        a->track_count = sqlite3_column_int64(stmt, 5);
        const char* genres = (const char*)sqlite3_column_text(stmt, 6);
        a->genres = (genres && *genres) ? strdup(genres) : NULL;
        const char* path = (const char*)sqlite3_column_text(stmt, 7);
        a->path = path ? strdup(path) : strdup("");
        const char* mbid = (const char*)sqlite3_column_text(stmt, 8);
        a->musicbrainz_release_id = mbid ? strdup(mbid) : NULL;
        *out = a;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

/* Fill a db_track_t from the current row of a statement using TRACK_SELECT_COLS order.
 * If `borrow` is true, string pointers alias SQLite memory (valid until next step/finalize).
 * If `borrow` is false, strings are strdup'd (caller must free). */
static void fill_track_from_row(sqlite3_stmt* stmt, db_track_t* t, bool borrow) {
    t->id          = sqlite3_column_int64(stmt, 0);
    t->duration_ms = sqlite3_column_int(stmt, 5);
    t->track_num   = sqlite3_column_int(stmt, 6);
    t->disc_num    = sqlite3_column_int(stmt, 7);
    if (t->disc_num == 0) t->disc_num = 1;
    t->year        = sqlite3_column_int(stmt, 8);
    t->album_id    = sqlite3_column_int64(stmt, 9);
    t->artist_id   = sqlite3_column_int64(stmt, 10);

    const char* title          = (const char*)sqlite3_column_text(stmt, 1);
    const char* artist         = (const char*)sqlite3_column_text(stmt, 2);
    const char* album          = (const char*)sqlite3_column_text(stmt, 3);
    const char* path           = (const char*)sqlite3_column_text(stmt, 4);
    const char* genre          = (const char*)sqlite3_column_text(stmt, 11);
    const char* album_path     = (const char*)sqlite3_column_text(stmt, 12);
    const char* album_mbid     = (const char*)sqlite3_column_text(stmt, 13);
    const char* artist_display = (const char*)sqlite3_column_text(stmt, 14);

    if (borrow) {
        /* Borrowed pointers — valid only until next sqlite3_step or finalize.
         * Cast away const; caller must treat as read-only. */
        t->title          = (char*)(title ? title : "Unknown");
        t->artist         = (char*)(artist ? artist : "Unknown Artist");
        t->album          = (char*)(album ? album : "Unknown Album");
        t->path           = (char*)(path ? path : "");
        t->genre          = (char*)genre;
        t->album_path     = (char*)album_path;
        t->album_musicbrainz_release_id = (char*)album_mbid;
        t->artist_display = (char*)artist_display;
    } else {
        t->title          = strdup(title ? title : "Unknown");
        t->artist         = strdup(artist ? artist : "Unknown Artist");
        t->album          = strdup(album ? album : "Unknown Album");
        t->path           = strdup(path ? path : "");
        t->genre          = genre ? strdup(genre) : NULL;
        t->album_path     = album_path ? strdup(album_path) : NULL;
        t->album_musicbrainz_release_id = album_mbid ? strdup(album_mbid) : NULL;
        t->artist_display = artist_display ? strdup(artist_display) : NULL;
    }
}

quadrature_result_t db_get_tracks_by_album(quadrature_db_t* db, int64_t album_id, db_track_t** out, size_t* count) {
    if (!db || !out || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    sqlite3_stmt* stmt = db->read_tracks_by_album;
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, album_id);

    /* Grow dynamically — no COUNT query needed. Most albums have <30 tracks. */
    size_t cap = 32;
    size_t i = 0;
    db_track_t* results = calloc(cap, sizeof(db_track_t));
    if (!results) {
        sqlite3_finalize(stmt);
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (i == cap) {
            cap *= 2;
            db_track_t* grown = realloc(results, cap * sizeof(db_track_t));
            if (!grown) {
                db_tracks_free(results, i);
                sqlite3_finalize(stmt);
                db_unlock(db);
                return QUADRATURE_ERROR_OUT_OF_MEMORY;
            }
            results = grown;
            memset(results + i, 0, (cap - i) * sizeof(db_track_t));
        }
        fill_track_from_row(stmt, &results[i], false);
        i++;
    }

    sqlite3_reset(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_tracks_by_album(%" G_GINT64_FORMAT "): %zu results in %.2f ms", album_id, i, elapsed / 1000.0);

    if (i == 0) {
        free(results);
        results = NULL;
    }

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

/* =============================================================================
 * Warming Iterators (no JOINs — resolve from already-loaded slot arrays)
 * ============================================================================= */

quadrature_result_t db_iter_all_artists(quadrature_db_t *db,
                                         db_artist_iter_cb cb, void *user_data) {
    if (!db || !cb) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt *stmt = db->iter_all_artists;
    if (!stmt) { db_unlock(db); return QUADRATURE_OK; }
    sqlite3_reset(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_artist_t a = {0};
        a.id   = sqlite3_column_int64(stmt, 0);
        a.name = (char *)sqlite3_column_text(stmt, 1);
        a.musicbrainz_id = (char *)sqlite3_column_text(stmt, 2);
        if (!a.name) a.name = "Unknown Artist";
        if (!cb(&a, user_data)) break;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_iter_all_albums(quadrature_db_t *db,
                                        db_album_iter_cb cb, void *user_data) {
    if (!db || !cb) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt *stmt = db->iter_all_albums;
    if (!stmt) { db_unlock(db); return QUADRATURE_OK; }
    sqlite3_reset(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_album_t a = {0};
        a.id        = sqlite3_column_int64(stmt, 0);
        a.title     = (char *)sqlite3_column_text(stmt, 1);
        a.artist_id = sqlite3_column_int64(stmt, 2);
        a.year      = sqlite3_column_int(stmt, 3);
        a.path      = (char *)sqlite3_column_text(stmt, 4);
        a.musicbrainz_release_id = (char *)sqlite3_column_text(stmt, 5);
        a.musicbrainz_release_group_id = (char *)sqlite3_column_text(stmt, 6);
        if (!a.title) a.title = "Unknown Album";
        if (!cb(&a, user_data)) break;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_iter_all_tracks(quadrature_db_t *db,
                                        db_track_iter_cb cb, void *user_data) {
    if (!db || !cb) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt *stmt = db->iter_all_tracks;
    if (!stmt) { db_unlock(db); return QUADRATURE_OK; }
    sqlite3_reset(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_track_lean_t t = {0};
        t.id          = sqlite3_column_int64(stmt, 0);
        t.title       = (const char *)sqlite3_column_text(stmt, 1);
        t.path        = (const char *)sqlite3_column_text(stmt, 2);
        t.duration_ms = (uint32_t)sqlite3_column_int(stmt, 3);
        t.track_num   = (uint16_t)sqlite3_column_int(stmt, 4);
        t.disc_num    = (uint16_t)sqlite3_column_int(stmt, 5);
        if (t.disc_num == 0) t.disc_num = 1;
        t.year        = (uint16_t)sqlite3_column_int(stmt, 6);
        t.album_id    = sqlite3_column_int64(stmt, 7);
        t.genre       = (const char *)sqlite3_column_text(stmt, 8);
        t.artist_display = (const char *)sqlite3_column_text(stmt, 9);
        if (!t.title) t.title = "Unknown";
        if (!cb(&t, user_data)) break;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_iter_all_track_artists(quadrature_db_t *db,
                                               db_track_artist_iter_cb cb,
                                               void *user_data) {
    if (!db || !cb) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt *stmt = db->iter_all_track_artists;
    if (!stmt) { db_unlock(db); return QUADRATURE_OK; }
    sqlite3_reset(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t track_id  = sqlite3_column_int64(stmt, 0);
        int64_t artist_id = sqlite3_column_int64(stmt, 1);
        const char *jp    = (const char *)sqlite3_column_text(stmt, 2);
        int position      = sqlite3_column_int(stmt, 3);
        if (!cb(track_id, artist_id, jp ? jp : "", position, user_data))
            break;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_get_max_ids(quadrature_db_t *db,
                                    int64_t *max_artist, int64_t *max_album,
                                    int64_t *max_track) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt *stmt = db->get_max_ids;
    if (!stmt) { db_unlock(db); return QUADRATURE_OK; }
    sqlite3_reset(stmt);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (max_artist) *max_artist = sqlite3_column_int64(stmt, 0);
        if (max_album)  *max_album  = sqlite3_column_int64(stmt, 1);
        if (max_track)  *max_track  = sqlite3_column_int64(stmt, 2);
    } else {
        if (max_artist) *max_artist = 0;
        if (max_album)  *max_album  = 0;
        if (max_track)  *max_track  = 0;
    }

    sqlite3_reset(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}


void db_albums_free(db_album_t* albums, size_t count) {
    if (!albums) return;
    for (size_t i = 0; i < count; i++) {
        free(albums[i].title);
        free(albums[i].artist_name);
        free(albums[i].genres);
        free(albums[i].path);
        free(albums[i].musicbrainz_release_id);
    }
    free(albums);
}

void db_tracks_free(db_track_t* tracks, size_t count) {
    if (!tracks) return;
    for (size_t i = 0; i < count; i++) {
        free(tracks[i].title);
        free(tracks[i].artist);
        free(tracks[i].artist_display);
        free(tracks[i].album);
        free(tracks[i].path);
        free(tracks[i].album_path);
        free(tracks[i].album_musicbrainz_release_id);
        free(tracks[i].genre);
    }
    free(tracks);
}

// =============================================================================
// Positional & MBID Bridge Queries (for credits navigation)
// =============================================================================

quadrature_result_t db_get_track_by_position(
    quadrature_db_t *db, const char *release_mbid,
    int disc_num, int track_num, int64_t *track_id_out) {

    if (!db || !release_mbid || !track_id_out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *track_id_out = 0;

    db_lock(db);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT t.id FROM tracks t"
        " JOIN albums al ON t.album_id = al.id"
        " WHERE al.musicbrainz_release_id = ? AND t.disc_num = ? AND t.track_num = ?"
        " LIMIT 1",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, release_mbid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, disc_num);
    sqlite3_bind_int(stmt, 3, track_num);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *track_id_out = sqlite3_column_int64(stmt, 0);
        res = QUADRATURE_OK;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);
    return res;
}

quadrature_result_t db_resolve_track_positions_batch(
    quadrature_db_t *db,
    const db_track_position_t *positions, size_t count,
    int64_t *track_ids_out) {

    if (!db || !positions || !track_ids_out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    memset(track_ids_out, 0, count * sizeof(int64_t));
    if (count == 0) return QUADRATURE_OK;

    db_lock(db);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT t.id FROM tracks t"
        " JOIN albums al ON t.album_id = al.id"
        " WHERE al.musicbrainz_release_id = ? AND t.disc_num = ? AND t.track_num = ?"
        " LIMIT 1",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    for (size_t i = 0; i < count; i++) {
        if (!positions[i].release_mbid) continue;

        sqlite3_bind_text(stmt, 1, positions[i].release_mbid, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, positions[i].disc_num);
        sqlite3_bind_int(stmt, 3, positions[i].track_num);

        if (sqlite3_step(stmt) == SQLITE_ROW)
            track_ids_out[i] = sqlite3_column_int64(stmt, 0);

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    sqlite3_finalize(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_get_artist_by_mbid(
    quadrature_db_t *db, const char *musicbrainz_id, int64_t *artist_id_out) {

    if (!db || !musicbrainz_id || !artist_id_out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    *artist_id_out = 0;

    db_lock(db);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists WHERE musicbrainz_id = ? LIMIT 1",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, musicbrainz_id, -1, SQLITE_STATIC);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *artist_id_out = sqlite3_column_int64(stmt, 0);
        res = QUADRATURE_OK;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);
    return res;
}

// =============================================================================
// Paginated Queries (for lazy loading)
// =============================================================================

static const char* get_artist_order_clause(db_sort_t sort) {
    switch (sort) {
        case DB_SORT_NAME_DESC: return "ORDER BY a.name COLLATE NOCASE DESC";
        case DB_SORT_RECENT:    return "ORDER BY a.id DESC";
        default:                return "ORDER BY a.name COLLATE NOCASE ASC";
    }
}

static const char* get_album_order_clause(db_sort_t sort) {
    switch (sort) {
        case DB_SORT_NAME_ASC:    return "ORDER BY al.title COLLATE NOCASE ASC";
        case DB_SORT_NAME_DESC:   return "ORDER BY al.title COLLATE NOCASE DESC";
        case DB_SORT_YEAR_ASC:    return "ORDER BY al.year ASC, al.title COLLATE NOCASE ASC";
        case DB_SORT_YEAR_DESC:   return "ORDER BY al.year DESC, al.title COLLATE NOCASE ASC";
        case DB_SORT_ARTIST_ASC:  return "ORDER BY ar.name COLLATE NOCASE ASC, al.year ASC, al.title COLLATE NOCASE ASC";
        case DB_SORT_ARTIST_DESC: return "ORDER BY ar.name COLLATE NOCASE DESC, al.year DESC, al.title COLLATE NOCASE DESC";
        case DB_SORT_ADDED_ASC:   return "ORDER BY al.id ASC";
        case DB_SORT_ADDED_DESC:  return "ORDER BY al.id DESC";
        case DB_SORT_RECENT:      return "ORDER BY al.id DESC";
        default:                  return "ORDER BY ar.name COLLATE NOCASE ASC, al.year ASC, al.title COLLATE NOCASE ASC";
    }
}


// =============================================================================
// SQL Filter Helpers (shared by paginated and ID-only queries)
// =============================================================================

/* Decade year ranges indexed by bit position in year_mask bitmask */
static const uint16_t DECADE_RANGES[][2] = {
    {2020, 2029}, {2010, 2019}, {2000, 2009}, {1990, 1999},
    {1980, 1989}, {1970, 1979}, {1960, 1969}, {0, 1959},
};
#define NUM_DECADE_BITS 8

/* Append "col BETWEEN lo AND hi [OR ...]" for each set bit in year_mask */
static void sql_append_year_or(GString *sql, uint16_t year_mask, const char *col) {
    gboolean first = TRUE;
    for (int i = 0; i < NUM_DECADE_BITS; i++) {
        if (!(year_mask & (1 << i))) continue;
        if (!first) g_string_append(sql, " OR ");
        g_string_append_printf(sql, "%s BETWEEN %u AND %u",
                               col, DECADE_RANGES[i][0], DECADE_RANGES[i][1]);
        first = FALSE;
    }
}

/* Bind genre LIKE parameters starting at bind_idx, returns next index */
static int sql_bind_genres(sqlite3_stmt *stmt, int idx, const db_search_opts_t *opts) {
    if (!opts) return idx;
    for (size_t i = 0; i < opts->genre_count; i++) {
        sqlite3_bind_text(stmt, idx++, opts->genres[i], -1, SQLITE_STATIC);
    }
    return idx;
}



// =============================================================================
// Album Mtime Operations (paged - for indexer delta detection)
// =============================================================================

quadrature_result_t db_get_album_mtimes_page(quadrature_db_t* db,
                                              size_t offset,
                                              size_t limit,
                                              db_album_mtime_t** out,
                                              size_t* count_out) {
    if (!db || !out || !count_out || limit == 0) return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count_out = 0;

    db_lock(db);

    db_album_mtime_t* results = calloc(limit, sizeof(db_album_mtime_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT id, path, last_updated_at, last_updated_size, mb_status FROM albums WHERE path != '' "
        "ORDER BY path LIMIT ? OFFSET ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (int64_t)limit);
    sqlite3_bind_int64(stmt, 2, (int64_t)offset);

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < limit) {
        results[i].album_id          = sqlite3_column_int64(stmt, 0);
        const char* path = (const char*)sqlite3_column_text(stmt, 1);
        results[i].path              = path ? strdup(path) : NULL;
        results[i].last_updated_at   = sqlite3_column_type(stmt, 2) != SQLITE_NULL
                                       ? sqlite3_column_int64(stmt, 2) : 0;
        results[i].last_updated_size = sqlite3_column_type(stmt, 3) != SQLITE_NULL
                                       ? sqlite3_column_int64(stmt, 3) : 0;
        results[i].mb_status         = sqlite3_column_int(stmt, 4);
        i++;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);

    *out = results;
    *count_out = i;
    return QUADRATURE_OK;
}

void db_free_album_mtimes(db_album_mtime_t* albums, size_t count) {
    if (!albums) return;
    for (size_t i = 0; i < count; i++) {
        free(albums[i].path);
    }
    free(albums);
}

// =============================================================================
// Indexer Error Read Operations (simplified path-based)
// =============================================================================

int64_t db_get_next_error_generation(quadrature_db_t* db) {
    if (!db) return 1;

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT COALESCE(MAX(scan_generation), 0) + 1 FROM indexer_errors",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return 1;
    }

    int64_t gen = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        gen = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    db_unlock(db);
    return gen;
}

quadrature_result_t db_get_error_count(quadrature_db_t* db, const char* path_prefix, size_t* count) {
    if (!db || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc;
    if (path_prefix && *path_prefix) {
        rc = sqlite3_prepare_v2(db->db,
            "SELECT COUNT(*) FROM indexer_errors WHERE path LIKE ? || '%'",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, path_prefix, -1, SQLITE_STATIC);
        }
    } else {
        rc = sqlite3_prepare_v2(db->db,
            "SELECT COUNT(*) FROM indexer_errors",
            -1, &stmt, NULL);
    }

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_step(stmt);
    *count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_get_errors_page(quadrature_db_t* db, const char* path_prefix,
                                       size_t offset, size_t limit,
                                       db_indexer_error_t** out, size_t* count) {
    if (!db || !out || !count || limit == 0) return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    db_lock(db);

    db_indexer_error_t* results = calloc(limit, sizeof(db_indexer_error_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    sqlite3_stmt* stmt;
    int rc;
    if (path_prefix && *path_prefix) {
        rc = sqlite3_prepare_v2(db->db,
            "SELECT id, path, message, created_at FROM indexer_errors "
            "WHERE path LIKE ?1 || '%' "
            "ORDER BY created_at DESC LIMIT ?2 OFFSET ?3",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, path_prefix, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (int64_t)limit);
            sqlite3_bind_int64(stmt, 3, (int64_t)offset);
        }
    } else {
        rc = sqlite3_prepare_v2(db->db,
            "SELECT id, path, message, created_at FROM indexer_errors "
            "ORDER BY created_at DESC LIMIT ?1 OFFSET ?2",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (int64_t)limit);
            sqlite3_bind_int64(stmt, 2, (int64_t)offset);
        }
    }

    if (rc != SQLITE_OK) {
        free(results);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < limit) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* path = (const char*)sqlite3_column_text(stmt, 1);
        results[i].path = path ? strdup(path) : strdup("");
        const char* message = (const char*)sqlite3_column_text(stmt, 2);
        results[i].message = message ? strdup(message) : strdup("");
        results[i].created_at = sqlite3_column_int64(stmt, 3);
        i++;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

// =============================================================================
// FTS Query Builder
// =============================================================================

/* Splits input on whitespace, strips FTS5 operators, appends '*' to each token.
 * "Within Daft P" → "Within* Daft* P*"
 * Returns g_malloc'd string (caller must g_free) or NULL if no valid tokens.
 * Tokens shorter than 2 chars are skipped (FTS5 ignores them in prefix mode). */
static char* build_fts_query(const char* input) {
    if (!input || !*input) return NULL;

    GString* out = g_string_new(NULL);
    gchar** tokens = g_strsplit_set(input, " \t\n\r", -1);

    for (int i = 0; tokens[i]; i++) {
        const gchar* tok = tokens[i];
        if (!tok || !*tok) continue;

        // Strip FTS5 special chars that would cause parse errors or alter semantics
        GString* clean = g_string_new(NULL);
        for (const char* c = tok; *c; c++) {
            switch (*c) {
                case '"': case '\'': case '(': case ')':
                case '^': case '*':  case '-': case '+':
                case '=': case '.':  case '`': case ':':
                    break;
                default:
                    g_string_append_c(clean, *c);
            }
        }

        if (clean->len >= 2) {
            if (out->len > 0) g_string_append_c(out, ' ');
            g_string_append(out, clean->str);
            g_string_append_c(out, '*');
        }
        g_string_free(clean, TRUE);
    }

    g_strfreev(tokens);

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
}

// =============================================================================
// ID-Only Filtered Queries (for cache-resolved filtering)
// =============================================================================

static quadrature_result_t get_artist_ids(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count) {
    if (!db || !opts || !out_ids || !out_count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out_ids = NULL;
    *out_count = 0;

    char* fts_query = (opts->search_text && opts->search_text[0])
                      ? build_fts_query(opts->search_text) : NULL;
    gboolean has_search = fts_query != NULL;
    gboolean has_genre = opts->filters && opts->filters->genre_count > 0;
    gboolean has_year = opts->filters && opts->filters->year_mask != 0;

    GString *sql_str = g_string_new(
        "SELECT a.id FROM artists a WHERE ("
        "EXISTS (SELECT 1 FROM track_artists ta WHERE ta.artist_id = a.id) OR "
        "EXISTS (SELECT 1 FROM albums al WHERE al.artist_id = a.id))");

    if (has_search)
        g_string_append(sql_str, " AND a.id IN (SELECT rowid FROM artists_fts WHERE artists_fts MATCH ?)");
    if (has_genre && has_year) {
        g_string_append(sql_str,
            " AND EXISTS (SELECT 1 FROM albums _al"
            " WHERE _al.artist_id = a.id AND (");
        sql_append_year_or(sql_str, opts->filters->year_mask, "_al.year");
        g_string_append(sql_str,
            ") AND EXISTS (SELECT 1 FROM tracks _t WHERE _t.album_id = _al.id AND (");
        for (size_t gi = 0; gi < opts->filters->genre_count; gi++) {
            if (gi > 0) g_string_append(sql_str, " OR ");
            g_string_append(sql_str, "';' || LOWER(_t.genre) || ';' LIKE '%;' || ? || ';%'");
        }
        g_string_append(sql_str, ")))");
    } else {
        if (has_genre) {
            g_string_append(sql_str,
                " AND EXISTS (SELECT 1 FROM track_artists _ta"
                " JOIN tracks _t ON _t.id = _ta.track_id"
                " WHERE _ta.artist_id = a.id AND (");
            for (size_t gi = 0; gi < opts->filters->genre_count; gi++) {
                if (gi > 0) g_string_append(sql_str, " OR ");
                g_string_append(sql_str, "';' || LOWER(_t.genre) || ';' LIKE '%;' || ? || ';%'");
            }
            g_string_append(sql_str, "))");
        }
        if (has_year) {
            g_string_append(sql_str,
                " AND EXISTS (SELECT 1 FROM albums _al"
                " WHERE _al.artist_id = a.id AND (");
            sql_append_year_or(sql_str, opts->filters->year_mask, "_al.year");
            g_string_append(sql_str, "))");
        }
    }
    g_string_append_printf(sql_str, " %s", get_artist_order_clause(opts->sort));

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    if (rc != SQLITE_OK) {
        g_free(fts_query);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int pidx = 1;
    if (has_search)
        sqlite3_bind_text(stmt, pidx++, fts_query, -1, SQLITE_TRANSIENT);
    if (has_genre)
        pidx = sql_bind_genres(stmt, pidx, opts->filters);

    size_t cap = 256;
    int64_t* ids = g_new(int64_t, cap);
    size_t n = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            ids = g_renew(int64_t, ids, cap);
        }
        ids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    g_free(fts_query);

    *out_ids = ids;
    *out_count = n;
    return QUADRATURE_OK;
}

static quadrature_result_t get_album_ids(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count) {
    if (!db || !opts || !out_ids || !out_count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out_ids = NULL;
    *out_count = 0;

    char* fts_query = (opts->search_text && opts->search_text[0])
                      ? build_fts_query(opts->search_text) : NULL;
    gboolean has_search = fts_query != NULL;
    gboolean has_genre = opts->filters && opts->filters->genre_count > 0;
    gboolean has_year = opts->filters && opts->filters->year_mask != 0;

    gboolean needs_artist_join = (opts->sort == DB_SORT_ARTIST_ASC ||
                                   opts->sort == DB_SORT_ARTIST_DESC);
    GString *sql_str = g_string_new(
        needs_artist_join
            ? "SELECT al.id FROM albums al LEFT JOIN artists ar ON ar.id = al.artist_id WHERE 1=1"
            : "SELECT al.id FROM albums al WHERE 1=1");

    if (has_search)
        g_string_append(sql_str, " AND al.id IN (SELECT rowid FROM albums_fts WHERE albums_fts MATCH ?)");
    if (has_year) {
        g_string_append(sql_str, " AND (");
        sql_append_year_or(sql_str, opts->filters->year_mask, "al.year");
        g_string_append_c(sql_str, ')');
    }
    if (has_genre) {
        g_string_append(sql_str,
            " AND EXISTS (SELECT 1 FROM tracks _t WHERE _t.album_id = al.id AND (");
        for (size_t gi = 0; gi < opts->filters->genre_count; gi++) {
            if (gi > 0) g_string_append(sql_str, " OR ");
            g_string_append(sql_str, "';' || LOWER(_t.genre) || ';' LIKE '%;' || ? || ';%'");
        }
        g_string_append(sql_str, "))");
    }

    g_string_append_printf(sql_str, " %s", get_album_order_clause(opts->sort));

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    if (rc != SQLITE_OK) {
        g_free(fts_query);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int pidx = 1;
    if (has_search)
        sqlite3_bind_text(stmt, pidx++, fts_query, -1, SQLITE_TRANSIENT);
    if (has_genre)
        pidx = sql_bind_genres(stmt, pidx, opts->filters);

    size_t cap = 256;
    int64_t* ids = g_new(int64_t, cap);
    size_t n = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            ids = g_renew(int64_t, ids, cap);
        }
        ids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    g_free(fts_query);

    *out_ids = ids;
    *out_count = n;
    return QUADRATURE_OK;
}

static quadrature_result_t get_track_ids(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count) {
    const char *query = opts ? opts->search_text : NULL;
    if (!db || !query || !out_ids || !out_count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out_ids = NULL;
    *out_count = 0;

    if (!query[0]) return QUADRATURE_OK;

    const db_search_opts_t *filters = opts->filters;
    size_t limit = opts->limit;

    char* q = build_fts_query(query);
    if (!q) return QUADRATURE_OK;

    GString *sql_str = g_string_new(
        "SELECT t.id FROM tracks_fts f"
        " JOIN tracks t ON f.rowid = t.id"
        " WHERE tracks_fts MATCH ?");

    if (filters && filters->genre_count > 0) {
        g_string_append(sql_str, " AND (");
        for (size_t gi = 0; gi < filters->genre_count; gi++) {
            if (gi > 0) g_string_append(sql_str, " OR ");
            g_string_append(sql_str, "';' || t.genre || ';' LIKE '%;' || ? || ';%'");
        }
        g_string_append_c(sql_str, ')');
    }
    if (filters && filters->year_mask) {
        g_string_append(sql_str, " AND (");
        sql_append_year_or(sql_str, filters->year_mask, "t.year");
        g_string_append_c(sql_str, ')');
    }
    // rank uses table-level BM25 weights: title=10x, artist=5x, album=1x
    g_string_append(sql_str, " ORDER BY rank LIMIT ?");

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    if (rc != SQLITE_OK) {
        g_free(q);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int pidx = 1;
    sqlite3_bind_text(stmt, pidx++, q, -1, SQLITE_STATIC);
    if (filters) pidx = sql_bind_genres(stmt, pidx, filters);
    sqlite3_bind_int64(stmt, pidx, limit > 0 ? (int64_t)limit : 100);

    size_t cap = 64;
    int64_t* ids = g_new(int64_t, cap);
    size_t n = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            ids = g_renew(int64_t, ids, cap);
        }
        ids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    g_free(q);

    *out_ids = ids;
    *out_count = n;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_entity_ids_filtered(quadrature_db_t* db,
    db_entity_t entity,
    const db_id_query_opts_t* opts,
    int64_t** out_ids, size_t* out_count) {
    switch (entity) {
        case DB_ENTITY_ARTIST: return get_artist_ids(db, opts, out_ids, out_count);
        case DB_ENTITY_ALBUM:  return get_album_ids(db, opts, out_ids, out_count);
        case DB_ENTITY_TRACK:  return get_track_ids(db, opts, out_ids, out_count);
    }
    return QUADRATURE_ERROR_INVALID_PARAM;
}

// =============================================================================
// Artist Art Queries
// =============================================================================

quadrature_result_t db_get_artists_with_mbid(quadrature_db_t* db,
    int64_t** artist_ids, char*** mbids, size_t* count) {
    if (!db || !artist_ids || !mbids || !count) return QUADRATURE_ERROR_INVALID_PARAM;
    *artist_ids = NULL;
    *mbids = NULL;
    *count = 0;

    db_lock(db);

    // First get the count
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT COUNT(*) FROM artists WHERE musicbrainz_id IS NOT NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    size_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    if (total == 0) {
        db_unlock(db);
        return QUADRATURE_OK;
    }

    // Fetch the data
    rc = sqlite3_prepare_v2(db->db,
        "SELECT id, musicbrainz_id FROM artists WHERE musicbrainz_id IS NOT NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int64_t* ids = g_malloc(total * sizeof(int64_t));
    char** mbs = g_malloc0((total + 1) * sizeof(char*));  // NULL-terminated for g_strfreev
    size_t i = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && i < total) {
        ids[i] = sqlite3_column_int64(stmt, 0);
        const char* mbid = (const char*)sqlite3_column_text(stmt, 1);
        mbs[i] = g_strdup(mbid);
        i++;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);

    *artist_ids = ids;
    *mbids = mbs;
    *count = i;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_albums_with_release_group_id(quadrature_db_t* db,
    int64_t** album_ids, char*** release_group_ids, char*** artist_mbids,
    size_t* count) {
    if (!db || !album_ids || !release_group_ids || !artist_mbids || !count)
        return QUADRATURE_ERROR_INVALID_PARAM;
    *album_ids = NULL;
    *release_group_ids = NULL;
    *artist_mbids = NULL;
    *count = 0;

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT COUNT(*) FROM albums al "
        "JOIN artists ar ON ar.id = al.artist_id "
        "WHERE al.musicbrainz_release_group_id IS NOT NULL "
        "AND ar.musicbrainz_id IS NOT NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    size_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    if (total == 0) {
        db_unlock(db);
        return QUADRATURE_OK;
    }

    rc = sqlite3_prepare_v2(db->db,
        "SELECT al.id, al.musicbrainz_release_group_id, ar.musicbrainz_id "
        "FROM albums al "
        "JOIN artists ar ON ar.id = al.artist_id "
        "WHERE al.musicbrainz_release_group_id IS NOT NULL "
        "AND ar.musicbrainz_id IS NOT NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int64_t* ids = g_malloc(total * sizeof(int64_t));
    char** rg_ids = g_malloc0((total + 1) * sizeof(char*));
    char** a_mbids = g_malloc0((total + 1) * sizeof(char*));
    size_t i = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && i < total) {
        ids[i] = sqlite3_column_int64(stmt, 0);
        rg_ids[i] = g_strdup((const char*)sqlite3_column_text(stmt, 1));
        a_mbids[i] = g_strdup((const char*)sqlite3_column_text(stmt, 2));
        i++;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);

    *album_ids = ids;
    *release_group_ids = rg_ids;
    *artist_mbids = a_mbids;
    *count = i;
    return QUADRATURE_OK;
}

// =============================================================================
// Aggregate Queries
// =============================================================================

quadrature_result_t db_get_entity_count(quadrature_db_t* db, db_entity_t entity, size_t* count) {
    if (!db || !count) return QUADRATURE_ERROR_INVALID_PARAM;
    *count = 0;

    const char* sql;
    switch (entity) {
        case DB_ENTITY_ARTIST: sql = "SELECT COUNT(*) FROM artists"; break;
        case DB_ENTITY_ALBUM:  sql = "SELECT COUNT(*) FROM albums";  break;
        case DB_ENTITY_TRACK:  sql = "SELECT COUNT(*) FROM tracks";  break;
        default: return QUADRATURE_ERROR_INVALID_PARAM;
    }

    db_lock(db);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_unlock(db); return QUADRATURE_ERROR_INTERNAL; }
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *count = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_get_last_indexed_time(quadrature_db_t* db, int64_t* unix_time) {
    if (!db || !unix_time) return QUADRATURE_ERROR_INVALID_PARAM;
    *unix_time = 0;

    db_lock(db);
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT COALESCE(MAX(last_updated_at), 0) FROM albums", -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_unlock(db); return QUADRATURE_ERROR_INTERNAL; }
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *unix_time = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return QUADRATURE_OK;
}

