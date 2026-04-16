#include <glib.h>
#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// =============================================================================
// Get or Create Artist
// =============================================================================

quadrature_result_t db_iter_artist_names(quadrature_db_t* db, db_artist_name_iter_cb cb, void* user_data) {
    if (!db || !cb) return QUADRATURE_ERROR_INVALID_PARAM;

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db, "SELECT id, name FROM artists", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return QUADRATURE_ERROR_INTERNAL;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id       = sqlite3_column_int64(stmt, 0);
        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        if (name) cb(id, name, user_data);
    }
    sqlite3_finalize(stmt);
    return QUADRATURE_OK;
}

int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name) {
    if (!name || !*name) name = "Unknown Artist";

    int64_t id = -1;

    // Always acquire the lock (recursive mutex allows re-entry from within a transaction
    // on the same thread; blocks different threads from racing on the SELECT→INSERT).
    db_lock(db);
    db_prepare_stmts(db);

    // Try to select existing
    sqlite3_bind_text(db->select_artist, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(db->select_artist) == SQLITE_ROW) {
        id = sqlite3_column_int64(db->select_artist, 0);
    }
    sqlite3_reset(db->select_artist);

    if (id < 0) {
        // Insert new
        sqlite3_bind_text(db->insert_artist, 1, name, -1, SQLITE_STATIC);
        sqlite3_step(db->insert_artist);
        sqlite3_reset(db->insert_artist);

        // Get the ID
        sqlite3_bind_text(db->select_artist, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(db->select_artist) == SQLITE_ROW) {
            id = sqlite3_column_int64(db->select_artist, 0);
        }
        sqlite3_reset(db->select_artist);

        // Populate artists_fts
        if (id >= 0) {
            sqlite3_bind_int64(db->insert_artist_fts, 1, id);
            sqlite3_bind_text(db->insert_artist_fts, 2, name, -1, SQLITE_STATIC);
            sqlite3_step(db->insert_artist_fts);
            sqlite3_reset(db->insert_artist_fts);
        }
    }

    db_unlock(db);

    return id;
}

// =============================================================================
// Track Write Operations
// =============================================================================

