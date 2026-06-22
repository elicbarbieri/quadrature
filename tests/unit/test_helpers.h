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
#include "quadrature/library_search.h"
#include "quadrature/database.h"
#include "quadrature/indexer.h"
#include "../../src/database/internal.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Database cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Remove a SQLite database and its WAL/SHM sidecar files. */
static inline void
test_cleanup_db(const char *path)
{
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

/* Fixture helpers — thin wrappers over the reconciler so tests can populate
 * a synthetic DB state without running the full indexer. */

/** Get/create an artist, returning the id directly (db_get_or_create_artist
 * uses an out-param; tests want the id as a return value). Returns -1 on error. */
static inline int64_t
test_goc_artist(quadrature_db_t *db, const char *name, const char *sort, const char *mbid)
{
    int64_t id = -1;
    db_get_or_create_artist(db, name, sort, mbid, &id);
    return id;
}

/** Create or fetch an album at `path` with title + artist_id. */
static inline int64_t
test_insert_album(
    quadrature_db_t *db, const char *path, const char *title, int64_t artist_id, uint16_t year)
{
    int64_t album_id = 0;
    cr_assert_eq(db_create_or_get_album_by_path(db, path, title, artist_id, year, &album_id),
                 QUADRATURE_OK,
                 "insert album '%s'",
                 path);
    return album_id;
}

/** Insert a single track into `album_id` via the reconciler.
 *  Returns the new track's row id. */
static inline int64_t
test_insert_track_full(quadrature_db_t *db,
                       int64_t album_id,
                       const char *rel_path,
                       const char *title,
                       int track_num,
                       int disc_num,
                       int duration_ms,
                       const int64_t *artist_ids,
                       const char **artist_names,
                       const char **join_phrases,
                       int artist_count)
{
    desired_track_artist_t credits[8];
    cr_assert(artist_count <= 8);
    for (int i = 0; i < artist_count; i++) {
        credits[i] = (desired_track_artist_t){
            .artist_id = artist_ids[i],
            .name = artist_names[i],
            .join_phrase = join_phrases ? join_phrases[i] : "",
            .position = i,
        };
    }

    desired_track_t track = {
        .path = rel_path,
        .present_fields = DESIRED_TRACK_TITLE | DESIRED_TRACK_NUM | DESIRED_TRACK_DISC
                          | DESIRED_TRACK_DURATION | DESIRED_TRACK_YEAR | DESIRED_TRACK_MTIME
                          | (artist_count > 0 ? DESIRED_TRACK_ARTISTS : 0),
        .title = title,
        .track_num = (uint16_t)track_num,
        .disc_num = (uint16_t)(disc_num > 0 ? disc_num : 1),
        .duration_ms = (uint32_t)duration_ms,
        .year = 2020,
        .mtime = 1000000 + track_num,
        .artists = artist_count > 0 ? credits : NULL,
        .artist_count = (size_t)artist_count,
    };
    desired_album_state_t desired = {
        .source = RECONCILE_SOURCE_TAGS,
        .tracks = &track,
        .track_count = 1,
    };
    reconcile_policy_t policy = { 0 }; /* no prune, no confidence gate */

    bool need_txn = !db->in_transaction;
    if (need_txn)
        cr_assert_eq(db_begin_transaction(db), QUADRATURE_OK);
    cr_assert_eq(db_reconcile_albums(db, &album_id, &desired, 1, &policy, NULL),
                 QUADRATURE_OK,
                 "reconcile track '%s'",
                 rel_path);
    if (need_txn)
        cr_assert_eq(db_commit(db), QUADRATURE_OK);

    /* Look up new track_id by (album_id, path). */
    db_lock(db);
    sqlite3_stmt *q = NULL;
    sqlite3_prepare_v2(
        db->db, "SELECT id FROM tracks WHERE album_id = ? AND path = ?", -1, &q, NULL);
    sqlite3_bind_int64(q, 1, album_id);
    sqlite3_bind_text(q, 2, rel_path, -1, SQLITE_STATIC);
    int64_t track_id = (sqlite3_step(q) == SQLITE_ROW) ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    db_unlock(db);
    cr_assert(track_id > 0, "track '%s' not inserted", rel_path);
    return track_id;
}

/** Create a track with default duration (200s). Returns track_id. */
static inline int64_t
test_create_track(
    quadrature_db_t *db, int64_t album_id, const char *title, int track_num, int disc_num)
{
    return test_insert_track_full(
        db, album_id, title, title, track_num, disc_num, 200000, NULL, NULL, NULL, 0);
}

/** Create a track with a custom duration (milliseconds). Returns track_id. */
static inline int64_t
test_create_track_with_duration(quadrature_db_t *db,
                                int64_t album_id,
                                const char *title,
                                int track_num,
                                int disc_num,
                                int duration_ms)
{
    return test_insert_track_full(
        db, album_id, title, title, track_num, disc_num, duration_ms, NULL, NULL, NULL, 0);
}

/** Link a track to a single artist (position=0, no join phrase). */
static inline void
test_link_track_artist(quadrature_db_t *db, int64_t track_id, int64_t artist_id)
{
    /* Direct SQL — needed to add credits AFTER initial reconcile-based insert
     * without re-running the full album reconcile. */
    bool need_txn = !db->in_transaction;
    if (need_txn)
        db_begin_transaction(db);
    db_lock(db);
    sqlite3_exec(db->db,
                 "DELETE FROM track_artists WHERE track_id = "
                 "(SELECT id FROM tracks WHERE id = ?)",
                 NULL,
                 NULL,
                 NULL);
    sqlite3_stmt *del = NULL;
    sqlite3_prepare_v2(db->db, "DELETE FROM track_artists WHERE track_id = ?", -1, &del, NULL);
    sqlite3_bind_int64(del, 1, track_id);
    sqlite3_step(del);
    sqlite3_finalize(del);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db->db,
                       "INSERT INTO track_artists(track_id, artist_id, position, join_phrase) "
                       "VALUES(?, ?, 0, '')",
                       -1,
                       &ins,
                       NULL);
    sqlite3_bind_int64(ins, 1, track_id);
    sqlite3_bind_int64(ins, 2, artist_id);
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    sqlite3_stmt *upd = NULL;
    sqlite3_prepare_v2(db->db,
                       "UPDATE tracks SET artist_display = (SELECT name FROM artists WHERE id = ?) "
                       "WHERE id = ?",
                       -1,
                       &upd,
                       NULL);
    sqlite3_bind_int64(upd, 1, artist_id);
    sqlite3_bind_int64(upd, 2, track_id);
    sqlite3_step(upd);
    sqlite3_finalize(upd);
    db_unlock(db);
    if (need_txn)
        db_commit(db);
}

