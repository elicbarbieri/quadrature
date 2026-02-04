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
        "       t.disc_num, t.year, t.album_id, ta.artist_id, t.genre "
        "FROM tracks t "
        "LEFT JOIN track_artists ta ON ta.track_id = t.id AND ta.position = 0 "
        "LEFT JOIN artists a ON a.id = ta.artist_id "
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
        track->artist_display = NULL;
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

        *out = track;
        res = QUADRATURE_OK;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);
    return res;
}

void db_track_free(db_track_t* track) {
    if (!track) return;
    free(track->title);
    free(track->artist);
    free(track->artist_display);
    free(track->album);
    free(track->path);
    free(track->genre);
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
        "  (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.id) AS track_count, "
        "  (SELECT GROUP_CONCAT(g, ';') FROM (SELECT DISTINCT genre AS g FROM tracks WHERE album_id = al.id AND genre IS NOT NULL AND genre != '')) AS genres "
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
        const char* genres = (const char*)sqlite3_column_text(stmt, 6);
        results[i].genres = (genres && *genres) ? strdup(genres) : NULL;
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
        "       t.disc_num, t.year, t.album_id, ta.artist_id, t.genre "
        "FROM tracks t "
        "LEFT JOIN track_artists ta ON ta.track_id = t.id AND ta.position = 0 "
        "LEFT JOIN artists a ON a.id = ta.artist_id "
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
        results[i].artist_display = NULL;
        results[i].album = album ? strdup(album) : strdup("Unknown Album");
        results[i].path = path ? strdup(path) : strdup("");
        results[i].duration_ms = sqlite3_column_int(stmt, 5);
        results[i].track_num = sqlite3_column_int(stmt, 6);
        results[i].disc_num = sqlite3_column_int(stmt, 7);
        if (results[i].disc_num == 0) results[i].disc_num = 1;
        results[i].year = sqlite3_column_int(stmt, 8);
        results[i].album_id = sqlite3_column_int64(stmt, 9);
        results[i].artist_id = sqlite3_column_int64(stmt, 10);
        const char* genre = (const char*)sqlite3_column_text(stmt, 11);
        results[i].genre = genre ? strdup(genre) : NULL;
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

// =============================================================================
// "Appears On" Operations (via track_artists junction table)
// =============================================================================

quadrature_result_t db_get_artist_appearances(quadrature_db_t* db, int64_t artist_id,
                                               db_album_t** out, size_t* count) {
    if (!db || !out || !count || artist_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get count of distinct albums where artist appears but isn't the album artist
    sqlite3_stmt* cnt_stmt;
    const char* cnt_sql =
        "SELECT COUNT(DISTINCT al.id) FROM albums al "
        "INNER JOIN tracks t ON t.album_id = al.id "
        "INNER JOIN track_artists ta ON ta.track_id = t.id "
        "WHERE ta.artist_id = ? AND al.artist_id != ?";
    sqlite3_prepare_v2(db->db, cnt_sql, -1, &cnt_stmt, NULL);
    sqlite3_bind_int64(cnt_stmt, 1, artist_id);
    sqlite3_bind_int64(cnt_stmt, 2, artist_id);
    sqlite3_step(cnt_stmt);
    size_t n = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (n == 0) {
        db_unlock(db);
        gint64 elapsed = g_get_monotonic_time() - start_time;
        g_debug("db_get_artist_appearances(%" G_GINT64_FORMAT "): 0 results in %.2f ms",
                artist_id, elapsed / 1000.0);
        return QUADRATURE_OK;
    }

    db_album_t* results = calloc(n, sizeof(db_album_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    // Get distinct albums where artist appears as track artist but not album artist
    const char* sql =
        "SELECT DISTINCT al.id, al.title, ar.name, al.artist_id, al.year, "
        "  (SELECT COUNT(*) FROM tracks t2 WHERE t2.album_id = al.id) AS track_count, "
        "  (SELECT GROUP_CONCAT(g, ';') FROM (SELECT DISTINCT genre AS g FROM tracks WHERE album_id = al.id AND genre IS NOT NULL AND genre != '')) AS genres "
        "FROM albums al "
        "INNER JOIN tracks t ON t.album_id = al.id "
        "INNER JOIN track_artists ta ON ta.track_id = t.id "
        "LEFT JOIN artists ar ON al.artist_id = ar.id "
        "WHERE ta.artist_id = ? AND al.artist_id != ? "
        "ORDER BY al.year DESC, al.title COLLATE NOCASE";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, artist_id);
    sqlite3_bind_int64(stmt, 2, artist_id);

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
        const char* genres = (const char*)sqlite3_column_text(stmt, 6);
        results[i].genres = (genres && *genres) ? strdup(genres) : NULL;
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_artist_appearances(%" G_GINT64_FORMAT "): %zu results in %.2f ms",
            artist_id, i, elapsed / 1000.0);

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_artist_appearance_tracks(quadrature_db_t* db, int64_t artist_id,
                                                     db_track_t** out, size_t* count) {
    if (!db || !out || !count || artist_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    gint64 start_time = g_get_monotonic_time();

    db_lock(db);

    // Get count of tracks where artist appears but album is by different artist
    sqlite3_stmt* cnt_stmt;
    const char* cnt_sql =
        "SELECT COUNT(*) FROM tracks t "
        "INNER JOIN albums al ON t.album_id = al.id "
        "INNER JOIN track_artists ta ON ta.track_id = t.id "
        "WHERE ta.artist_id = ? AND al.artist_id != ?";
    sqlite3_prepare_v2(db->db, cnt_sql, -1, &cnt_stmt, NULL);
    sqlite3_bind_int64(cnt_stmt, 1, artist_id);
    sqlite3_bind_int64(cnt_stmt, 2, artist_id);
    sqlite3_step(cnt_stmt);
    size_t n = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (n == 0) {
        db_unlock(db);
        gint64 elapsed = g_get_monotonic_time() - start_time;
        g_debug("db_get_artist_appearance_tracks(%" G_GINT64_FORMAT "): 0 results in %.2f ms",
                artist_id, elapsed / 1000.0);
        return QUADRATURE_OK;
    }

    db_track_t* results = calloc(n, sizeof(db_track_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    // Get tracks where artist appears but album belongs to different artist
    const char* sql =
        "SELECT t.id, t.title, a.name, al.title, t.path, t.duration_ms, t.track_num, "
        "       t.disc_num, t.year, t.album_id, ta.artist_id, t.genre "
        "FROM tracks t "
        "INNER JOIN albums al ON t.album_id = al.id "
        "INNER JOIN track_artists ta ON ta.track_id = t.id "
        "LEFT JOIN artists a ON a.id = ta.artist_id "
        "WHERE ta.artist_id = ? AND al.artist_id != ? AND ta.position = 0 "
        "ORDER BY al.title COLLATE NOCASE, t.disc_num, t.track_num";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, artist_id);
    sqlite3_bind_int64(stmt, 2, artist_id);

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < n) {
        results[i].id = sqlite3_column_int64(stmt, 0);
        const char* title = (const char*)sqlite3_column_text(stmt, 1);
        const char* artist = (const char*)sqlite3_column_text(stmt, 2);
        const char* album = (const char*)sqlite3_column_text(stmt, 3);
        const char* path = (const char*)sqlite3_column_text(stmt, 4);
        results[i].title = title ? strdup(title) : strdup("Unknown");
        results[i].artist = artist ? strdup(artist) : strdup("Unknown Artist");
        results[i].artist_display = NULL;
        results[i].album = album ? strdup(album) : strdup("Unknown Album");
        results[i].path = path ? strdup(path) : strdup("");
        results[i].duration_ms = sqlite3_column_int(stmt, 5);
        results[i].track_num = sqlite3_column_int(stmt, 6);
        results[i].disc_num = sqlite3_column_int(stmt, 7);
        if (results[i].disc_num == 0) results[i].disc_num = 1;
        results[i].year = sqlite3_column_int(stmt, 8);
        results[i].album_id = sqlite3_column_int64(stmt, 9);
        results[i].artist_id = sqlite3_column_int64(stmt, 10);
        const char* genre = (const char*)sqlite3_column_text(stmt, 11);
        results[i].genre = genre ? strdup(genre) : NULL;
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_artist_appearance_tracks(%" G_GINT64_FORMAT "): %zu results in %.2f ms",
            artist_id, i, elapsed / 1000.0);

    *out = results;
    *count = i;
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
        free(albums[i].genres);
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
        free(tracks[i].genre);
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
        case DB_SORT_YEAR_ASC:      return "ORDER BY t.year ASC, al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_YEAR_DESC:     return "ORDER BY t.year DESC, al.title COLLATE NOCASE DESC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ARTIST_ASC:    return "ORDER BY ar.name COLLATE NOCASE ASC, t.year ASC, al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ARTIST_DESC:   return "ORDER BY ar.name COLLATE NOCASE DESC, t.year DESC, al.title COLLATE NOCASE DESC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ADDED_ASC:     return "ORDER BY t.id ASC";
        case DB_SORT_ADDED_DESC:    return "ORDER BY t.id DESC";
        case DB_SORT_RECENT:        return "ORDER BY t.id DESC";
        case DB_SORT_ALBUM_ASC:     return "ORDER BY al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_ALBUM_DESC:    return "ORDER BY al.title COLLATE NOCASE DESC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_DURATION_ASC:  return "ORDER BY t.duration_ms ASC";
        case DB_SORT_DURATION_DESC: return "ORDER BY t.duration_ms DESC";
        case DB_SORT_TRACK_NUM_ASC: return "ORDER BY al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        case DB_SORT_DISC_NUM_ASC:  return "ORDER BY al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC";
        default:                    return "ORDER BY ar.name COLLATE NOCASE ASC, t.year ASC, al.title COLLATE NOCASE ASC, t.disc_num ASC, t.track_num ASC, t.title COLLATE NOCASE ASC";
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

/* Append genre/year WHERE clauses for artist filtering.
 * When both genre and year are active, requires both to match on the SAME album. */
static void sql_append_artist_filters(GString *sql,
                                       gboolean has_search,
                                       gboolean has_genre,
                                       gboolean has_year,
                                       const db_search_opts_t *filters) {
    if (has_search)
        g_string_append(sql, " AND a.name LIKE '%' || ? || '%' COLLATE NOCASE");

    if (has_genre && has_year) {
        // Combined: genre and year must match on the SAME album
        g_string_append(sql,
            " AND EXISTS (SELECT 1 FROM albums _al"
            " WHERE _al.artist_id = a.id AND (");
        sql_append_year_or(sql, filters->year_mask, "_al.year");
        g_string_append(sql,
            ") AND EXISTS (SELECT 1 FROM tracks _t WHERE _t.album_id = _al.id AND (");
        for (size_t gi = 0; gi < filters->genre_count; gi++) {
            if (gi > 0) g_string_append(sql, " OR ");
            g_string_append(sql, "';' || _t.genre || ';' LIKE '%;' || ? || ';%'");
        }
        g_string_append(sql, ")))");
    } else {
        if (has_genre) {
            g_string_append(sql,
                " AND EXISTS (SELECT 1 FROM track_artists _ta"
                " JOIN tracks _t ON _t.id = _ta.track_id"
                " WHERE _ta.artist_id = a.id AND (");
            for (size_t gi = 0; gi < filters->genre_count; gi++) {
                if (gi > 0) g_string_append(sql, " OR ");
                g_string_append(sql, "';' || _t.genre || ';' LIKE '%;' || ? || ';%'");
            }
            g_string_append(sql, "))");
        }
        if (has_year) {
            g_string_append(sql,
                " AND EXISTS (SELECT 1 FROM albums _al"
                " WHERE _al.artist_id = a.id AND (");
            sql_append_year_or(sql, filters->year_mask, "_al.year");
            g_string_append(sql, "))");
        }
    }
}

/* Bind search_text + genre params for artist filter clauses, returns next index */
static int sql_bind_artist_filters(sqlite3_stmt *stmt, int pidx,
                                    gboolean has_search, const char *search_text,
                                    gboolean has_genre, const db_search_opts_t *filters) {
    if (has_search)
        sqlite3_bind_text(stmt, pidx++, search_text, -1, SQLITE_STATIC);
    if (has_genre)
        pidx = sql_bind_genres(stmt, pidx, filters);
    return pidx;
}

quadrature_result_t db_get_artists_page(quadrature_db_t* db,
                                         const db_page_opts_t* opts,
                                         db_artist_t** out,
                                         size_t* out_count,
                                         size_t* total_count) {
    if (!db || !opts || !out || !out_count || !total_count)
        return QUADRATURE_ERROR_INVALID_PARAM;

    gint64 start_time = g_get_monotonic_time();

    gboolean has_search = opts->search_text && opts->search_text[0];
    gboolean has_genre = opts->filters && opts->filters->genre_count > 0;
    gboolean has_year = opts->filters && opts->filters->year_mask != 0;
    gboolean has_filters = has_search || has_genre || has_year;

    db_lock(db);

    // --- Count query ---
    if (has_filters) {
        GString *cnt_sql = g_string_new("SELECT COUNT(*) FROM artists a WHERE 1=1");
        sql_append_artist_filters(cnt_sql, has_search, has_genre, has_year, opts->filters);

        sqlite3_stmt* cnt_stmt;
        sqlite3_prepare_v2(db->db, cnt_sql->str, -1, &cnt_stmt, NULL);
        g_string_free(cnt_sql, TRUE);

        sql_bind_artist_filters(cnt_stmt, 1, has_search, opts->search_text,
                                has_genre, opts->filters);

        sqlite3_step(cnt_stmt);
        *total_count = sqlite3_column_int64(cnt_stmt, 0);
        sqlite3_finalize(cnt_stmt);
    } else {
        sqlite3_stmt* cnt_stmt;
        sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM artists", -1, &cnt_stmt, NULL);
        sqlite3_step(cnt_stmt);
        *total_count = sqlite3_column_int64(cnt_stmt, 0);
        sqlite3_finalize(cnt_stmt);
    }

    if (*total_count == 0 || opts->offset >= *total_count) {
        db_unlock(db);
        *out = NULL;
        *out_count = 0;
        return QUADRATURE_OK;
    }

    // --- Data query ---
    GString *sql_str = g_string_new(
        "SELECT a.id, a.name, "
        "COUNT(DISTINCT al.id) AS album_count, "
        "COUNT(DISTINCT ta.track_id) AS track_count "
        "FROM artists a "
        "LEFT JOIN albums al ON al.artist_id = a.id "
        "LEFT JOIN track_artists ta ON ta.artist_id = a.id");

    if (has_filters) {
        g_string_append(sql_str, " WHERE 1=1");
        sql_append_artist_filters(sql_str, has_search, has_genre, has_year, opts->filters);
    }

    g_string_append_printf(sql_str, " GROUP BY a.id %s LIMIT ? OFFSET ?",
                           get_artist_order_clause(opts->sort));

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    int pidx = sql_bind_artist_filters(stmt, 1, has_search, opts->search_text,
                                       has_genre, opts->filters);
    sqlite3_bind_int64(stmt, pidx++, opts->limit);
    sqlite3_bind_int64(stmt, pidx, opts->offset);

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
    g_debug("db_get_artists_page(offset=%zu, limit=%zu, filtered=%d): %zu results in %.2f ms",
            opts->offset, opts->limit, has_filters, i, elapsed / 1000.0);

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

    gboolean has_search = opts->search_text && opts->search_text[0];
    gboolean has_genre = opts->filters && opts->filters->genre_count > 0;
    gboolean has_year = opts->filters && opts->filters->year_mask != 0;
    gboolean has_filters = has_search || has_genre || has_year;

    db_lock(db);

    // --- Count query ---
    if (has_filters) {
        GString *cnt_sql = g_string_new(
            "SELECT COUNT(*) FROM albums al"
            " LEFT JOIN artists ar ON al.artist_id = ar.id"
            " WHERE 1=1");
        if (has_search) {
            g_string_append(cnt_sql,
                " AND (al.title LIKE '%' || ? || '%' COLLATE NOCASE"
                " OR ar.name LIKE '%' || ? || '%' COLLATE NOCASE)");
        }
        if (has_year) {
            g_string_append(cnt_sql, " AND (");
            sql_append_year_or(cnt_sql, opts->filters->year_mask, "al.year");
            g_string_append_c(cnt_sql, ')');
        }
        if (has_genre) {
            g_string_append(cnt_sql,
                " AND EXISTS (SELECT 1 FROM tracks _t WHERE _t.album_id = al.id AND (");
            for (size_t gi = 0; gi < opts->filters->genre_count; gi++) {
                if (gi > 0) g_string_append(cnt_sql, " OR ");
                g_string_append(cnt_sql, "';' || _t.genre || ';' LIKE '%;' || ? || ';%'");
            }
            g_string_append(cnt_sql, "))");
        }

        sqlite3_stmt* cnt_stmt;
        sqlite3_prepare_v2(db->db, cnt_sql->str, -1, &cnt_stmt, NULL);
        g_string_free(cnt_sql, TRUE);

        int pidx = 1;
        if (has_search) {
            sqlite3_bind_text(cnt_stmt, pidx++, opts->search_text, -1, SQLITE_STATIC);
            sqlite3_bind_text(cnt_stmt, pidx++, opts->search_text, -1, SQLITE_STATIC);
        }
        if (has_genre)
            pidx = sql_bind_genres(cnt_stmt, pidx, opts->filters);

        sqlite3_step(cnt_stmt);
        *total_count = sqlite3_column_int64(cnt_stmt, 0);
        sqlite3_finalize(cnt_stmt);
    } else {
        sqlite3_stmt* cnt_stmt;
        sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM albums", -1, &cnt_stmt, NULL);
        sqlite3_step(cnt_stmt);
        *total_count = sqlite3_column_int64(cnt_stmt, 0);
        sqlite3_finalize(cnt_stmt);
    }

    if (*total_count == 0 || opts->offset >= *total_count) {
        db_unlock(db);
        *out = NULL;
        *out_count = 0;
        return QUADRATURE_OK;
    }

    // --- Data query ---
    GString *sql_str = g_string_new(
        "SELECT al.id, al.title, ar.name, al.artist_id, al.year, "
        "COUNT(t.id) AS track_count, "
        "(SELECT GROUP_CONCAT(g, ';') FROM (SELECT DISTINCT genre AS g FROM tracks WHERE album_id = al.id AND genre IS NOT NULL AND genre != '')) AS genres "
        "FROM albums al "
        "LEFT JOIN artists ar ON al.artist_id = ar.id "
        "LEFT JOIN tracks t ON t.album_id = al.id");

    if (has_filters) {
        g_string_append(sql_str, " WHERE 1=1");
        if (has_search) {
            g_string_append(sql_str,
                " AND (al.title LIKE '%' || ? || '%' COLLATE NOCASE"
                " OR ar.name LIKE '%' || ? || '%' COLLATE NOCASE)");
        }
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
                g_string_append(sql_str, "';' || _t.genre || ';' LIKE '%;' || ? || ';%'");
            }
            g_string_append(sql_str, "))");
        }
    }

    g_string_append_printf(sql_str, " GROUP BY al.id %s LIMIT ? OFFSET ?",
                           get_album_order_clause(opts->sort));

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    int pidx = 1;
    if (has_search) {
        sqlite3_bind_text(stmt, pidx++, opts->search_text, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, pidx++, opts->search_text, -1, SQLITE_STATIC);
    }
    if (has_genre)
        pidx = sql_bind_genres(stmt, pidx, opts->filters);
    sqlite3_bind_int64(stmt, pidx++, opts->limit);
    sqlite3_bind_int64(stmt, pidx, opts->offset);

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
        const char* genres = (const char*)sqlite3_column_text(stmt, 6);
        results[i].genres = (genres && *genres) ? strdup(genres) : NULL;
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    gint64 elapsed = g_get_monotonic_time() - start_time;
    g_debug("db_get_albums_page(offset=%zu, limit=%zu, filtered=%d): %zu results in %.2f ms",
            opts->offset, opts->limit, has_filters, i, elapsed / 1000.0);

    *out = results;
    *out_count = i;
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
// Track Artist Read Operations (multi-artist via junction table)
// =============================================================================

quadrature_result_t db_get_track_artists(quadrature_db_t* db, int64_t track_id,
                                          db_track_artist_t** out, size_t* count) {
    if (!db || track_id <= 0 || !out || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out = NULL;
    *count = 0;

    db_lock(db);

    // Get count first
    sqlite3_stmt* cnt_stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT COUNT(*) FROM track_artists WHERE track_id = ?",
        -1, &cnt_stmt, NULL);
    sqlite3_bind_int64(cnt_stmt, 1, track_id);
    sqlite3_step(cnt_stmt);
    size_t n = sqlite3_column_int64(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    if (n == 0) {
        db_unlock(db);
        return QUADRATURE_OK;
    }

    db_track_artist_t* results = calloc(n, sizeof(db_track_artist_t));
    if (!results) {
        db_unlock(db);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT ta.artist_id, a.name, ta.role, ta.position "
        "FROM track_artists ta "
        "LEFT JOIN artists a ON a.id = ta.artist_id "
        "WHERE ta.track_id = ? "
        "ORDER BY ta.position",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, track_id);

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < n) {
        results[i].artist_id = sqlite3_column_int64(stmt, 0);
        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        results[i].name = name ? strdup(name) : strdup("Unknown Artist");
        results[i].role = sqlite3_column_int(stmt, 2);
        results[i].position = sqlite3_column_int(stmt, 3);
        i++;
    }

    sqlite3_finalize(stmt);
    db_unlock(db);

    *out = results;
    *count = i;
    return QUADRATURE_OK;
}

void db_track_artists_free(db_track_artist_t* artists, size_t count) {
    if (!artists) return;
    for (size_t i = 0; i < count; i++) {
        free(artists[i].name);
    }
    free(artists);
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

// =============================================================================
// ID-Only Filtered Queries (for cache-resolved filtering)
// =============================================================================

int64_t db_get_max_id(quadrature_db_t* db, const char* table_name) {
    if (!db || !table_name) return 0;

    db_lock(db);

    // table_name is always a compile-time constant — safe to interpolate
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT MAX(id) FROM %s", table_name);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return 0;
    }

    int64_t max_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        max_id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return max_id;
}

quadrature_result_t db_get_artist_ids_filtered(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count) {
    if (!db || !opts || !out_ids || !out_count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out_ids = NULL;
    *out_count = 0;

    gboolean has_search = opts->search_text && opts->search_text[0];
    gboolean has_genre = opts->filters && opts->filters->genre_count > 0;
    gboolean has_year = opts->filters && opts->filters->year_mask != 0;

    GString *sql_str = g_string_new("SELECT a.id FROM artists a WHERE 1=1");
    sql_append_artist_filters(sql_str, has_search, has_genre, has_year, opts->filters);
    g_string_append_printf(sql_str, " %s", get_artist_order_clause(opts->sort));

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sql_bind_artist_filters(stmt, 1, has_search, opts->search_text,
                            has_genre, opts->filters);

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

    *out_ids = ids;
    *out_count = n;
    return QUADRATURE_OK;
}

quadrature_result_t db_get_album_ids_filtered(quadrature_db_t* db,
    const db_id_query_opts_t* opts, int64_t** out_ids, size_t* out_count) {
    if (!db || !opts || !out_ids || !out_count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out_ids = NULL;
    *out_count = 0;

    gboolean has_search = opts->search_text && opts->search_text[0];
    gboolean has_genre = opts->filters && opts->filters->genre_count > 0;
    gboolean has_year = opts->filters && opts->filters->year_mask != 0;

    GString *sql_str = g_string_new(
        "SELECT al.id FROM albums al"
        " LEFT JOIN artists ar ON al.artist_id = ar.id"
        " WHERE 1=1");

    if (has_search) {
        g_string_append(sql_str,
            " AND (al.title LIKE '%' || ? || '%' COLLATE NOCASE"
            " OR ar.name LIKE '%' || ? || '%' COLLATE NOCASE)");
    }
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
            g_string_append(sql_str, "';' || _t.genre || ';' LIKE '%;' || ? || ';%'");
        }
        g_string_append(sql_str, "))");
    }

    g_string_append_printf(sql_str, " %s", get_album_order_clause(opts->sort));

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, sql_str->str, -1, &stmt, NULL);
    g_string_free(sql_str, TRUE);

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    int pidx = 1;
    if (has_search) {
        sqlite3_bind_text(stmt, pidx++, opts->search_text, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, pidx++, opts->search_text, -1, SQLITE_STATIC);
    }
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

    *out_ids = ids;
    *out_count = n;
    return QUADRATURE_OK;
}

quadrature_result_t db_search_track_ids(quadrature_db_t* db,
    const char* query, const db_search_opts_t* opts, size_t limit,
    int64_t** out_ids, size_t* out_count) {
    if (!db || !query || !out_ids || !out_count) return QUADRATURE_ERROR_INVALID_PARAM;

    *out_ids = NULL;
    *out_count = 0;

    size_t qlen = strlen(query);
    if (qlen == 0) return QUADRATURE_OK;

    char* q = g_strdup_printf("%s*", query);

    GString *sql_str = g_string_new(
        "SELECT t.id FROM tracks_fts f"
        " JOIN tracks t ON f.rowid = t.id"
        " WHERE tracks_fts MATCH ?");

    if (opts && opts->genre_count > 0) {
        g_string_append(sql_str, " AND (");
        for (size_t gi = 0; gi < opts->genre_count; gi++) {
            if (gi > 0) g_string_append(sql_str, " OR ");
            g_string_append(sql_str, "';' || t.genre || ';' LIKE '%;' || ? || ';%'");
        }
        g_string_append_c(sql_str, ')');
    }
    if (opts && opts->year_mask) {
        g_string_append(sql_str, " AND (");
        sql_append_year_or(sql_str, opts->year_mask, "t.year");
        g_string_append_c(sql_str, ')');
    }
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
    if (opts) pidx = sql_bind_genres(stmt, pidx, opts);
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
