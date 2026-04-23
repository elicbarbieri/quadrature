#include <glib.h>
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// =============================================================================
// Get or Create Artist
// =============================================================================

/* Fast path: name-only lookup/insert. Used when sort_name and mbid are NULL. */
static int64_t artist_get_or_create_plain(quadrature_db_t* db, const char* name) {
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

int64_t db_get_or_create_artist(quadrature_db_t* db,
                                 const char* name,
                                 const char* sort_name,
                                 const char* musicbrainz_id) {
    /* Fast path when caller has no MB data — simpler stmts, fewer lookups. */
    bool has_mb = (sort_name && *sort_name) || (musicbrainz_id && *musicbrainz_id);
    if (!has_mb) return artist_get_or_create_plain(db, name);

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


// =============================================================================
// Indexer Error Operations (simplified path-based)
// =============================================================================

quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message,
                                int64_t scan_generation) {
    if (!db || !path || !message) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO indexer_errors(path, message, scan_generation) VALUES(?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (need_unlock) db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, message, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, scan_generation);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
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

/* db_reconcile_album_tracks was removed. Use db_reconcile_album (with
 * prune_missing_tracks=true) for per-album reconciliation or db_delete_album
 * for whole-album teardown. Both live in src/database/reconciler.c. */