/**
 * Link a track to multiple artists (featured credit pattern).
 * Positions assigned in array order.
 */
static inline void
test_link_track_artists(quadrature_db_t *db,
                        int64_t track_id,
                        const int64_t *artist_ids,
                        const char **join_phrases,
                        int count)
{
    bool need_txn = !db->in_transaction;
    if (need_txn)
        db_begin_transaction(db);
    db_lock(db);
    sqlite3_stmt *del = NULL;
    sqlite3_prepare_v2(db->db, "DELETE FROM track_artists WHERE track_id = ?", -1, &del, NULL);
    sqlite3_bind_int64(del, 1, track_id);
    sqlite3_step(del);
    sqlite3_finalize(del);

    GString *display = g_string_new(NULL);
    for (int i = 0; i < count; i++) {
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db->db,
                           "INSERT INTO track_artists(track_id, artist_id, position, join_phrase) "
                           "VALUES(?, ?, ?, ?)",
                           -1,
                           &ins,
                           NULL);
        sqlite3_bind_int64(ins, 1, track_id);
        sqlite3_bind_int64(ins, 2, artist_ids[i]);
        sqlite3_bind_int(ins, 3, i);
        sqlite3_bind_text(ins, 4, join_phrases ? join_phrases[i] : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_finalize(ins);

        /* Append "name + join_phrase" for artist_display. */
        sqlite3_stmt *q = NULL;
        sqlite3_prepare_v2(db->db, "SELECT name FROM artists WHERE id = ?", -1, &q, NULL);
        sqlite3_bind_int64(q, 1, artist_ids[i]);
        if (sqlite3_step(q) == SQLITE_ROW) {
            const char *n = (const char *)sqlite3_column_text(q, 0);
            if (n)
                g_string_append(display, n);
        }
        sqlite3_finalize(q);
        if (join_phrases && join_phrases[i])
            g_string_append(display, join_phrases[i]);
    }

    sqlite3_stmt *upd = NULL;
    sqlite3_prepare_v2(db->db, "UPDATE tracks SET artist_display = ? WHERE id = ?", -1, &upd, NULL);
    sqlite3_bind_text(upd, 1, display->str, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, track_id);
    sqlite3_step(upd);
    sqlite3_finalize(upd);
    g_string_free(display, TRUE);

    /* Refresh tracks_fts for the whole album. */
    sqlite3_stmt *fts = NULL;
    sqlite3_prepare_v2(db->db,
                       "INSERT OR REPLACE INTO tracks_fts(rowid, title, artist, album) "
                       "SELECT t.id, t.title, COALESCE(t.artist_display,''), COALESCE(al.title,'') "
                       "FROM tracks t JOIN albums al ON t.album_id = al.id WHERE t.id = ?",
                       -1,
                       &fts,
                       NULL);
    sqlite3_bind_int64(fts, 1, track_id);
    sqlite3_step(fts);
    sqlite3_finalize(fts);
    db_unlock(db);
    if (need_txn)
        db_commit(db);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library cache query helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Count how many artists in the array match `name` (case-insensitive). */
static inline int
test_count_artist_name(const GPtrArray *artists, const char *name)
{
    int count = 0;
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0)
            count++;
    }
    return count;
}

