#include <glib.h>
#include "db_internal.h"
#include "quadrature/database/database.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// =============================================================================
// LRU Cache Implementation
// =============================================================================

// Simple hash function for strings
static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

void db_cache_init(db_cache_t* cache) {
    memset(cache->entries, 0, sizeof(cache->entries));
    pthread_mutex_init(&cache->lock, NULL);
}

void db_cache_destroy(db_cache_t* cache) {
    for (int i = 0; i < DB_CACHE_SIZE; i++) {
        free(cache->entries[i].key);
    }
    pthread_mutex_destroy(&cache->lock);
}

int64_t db_cache_get(db_cache_t* cache, const char* key) {
    uint32_t idx = hash_string(key) % DB_CACHE_SIZE;

    pthread_mutex_lock(&cache->lock);
    db_cache_entry_t* entry = &cache->entries[idx];
    int64_t result = -1;

    if (entry->key && strcmp(entry->key, key) == 0) {
        entry->access_count++;
        result = entry->id;
    }
    pthread_mutex_unlock(&cache->lock);

    return result;
}

void db_cache_put(db_cache_t* cache, const char* key, int64_t id) {
    uint32_t idx = hash_string(key) % DB_CACHE_SIZE;

    pthread_mutex_lock(&cache->lock);
    db_cache_entry_t* entry = &cache->entries[idx];

    free(entry->key);
    entry->key = strdup(key);
    entry->id = id;
    entry->access_count = 1;
    pthread_mutex_unlock(&cache->lock);
}

void db_cache_clear(db_cache_t* cache) {
    pthread_mutex_lock(&cache->lock);
    for (int i = 0; i < DB_CACHE_SIZE; i++) {
        free(cache->entries[i].key);
        cache->entries[i].key = NULL;
        cache->entries[i].id = 0;
        cache->entries[i].access_count = 0;
    }
    pthread_mutex_unlock(&cache->lock);
}

// =============================================================================
// Get or Create Artist/Album (with caching)
// =============================================================================

int64_t db_get_or_create_artist(quadrature_db_t* db, const char* name) {
    if (!name || !*name) name = "Unknown Artist";

    // Check cache first (cache has its own lock)
    int64_t id = db_cache_get(&db->artist_cache, name);
    if (id >= 0) return id;

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

    if (id >= 0) {
        db_cache_put(&db->artist_cache, name, id);
    }

    return id;
}

