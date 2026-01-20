#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

// =============================================================================
// Track Read Operations
// =============================================================================

quadrature_result_t db_get_track(quadrature_db_t* db, int64_t id, db_track_t** out) {
    if (!db || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    const char* sql =
        "SELECT t.id, t.title, a.name, al.title, t.path, t.duration_ms, t.track_num, "
        "       t.disc_num, al.year, t.album_id "
        "FROM tracks t "
        "LEFT JOIN artists a ON t.artist_id = a.id "
        "LEFT JOIN albums al ON t.album_id = al.id "
        "WHERE t.id = ?";

    sqlite3_stmt* stmt;
    db_lock(db);
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, id);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        db_track_t* track = calloc(1, sizeof(db_track_t));
        if (!track) {
            sqlite3_finalize(stmt);
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
        track->album = album ? strdup(album) : strdup("Unknown Album");
        track->path = path ? strdup(path) : strdup("");
        track->duration_ms = sqlite3_column_int(stmt, 5);
        track->track_num = sqlite3_column_int(stmt, 6);
        track->disc_num = sqlite3_column_int(stmt, 7);
        if (track->disc_num == 0) track->disc_num = 1;
        track->year = sqlite3_column_int(stmt, 8);
        track->album_id = sqlite3_column_int64(stmt, 9);

        *out = track;
        res = QUADRATURE_OK;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);
    return res;
}

