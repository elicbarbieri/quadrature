/**
 * Shared test helpers for Quadrature integration and unit tests.
 *
 * Include this header in any test file that uses library_cache, database,
 * or indexer APIs. All functions are static inline to avoid linker conflicts
 * when multiple test binaries include this header.
 */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <criterion/criterion.h>
#include "quadrature/library.h"
#include "quadrature/database.h"
#include "../../src/database/internal.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Database cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Remove a SQLite database and its WAL/SHM sidecar files. */
static inline void test_cleanup_db(const char *path) {
    char buf[280];
    unlink(path);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path);
    unlink(buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Track / artist creation helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Create a track with default duration (200s) and link it to an album. */
static inline void test_create_track(quadrature_db_t *db, int64_t album_id,
                                     const char *title, int track_num,
                                     int disc_num) {
    db_index_item_t item = {
        .path        = title,   /* unique within album */
        .title       = title,
        .album       = "unused",
        .duration_ms = 200000,
        .track_num   = (uint16_t)track_num,
        .disc_num    = (uint16_t)disc_num,
        .year        = 2020,
        .mtime       = 1000000 + track_num,
    };
    quadrature_result_t res = db_upsert_track_with_album(db, &item, album_id, NULL);
    cr_assert_eq(res, QUADRATURE_OK, "failed to create track '%s'", title);
}

/**
 * Create a track with a custom duration (milliseconds).
 * Useful for tests that verify duration-based matching or sorting.
 */
static inline void test_create_track_with_duration(quadrature_db_t *db,
                                                   int64_t album_id,
                                                   const char *title,
                                                   int track_num,
                                                   int disc_num,
                                                   int duration_ms) {
    db_index_item_t item = {
        .path        = title,
        .title       = title,
        .album       = "unused",
        .duration_ms = duration_ms,
        .track_num   = (uint16_t)track_num,
        .disc_num    = (uint16_t)disc_num,
        .year        = 2020,
        .mtime       = 1000000 + track_num,
    };
    quadrature_result_t res = db_upsert_track_with_album(db, &item, album_id, NULL);
    cr_assert_eq(res, QUADRATURE_OK, "failed to create track '%s'", title);
}

/** Link a track to a single artist (position=0, no join phrase). */
static inline void test_link_track_artist(quadrature_db_t *db,
                                          int64_t track_id,
                                          int64_t artist_id) {
    db_track_artist_t ta = {
        .artist_id = artist_id, .position = 0, .join_phrase = ""
    };
    db_set_track_artists(db, track_id, &ta, 1);
}

/**
 * Link a track to multiple artists (featured credit pattern).
 * artists[] and join_phrases[] must have `count` elements.
 * Positions assigned in array order.
 */
static inline void test_link_track_artists(quadrature_db_t *db,
                                           int64_t track_id,
                                           const int64_t *artist_ids,
                                           const char **join_phrases,
                                           int count) {
    db_track_artist_t ta[8];
    cr_assert(count <= 8, "test_link_track_artists: max 8 artists");
    for (int i = 0; i < count; i++) {
        ta[i].artist_id = artist_ids[i];
        ta[i].position = i;
        ta[i].join_phrase = (char *)join_phrases[i];
    }
    db_set_track_artists(db, track_id, ta, count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library cache query helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Count how many artists in the array match `name` (case-insensitive). */
static inline int test_count_artist_name(const GPtrArray *artists,
                                         const char *name) {
    int count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) count++;
    }
    return count;
}

/** Count how many albums in the array match `title` (case-insensitive). */
static inline int test_count_album_title(const GPtrArray *albums,
                                         const char *title) {
    int count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) count++;
    }
    return count;
}

/** Count albums whose title starts with `prefix` (case-insensitive). */
static inline int test_count_album_title_prefix(const GPtrArray *albums,
                                                const char *prefix) {
    int count = 0;
    size_t len = strlen(prefix);
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strncasecmp(a->title, prefix, len) == 0) count++;
    }
    return count;
}

/** Find an artist by name (case-insensitive). Returns NULL if not found. */
static inline const library_artist_info_t *
test_find_artist(const GPtrArray *artists, const char *name) {
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) return a;
    }
    return NULL;
}

/** Find an album by title (case-insensitive). Returns NULL if not found. */
static inline const library_album_info_t *
test_find_album(const GPtrArray *albums, const char *title) {
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0) return a;
    }
    return NULL;
}

/** Check if any album matches `title` (case-insensitive). */
static inline bool test_has_album_title(const GPtrArray *albums,
                                        const char *title) {
    return test_find_album(albums, title) != NULL;
}

/**
 * Find an artist's global ID from the cache by name.
 * Asserts if the artist is not found.
 */
static inline int64_t test_find_artist_id(library_cache_t *cache,
                                          const char *name) {
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, name, NULL, LIBRARY_MASK_ALL);
    cr_assert_not_null(artists, "artist query for '%s' returned NULL", name);
    int64_t found_id = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            found_id = a->artist_id;
            break;
        }
    }
    g_ptr_array_unref(artists);
    cr_assert(found_id != 0, "artist '%s' not found", name);
    return found_id;
}

/**
 * Find an artist's global ID scoped to a specific library mask.
 * Returns 0 if not found (does not assert).
 */
static inline int64_t test_find_artist_id_in_library(library_cache_t *cache,
                                                     const char *name,
                                                     uint32_t mask) {
    GPtrArray *artists = library_cache_get_artists_filtered(
        cache, LIBRARY_SORT_NAME_ASC, name, NULL, mask);
    if (!artists) return 0;
    int64_t found_id = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0) {
            found_id = a->artist_id;
            break;
        }
    }
    g_ptr_array_unref(artists);
    return found_id;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Direct SQL helpers (for assertions that need raw DB access)
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Count rows returned by a simple COUNT(*) SQL statement. */
static inline int test_count_rows(quadrature_db_t *db, const char *sql) {
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_step(stmt);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return count;
}

/** Count rows from a COUNT(*) query with a single int64 parameter. */
static inline int test_count_rows_param(quadrature_db_t *db, const char *sql,
                                        int64_t param) {
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, param);
    sqlite3_step(stmt);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    db_unlock(db);
    return count;
}

/** Read a single int64 column from a query with one int64 parameter. */
static inline int64_t test_read_int64(quadrature_db_t *db, const char *sql,
                                      int64_t param) {
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, param);
    int rc = sqlite3_step(stmt);
    int64_t result = (rc == SQLITE_ROW) ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    db_unlock(db);
    return result;
}

/** Read a single text column from a query with one int64 parameter. Returns NULL on no row. */
static inline char *test_read_text(quadrature_db_t *db, const char *sql,
                                   int64_t param) {
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, param);
    int rc = sqlite3_step(stmt);
    char *result = NULL;
    if (rc == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (text) result = strdup(text);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return result;
}

#endif /* TEST_HELPERS_H */
