#include <glib.h>
#include "db_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// =============================================================================
// Get or Create Artist
// =============================================================================

int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name) {
    if (!name || !*name) name = "Unknown Artist";

    int64_t id = -1;

    // Acquire lock for DB operations
    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

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
    }

    if (need_unlock) db_unlock(db);

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
    sqlite3_bind_text(db->select_track_by_path, 1, item->path, -1, SQLITE_STATIC);
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
    sqlite3_bind_text(db->upsert_track, 10, item->metadata_json ? item->metadata_json : "{}", -1, SQLITE_STATIC);

    int rc = sqlite3_step(db->upsert_track);
    sqlite3_reset(db->upsert_track);

    if (rc != SQLITE_DONE) {
        g_critical("Failed to upsert track: %s", sqlite3_errmsg(db->db));
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Get track_id (use existing if it was an update, otherwise get new row id)
    int64_t track_id = existing_track_id > 0 ? existing_track_id : sqlite3_last_insert_rowid(db->db);

    if (track_id_out) *track_id_out = track_id;

    // Update FTS (INSERT OR REPLACE handles both insert and update)
    sqlite3_bind_int64(db->insert_fts, 1, track_id);
    sqlite3_bind_text(db->insert_fts, 2, item->title ? item->title : "Unknown", -1, SQLITE_STATIC);
    sqlite3_step(db->insert_fts);
    sqlite3_reset(db->insert_fts);

    return QUADRATURE_OK;
}

quadrature_result_t db_delete_track(quadrature_db_t* db, const char* path) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    // Get track ID first
    sqlite3_stmt* sel_stmt;
    sqlite3_prepare_v2(db->db, "SELECT id FROM tracks WHERE path = ?", -1, &sel_stmt, NULL);
    sqlite3_bind_text(sel_stmt, 1, path, -1, SQLITE_STATIC);

    int64_t track_id = 0;
    if (sqlite3_step(sel_stmt) == SQLITE_ROW) {
        track_id = sqlite3_column_int64(sel_stmt, 0);
    }
    sqlite3_finalize(sel_stmt);

    if (track_id > 0) {
        // Delete from FTS
        sqlite3_bind_int64(db->delete_fts, 1, track_id);
        sqlite3_step(db->delete_fts);
        sqlite3_reset(db->delete_fts);

        // Delete from tracks
        sqlite3_bind_text(db->delete_track, 1, path, -1, SQLITE_STATIC);
        sqlite3_step(db->delete_track);
        sqlite3_reset(db->delete_track);
    }

    if (need_unlock) db_unlock(db);
    return QUADRATURE_OK;
}

// =============================================================================
// Watch Path Write Operations
// =============================================================================

quadrature_result_t db_add_watch_path(quadrature_db_t* db, const char* path) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "INSERT OR IGNORE INTO watch_paths(path, enabled) VALUES(?, 1)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_remove_watch_path(quadrature_db_t* db, const char* path) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "DELETE FROM watch_paths WHERE path = ?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_set_watch_path_enabled(quadrature_db_t* db, const char* path, bool enabled) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE watch_paths SET enabled = ? WHERE path = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_update_watch_path_scanned(quadrature_db_t* db, const char* path, int64_t timestamp) {
    if (!db || !path) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE watch_paths SET last_scanned = ? WHERE path = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, timestamp);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

// =============================================================================
// Album Mtime Write Operations (batch only)
// =============================================================================