int64_t db_get_or_create_album(quadrature_db_t* db, const char* title, int64_t artist_id,
                               const char* path, int year) {
    if (!title || !*title) title = "Unknown Album";

    // Build cache key: "title:artist_id"
    char cache_key[512];
    snprintf(cache_key, sizeof(cache_key), "%s:%lld", title, (long long)artist_id);

    int64_t id = db_cache_get(&db->album_cache, cache_key);
    if (id >= 0) return id;

    // Acquire lock for DB operations
    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        db_prepare_stmts(db);
        need_unlock = true;
    }

    // Try to select existing
    sqlite3_bind_text(db->select_album, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_int64(db->select_album, 2, artist_id);
    if (sqlite3_step(db->select_album) == SQLITE_ROW) {
        id = sqlite3_column_int64(db->select_album, 0);
    }
    sqlite3_reset(db->select_album);

    if (id < 0) {
        // Insert new
        sqlite3_bind_text(db->insert_album, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(db->insert_album, 2, artist_id);
        sqlite3_bind_text(db->insert_album, 3, path ? path : "", -1, SQLITE_STATIC);
        if (year > 0) {
            sqlite3_bind_int(db->insert_album, 4, year);
        } else {
            sqlite3_bind_null(db->insert_album, 4);
        }
        sqlite3_step(db->insert_album);
        sqlite3_reset(db->insert_album);

        // Get the ID
        sqlite3_bind_text(db->select_album, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(db->select_album, 2, artist_id);
        if (sqlite3_step(db->select_album) == SQLITE_ROW) {
            id = sqlite3_column_int64(db->select_album, 0);
        }
        sqlite3_reset(db->select_album);
    }

    if (need_unlock) db_unlock(db);

    if (id >= 0) {
        db_cache_put(&db->album_cache, cache_key, id);
    }

    return id;
}

// =============================================================================
// Track Write Operations
// =============================================================================

quadrature_result_t db_upsert_track(quadrature_db_t* db, const db_index_item_t* item, int64_t scan_time) {
    if (!db || !item || !item->path) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    // Get or create artist and album
    int64_t artist_id = db_get_or_create_artist(db, item->artist);

    // Extract album path from track path (directory containing the track)
    char album_path[4096] = "";
    const char* last_slash = strrchr(item->path, '/');
    if (last_slash) {
        size_t len = last_slash - item->path;
        if (len < sizeof(album_path)) {
            memcpy(album_path, item->path, len);
            album_path[len] = '\0';
        }
    }

    int64_t album_id = db_get_or_create_album(db, item->album, artist_id, album_path, item->year);

    // Check if track already exists (for FTS update)
    int64_t existing_track_id = 0;
    sqlite3_stmt* sel_stmt;
    sqlite3_prepare_v2(db->db, "SELECT id FROM tracks WHERE path = ?", -1, &sel_stmt, NULL);
    sqlite3_bind_text(sel_stmt, 1, item->path, -1, SQLITE_STATIC);
    if (sqlite3_step(sel_stmt) == SQLITE_ROW) {
        existing_track_id = sqlite3_column_int64(sel_stmt, 0);
    }
    sqlite3_finalize(sel_stmt);

    // Upsert track
    sqlite3_bind_text(db->upsert_track, 1, item->title ? item->title : "Unknown", -1, SQLITE_STATIC);
    sqlite3_bind_int64(db->upsert_track, 2, artist_id);
    sqlite3_bind_int64(db->upsert_track, 3, album_id);
    sqlite3_bind_text(db->upsert_track, 4, item->path, -1, SQLITE_STATIC);
    sqlite3_bind_int(db->upsert_track, 5, item->duration_ms);
    sqlite3_bind_int(db->upsert_track, 6, item->track_num);
    sqlite3_bind_int(db->upsert_track, 7, item->disc_num > 0 ? item->disc_num : 1);
    sqlite3_bind_int64(db->upsert_track, 8, item->mtime);
    sqlite3_bind_int64(db->upsert_track, 9, item->size);
    sqlite3_bind_int64(db->upsert_track, 10, scan_time);

    int rc = sqlite3_step(db->upsert_track);
    sqlite3_reset(db->upsert_track);

    if (rc != SQLITE_DONE) {
        g_critical("Failed to upsert track: %s", sqlite3_errmsg(db->db));
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Get track_id (use existing if it was an update, otherwise get new row id)
    int64_t track_id = existing_track_id > 0 ? existing_track_id : sqlite3_last_insert_rowid(db->db);

    // Update FTS (INSERT OR REPLACE handles both insert and update)
    sqlite3_bind_int64(db->insert_fts, 1, track_id);
    sqlite3_bind_text(db->insert_fts, 2, item->title ? item->title : "Unknown", -1, SQLITE_STATIC);
    sqlite3_step(db->insert_fts);
    sqlite3_reset(db->insert_fts);

    return QUADRATURE_OK;
}

quadrature_result_t db_upsert_track_with_album(quadrature_db_t* db, const db_index_item_t* item,
                                                int64_t album_id, int64_t scan_time) {
    if (!db || !item || !item->path) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;
    if (album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    // Get or create artist (but use provided album_id)
    int64_t artist_id = db_get_or_create_artist(db, item->artist);

    // Check if track already exists (for FTS update)
    int64_t existing_track_id = 0;
    sqlite3_stmt* sel_stmt;
    sqlite3_prepare_v2(db->db, "SELECT id FROM tracks WHERE path = ?", -1, &sel_stmt, NULL);
    sqlite3_bind_text(sel_stmt, 1, item->path, -1, SQLITE_STATIC);
    if (sqlite3_step(sel_stmt) == SQLITE_ROW) {
        existing_track_id = sqlite3_column_int64(sel_stmt, 0);
    }
    sqlite3_finalize(sel_stmt);

    // Upsert track using provided album_id
    sqlite3_bind_text(db->upsert_track, 1, item->title ? item->title : "Unknown", -1, SQLITE_STATIC);
    sqlite3_bind_int64(db->upsert_track, 2, artist_id);
    sqlite3_bind_int64(db->upsert_track, 3, album_id);
    sqlite3_bind_text(db->upsert_track, 4, item->path, -1, SQLITE_STATIC);
    sqlite3_bind_int(db->upsert_track, 5, item->duration_ms);
    sqlite3_bind_int(db->upsert_track, 6, item->track_num);
    sqlite3_bind_int(db->upsert_track, 7, item->disc_num > 0 ? item->disc_num : 1);
    sqlite3_bind_int64(db->upsert_track, 8, item->mtime);
    sqlite3_bind_int64(db->upsert_track, 9, item->size);
    sqlite3_bind_int64(db->upsert_track, 10, scan_time);

    int rc = sqlite3_step(db->upsert_track);
    sqlite3_reset(db->upsert_track);

    if (rc != SQLITE_DONE) {
        g_critical("Failed to upsert track: %s", sqlite3_errmsg(db->db));
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Get track_id (use existing if it was an update, otherwise get new row id)
    int64_t track_id = existing_track_id > 0 ? existing_track_id : sqlite3_last_insert_rowid(db->db);

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
        need_unlock = true;
    }

    // Try to find existing album by folder path first
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT id FROM albums WHERE path = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (need_unlock) db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, folder_path, -1, SQLITE_STATIC);

    int64_t album_id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        album_id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (album_id >= 0) {
        // Update existing album
        rc = sqlite3_prepare_v2(db->db,
            "UPDATE albums SET title = ?, artist_id = ?, album_artist_id = ?, "
            "is_compilation = ?, year = ? WHERE id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            if (need_unlock) db_unlock(db);
            return QUADRATURE_ERROR_INTERNAL;
        }

        sqlite3_bind_text(stmt, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, artist_id);
        if (album_artist_id > 0) {
            sqlite3_bind_int64(stmt, 3, album_artist_id);
        } else {
            sqlite3_bind_null(stmt, 3);
        }
        sqlite3_bind_int(stmt, 4, is_compilation ? 1 : 0);
        if (year > 0) {
            sqlite3_bind_int(stmt, 5, year);
        } else {
            sqlite3_bind_null(stmt, 5);
        }
        sqlite3_bind_int64(stmt, 6, album_id);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    } else {
        // Insert new album
        rc = sqlite3_prepare_v2(db->db,
            "INSERT INTO albums(title, artist_id, path, year, is_compilation, album_artist_id) "
            "VALUES(?, ?, ?, ?, ?, ?)",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            if (need_unlock) db_unlock(db);
            return QUADRATURE_ERROR_INTERNAL;
        }

        sqlite3_bind_text(stmt, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, artist_id);
        sqlite3_bind_text(stmt, 3, folder_path, -1, SQLITE_STATIC);
        if (year > 0) {
            sqlite3_bind_int(stmt, 4, year);
        } else {
            sqlite3_bind_null(stmt, 4);
        }
        sqlite3_bind_int(stmt, 5, is_compilation ? 1 : 0);
        if (album_artist_id > 0) {
            sqlite3_bind_int64(stmt, 6, album_artist_id);
        } else {
            sqlite3_bind_null(stmt, 6);
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

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
// Track Metadata Operations
// =============================================================================

quadrature_result_t db_insert_track_metadata(quadrature_db_t* db,
                                              int64_t track_id,
                                              const db_track_metadata_t* meta) {
    if (!db || track_id < 0 || !meta) return QUADRATURE_ERROR_INVALID_PARAM;

    bool need_unlock = false;
    if (!db->in_transaction) {
        db_lock(db);
        need_unlock = true;
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO track_metadata("
        "track_id, raw_json, bitrate, sample_rate, channels, codec, "
        "album_artist, genre, comment, compilation, disc_total, track_total, has_embedded_art"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (need_unlock) db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    sqlite3_bind_int64(stmt, 1, track_id);
    sqlite3_bind_text(stmt, 2, meta->raw_json ? meta->raw_json : "{}", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, meta->bitrate);
    sqlite3_bind_int(stmt, 4, meta->sample_rate);
    sqlite3_bind_int(stmt, 5, meta->channels);
    sqlite3_bind_text(stmt, 6, meta->codec ? meta->codec : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, meta->album_artist ? meta->album_artist : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, meta->genre ? meta->genre : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, meta->comment ? meta->comment : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 10, meta->compilation ? 1 : 0);
    sqlite3_bind_int(stmt, 11, meta->disc_total);
    sqlite3_bind_int(stmt, 12, meta->track_total);
    sqlite3_bind_int(stmt, 13, meta->has_embedded_art ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (need_unlock) db_unlock(db);
    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
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

void db_track_metadata_free(db_track_metadata_t* meta) {
    if (!meta) return;
    free(meta->raw_json);
    free(meta->codec);
    free(meta->album_artist);
    free(meta->genre);
    free(meta->comment);
    free(meta);
}

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