quadrature_result_t db_get_track_count(quadrature_db_t* db, size_t* count) {
    if (!db || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM tracks", -1, &stmt, NULL);
    sqlite3_step(stmt);
    *count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return QUADRATURE_OK;
}

void db_track_free(db_track_t* track) {
    if (!track) return;
    free(track->title);
    free(track->artist);
    free(track->album);
    free(track->path);
    free(track);
}

// =============================================================================
// Watch Path Read Operations
// =============================================================================

quadrature_result_t db_get_watch_paths(quadrature_db_t* db, db_watch_path_t** out, size_t* count) {
    if (!db || !out || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    // Get count
    sqlite3_stmt* cnt_stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM watch_paths", -1, &cnt_stmt, NULL);
    sqlite3_step(cnt_stmt);
    size_t n = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (n == 0) {
        db_unlock(db);
        *out = NULL;
        *count = 0;
        return QUADRATURE_OK;
    }

    db_watch_path_t* results = malloc(n * sizeof(db_watch_path_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT id, path, enabled, last_scanned FROM watch_paths ORDER BY id",
        -1, &stmt, NULL);

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < n) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* path_str = (const char*)sqlite3_column_text(stmt, 1);
        results[i].path = path_str ? strdup(path_str) : strdup("");
        results[i].enabled = sqlite3_column_int(stmt, 2);
        results[i].last_scanned = sqlite3_column_int64(stmt, 3);
        i++;
    }
    sqlite3_finalize(stmt);
    db_unlock(db);

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

void db_free_watch_paths(db_watch_path_t* paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) {
        free(paths[i].path);
    }
    free(paths);
}

quadrature_result_t db_get_track_count_for_path(quadrature_db_t* db, const char* path, size_t* count) {
    if (!db || !path || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    // Count tracks whose path starts with the given directory path
    // Need to match "path/" prefix to avoid matching similarly-named directories
    sqlite3_stmt* stmt;
    const char* sql = "SELECT COUNT(*) FROM tracks WHERE path LIKE ? || '/%'";
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    return QUADRATURE_OK;
}

// =============================================================================
// Artist/Album Read Operations
// =============================================================================

quadrature_result_t db_get_albums_by_artist(quadrature_db_t* db, int64_t artist_id, db_album_t** out, size_t* count) {
    if (!db || !out || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get count first
    sqlite3_stmt* cnt_stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM albums WHERE artist_id = ?", -1, &cnt_stmt, NULL);
    sqlite3_bind_int64(cnt_stmt, 1, artist_id);
    sqlite3_step(cnt_stmt);
    size_t n = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (n == 0) {
        db_unlock(db);
        *out = NULL;
        *count = 0;
        gint64 elapsed = g_get_monotonic_time() - start_time;
        g_debug("db_get_albums_by_artist(%" G_GINT64_FORMAT "): 0 results in %.2f ms", artist_id, elapsed / 1000.0);
        return QUADRATURE_OK;
    }

    db_album_t* results = calloc(n, sizeof(db_album_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    const char* sql =
        "SELECT al.id, al.title, a.name, al.artist_id, al.year, "
        "  (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.id) AS track_count "
        "FROM albums al "
        "LEFT JOIN artists a ON al.artist_id = a.id "
        "WHERE al.artist_id = ? "
        "ORDER BY al.year, al.title COLLATE NOCASE";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, artist_id);

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < n) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        results[i].title = title ? strdup(title) : strdup("Unknown Album");
        results[i].artist_name = artist ? strdup(artist) : strdup("Unknown Artist");
        results[i].artist_id = sqlite3_column_int64(stmt, 3);
        results[i].year = sqlite3_column_int(stmt, 4);
        results[i].track_count = sqlite3_column_int64(stmt, 5);
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_albums_by_artist(%" G_GINT64_FORMAT "): %zu results in %.2f ms", artist_id, i, elapsed / 1000.0);

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_tracks_by_album(quadrature_db_t* db, int64_t album_id, db_track_t** out, size_t* count) {
    if (!db || !out || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get count first
    sqlite3_stmt* cnt_stmt;
    int rc = sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM tracks WHERE album_id = ?", -1, &cnt_stmt, NULL);
    if (rc != SQLITE_OK) {
        g_warning("db_get_tracks_by_album: prepare failed: %s", sqlite3_errmsg(db->db));
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    rc = sqlite3_bind_int64(cnt_stmt, 1, album_id);
    if (rc != SQLITE_OK) {
        g_warning("db_get_tracks_by_album: bind failed: %s", sqlite3_errmsg(db->db));
    }
    rc = sqlite3_step(cnt_stmt);
    if (rc != SQLITE_ROW) {
        g_warning("db_get_tracks_by_album: count query step returned %d (expected SQLITE_ROW=%d) for album_id=%" G_GINT64_FORMAT,
                  rc, SQLITE_ROW, album_id);
    }
    size_t n = sqlite3_column_int64(cnt_stmt, 0);
    g_info("db_get_tracks_by_album: count=%zu for album_id=%" G_GINT64_FORMAT " (db=%s)",
           n, album_id, db->db_path ? db->db_path : "memory");
    sqlite3_finalize(cnt_stmt);

    if (n == 0) {
        db_unlock(db);
        *out = NULL;
        *count = 0;
        gint64 elapsed = g_get_monotonic_time() - start_time;
        g_debug("db_get_tracks_by_album(%" G_GINT64_FORMAT "): 0 results in %.2f ms", album_id, elapsed / 1000.0);
        return QUADRATURE_OK;
    }

    db_track_t* results = calloc(n, sizeof(db_track_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    const char* sql =
        "SELECT t.id, t.title, a.name, al.title, t.path, t.duration_ms, t.track_num, "
        "       t.disc_num, al.year, t.album_id, al.artist_id "
        "FROM tracks t "
        "LEFT JOIN artists a ON t.artist_id = a.id "
        "LEFT JOIN albums al ON t.album_id = al.id "
        "WHERE t.album_id = ? "
        "ORDER BY t.disc_num, t.track_num, t.title COLLATE NOCASE";

    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_critical("db_get_tracks_by_album: prepare SELECT failed: %s", sqlite3_errmsg(db->db));
        free(results);
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    rc = sqlite3_bind_int64(stmt, 1, album_id);
    if (rc != SQLITE_OK) {
        g_critical("db_get_tracks_by_album: bind SELECT failed: %s", sqlite3_errmsg(db->db));
    }

    size_t i = 0;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        g_warning("db_get_tracks_by_album: first step returned %d: %s", rc, sqlite3_errmsg(db->db));
    }
    while (rc == SQLITE_ROW && i < n) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        const char* album = (const char*)sqlite3_column_text(stmt, 3);
        const char* path = (const char*)sqlite3_column_text(stmt, 4);
        results[i].title = title ? strdup(title) : strdup("Unknown");
        results[i].artist = artist ? strdup(artist) : strdup("Unknown Artist");
        results[i].album = album ? strdup(album) : strdup("Unknown Album");
        results[i].path = path ? strdup(path) : strdup("");
        results[i].duration_ms = sqlite3_column_int(stmt, 5);
        results[i].track_num = sqlite3_column_int(stmt, 6);
        results[i].disc_num = sqlite3_column_int(stmt, 7);
        if (results[i].disc_num == 0) results[i].disc_num = 1;
        results[i].year = sqlite3_column_int(stmt, 8);
        results[i].album_id = sqlite3_column_int64(stmt, 9);
        results[i].artist_id = sqlite3_column_int64(stmt, 10);
        i++;
        rc = sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_tracks_by_album(%" G_GINT64_FORMAT "): %zu results in %.2f ms", album_id, i, elapsed / 1000.0);

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_artist_count(quadrature_db_t* db, size_t* count) {
    if (!db || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM artists", -1, &stmt, NULL);
    sqlite3_step(stmt);
    *count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return QUADRATURE_OK;
}

quadrature_result_t db_get_album_count(quadrature_db_t* db, size_t* count) {
    if (!db || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM albums", -1, &stmt, NULL);
    sqlite3_step(stmt);
    *count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return QUADRATURE_OK;
}

void db_artists_free(db_artist_t* artists, size_t count) {
    if (!artists) return;
    for (size_t i = 0; i < count; i++) {
        free(artists[i].name);
    }
    free(artists);
}

void db_albums_free(db_album_t* albums, size_t count) {
    if (!albums) return;
    for (size_t i = 0; i < count; i++) {
        free(albums[i].title);
        free(albums[i].artist_name);
    }
    free(albums);
}

void db_tracks_free(db_track_t* tracks, size_t count) {
    if (!tracks) return;
    for (size_t i = 0; i < count; i++) {
        free(tracks[i].title);
        free(tracks[i].artist);
        free(tracks[i].album);
        free(tracks[i].path);
    }
    free(tracks);
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

static const char* get_track_order_clause(db_sort_t sort) {
    switch (sort) {
        case DB_SORT_NAME_ASC:      return "ORDER BY t.title COLLATE NOCASE ASC";
        case DB_SORT_NAME_DESC:     return "ORDER BY t.title COLLATE NOCASE DESC";
        case DB_SORT_YEAR_ASC:      return "ORDER BY al.year ASC, al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_YEAR_DESC:     return "ORDER BY al.year DESC, al.title COLLATE NOCASE DESC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ARTIST_ASC:    return "ORDER BY ar.name COLLATE NOCASE ASC, al.year ASC, al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ARTIST_DESC:   return "ORDER BY ar.name COLLATE NOCASE DESC, al.year DESC, al.title COLLATE NOCASE DESC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ADDED_ASC:     return "ORDER BY t.id ASC";
        case DB_SORT_ADDED_DESC:    return "ORDER BY t.id DESC";
        case DB_SORT_RECENT:        return "ORDER BY t.id DESC";
        case DB_SORT_ALBUM_ASC:     return "ORDER BY al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ALBUM_DESC:    return "ORDER BY al.title COLLATE NOCASE DESC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_DURATION_ASC:  return "ORDER BY t.duration_ms ASC";
        case DB_SORT_DURATION_DESC: return "ORDER BY t.duration_ms DESC";
        case DB_SORT_TRACK_NUM_ASC: return "ORDER BY al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_DISC_NUM_ASC:  return "ORDER BY al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        default:                    return "ORDER BY ar.name COLLATE NOCASE ASC, al.year ASC, al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC, t.title COLLATE NOCASE ASC";
    }
}

quadrature_result_t db_get_artists_page(quadrature_db_t* db,
                                         const db_page_opts_t* opts,
                                         db_artist_t** out,
                                         size_t* out_count,
                                         size_t* total_count) {
    if (!db || !opts || !out || !out_count || !total_count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get total count
    sqlite3_stmt* cnt_stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM artists", -1, &cnt_stmt, NULL);
    sqlite3_step(cnt_stmt);
    *total_count = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (*total_count == 0 || opts->offset >= *total_count) {
        db_unlock(db);
        *out = NULL;
        *out_count = 0;
        return QUADRATURE_OK;
    }

    // Build query with JOINs (faster than subqueries)
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT a.id, a.name, "
        "COUNT(DISTINCT al.id) AS album_count, "
        "COUNT(DISTINCT t.id) AS track_count "
        "FROM artists a "
        "LEFT JOIN albums al ON al.artist_id = a.id "
        "LEFT JOIN tracks t ON t.artist_id = a.id "
        "GROUP BY a.id "
        "%s "
        "LIMIT ? OFFSET ?",
        get_artist_order_clause(opts->sort));

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, opts->limit);
    sqlite3_bind_int64(stmt, 2, opts->offset);

    // Allocate for max possible results
    size_t max_results = opts->limit;
    if (opts->offset + max_results > *total_count)
        max_results = *total_count - opts->offset;

    db_artist_t* results = calloc(max_results, sizeof(db_artist_t));
    if (!results) {
        sqlite3_finalize(stmt);
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < max_results) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        results[i].name = name ? strdup(name) : strdup("Unknown Artist");
        results[i].album_count = sqlite3_column_int64(stmt, 2);
        results[i].track_count = sqlite3_column_int64(stmt, 3);
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_artists_page(offset=%zu, limit=%zu): %zu results in %.2f ms",
            opts->offset, opts->limit, i, elapsed / 1000.0);

    *out = results;
    *out_count = i;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_albums_page(quadrature_db_t* db,
                                        const db_page_opts_t* opts,
                                        db_album_t** out,
                                        size_t* out_count,
                                        size_t* total_count) {
    if (!db || !opts || !out || !out_count || !total_count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get total count
    sqlite3_stmt* cnt_stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM albums", -1, &cnt_stmt, NULL);
    sqlite3_step(cnt_stmt);
    *total_count = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (*total_count == 0 || opts->offset >= *total_count) {
        db_unlock(db);
        *out = NULL;
        *out_count = 0;
        return QUADRATURE_OK;
    }

    // Build query with JOINs
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT al.id, al.title, ar.name, al.artist_id, al.year, "
        "COUNT(t.id) AS track_count "
        "FROM albums al "
        "LEFT JOIN artists ar ON al.artist_id = ar.id "
        "LEFT JOIN tracks t ON t.album_id = al.id "
        "GROUP BY al.id "
        "%s "
        "LIMIT ? OFFSET ?",
        get_album_order_clause(opts->sort));

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, opts->limit);
    sqlite3_bind_int64(stmt, 2, opts->offset);

    size_t max_results = opts->limit;
    if (opts->offset + max_results > *total_count)
        max_results = *total_count - opts->offset;

    db_album_t* results = calloc(max_results, sizeof(db_album_t));
    if (!results) {
        sqlite3_finalize(stmt);
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < max_results) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        results[i].title = title ? strdup(title) : strdup("Unknown Album");
        results[i].artist_name = artist ? strdup(artist) : strdup("Unknown Artist");
        results[i].artist_id = sqlite3_column_int64(stmt, 3);
        results[i].year = sqlite3_column_int(stmt, 4);
        results[i].track_count = sqlite3_column_int64(stmt, 5);
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_albums_page(offset=%zu, limit=%zu): %zu results in %.2f ms",
            opts->offset, opts->limit, i, elapsed / 1000.0);

    *out = results;
    *out_count = i;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_tracks_page(quadrature_db_t* db,
                                        const db_page_opts_t* opts,
                                        db_track_t** out,
                                        size_t* out_count,
                                        size_t* total_count) {
    if (!db || !opts || !out || !out_count || !total_count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get total count
    sqlite3_stmt* cnt_stmt;
    sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM tracks", -1, &cnt_stmt, NULL);
    sqlite3_step(cnt_stmt);
    *total_count = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (*total_count == 0 || opts->offset >= *total_count) {
        db_unlock(db);
        *out = NULL;
        *out_count = 0;
        return QUADRATURE_OK;
    }

    // Build query
    char sql[640];
    snprintf(sql, sizeof(sql),
        "SELECT t.id, t.title, ar.name, al.title, t.path, t.duration_ms, t.track_num, "
        "       t.disc_num, al.year, t.album_id, al.artist_id "
        "FROM tracks t "
        "LEFT JOIN artists ar ON t.artist_id = ar.id "
        "LEFT JOIN albums al ON t.album_id = al.id "
        "%s "
        "LIMIT ? OFFSET ?",
        get_track_order_clause(opts->sort));

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, opts->limit);
    sqlite3_bind_int64(stmt, 2, opts->offset);

    size_t max_results = opts->limit;
    if (opts->offset + max_results > *total_count)
        max_results = *total_count - opts->offset;

    db_track_t* results = calloc(max_results, sizeof(db_track_t));
    if (!results) {
        sqlite3_finalize(stmt);
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < max_results) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        const char* album = (const char*)sqlite3_column_text(stmt, 3);
        const char* path = (const char*)sqlite3_column_text(stmt, 4);
        results[i].title = title ? strdup(title) : strdup("Unknown");
        results[i].artist = artist ? strdup(artist) : strdup("Unknown Artist");
        results[i].album = album ? strdup(album) : strdup("Unknown Album");
        results[i].path = path ? strdup(path) : strdup("");
        results[i].duration_ms = sqlite3_column_int(stmt, 5);
        results[i].track_num = sqlite3_column_int(stmt, 6);
        results[i].disc_num = sqlite3_column_int(stmt, 7);
        if (results[i].disc_num == 0) results[i].disc_num = 1;
        results[i].year = sqlite3_column_int(stmt, 8);
        results[i].album_id = sqlite3_column_int64(stmt, 9);
        results[i].artist_id = sqlite3_column_int64(stmt, 10);
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_tracks_page(offset=%zu, limit=%zu): %zu results in %.2f ms",
            opts->offset, opts->limit, i, elapsed / 1000.0);

    *out = results;
    *out_count = i;
    return QUADRATURE_OK;
}

// =============================================================================
// Typed Search (for grouped search results)
// =============================================================================

void db_search_results_free(db_search_results_t* results) {
    if (!results) return;
    db_artists_free(results->artists, results->artist_count);
    db_albums_free(results->albums, results->album_count);
    db_tracks_free(results->tracks, results->track_count);
    memset(results, 0, sizeof(*results));
}

quadrature_result_t db_search_typed(quadrature_db_t* db, const char* query,
                                     db_search_type_t type, size_t limit,
                                     db_search_results_t* out) {
    if (!db || !query || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    gint64 start_time = g_get_monotonic_time();

    // Append * for prefix matching
    size_t qlen = strlen(query);
    if (qlen == 0) return QUADRATURE_OK;

    char* q = malloc(qlen + 2);
    if (!q) return QUADRATURE_ERROR_OUT_OF_MEMORY;
    memcpy(q, query, qlen);
    q[qlen] = '*';
    q[qlen + 1] = '\0';

    db_lock(db);

    // Search artists
    if (type == DB_SEARCH_ALL || type == DB_SEARCH_ARTISTS) {
        const char* sql =
            "SELECT a.id, a.name, "
            "  (SELECT COUNT(DISTINCT al.id) FROM albums al WHERE al.artist_id = a.id), "
            "  (SELECT COUNT(*) FROM tracks t WHERE t.artist_id = a.id) "
            "FROM artists a "
            "WHERE a.name LIKE '%' || ? || '%' COLLATE NOCASE "
            "ORDER BY a.name COLLATE NOCASE "
            "LIMIT ?";

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, type == DB_SEARCH_ALL ? (limit > 0 ? limit : 5) : (limit > 0 ? limit : 100));

        // Count results first
        size_t cap = 16;
        db_artist_t* artists = calloc(cap, sizeof(db_artist_t));
        size_t n = 0;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n >= cap) {
                cap *= 2;
                artists = realloc(artists, cap * sizeof(db_artist_t));
            }
            artists[n].id = sqlite3_column_int64(stmt, 0);
            const char* name = (const char*)sqlite3_column_text(stmt, 1);
            artists[n].name = name ? strdup(name) : strdup("Unknown Artist");
            artists[n].album_count = sqlite3_column_int64(stmt, 2);
            artists[n].track_count = sqlite3_column_int64(stmt, 3);
            n++;
        }
        sqlite3_finalize(stmt);

        out->artists = artists;
        out->artist_count = n;
    }

    // Search albums
    if (type == DB_SEARCH_ALL || type == DB_SEARCH_ALBUMS) {
        const char* sql =
            "SELECT al.id, al.title, ar.name, al.artist_id, al.year, "
            "  (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.id) "
            "FROM albums al "
            "LEFT JOIN artists ar ON al.artist_id = ar.id "
            "WHERE al.title LIKE '%' || ? || '%' COLLATE NOCASE "
            "ORDER BY al.title COLLATE NOCASE "
            "LIMIT ?";

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, type == DB_SEARCH_ALL ? (limit > 0 ? limit : 5) : (limit > 0 ? limit : 100));

        size_t cap = 16;
        db_album_t* albums = calloc(cap, sizeof(db_album_t));
        size_t n = 0;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n >= cap) {
                cap *= 2;
                albums = realloc(albums, cap * sizeof(db_album_t));
            }
            albums[n].id = sqlite3_column_int64(stmt, 0);
            const char* title = (const char*)sqlite3_column_text(stmt, 1);
            const char* artist = (const char*)sqlite3_column_text(stmt, 2);
            albums[n].title = title ? strdup(title) : strdup("Unknown Album");
            albums[n].artist_name = artist ? strdup(artist) : strdup("Unknown Artist");
            albums[n].artist_id = sqlite3_column_int64(stmt, 3);
            albums[n].year = sqlite3_column_int(stmt, 4);
            albums[n].track_count = sqlite3_column_int64(stmt, 5);
            n++;
        }
        sqlite3_finalize(stmt);

        out->albums = albums;
        out->album_count = n;
    }

    // Search tracks (using FTS for better relevance)
    if (type == DB_SEARCH_ALL || type == DB_SEARCH_TRACKS) {
        const char* sql =
            "SELECT t.id, t.title, ar.name, al.title, t.path, t.duration_ms, t.track_num, "
            "       t.disc_num, al.year, t.album_id, al.artist_id "
            "FROM tracks_fts f "
            "JOIN tracks t ON f.rowid = t.id "
            "LEFT JOIN artists ar ON t.artist_id = ar.id "
            "LEFT JOIN albums al ON t.album_id = al.id "
            "WHERE tracks_fts MATCH ? "
            "ORDER BY rank "
            "LIMIT ?";

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, q, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, type == DB_SEARCH_ALL ? (limit > 0 ? limit : 10) : (limit > 0 ? limit : 100));

        size_t cap = 16;
        db_track_t* tracks = calloc(cap, sizeof(db_track_t));
        size_t n = 0;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n >= cap) {
                cap *= 2;
                tracks = realloc(tracks, cap * sizeof(db_track_t));
            }
            tracks[n].id = sqlite3_column_int64(stmt, 0);
            const char* title = (const char*)sqlite3_column_text(stmt, 1);
            const char* artist = (const char*)sqlite3_column_text(stmt, 2);
            const char* album = (const char*)sqlite3_column_text(stmt, 3);
            const char* path = (const char*)sqlite3_column_text(stmt, 4);
            tracks[n].title = title ? strdup(title) : strdup("Unknown");
            tracks[n].artist = artist ? strdup(artist) : strdup("Unknown Artist");
            tracks[n].album = album ? strdup(album) : strdup("Unknown Album");
            tracks[n].path = path ? strdup(path) : strdup("");
            tracks[n].duration_ms = sqlite3_column_int(stmt, 5);
            tracks[n].track_num = sqlite3_column_int(stmt, 6);
            tracks[n].disc_num = sqlite3_column_int(stmt, 7);
            if (tracks[n].disc_num == 0) tracks[n].disc_num = 1;
            tracks[n].year = sqlite3_column_int(stmt, 8);
            tracks[n].album_id = sqlite3_column_int64(stmt, 9);
            tracks[n].artist_id = sqlite3_column_int64(stmt, 10);
            n++;
        }
        sqlite3_finalize(stmt);

        out->tracks = tracks;
        out->track_count = n;
    }

    db_unlock(db);
    free(q);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_search_typed(\"%s\", type=%d): %zu artists, %zu albums, %zu tracks in %.2f ms",
            query, type, out->artist_count, out->album_count, out->track_count, elapsed / 1000.0);

    return QUADRATURE_OK;
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
        "SELECT id, path, last_updated_at FROM albums WHERE path != '' "
        "ORDER BY path LIMIT ? OFFSET ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (int64_t)limit);
    sqlite3_bind_int64(stmt, 2, (int64_t)offset);

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < limit) {
        results[i].album_id = sqlite3_column_int64(stmt, 0);
        const char* path = (const char*)sqlite3_column_text(stmt, 1);
        results[i].path = path ? strdup(path) : NULL;
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            results[i].last_updated_at = sqlite3_column_int64(stmt, 2);
        } else {
            results[i].last_updated_at = 0;
        }
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
// Track Metadata Read Operations
// =============================================================================