quadrature_result_t db_upsert_track_with_album(quadrature_db_t* db, const db_index_item_t* item,
                                                int64_t album_id, int64_t* track_id_out) {
    if (!db || !item || !item->path) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;
    if (album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    // Check if track already exists (for FTS update)
    int64_t existing_track_id = 0;
    sqlite3_bind_int64(db->select_track_by_path, 1, album_id);
    sqlite3_bind_text(db->select_track_by_path, 2, item->path, -1, SQLITE_STATIC);
    if (sqlite3_step(db->select_track_by_path) == SQLITE_ROW) {
        existing_track_id = sqlite3_column_int64(db->select_track_by_path, 0);
    }
    sqlite3_reset(db->select_track_by_path);

    // Upsert track using provided album_id
    sqlite3_bind_text(db->upsert_track, 1, item->title ? item->title : "Unknown", -1, SQLITE_STATIC);
    sqlite3_bind_int64(db->upsert_track, 2, album_id);
    sqlite3_bind_text(db->upsert_track, 3, item->path, -1, SQLITE_STATIC);
    sqlite3_bind_int(db->upsert_track, 4, item->duration_ms);
    sqlite3_bind_int(db->upsert_track, 5, item->track_num);
    sqlite3_bind_int(db->upsert_track, 6, item->disc_num > 0 ? item->disc_num : 1);
    sqlite3_bind_int64(db->upsert_track, 7, item->mtime);
    sqlite3_bind_int(db->upsert_track, 8, item->year);
    if (item->genre) {
        sqlite3_bind_text(db->upsert_track, 9, item->genre, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(db->upsert_track, 9);
    }

    int rc = sqlite3_step(db->upsert_track);
    sqlite3_reset(db->upsert_track);

    if (rc != SQLITE_DONE) {
        g_critical("Failed to upsert track: %s", sqlite3_errmsg(db->db));
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Get track_id (use existing if it was an update, otherwise get new row id)
    int64_t track_id = existing_track_id > 0 ? existing_track_id : sqlite3_last_insert_rowid(db->db);

    if (track_id_out) *track_id_out = track_id;

    return QUADRATURE_OK;
}

// =============================================================================
// Album Mtime Write Operations (batch only)
// =============================================================================

quadrature_result_t db_set_album_mtimes_batch(quadrature_db_t* db,
                                               const int64_t* album_ids,
                                               const int64_t* mtimes,
                                               const int64_t* sizes,
                                               size_t count) {
    if (!db || !album_ids || !mtimes || count == 0) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    char* err = NULL;
    sqlite3_exec(db->db, "BEGIN", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET last_updated_at = ?, last_updated_size = ? WHERE id = ?",
        -1, &stmt, NULL);

    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_int64(stmt, 1, mtimes[i]);
        sqlite3_bind_int64(stmt, 2, sizes ? sizes[i] : 0);
        sqlite3_bind_int64(stmt, 3, album_ids[i]);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (err) sqlite3_free(err);

    db_unlock(db);
    return QUADRATURE_OK;
}

// =============================================================================
// Folder-Based Album Operations
// =============================================================================

quadrature_result_t db_upsert_folder_album(quadrature_db_t* db,
                                            const char* folder_path,
                                            const char* title,
                                            int64_t artist_id,
                                            uint16_t year,
                                            int64_t* album_id_out) {
    if (!db || !folder_path || !title) return QUADRATURE_ERROR_INVALID_PARAM;

    // Always acquire the lock (recursive mutex allows re-entry from within a transaction
    // on the same thread; blocks different threads from racing on the SELECT→INSERT).
    db_lock(db);
    db_prepare_stmts(db);

    // Try to find existing album by folder path first
    sqlite3_bind_text(db->select_album_by_path, 1, folder_path, -1, SQLITE_STATIC);

    int64_t album_id = -1;
    if (sqlite3_step(db->select_album_by_path) == SQLITE_ROW) {
        album_id = sqlite3_column_int64(db->select_album_by_path, 0);
    }
    sqlite3_reset(db->select_album_by_path);

    if (album_id >= 0) {
        // Update existing album (WHERE mb_status != 1 preserves MB-resolved albums)
        sqlite3_bind_text(db->update_album_by_id, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(db->update_album_by_id, 2, artist_id);
        sqlite3_bind_int(db->update_album_by_id, 3, 0);
        if (year > 0) {
            sqlite3_bind_int(db->update_album_by_id, 4, year);
        } else {
            sqlite3_bind_null(db->update_album_by_id, 4);
        }
        sqlite3_bind_int64(db->update_album_by_id, 5, album_id);

        sqlite3_step(db->update_album_by_id);
        sqlite3_reset(db->update_album_by_id);
    } else {
        // Insert new album
        sqlite3_bind_text(db->insert_folder_album, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(db->insert_folder_album, 2, artist_id);
        sqlite3_bind_text(db->insert_folder_album, 3, folder_path, -1, SQLITE_STATIC);
        if (year > 0) {
            sqlite3_bind_int(db->insert_folder_album, 4, year);
        } else {
            sqlite3_bind_null(db->insert_folder_album, 4);
        }
        sqlite3_bind_int(db->insert_folder_album, 5, 0);

        int rc = sqlite3_step(db->insert_folder_album);
        if (rc != SQLITE_DONE) {
            // Capture error before any further SQLite calls can clear it
            const char* errmsg = sqlite3_errmsg(db->db);
            sqlite3_reset(db->insert_folder_album);

            /* INSERT failed — re-SELECT to find any row at this path */
            sqlite3_bind_text(db->select_album_by_path, 1, folder_path, -1, SQLITE_STATIC);
            if (sqlite3_step(db->select_album_by_path) == SQLITE_ROW)
                album_id = sqlite3_column_int64(db->select_album_by_path, 0);
            sqlite3_reset(db->select_album_by_path);
            if (album_id < 0)
                g_critical("db_upsert_folder_album: insert failed for path='%s': %s",
                           folder_path, errmsg);
        } else {
            sqlite3_reset(db->insert_folder_album);
            album_id = sqlite3_last_insert_rowid(db->db);
        }
    }

    if (album_id_out) *album_id_out = album_id;

    // Sync albums_fts (joins artists table so artist name is always current)
    if (album_id >= 0) {
        sqlite3_bind_int64(db->update_album_fts, 1, album_id);
        sqlite3_step(db->update_album_fts);
        sqlite3_reset(db->update_album_fts);
    }

    db_unlock(db);
    return (album_id >= 0) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// MusicBrainz Resolution Operations
// =============================================================================

/* Strip spaces and hyphens then casefold: "2 Mex" → "2mex", "AC-DC" → "acdc".
 * Uses g_utf8_casefold for correct Unicode handling (ö→ö not left uppercase).
 * Returns g_malloc'd string; caller must g_free(). Returns NULL on empty input. */
static char *artist_normalize(const char *name) {
    if (!name || !*name) return NULL;

    char *folded = g_utf8_casefold(name, -1);
    GString *out = g_string_new(NULL);

    /* ASCII space (0x20) and hyphen-minus (0x2D) are single-byte in UTF-8.
     * Multi-byte continuation bytes are always >= 0x80, so this loop is safe. */
    for (const char *p = folded; *p; p++) {
        if (*p != ' ' && *p != '-')
            g_string_append_c(out, *p);
    }
    g_free(folded);

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

/* Apply canonical MB name/MBID/sort_name to an existing artist row.
 * Also updates artists_fts so search stays consistent.
 * Returns SQLITE_OK on success, SQLITE_CONSTRAINT on name UNIQUE conflict. */
static int rename_artist_inplace(quadrature_db_t* db, int64_t id,
                                  const char* name, const char* mbid,
                                  const char* sort_name) {
    sqlite3_bind_text(db->rename_artist_mb, 1, name, -1, SQLITE_STATIC);
    if (mbid && *mbid)
        sqlite3_bind_text(db->rename_artist_mb, 2, mbid, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(db->rename_artist_mb, 2);
    if (sort_name && *sort_name)
        sqlite3_bind_text(db->rename_artist_mb, 3, sort_name, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(db->rename_artist_mb, 3);
    sqlite3_bind_int64(db->rename_artist_mb, 4, id);

    int step_rc = sqlite3_step(db->rename_artist_mb);
    sqlite3_reset(db->rename_artist_mb);

    if (step_rc != SQLITE_DONE)
        return sqlite3_extended_errcode(db->db);

    /* Sync FTS: INSERT OR REPLACE overwrites existing row for this rowid */
    sqlite3_bind_int64(db->insert_artist_fts_replace, 1, id);
    sqlite3_bind_text(db->insert_artist_fts_replace, 2, name, -1, SQLITE_STATIC);
    sqlite3_step(db->insert_artist_fts_replace);
    sqlite3_reset(db->insert_artist_fts_replace);
    return SQLITE_OK;
}

/* Conflict resolution: two artist rows with the same normalized name exist.
 * `old_id` was found by normalized lookup (no MBID, e.g. "2 Mex").
 * `new_name`/`new_mbid` identify the row that already has the canonical form.
 * Re-homes all track_artists from old_id → new_id, then deletes old_id.
 * Returns new_id on success, old_id if the canonical row cannot be found. */
static int64_t merge_duplicate_artist(quadrature_db_t* db, int64_t old_id,
                                       const char* new_name, const char* new_mbid) {
    /* Find the canonical row (has same name NOCASE + same MBID) */
    int64_t new_id = -1;
    sqlite3_bind_text(db->select_artist_by_name_and_mbid, 1, new_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->select_artist_by_name_and_mbid, 2, new_mbid, -1, SQLITE_STATIC);
    if (sqlite3_step(db->select_artist_by_name_and_mbid) == SQLITE_ROW)
        new_id = sqlite3_column_int64(db->select_artist_by_name_and_mbid, 0);
    sqlite3_reset(db->select_artist_by_name_and_mbid);

    if (new_id < 0) return old_id;  /* Can't locate canonical row — leave as-is */

    /* Move track_artists: UPDATE OR IGNORE avoids PK conflicts */
    sqlite3_bind_int64(db->move_track_artists, 1, new_id);
    sqlite3_bind_int64(db->move_track_artists, 2, old_id);
    sqlite3_step(db->move_track_artists);
    sqlite3_reset(db->move_track_artists);

    /* Delete any remaining track_artists for old_id (OR IGNORE left them) */
    sqlite3_bind_int64(db->delete_track_artists_artist_id, 1, old_id);
    sqlite3_step(db->delete_track_artists_artist_id);
    sqlite3_reset(db->delete_track_artists_artist_id);

    /* Delete FTS entry for old artist */
    sqlite3_bind_int64(db->delete_artist_fts, 1, old_id);
    sqlite3_step(db->delete_artist_fts);
    sqlite3_reset(db->delete_artist_fts);

    /* Delete old artist row */
    sqlite3_bind_int64(db->delete_artist, 1, old_id);
    sqlite3_step(db->delete_artist);
    sqlite3_reset(db->delete_artist);

    g_message("merge_duplicate_artist: merged id=%" G_GINT64_FORMAT " into %" G_GINT64_FORMAT
              " (\"%s\")", old_id, new_id, new_name);
    return new_id;
}

int64_t db_get_or_create_artist_mb(quadrature_db_t* db,
                                    const char* name,
                                    const char* sort_name,
                                    const char* musicbrainz_id) {
    if (!name || !*name) name = "Unknown Artist";

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    int64_t id = -1;

    // Step 1: Lookup by musicbrainz_id (O(log n) via partial unique index)
    if (musicbrainz_id && *musicbrainz_id) {
        sqlite3_bind_text(db->select_artist_by_mb_id, 1, musicbrainz_id, -1, SQLITE_STATIC);
        if (sqlite3_step(db->select_artist_by_mb_id) == SQLITE_ROW)
            id = sqlite3_column_int64(db->select_artist_by_mb_id, 0);
        sqlite3_reset(db->select_artist_by_mb_id);

        if (id >= 0) {
            /* Update sort_name only if not already set */
            if (sort_name && *sort_name) {
                sqlite3_bind_text(db->update_artist_sort_name, 1, sort_name, -1, SQLITE_STATIC);
                sqlite3_bind_int64(db->update_artist_sort_name, 2, id);
                sqlite3_step(db->update_artist_sort_name);
                sqlite3_reset(db->update_artist_sort_name);
            }
            if (need_unlock) db_unlock(db);
            return id;
        }
    }

    // Step 2: Exact name lookup (case-insensitive).
    // Only matches rows with NULL MBID or the same MBID — never steals a row
    // already claimed by a different artist.
    sqlite3_bind_text(db->select_artist_by_name_nocase, 1, name, -1, SQLITE_STATIC);
    if (musicbrainz_id && *musicbrainz_id)
        sqlite3_bind_text(db->select_artist_by_name_nocase, 2, musicbrainz_id, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(db->select_artist_by_name_nocase, 2);
    if (sqlite3_step(db->select_artist_by_name_nocase) == SQLITE_ROW)
        id = sqlite3_column_int64(db->select_artist_by_name_nocase, 0);
    sqlite3_reset(db->select_artist_by_name_nocase);

    if (id >= 0) {
        /* Rename in-place: apply canonical MB name/MBID/sort_name */
        rename_artist_inplace(db, id, name, musicbrainz_id, sort_name);
        if (need_unlock) db_unlock(db);
        return id;
    }

    // Step 3: Normalized lookup — strips spaces and hyphens so "2 Mex" matches
    // "2Mex" and "AC-DC" matches "ACDC".  Only matches rows without a MBID
    // (rows already claimed by Phase 4 are excluded via the MBID check in Step 1).
    char *normalized = artist_normalize(name);
    if (normalized) {
        sqlite3_bind_text(db->select_artist_normalized_no_mbid, 1, normalized, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(db->select_artist_normalized_no_mbid) == SQLITE_ROW)
            id = sqlite3_column_int64(db->select_artist_normalized_no_mbid, 0);
        sqlite3_reset(db->select_artist_normalized_no_mbid);
        g_free(normalized);

        if (id >= 0) {
            /* Rename in-place.  If the name being assigned already exists (because
             * a prior buggy run created both "2 Mex" and "2Mex"), detect the
             * UNIQUE constraint violation and merge the duplicate into the canonical. */
            int rc = rename_artist_inplace(db, id, name, musicbrainz_id, sort_name);
            if (rc == SQLITE_CONSTRAINT || rc == SQLITE_CONSTRAINT_UNIQUE) {
                id = merge_duplicate_artist(db, id, name, musicbrainz_id);
            }
            if (need_unlock) db_unlock(db);
            return id;
        }
    }

    // Step 4: Not found — insert new row
    sqlite3_bind_text(db->insert_artist_mb, 1, name, -1, SQLITE_STATIC);
    if (musicbrainz_id && *musicbrainz_id)
        sqlite3_bind_text(db->insert_artist_mb, 2, musicbrainz_id, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(db->insert_artist_mb, 2);
    if (sort_name && *sort_name)
        sqlite3_bind_text(db->insert_artist_mb, 3, sort_name, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(db->insert_artist_mb, 3);

    int rc = sqlite3_step(db->insert_artist_mb);
    sqlite3_reset(db->insert_artist_mb);

    if (rc == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db->db);
        sqlite3_bind_int64(db->insert_artist_fts_replace, 1, id);
        sqlite3_bind_text(db->insert_artist_fts_replace, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(db->insert_artist_fts_replace);
        sqlite3_reset(db->insert_artist_fts_replace);
    }

    if (need_unlock) db_unlock(db);
    return id;
}

quadrature_result_t db_update_album_mb(quadrature_db_t* db, int64_t album_id,
    const char* title,
    const char* musicbrainz_release_id,
    const char* musicbrainz_release_group_id,
    uint16_t year,
    int mb_status) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    int p = 1;
    if (title && *title) sqlite3_bind_text(db->update_album_mb, p++, title, -1, SQLITE_STATIC);
    else sqlite3_bind_null(db->update_album_mb, p++);
    sqlite3_bind_text(db->update_album_mb, p++, musicbrainz_release_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(db->update_album_mb, p++, musicbrainz_release_group_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(db->update_album_mb, p++, year);
    sqlite3_bind_int(db->update_album_mb, p++, year);
    sqlite3_bind_int(db->update_album_mb, p++, mb_status);
    sqlite3_bind_int64(db->update_album_mb, p++, (int64_t)time(NULL));
    sqlite3_bind_int64(db->update_album_mb, p++, album_id);

    int rc = sqlite3_step(db->update_album_mb);
    sqlite3_reset(db->update_album_mb);

    // Sync albums_fts — title/artist may have changed
    if (rc == SQLITE_DONE) {
        sqlite3_bind_int64(db->sync_album_fts, 1, album_id);
        sqlite3_step(db->sync_album_fts);
        sqlite3_reset(db->sync_album_fts);
    }

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_update_track_title(quadrature_db_t* db, int64_t track_id,
    const char* title) {
    if (!db || track_id <= 0 || !title || !*title) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    sqlite3_bind_text(db->update_track_title, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_int64(db->update_track_title, 2, track_id);
    int rc = sqlite3_step(db->update_track_title);
    sqlite3_reset(db->update_track_title);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_merge_track_genres(quadrature_db_t* db,
    int64_t track_id, const char* mb_genres) {
    if (!db || track_id <= 0 || !mb_genres || !*mb_genres)
        return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    // Read current genre for this track
    sqlite3_stmt* sel;
    sqlite3_prepare_v2(db->db, "SELECT genre FROM tracks WHERE id = ?", -1, &sel, NULL);
    sqlite3_bind_int64(sel, 1, track_id);

    const char* current = NULL;
    if (sqlite3_step(sel) == SQLITE_ROW)
        current = (const char*)sqlite3_column_text(sel, 0);

    // Build deduplicated set from existing genres
    GHashTable* seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray* ordered = g_ptr_array_new();

    // Split and add existing genres (preserving order)
    if (current && *current) {
        char** parts = g_strsplit(current, ";", -1);
        for (char** p = parts; *p; p++) {
            char* trimmed = g_strstrip(*p);
            if (*trimmed && !g_hash_table_contains(seen, trimmed)) {
                char* key = g_strdup(trimmed);
                g_hash_table_add(seen, key);
                g_ptr_array_add(ordered, key);
            }
        }
        g_strfreev(parts);
    }

    // Split and add MB genres (lowercase, dedup)
    char** mb_parts = g_strsplit(mb_genres, ";", -1);
    for (char** p = mb_parts; *p; p++) {
        char* trimmed = g_strstrip(*p);
        // Lowercase MB genre for consistency
        for (char* c = trimmed; *c; c++) *c = tolower((unsigned char)*c);
        if (*trimmed && !g_hash_table_contains(seen, trimmed)) {
            char* key = g_strdup(trimmed);
            g_hash_table_add(seen, key);
            g_ptr_array_add(ordered, key);
        }
    }
    g_strfreev(mb_parts);

    // Build merged string
    GString* merged = g_string_new(NULL);
    for (guint i = 0; i < ordered->len; i++) {
        if (i > 0) g_string_append_c(merged, ';');
        g_string_append(merged, (const char*)g_ptr_array_index(ordered, i));
    }

    // Write back
    if (merged->len > 0) {
        sqlite3_bind_text(db->update_track_genre, 1, merged->str, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(db->update_track_genre, 1);
    }
    sqlite3_bind_int64(db->update_track_genre, 2, track_id);
    int rc = sqlite3_step(db->update_track_genre);
    sqlite3_reset(db->update_track_genre);

    sqlite3_finalize(sel);
    g_ptr_array_free(ordered, TRUE);
    g_hash_table_destroy(seen);
    g_string_free(merged, TRUE);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_set_album_release_id_from_tags(quadrature_db_t* db,
    int64_t album_id, const char* musicbrainz_release_id) {
    if (!db || album_id <= 0 || !musicbrainz_release_id || !musicbrainz_release_id[0])
        return QUADRATURE_ERROR_INVALID_PARAM;
    g_assert(db->in_transaction);

    sqlite3_bind_text(db->set_album_release_id, 1, musicbrainz_release_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(db->set_album_release_id, 2, MB_STATUS_HAS_RELEASE_ID);
    sqlite3_bind_int64(db->set_album_release_id, 3, album_id);
    int rc = sqlite3_step(db->set_album_release_id);
    sqlite3_reset(db->set_album_release_id);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_set_album_release_group_id_from_tags(quadrature_db_t* db,
    int64_t album_id, const char* musicbrainz_release_group_id) {
    if (!db || album_id <= 0 || !musicbrainz_release_group_id || !musicbrainz_release_group_id[0])
        return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "UPDATE albums SET musicbrainz_release_group_id = ? "
        "WHERE id = ? AND (musicbrainz_release_group_id IS NULL OR musicbrainz_release_group_id = '') "
        "AND mb_status != 2",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, musicbrainz_release_group_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, album_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_get_unresolved_albums(quadrature_db_t* db,
    int64_t retry_no_match_before,
    int64_t** album_ids, size_t* count) {
    if (!db || !album_ids || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    *album_ids = NULL;
    *count = 0;

    db_lock(db);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM albums "
        "WHERE (mb_status IN (0, 1, 4) "
        "   OR (mb_status = 3 AND mb_resolved_at < ?)) "
        "AND path != '' ORDER BY id",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, retry_no_match_before);

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

    *album_ids = ids;
    *count = n;
    return QUADRATURE_OK;
}

quadrature_result_t db_set_album_mb_status(quadrature_db_t* db,
    int64_t album_id, int status, int64_t resolved_at) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    sqlite3_bind_int(db->set_album_mb_status, 1, status);
    sqlite3_bind_int64(db->set_album_mb_status, 2, resolved_at);
    sqlite3_bind_int64(db->set_album_mb_status, 3, album_id);
    int rc = sqlite3_step(db->set_album_mb_status);
    sqlite3_reset(db->set_album_mb_status);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_update_album_artist(quadrature_db_t* db, int64_t album_id,
    int64_t artist_id, bool is_compilation) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    sqlite3_bind_int64(db->update_album_artist, 1, artist_id);
    sqlite3_bind_int(db->update_album_artist, 2, is_compilation ? 1 : 0);
    sqlite3_bind_int64(db->update_album_artist, 3, album_id);
    int rc = sqlite3_step(db->update_album_artist);
    sqlite3_reset(db->update_album_artist);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// Track Artist Operations (multi-artist via junction table)
// =============================================================================

quadrature_result_t db_set_track_artists(quadrature_db_t* db, int64_t track_id,
                                          const db_track_artist_t* artists, size_t count) {
    if (!db || track_id <= 0 || (!artists && count > 0)) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    // Delete existing associations
    sqlite3_bind_int64(db->delete_track_artists, 1, track_id);
    sqlite3_step(db->delete_track_artists);
    sqlite3_reset(db->delete_track_artists);

    // Insert new associations and build artist_display string simultaneously.
    // artist_display = concat of name + join_phrase for each artist.
    GString* display = g_string_new(NULL);

    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_int64(db->insert_track_artist, 1, track_id);
        sqlite3_bind_int64(db->insert_track_artist, 2, artists[i].artist_id);
        sqlite3_bind_int(db->insert_track_artist, 3, artists[i].position);
        sqlite3_bind_text(db->insert_track_artist, 4,
            artists[i].join_phrase ? artists[i].join_phrase : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(db->insert_track_artist);
        sqlite3_reset(db->insert_track_artist);

        if (artists[i].name) g_string_append(display, artists[i].name);
        if (artists[i].join_phrase) g_string_append(display, artists[i].join_phrase);
    }

    // Write artist_display to tracks table
    if (display->len > 0) {
        sqlite3_bind_text(db->update_track_artist_display, 1, display->str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(db->update_track_artist_display, 2, track_id);
        sqlite3_step(db->update_track_artist_display);
        sqlite3_reset(db->update_track_artist_display);
    }

    g_string_free(display, TRUE);

    if (need_unlock) db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_sync_album_fts(quadrature_db_t* db, int64_t album_id) {
    g_assert(db && db->in_transaction);
    sqlite3_bind_int64(db->sync_album_tracks_fts, 1, album_id);
    sqlite3_step(db->sync_album_tracks_fts);
    sqlite3_reset(db->sync_album_tracks_fts);
    return QUADRATURE_OK;
}

// =============================================================================
// Indexer Error Operations (simplified path-based)
// =============================================================================

quadrature_result_t db_log_error_ex(quadrature_db_t* db, const char* path,
                                    indexer_error_code_t error_code, int phase,
                                    indexer_error_severity_t severity,
                                    const char* message, int64_t scan_generation) {
    if (!db || !path || !message) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO indexer_errors(path, error_code, phase, severity, message, scan_generation) "
        "VALUES(?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (need_unlock) db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)error_code);
    sqlite3_bind_int(stmt, 3, phase);
    sqlite3_bind_int(stmt, 4, (int)severity);
    sqlite3_bind_text(stmt, 5, message, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, scan_generation);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message,
                                int64_t scan_generation) {
    return db_log_error_ex(db, path, INDEXER_ERR_UNKNOWN, 0, INDEXER_SEV_ERROR,
                           message, scan_generation);
}

quadrature_result_t db_clear_errors_for_path(quadrature_db_t* db, const char* path_prefix) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc;
    if (path_prefix && *path_prefix) {
        rc = sqlite3_prepare_v2(db->db,
            "DELETE FROM indexer_errors WHERE path LIKE ? || '%'",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, path_prefix, -1, SQLITE_STATIC);
        }
    } else {
        rc = sqlite3_prepare_v2(db->db,
            "DELETE FROM indexer_errors",
            -1, &stmt, NULL);
    }

    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_prune_orphan_errors(quadrature_db_t* db, const char* library_root) {
    if (!db || !library_root) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "DELETE FROM indexer_errors WHERE id IN ("
        "  SELECT e.id FROM indexer_errors e"
        "  WHERE NOT EXISTS ("
        "    SELECT 1 FROM albums a WHERE a.path != ''"
        "    AND e.path LIKE ?1 || '/' || a.path || '%'"
        "  )"
        ")",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, library_root, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(stmt);

    if (changes > 0)
        g_message("db_prune_orphan_errors: removed %d orphan error(s)", changes);

    db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// Free Functions for New Types
// =============================================================================

void db_indexer_error_free(db_indexer_error_t* err) {
    if (!err) return;
    free(err->path);
    free(err->message);
    free(err);
}

void db_indexer_errors_free(db_indexer_error_t* errors, size_t count) {
    if (!errors) return;
    for (size_t i = 0; i < count; i++) {
        free(errors[i].path);
        free(errors[i].message);
    }
    free(errors);
}

// =============================================================================
// Orphan Cleanup
// =============================================================================

quadrature_result_t db_prune_orphan_artists(quadrature_db_t* db) {
    if (!db) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    char* err = NULL;
    sqlite3_exec(db->db, "BEGIN", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* An artist is "alive" if it appears in track_artists OR is an album artist.
     * Both must be checked: MusicBrainz resolve can replace track credits with
     * corrected artists, orphaning Phase 2 entries that may still be album artists. */
    sqlite3_stmt* del;
    sqlite3_prepare_v2(db->db,
        "DELETE FROM artists "
        "WHERE NOT EXISTS "
        "  (SELECT 1 FROM track_artists ta WHERE ta.artist_id = artists.id) "
        "AND NOT EXISTS "
        "  (SELECT 1 FROM albums al WHERE al.artist_id = artists.id)",
        -1, &del, NULL);
    sqlite3_step(del);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(del);

    /* Clean FTS shadow table for any deleted rows */
    sqlite3_stmt* del_fts;
    sqlite3_prepare_v2(db->db,
        "DELETE FROM artists_fts WHERE rowid NOT IN (SELECT id FROM artists)",
        -1, &del_fts, NULL);
    sqlite3_step(del_fts);
    sqlite3_finalize(del_fts);

    sqlite3_exec(db->db, "COMMIT", NULL, NULL, &err);
    if (err) sqlite3_free(err);

    if (changes > 0)
        g_message("db_prune_orphan_artists: removed %d orphan artist(s)", changes);

    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_reconcile_album_tracks(quadrature_db_t* db,
                                                int64_t album_id,
                                                const char* const* current_paths,
                                                size_t current_path_count) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    db_lock(db);

    /* Build a TEMP set of current paths. When current_path_count == 0 the
     * table stays empty, so every track for the album matches the "not in
     * current" predicate and gets pruned — that's the whole-album delete
     * flow used by Phase 1's orphan sweep. Safe to reuse across successive
     * calls within the same transaction (DELETE clears prior rows). */
    sqlite3_exec(db->db,
        "CREATE TEMP TABLE IF NOT EXISTS _current_track_paths(path TEXT PRIMARY KEY)",
        NULL, NULL, NULL);
    sqlite3_exec(db->db, "DELETE FROM _current_track_paths", NULL, NULL, NULL);

    if (current_path_count > 0 && current_paths) {
        sqlite3_stmt* ins = NULL;
        if (sqlite3_prepare_v2(db->db,
                "INSERT OR IGNORE INTO _current_track_paths(path) VALUES(?)",
                -1, &ins, NULL) != SQLITE_OK) {
            db_unlock(db);
            return QUADRATURE_ERROR_INTERNAL;
        }
        for (size_t i = 0; i < current_path_count; i++) {
            if (!current_paths[i]) continue;
            sqlite3_bind_text(ins, 1, current_paths[i], -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
    }

    /* Prune tracks not in the current set (track_artists cascades via FK). */
    sqlite3_stmt* del_tracks = NULL;
    if (sqlite3_prepare_v2(db->db,
            "DELETE FROM tracks "
            "WHERE album_id = ? "
            "  AND path NOT IN (SELECT path FROM _current_track_paths)",
            -1, &del_tracks, NULL) != SQLITE_OK) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }
    sqlite3_bind_int64(del_tracks, 1, album_id);
    sqlite3_step(del_tracks);
    int tracks_pruned = sqlite3_changes(db->db);
    sqlite3_finalize(del_tracks);

    if (tracks_pruned > 0) {
        sqlite3_exec(db->db,
            "DELETE FROM tracks_fts WHERE rowid NOT IN (SELECT id FROM tracks)",
            NULL, NULL, NULL);
    }

    /* If the album now has no tracks, delete it too. This collapses the
     * "whole album gone" and "some tracks gone" cases into one operation. */
    sqlite3_stmt* count_stmt = NULL;
    sqlite3_prepare_v2(db->db,
        "SELECT COUNT(*) FROM tracks WHERE album_id = ?",
        -1, &count_stmt, NULL);
    sqlite3_bind_int64(count_stmt, 1, album_id);
    int remaining = (sqlite3_step(count_stmt) == SQLITE_ROW)
        ? sqlite3_column_int(count_stmt, 0) : -1;
    sqlite3_finalize(count_stmt);

    int album_deleted = 0;
    if (remaining == 0) {
        sqlite3_stmt* del_album = NULL;
        sqlite3_prepare_v2(db->db,
            "DELETE FROM albums WHERE id = ?", -1, &del_album, NULL);
        sqlite3_bind_int64(del_album, 1, album_id);
        sqlite3_step(del_album);
        album_deleted = sqlite3_changes(db->db);
        sqlite3_finalize(del_album);

        if (album_deleted > 0) {
            sqlite3_exec(db->db,
                "DELETE FROM albums_fts WHERE rowid NOT IN (SELECT id FROM albums)",
                NULL, NULL, NULL);
        }
    }

    if (tracks_pruned > 0 || album_deleted > 0) {
        g_debug("db_reconcile_album_tracks: album %" G_GINT64_FORMAT
                " — pruned %d track(s)%s",
                album_id, tracks_pruned,
                album_deleted ? ", album deleted" : "");
    }

    db_unlock(db);
    return QUADRATURE_OK;
}