quadrature_result_t db_set_album_mtimes_batch(quadrature_db_t* db,
                                               const int64_t* album_ids,
                                               const int64_t* mtimes,
                                               size_t count) {
    if (!db || !album_ids || !mtimes || count == 0) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    char* err = NULL;
    sqlite3_exec(db->db, "BEGIN", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET last_updated_at = ? WHERE id = ?",
        -1, &stmt, NULL);

    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_int64(stmt, 1, mtimes[i]);
        sqlite3_bind_int64(stmt, 2, album_ids[i]);
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
                                            int64_t album_artist_id,
                                            bool is_compilation,
                                            uint16_t year,
                                            int64_t* album_id_out) {
    if (!db || !folder_path || !title) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    // Try to find existing album by folder path first
    sqlite3_bind_text(db->select_album_by_path, 1, folder_path, -1, SQLITE_STATIC);

    int64_t album_id = -1;
    if (sqlite3_step(db->select_album_by_path) == SQLITE_ROW) {
        album_id = sqlite3_column_int64(db->select_album_by_path, 0);
    }
    sqlite3_reset(db->select_album_by_path);

    if (album_id >= 0) {
        // Update existing album
        sqlite3_bind_text(db->update_album_by_id, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(db->update_album_by_id, 2, artist_id);
        if (album_artist_id > 0) {
            sqlite3_bind_int64(db->update_album_by_id, 3, album_artist_id);
        } else {
            sqlite3_bind_null(db->update_album_by_id, 3);
        }
        sqlite3_bind_int(db->update_album_by_id, 4, is_compilation ? 1 : 0);
        if (year > 0) {
            sqlite3_bind_int(db->update_album_by_id, 5, year);
        } else {
            sqlite3_bind_null(db->update_album_by_id, 5);
        }
        sqlite3_bind_int64(db->update_album_by_id, 6, album_id);

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
        sqlite3_bind_int(db->insert_folder_album, 5, is_compilation ? 1 : 0);
        if (album_artist_id > 0) {
            sqlite3_bind_int64(db->insert_folder_album, 6, album_artist_id);
        } else {
            sqlite3_bind_null(db->insert_folder_album, 6);
        }

        int rc = sqlite3_step(db->insert_folder_album);
        sqlite3_reset(db->insert_folder_album);

        if (rc == SQLITE_DONE) {
            album_id = sqlite3_last_insert_rowid(db->db);
        }
    }

    if (album_id_out) *album_id_out = album_id;

    if (need_unlock) db_unlock(db);
    return (album_id >= 0) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

int64_t db_get_track_id_by_path(quadrature_db_t* db, const char* path) {
    if (!db || !path) return 0;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM tracks WHERE path = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    int64_t track_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        track_id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return track_id;
}


// =============================================================================
// Fingerprint Cache Operations
// =============================================================================

quadrature_result_t db_set_track_fingerprint(quadrature_db_t* db, int64_t track_id,
                                              const char* chromaprint, int duration) {
    if (!db || track_id <= 0 || !chromaprint) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE tracks SET chromaprint = ?, chromaprint_duration = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, chromaprint, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, duration);
    sqlite3_bind_int64(stmt, 3, track_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_get_track_fingerprint(quadrature_db_t* db, int64_t track_id,
                                              char** chromaprint_out, int* duration_out) {
    if (!db || track_id <= 0 || !chromaprint_out || !duration_out) return QUADRATURE_ERROR_INVALID_PARAM;

    *chromaprint_out = NULL;
    *duration_out = 0;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT chromaprint, chromaprint_duration FROM tracks WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, track_id);

    quadrature_result_t res = QUADRATURE_ERROR_FILE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* fp = (const char*)sqlite3_column_text(stmt, 0);
        if (fp && fp[0]) {
            *chromaprint_out = g_strdup(fp);
            *duration_out = sqlite3_column_int(stmt, 1);
            res = QUADRATURE_OK;
        }
    }
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return res;
}

// =============================================================================
// MusicBrainz Resolution Operations
// =============================================================================

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

    // 1. Look up by musicbrainz_id first (if provided)
    if (musicbrainz_id && *musicbrainz_id) {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db->db,
            "SELECT id FROM artists WHERE musicbrainz_id = ?",
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, musicbrainz_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);

        if (id >= 0) {
            // Update sort_name if provided
            if (sort_name && *sort_name) {
                sqlite3_stmt* upd;
                sqlite3_prepare_v2(db->db,
                    "UPDATE artists SET sort_name = ? WHERE id = ? AND (sort_name IS NULL OR sort_name = '')",
                    -1, &upd, NULL);
                sqlite3_bind_text(upd, 1, sort_name, -1, SQLITE_STATIC);
                sqlite3_bind_int64(upd, 2, id);
                sqlite3_step(upd);
                sqlite3_finalize(upd);
            }
            if (need_unlock) db_unlock(db);
            return id;
        }
    }

    // 2. Look up by name (case-insensitive)
    sqlite3_stmt* sel;
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM artists WHERE name = ? COLLATE NOCASE",
        -1, &sel, NULL);
    sqlite3_bind_text(sel, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(sel) == SQLITE_ROW) {
        id = sqlite3_column_int64(sel, 0);
    }
    sqlite3_finalize(sel);

    if (id >= 0) {
        // Found by name — update with MB data
        sqlite3_stmt* upd;
        sqlite3_prepare_v2(db->db,
            "UPDATE artists SET musicbrainz_id = ?, sort_name = ? WHERE id = ?",
            -1, &upd, NULL);
        if (musicbrainz_id && *musicbrainz_id)
            sqlite3_bind_text(upd, 1, musicbrainz_id, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(upd, 1);
        if (sort_name && *sort_name)
            sqlite3_bind_text(upd, 2, sort_name, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(upd, 2);
        sqlite3_bind_int64(upd, 3, id);
        sqlite3_step(upd);
        sqlite3_finalize(upd);

        if (need_unlock) db_unlock(db);
        return id;
    }

    // 3. Not found — insert new
    sqlite3_stmt* ins;
    sqlite3_prepare_v2(db->db,
        "INSERT INTO artists(name, musicbrainz_id, sort_name) VALUES(?, ?, ?)",
        -1, &ins, NULL);
    sqlite3_bind_text(ins, 1, name, -1, SQLITE_STATIC);
    if (musicbrainz_id && *musicbrainz_id)
        sqlite3_bind_text(ins, 2, musicbrainz_id, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 2);
    if (sort_name && *sort_name)
        sqlite3_bind_text(ins, 3, sort_name, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(ins, 3);

    int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);

    if (rc == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db->db);
    }

    if (need_unlock) db_unlock(db);

    return id;
}