quadrature_result_t db_get_track_metadata(quadrature_db_t* db,
                                           int64_t track_id,
                                           db_track_metadata_t** out) {
    if (!db || track_id < 0 || !out) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT track_id, raw_json, bitrate, sample_rate, channels, codec, "
        "album_artist, genre, comment, compilation, disc_total, track_total, has_embedded_art "
        "FROM track_metadata WHERE track_id = ?",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_int64(stmt, 1, track_id);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        db_track_metadata_t* meta = calloc(1, sizeof(db_track_metadata_t));
        if (!meta) {
            sqlite3_finalize(stmt);
            db_unlock(db);
            return QUADRATURE_ERROR_OUT_OF_MEMORY;
        }

        meta->track_id = sqlite3_column_int64(stmt, 0);

        const char* raw_json = (const char*)sqlite3_column_text(stmt, 1);
        meta->raw_json = raw_json ? strdup(raw_json) : strdup("{}");

        meta->bitrate = sqlite3_column_int(stmt, 2);
        meta->sample_rate = sqlite3_column_int(stmt, 3);
        meta->channels = sqlite3_column_int(stmt, 4);

        const char* codec = (const char*)sqlite3_column_text(stmt, 5);
        meta->codec = codec ? strdup(codec) : NULL;

        const char* album_artist = (const char*)sqlite3_column_text(stmt, 6);
        meta->album_artist = album_artist && *album_artist ? strdup(album_artist) : NULL;

        const char* genre = (const char*)sqlite3_column_text(stmt, 7);
        meta->genre = genre && *genre ? strdup(genre) : NULL;

        const char* comment = (const char*)sqlite3_column_text(stmt, 8);
        meta->comment = comment && *comment ? strdup(comment) : NULL;

        meta->compilation = sqlite3_column_int(stmt, 9) != 0;
        meta->disc_total = sqlite3_column_int(stmt, 10);
        meta->track_total = sqlite3_column_int(stmt, 11);
        meta->has_embedded_art = sqlite3_column_int(stmt, 12) != 0;

        *out = meta;
        res = QUADRATURE_OK;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);
    return res;
}

// =============================================================================
// Indexer Error Read Operations (simplified path-based)
// =============================================================================

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