/** Count how many albums in the array match `title` (case-insensitive). */
static inline int
test_count_album_title(const GPtrArray *albums, const char *title)
{
    int count = 0;
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0)
            count++;
    }
    return count;
}

/** Count albums whose title starts with `prefix` (case-insensitive). */
static inline int
test_count_album_title_prefix(const GPtrArray *albums, const char *prefix)
{
    int count = 0;
    size_t len = strlen(prefix);
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strncasecmp(a->title, prefix, len) == 0)
            count++;
    }
    return count;
}

/** Find an artist by name (case-insensitive). Returns NULL if not found. */
static inline const library_artist_info_t *
test_find_artist(const GPtrArray *artists, const char *name)
{
    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *a = g_ptr_array_index(artists, i);
        if (g_ascii_strcasecmp(a->name, name) == 0)
            return a;
    }
    return NULL;
}

/** Find an album by title (case-insensitive). Returns NULL if not found. */
static inline const library_album_info_t *
test_find_album(const GPtrArray *albums, const char *title)
{
    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *a = g_ptr_array_index(albums, i);
        if (g_ascii_strcasecmp(a->title, title) == 0)
            return a;
    }
    return NULL;
}

/** Check if any album matches `title` (case-insensitive). */
static inline bool
test_has_album_title(const GPtrArray *albums, const char *title)
{
    return test_find_album(albums, title) != NULL;
}

/**
 * Find an artist's global ID from the cache by name.
 * Asserts if the artist is not found.
 */
static inline int64_t
test_find_artist_id(library_cache_t *cache, const char *name)
{
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
static inline int64_t
test_find_artist_id_in_library(library_cache_t *cache, const char *name, uint32_t mask)
{
    GPtrArray *artists
        = library_cache_get_artists_filtered(cache, LIBRARY_SORT_NAME_ASC, name, NULL, mask);
    if (!artists)
        return 0;
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
static inline int
test_count_rows(quadrature_db_t *db, const char *sql)
{
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
static inline int
test_count_rows_param(quadrature_db_t *db, const char *sql, int64_t param)
{
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
static inline int64_t
test_read_int64(quadrature_db_t *db, const char *sql, int64_t param)
{
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
static inline char *
test_read_text(quadrature_db_t *db, const char *sql, int64_t param)
{
    db_lock(db);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, param);
    int rc = sqlite3_step(stmt);
    char *result = NULL;
    if (rc == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (text)
            result = strdup(text);
    }
    sqlite3_finalize(stmt);
    db_unlock(db);
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Fixture FLAC synthesis
 *
 * Builds real FLAC files with Vorbis-comment tags using ffmpeg's `anullsrc`
 * (digital silence). Silence compresses to ~195 bytes/second in FLAC, so a
 * 6-minute track is ~70 KB — tiny enough for tests that need hundreds of
 * tracks without blowing up build time or disk.
 *
 * The indexer reads duration from the stream header (`-t <secs>`), so passing
 * a track's real duration makes MB lookups accurate without shipping audio.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Quote a string for inclusion in a single-quoted shell argument. */
static inline char *
shell_escape(const char *s)
{
    size_t len = strlen(s);
    char *out = (char *)malloc(len * 4 + 1);
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *p++ = '\'';
            *p++ = '\\';
            *p++ = '\'';
            *p++ = '\'';
        } else {
            *p++ = s[i];
        }
    }
    *p = '\0';
    return out;
}

/**
 * Create a FLAC file at `path` with the given Vorbis-comment tags and
 * exact duration. `metadata_pairs` is a NULL-terminated array of "key=value"
 * strings. Returns ffmpeg's exit status (0 on success).
 *
 * Audio is digital silence — ~195 B/s after FLAC compression. Use real
 * durations when MB lookup accuracy matters.
 */
static inline int
create_flac(const char *path, const char *const *metadata_pairs, int duration_secs)
{
    char cmd[8192];
    double dur = duration_secs > 0 ? (double)duration_secs : 1;
    int off = snprintf(cmd,
                       sizeof(cmd),
                       "ffmpeg -y -loglevel error -f lavfi -i anullsrc=r=8000:cl=mono "
                       "-t %.1f ",
                       dur);

    for (const char *const *p = metadata_pairs; *p; p++) {
        char *escaped = shell_escape(*p);
        off += snprintf(cmd + off, sizeof(cmd) - off, "-metadata '%s' ", escaped);
        free(escaped);
    }

    char *epath = shell_escape(path);
    off += snprintf(cmd + off, sizeof(cmd) - off, "'%s'", epath);
    free(epath);

    return system(cmd);
}

#endif /* TEST_HELPERS_H */