quadrature_result_t db_update_album_mb(quadrature_db_t* db, int64_t album_id,
    const char* musicbrainz_release_id,
    const char* musicbrainz_release_group_id,
    const char* release_type,
    const char* label,
    const char* barcode,
    uint16_t year,
    int mb_status) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET "
        "musicbrainz_release_id = ?, "
        "musicbrainz_release_group_id = ?, "
        "release_type = ?, "
        "label = ?, "
        "barcode = ?, "
        "year = CASE WHEN ? > 0 THEN ? ELSE year END, "
        "mb_status = ?, "
        "mb_resolved_at = ? "
        "WHERE id = ?",
        -1, &stmt, NULL);

    int p = 1;
    sqlite3_bind_text(stmt, p++, musicbrainz_release_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, p++, musicbrainz_release_group_id, -1, SQLITE_STATIC);
    if (release_type) sqlite3_bind_text(stmt, p++, release_type, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, p++);
    if (label) sqlite3_bind_text(stmt, p++, label, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, p++);
    if (barcode) sqlite3_bind_text(stmt, p++, barcode, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, p++);
    sqlite3_bind_int(stmt, p++, year);
    sqlite3_bind_int(stmt, p++, year);
    sqlite3_bind_int(stmt, p++, mb_status);
    sqlite3_bind_int64(stmt, p++, (int64_t)time(NULL));
    sqlite3_bind_int64(stmt, p++, album_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_update_track_mb(quadrature_db_t* db, int64_t track_id,
    const char* musicbrainz_recording_id,
    const char* title) {
    if (!db || track_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    if (title && *title) {
        sqlite3_prepare_v2(db->db,
            "UPDATE tracks SET musicbrainz_recording_id = ?, title = ? WHERE id = ?",
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, musicbrainz_recording_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, track_id);
    } else {
        sqlite3_prepare_v2(db->db,
            "UPDATE tracks SET musicbrainz_recording_id = ? WHERE id = ?",
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, musicbrainz_recording_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, track_id);
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Update FTS if title changed
    if (title && *title && rc == SQLITE_DONE) {
        sqlite3_stmt* fts;
        sqlite3_prepare_v2(db->db,
            "INSERT OR REPLACE INTO tracks_fts(rowid, title) VALUES(?, ?)",
            -1, &fts, NULL);
        sqlite3_bind_int64(fts, 1, track_id);
        sqlite3_bind_text(fts, 2, title, -1, SQLITE_STATIC);
        sqlite3_step(fts);
        sqlite3_finalize(fts);
    }

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_get_unresolved_albums(quadrature_db_t* db,
    int64_t** album_ids, size_t* count) {
    if (!db || !album_ids || !count) return QUADRATURE_ERROR_INVALID_PARAM;

    *album_ids = NULL;
    *count = 0;

    db_lock(db);

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "SELECT id FROM albums WHERE mb_status = 0 AND path != '' ORDER BY id",
        -1, &stmt, NULL);

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
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET mb_status = ?, mb_resolved_at = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, status);
    sqlite3_bind_int64(stmt, 2, resolved_at);
    sqlite3_bind_int64(stmt, 3, album_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}

quadrature_result_t db_update_album_artist(quadrature_db_t* db, int64_t album_id,
    int64_t artist_id, int64_t album_artist_id, bool is_compilation) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db->db,
        "UPDATE albums SET artist_id = ?, album_artist_id = ?, is_compilation = ? WHERE id = ?",
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, artist_id);
    sqlite3_bind_int64(stmt, 2, album_artist_id);
    sqlite3_bind_int(stmt, 3, is_compilation ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, album_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

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

    // Insert new associations
    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_int64(db->insert_track_artist, 1, track_id);
        sqlite3_bind_int64(db->insert_track_artist, 2, artists[i].artist_id);
        sqlite3_bind_int(db->insert_track_artist, 3, artists[i].role);
        sqlite3_bind_int(db->insert_track_artist, 4, artists[i].position);
        sqlite3_step(db->insert_track_artist);
        sqlite3_reset(db->insert_track_artist);
    }

    if (need_unlock) db_unlock(db);
    return QUADRATURE_OK;
}

// =============================================================================
// Indexer Error Operations (simplified path-based)
// =============================================================================

quadrature_result_t db_log_error(quadrature_db_t* db, const char* path, const char* message) {
    if (!db || !path || !message) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO indexer_errors(path, message) VALUES(?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (need_unlock) db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, message, -1, SQLITE_STATIC);
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
