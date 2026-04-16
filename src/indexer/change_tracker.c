/**
 * ChangeTracker — tracks DB row mutations via sqlite3_update_hook.
 *
 * Registered on the indexer's writer connection; every committed
 * INSERT/UPDATE/DELETE on `artists`, `albums`, `tracks`, `track_artists`
 * is recorded. Drained into a library_cache_changeset_t just before the
 * indexer emits INDEXER_LIBRARY_UPDATED so the COW refresh knows exactly
 * which entities to re-read from DB.
 *
 * Thread-safety: the hook fires on whatever thread holds the sqlite3_step
 * that performed the mutation. For the indexer that's usually the worker
 * thread, but Phase 2 uses a thread pool and Phase 6 runs batched queries.
 * All mutations of the internal GHashTables are serialised on ct->lock.
 *
 * Contract (see LIBRARY_CACHE.md → "COW Refresh Invariants"):
 *   - The tracker may over-report (extra rowids in the set is fine).
 *   - The tracker may under-report: the COW refresh callbacks are
 *     authoritative, so missing rowids only cost an allocation, not
 *     correctness.
 *   - track_artists is tracked as a boolean flag, not per-row, because
 *     the hook gives us the junction-table rowid, not a track_id.
 */

#include "internal.h"
#include "../database/internal.h"

#include <glib.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct change_tracker {
    quadrature_db_t *db;        /* Non-owning. */

    GMutex       lock;
    GHashTable  *artists;       /* int64_t rowid → self (set semantics) */
    GHashTable  *albums;
    GHashTable  *tracks;
    gboolean     track_artists_dirty;
};

/* GHashTable key helpers for int64 sets. Keys are malloc'd int64_t. */
static guint int64_ptr_hash(gconstpointer p) {
    return g_int64_hash(p);
}
static gboolean int64_ptr_equal(gconstpointer a, gconstpointer b) {
    return g_int64_equal(a, b);
}

/* Insert rowid into set iff not already present. */
static void set_add(GHashTable *set, sqlite3_int64 rowid) {
    int64_t v = (int64_t)rowid;
    if (g_hash_table_contains(set, &v)) return;
    int64_t *key = g_new(int64_t, 1);
    *key = v;
    g_hash_table_add(set, key);
}

/* sqlite3_update_hook callback. op is SQLITE_INSERT / SQLITE_UPDATE / SQLITE_DELETE. */
static void update_hook(void *user_data,
                        int op,
                        const char *db_name,
                        const char *table_name,
                        sqlite3_int64 rowid) {
    (void)op;
    (void)db_name;
    change_tracker_t *ct = user_data;
    if (!ct || !table_name) return;

    g_mutex_lock(&ct->lock);
    if (strcmp(table_name, "artists") == 0) {
        set_add(ct->artists, rowid);
    } else if (strcmp(table_name, "albums") == 0) {
        set_add(ct->albums, rowid);
    } else if (strcmp(table_name, "tracks") == 0) {
        set_add(ct->tracks, rowid);
    } else if (strcmp(table_name, "track_artists") == 0) {
        ct->track_artists_dirty = TRUE;
    }
    /* Other tables (FTS shadow tables, errors, album_mtimes,
     * album_fingerprints, …) are intentionally ignored — none of them back
     * library_cache entities. */
    g_mutex_unlock(&ct->lock);
}

change_tracker_t *change_tracker_new(quadrature_db_t *db) {
    g_return_val_if_fail(db != NULL, NULL);
    g_return_val_if_fail(db->db != NULL, NULL);

    change_tracker_t *ct = g_new0(change_tracker_t, 1);
    ct->db = db;
    g_mutex_init(&ct->lock);
    ct->artists = g_hash_table_new_full(int64_ptr_hash, int64_ptr_equal, g_free, NULL);
    ct->albums  = g_hash_table_new_full(int64_ptr_hash, int64_ptr_equal, g_free, NULL);
    ct->tracks  = g_hash_table_new_full(int64_ptr_hash, int64_ptr_equal, g_free, NULL);
    ct->track_artists_dirty = FALSE;

    /* Register the hook. Returns the previous user_data (we don't chain). */
    sqlite3_update_hook(db->db, update_hook, ct);
    return ct;
}

void change_tracker_destroy(change_tracker_t *ct) {
    if (!ct) return;
    /* Unregister the hook before tearing down state (the hook dereferences ct). */
    if (ct->db && ct->db->db)
        sqlite3_update_hook(ct->db->db, NULL, NULL);

    g_hash_table_destroy(ct->artists);
    g_hash_table_destroy(ct->albums);
    g_hash_table_destroy(ct->tracks);
    g_mutex_clear(&ct->lock);
    g_free(ct);
}

/* Drain a hashset into a freshly-allocated int64 array. */
static int64_t *drain_set(GHashTable *set, size_t *out_count) {
    guint n = g_hash_table_size(set);
    if (n == 0) {
        *out_count = 0;
        g_hash_table_remove_all(set);
        return NULL;
    }
    int64_t *arr = g_new(int64_t, n);
    GHashTableIter iter;
    gpointer key;
    guint i = 0;
    g_hash_table_iter_init(&iter, set);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        arr[i++] = *(int64_t *)key;
    }
    g_hash_table_remove_all(set);
    *out_count = (size_t)n;
    return arr;
}

library_cache_changeset_t *change_tracker_snapshot_and_clear(change_tracker_t *ct) {
    if (!ct) return NULL;

    library_cache_changeset_t *cs = library_cache_changeset_new();

    g_mutex_lock(&ct->lock);
    cs->artists = drain_set(ct->artists, &cs->artists_count);
    cs->albums  = drain_set(ct->albums,  &cs->albums_count);
    cs->tracks  = drain_set(ct->tracks,  &cs->tracks_count);
    cs->track_artists_dirty = ct->track_artists_dirty;
    ct->track_artists_dirty = FALSE;
    g_mutex_unlock(&ct->lock);

    return cs;
}

void change_tracker_reset(change_tracker_t *ct) {
    if (!ct) return;
    g_mutex_lock(&ct->lock);
    g_hash_table_remove_all(ct->artists);
    g_hash_table_remove_all(ct->albums);
    g_hash_table_remove_all(ct->tracks);
    ct->track_artists_dirty = FALSE;
    g_mutex_unlock(&ct->lock);
}
