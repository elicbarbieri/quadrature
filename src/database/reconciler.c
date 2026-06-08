/**
 * Album State Reconciler — the single writer for album/track updates.
 *
 * See include/quadrature/indexer.h for the contract.
 *
 * Architecture (post-rewrite):
 *   - Batch API: db_reconcile_albums(ids[], desireds[], count). Single-album
 *     callers pass count=1.
 *   - Three bulk SELECTs via json_each(?) load current albums, tracks, and
 *     track_artists for the whole batch in one transaction pass each.
 *   - All writes use cached prepared statements on quadrature_db_t — no
 *     sqlite3_prepare_v2 on the hot path.
 *   - Canonical UPDATEs: one statement per table, always binds every column
 *     (pass-through binds current value for fields not in present_fields).
 *     SQLite's row storage is row-level COW, so binding unchanged columns has
 *     no page-write cost beyond the single row rewrite we'd do anyway.
 *   - mb_status-only updates (failure/no-match) bypass the reconciler entirely
 *     via db_reconcile_album_mb_status → one cached UPDATE, no diff.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/indexer.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Preset policies
 * ========================================================================== */

const reconcile_policy_t RECONCILE_POLICY_MB = {
    .prune_missing_tracks       = false,
    .mb_position_min_confidence = RECONCILE_CONFIDENCE_FUZZY,
    .respect_user_edits         = false,
};

const reconcile_policy_t RECONCILE_POLICY_TAGS = {
    .prune_missing_tracks       = true,
    .mb_position_min_confidence = RECONCILE_CONFIDENCE_NONE,
    .respect_user_edits         = true,
};

/* ============================================================================
 * Current-state snapshot types
 * ========================================================================== */

typedef struct {
    int64_t  id;
    int64_t  album_id;
    char*    path;
    char*    title;
    uint16_t track_num;
    uint16_t disc_num;
    uint32_t duration_ms;
    uint16_t year;
    char*    genre;
    char*    artist_display;
    int64_t  mtime;

    /* Artist credits, position-ordered. Populated eagerly by batch loader. */
    GPtrArray* artists;   /* of db_track_artist_t*, owned */
} current_track_t;

typedef struct {
    int64_t  id;
    char*    path;
    char*    title;
    int64_t  artist_id;
    bool     is_compilation;
    uint16_t year;
    char*    mb_release_id;
    char*    mb_release_group_id;
    int      mb_status;
    int64_t  mb_resolved_at;
} current_album_t;

static void current_album_free(gpointer p) {
    current_album_t* a = p;
    if (!a) return;
    g_free(a->path);
    g_free(a->title);
    g_free(a->mb_release_id);
    g_free(a->mb_release_group_id);
    g_free(a);
}

static void current_track_free(gpointer p) {
    current_track_t* t = p;
    if (!t) return;
    g_free(t->path);
    g_free(t->title);
    g_free(t->genre);
    g_free(t->artist_display);
    if (t->artists) g_ptr_array_unref(t->artists);
    g_free(t);
}

static void track_artist_free_gp(gpointer p) {
    db_track_artist_t* a = p;
    if (!a) return;
    g_free(a->name);
    g_free(a->join_phrase);
    g_free(a);
}

/* Per-album bucket of loaded tracks, keyed by path for O(1) match against
 * desired_track_t::path. */
typedef struct {
    GHashTable* by_path;   /* char* path → current_track_t* (track owned by tracks_by_id) */
} album_tracks_t;

static void album_tracks_free(gpointer p) {
    album_tracks_t* b = p;
    if (!b) return;
    if (b->by_path) g_hash_table_destroy(b->by_path);
    g_free(b);
}

typedef struct {
    /* int64_t id → current_album_t*, owned */
    GHashTable* albums;
    /* int64_t album_id → album_tracks_t*, owned (bucket only; tracks owned by tracks_by_id) */
    GHashTable* tracks_by_album;
    /* int64_t track_id → current_track_t*, owned (authoritative ownership) */
    GHashTable* tracks_by_id;
} reconcile_snapshot_t;

/* ============================================================================
 * Batch loading via json_each
 * ========================================================================== */

static char* json_array_from_ids(const int64_t* ids, size_t n) {
    GString* s = g_string_sized_new(n * 12);
    g_string_append_c(s, '[');
    for (size_t i = 0; i < n; i++) {
        if (i > 0) g_string_append_c(s, ',');
        g_string_append_printf(s, "%" G_GINT64_FORMAT, ids[i]);
    }
    g_string_append_c(s, ']');
    return g_string_free(s, FALSE);
}

static void snapshot_init(reconcile_snapshot_t* s) {
    s->albums          = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, current_album_free);
    s->tracks_by_album = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, album_tracks_free);
    s->tracks_by_id    = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, current_track_free);
}

static void snapshot_free(reconcile_snapshot_t* s) {
    if (s->albums)          g_hash_table_destroy(s->albums);
    if (s->tracks_by_album) g_hash_table_destroy(s->tracks_by_album);
    if (s->tracks_by_id)    g_hash_table_destroy(s->tracks_by_id);
    s->albums = s->tracks_by_album = s->tracks_by_id = NULL;
}

static int64_t* int64_dup(int64_t v) {
    int64_t* p = g_new(int64_t, 1);
    *p = v;
    return p;
}

static void snapshot_load(quadrature_db_t* db,
                           const int64_t* album_ids, size_t n,
                           reconcile_snapshot_t* snap) {
    snapshot_init(snap);
    if (n == 0) return;

    char* album_json = json_array_from_ids(album_ids, n);

    /* --- Albums --- */
    sqlite3_stmt* s = db->reconcile_load_albums_batch;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, album_json, -1, SQLITE_STATIC);
    while (sqlite3_step(s) == SQLITE_ROW) {
        current_album_t* a = g_new0(current_album_t, 1);
        a->id                  = sqlite3_column_int64(s, 0);
        const char* p          = (const char*)sqlite3_column_text(s, 1);
        const char* t          = (const char*)sqlite3_column_text(s, 2);
        a->path                = p ? g_strdup(p) : NULL;
        a->title               = t ? g_strdup(t) : NULL;
        a->artist_id           = sqlite3_column_int64(s, 3);
        a->is_compilation      = sqlite3_column_int(s, 4) != 0;
        a->year                = (uint16_t)sqlite3_column_int(s, 5);
        const char* rid        = (const char*)sqlite3_column_text(s, 6);
        const char* rgid       = (const char*)sqlite3_column_text(s, 7);
        a->mb_release_id       = rid  ? g_strdup(rid)  : NULL;
        a->mb_release_group_id = rgid ? g_strdup(rgid) : NULL;
        a->mb_status           = sqlite3_column_int(s, 8);
        a->mb_resolved_at      = sqlite3_column_int64(s, 9);
        g_hash_table_insert(snap->albums, int64_dup(a->id), a);
    }
    sqlite3_reset(s);

    /* --- Tracks --- */
    s = db->reconcile_load_tracks_batch;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, album_json, -1, SQLITE_STATIC);
    while (sqlite3_step(s) == SQLITE_ROW) {
        current_track_t* t = g_new0(current_track_t, 1);
        t->id                = sqlite3_column_int64(s, 0);
        t->album_id          = sqlite3_column_int64(s, 1);
        const char* path     = (const char*)sqlite3_column_text(s, 2);
        const char* title    = (const char*)sqlite3_column_text(s, 3);
        t->path              = path  ? g_strdup(path)  : g_strdup("");
        t->title             = title ? g_strdup(title) : NULL;
        t->track_num         = (uint16_t)sqlite3_column_int(s, 4);
        t->disc_num          = (uint16_t)sqlite3_column_int(s, 5);
        t->duration_ms       = (uint32_t)sqlite3_column_int(s, 6);
        t->year              = (uint16_t)sqlite3_column_int(s, 7);
        const char* genre    = (const char*)sqlite3_column_text(s, 8);
        const char* adisplay = (const char*)sqlite3_column_text(s, 9);
        t->genre             = genre    ? g_strdup(genre)    : NULL;
        t->artist_display    = adisplay ? g_strdup(adisplay) : NULL;
        t->mtime             = sqlite3_column_int64(s, 10);
        t->artists           = g_ptr_array_new_with_free_func(track_artist_free_gp);

        g_hash_table_insert(snap->tracks_by_id, int64_dup(t->id), t);

        album_tracks_t* bucket = g_hash_table_lookup(snap->tracks_by_album, &t->album_id);
        if (!bucket) {
            bucket = g_new0(album_tracks_t, 1);
            bucket->by_path = g_hash_table_new(g_str_hash, g_str_equal);  /* keys owned by current_track_t */
            g_hash_table_insert(snap->tracks_by_album, int64_dup(t->album_id), bucket);
        }
        g_hash_table_insert(bucket->by_path, t->path, t);
    }
    sqlite3_reset(s);

    g_free(album_json);

    /* --- Track artists (one query covering all tracks in batch) --- */
    guint track_count = g_hash_table_size(snap->tracks_by_id);
    if (track_count == 0) return;

    /* Collect track ids into an array for JSON bulk bind. */
    int64_t* track_ids = g_new(int64_t, track_count);
    size_t tidx = 0;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, snap->tracks_by_id);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        (void)v;
        track_ids[tidx++] = *(int64_t*)k;
    }

    char* track_json = json_array_from_ids(track_ids, track_count);
    g_free(track_ids);

    s = db->reconcile_load_track_artists_batch;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, track_json, -1, SQLITE_STATIC);
    while (sqlite3_step(s) == SQLITE_ROW) {
        int64_t tid = sqlite3_column_int64(s, 0);
        current_track_t* t = g_hash_table_lookup(snap->tracks_by_id, &tid);
        if (!t) continue;  /* defensive; should not happen */

        db_track_artist_t* a = g_new0(db_track_artist_t, 1);
        a->artist_id   = sqlite3_column_int64(s, 1);
        const char* nm = (const char*)sqlite3_column_text(s, 2);
        const char* jp = (const char*)sqlite3_column_text(s, 3);
        a->name        = nm ? g_strdup(nm) : NULL;
        a->join_phrase = jp ? g_strdup(jp) : g_strdup("");
        a->position    = sqlite3_column_int(s, 4);
        g_ptr_array_add(t->artists, a);
    }
    sqlite3_reset(s);

    g_free(track_json);
}

/* ============================================================================
 * Diff helpers
 * ========================================================================== */

static bool str_equal_nullsafe(const char* a, const char* b) {
    if (a == b) return true;
    if (!a) return !b || !b[0];
    if (!b) return !a || !a[0];
    return g_strcmp0(a, b) == 0;
}

static bool artist_credits_equal(const GPtrArray* cur,
                                  const desired_track_artist_t* des, size_t n) {
    size_t cur_n = cur ? cur->len : 0;
    if (cur_n != n) return false;
    for (size_t i = 0; i < n; i++) {
        const db_track_artist_t* c = g_ptr_array_index(cur, i);
        if (c->artist_id != des[i].artist_id) return false;
        if (!str_equal_nullsafe(c->join_phrase,
                                des[i].join_phrase ? des[i].join_phrase : "")) return false;
        if (c->position != des[i].position) return false;
    }
    return true;
}

static char* build_artist_display(const desired_track_artist_t* a, size_t n) {
    if (n == 0) return g_strdup("");
    GString* s = g_string_new(NULL);
    for (size_t i = 0; i < n; i++) {
        const char* name = a[i].name ? a[i].name : "";
        g_string_append(s, name);
        const char* jp = a[i].join_phrase;
        if (jp && jp[0] && i + 1 < n) g_string_append(s, jp);
    }
    return g_string_free(s, FALSE);
}

static char* merge_genre_strings(const char* existing, const char* incoming) {
    if (!existing || !existing[0]) return incoming ? g_strdup(incoming) : NULL;
    if (!incoming || !incoming[0]) return g_strdup(existing);

    GHashTable* seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray*  out  = g_ptr_array_new_with_free_func(g_free);

    const char* inputs[2] = { existing, incoming };
    for (int i = 0; i < 2; i++) {
        char** parts = g_strsplit(inputs[i], ";", -1);
        for (char** p = parts; *p; p++) {
            char* trimmed = g_strstrip(*p);
            if (!*trimmed) continue;
            char* key = g_utf8_strdown(trimmed, -1);
            if (g_hash_table_contains(seen, key)) { g_free(key); continue; }
            g_hash_table_insert(seen, key, GINT_TO_POINTER(1));
            g_ptr_array_add(out, g_strdup(trimmed));
        }
        g_strfreev(parts);
    }

    GString* result = g_string_new(NULL);
    for (guint i = 0; i < out->len; i++) {
        if (i > 0) g_string_append_c(result, ';');
        g_string_append(result, g_ptr_array_index(out, i));
    }
    g_ptr_array_unref(out);
    g_hash_table_destroy(seen);
    return g_string_free(result, FALSE);
}

/* ============================================================================
 * Canonical writes (cached prepared statements, all binds positional)
 * ========================================================================== */

/* Returns count of fields that actually changed; 0 means no UPDATE emitted. */
static int album_update_canonical(quadrature_db_t* db,
                                   const current_album_t* cur,
                                   const desired_album_state_t* d,
                                   bool* fts_dirty) {
    /* Resolve each column's final value: desired if present, else current. */
    const char* title    = (d->present_fields & DESIRED_ALBUM_TITLE) ? d->title : cur->title;
    int64_t artist_id    = (d->present_fields & DESIRED_ALBUM_ARTIST_ID) ? d->artist_id : cur->artist_id;
    bool is_comp         = (d->present_fields & DESIRED_ALBUM_COMPILATION) ? d->is_compilation : cur->is_compilation;
    uint16_t year        = (d->present_fields & DESIRED_ALBUM_YEAR) ? d->year : cur->year;
    const char* rid      = (d->present_fields & DESIRED_ALBUM_MB_RELEASE_ID) ? d->musicbrainz_release_id : cur->mb_release_id;
    const char* rgid     = (d->present_fields & DESIRED_ALBUM_MB_RELEASE_GROUP) ? d->musicbrainz_release_group_id : cur->mb_release_group_id;
    int mb_status        = (d->present_fields & DESIRED_ALBUM_MB_STATUS) ? d->mb_status : cur->mb_status;
    int64_t mb_resolved  = (d->present_fields & DESIRED_ALBUM_MB_RESOLVED_AT) ? d->mb_resolved_at : cur->mb_resolved_at;

    /* Count diffs to decide whether to emit at all. */
    int changes = 0;
    bool title_diff    = !str_equal_nullsafe(title, cur->title);         if (title_diff) changes++;
    bool artist_diff   = artist_id != cur->artist_id;                     if (artist_diff) changes++;
    if (is_comp != cur->is_compilation)                                   changes++;
    if (year != cur->year)                                                changes++;
    if (!str_equal_nullsafe(rid, cur->mb_release_id))                     changes++;
    if (!str_equal_nullsafe(rgid, cur->mb_release_group_id))              changes++;
    if (mb_status != cur->mb_status)                                      changes++;
    if (mb_resolved != cur->mb_resolved_at)                               changes++;

    if (changes == 0) return 0;

    sqlite3_stmt* s = db->reconcile_update_album;
    sqlite3_reset(s);
    if (title) sqlite3_bind_text (s, 1, title, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 1);
    sqlite3_bind_int64(s, 2, artist_id);
    sqlite3_bind_int  (s, 3, is_comp ? 1 : 0);
    sqlite3_bind_int  (s, 4, year);
    if (rid)  sqlite3_bind_text (s, 5, rid,  -1, SQLITE_STATIC); else sqlite3_bind_null(s, 5);
    if (rgid) sqlite3_bind_text (s, 6, rgid, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 6);
    sqlite3_bind_int  (s, 7, mb_status);
    sqlite3_bind_int64(s, 8, mb_resolved);
    sqlite3_bind_int64(s, 9, cur->id);
    sqlite3_step(s);
    sqlite3_reset(s);

    if (title_diff || artist_diff) *fts_dirty = true;
    return changes;
}

/* Returns true if the row changed. Fills counters + fts flag on change. */
static bool track_update_canonical(quadrature_db_t* db,
                                    const current_track_t* cur,
                                    const desired_track_t* d,
                                    const reconcile_policy_t* policy,
                                    reconcile_source_t source,
                                    const char* new_display,  /* may be NULL */
                                    char** inout_desired_genre,  /* may contain merged string */
                                    bool* fts_dirty,
                                    int* title_changes,
                                    int* position_changes,
                                    int* genre_changes) {
    const char* title    = (d->present_fields & DESIRED_TRACK_TITLE) ? d->title : cur->title;

    uint16_t track_num = cur->track_num;
    uint16_t disc_num  = cur->disc_num;
    if (d->present_fields & DESIRED_TRACK_NUM) {
        bool accept = (source != RECONCILE_SOURCE_MB) ||
                       (d->position_confidence >= policy->mb_position_min_confidence);
        if (accept) track_num = d->track_num;
    }
    if (d->present_fields & DESIRED_TRACK_DISC) {
        bool accept = (source != RECONCILE_SOURCE_MB) ||
                       (d->position_confidence >= policy->mb_position_min_confidence);
        if (accept) disc_num = d->disc_num;
    }

    uint32_t duration_ms = (d->present_fields & DESIRED_TRACK_DURATION) ? d->duration_ms : cur->duration_ms;
    uint16_t year        = (d->present_fields & DESIRED_TRACK_YEAR)     ? d->year        : cur->year;
    int64_t  mtime       = (d->present_fields & DESIRED_TRACK_MTIME)    ? d->mtime       : cur->mtime;

    /* Genre: caller may have computed a merged string into *inout_desired_genre.
     * If replace-mode is set, use d->genre directly. Otherwise stick with cur. */
    const char* genre;
    if (d->present_fields & DESIRED_TRACK_GENRE) {
        genre = d->genre;
    } else if (d->present_fields & DESIRED_TRACK_GENRE_MERGE) {
        genre = *inout_desired_genre;
    } else {
        genre = cur->genre;
    }

    const char* artist_display = (d->present_fields & DESIRED_TRACK_ARTISTS)
                                    ? new_display
                                    : cur->artist_display;

    /* Diff */
    bool title_diff    = !str_equal_nullsafe(title, cur->title);
    bool tnum_diff     = track_num != cur->track_num;
    bool disc_diff     = disc_num != cur->disc_num;
    bool dur_diff      = duration_ms != cur->duration_ms;
    bool year_diff     = year != cur->year;
    bool mtime_diff    = mtime != cur->mtime;
    bool genre_diff    = !str_equal_nullsafe(genre, cur->genre);
    bool display_diff  = !str_equal_nullsafe(artist_display, cur->artist_display);

    if (!title_diff && !tnum_diff && !disc_diff && !dur_diff &&
        !year_diff && !mtime_diff && !genre_diff && !display_diff) {
        return false;
    }

    sqlite3_stmt* s = db->reconcile_update_track;
    sqlite3_reset(s);
    if (title) sqlite3_bind_text (s, 1, title, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 1);
    sqlite3_bind_int  (s, 2, track_num);
    sqlite3_bind_int  (s, 3, disc_num);
    sqlite3_bind_int  (s, 4, (int)duration_ms);
    sqlite3_bind_int  (s, 5, year);
    sqlite3_bind_int64(s, 6, mtime);
    if (genre) sqlite3_bind_text(s, 7, genre, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 7);
    if (artist_display) sqlite3_bind_text(s, 8, artist_display, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 8);
    sqlite3_bind_int64(s, 9, cur->id);
    sqlite3_step(s);
    sqlite3_reset(s);

    if (title_diff || display_diff) *fts_dirty = true;
    if (title_diff)              (*title_changes)++;
    if (tnum_diff || disc_diff)  (*position_changes)++;
    if (genre_diff)              (*genre_changes)++;
    return true;
}

/* Insert a new track from desired state via cached prepared stmt.
 * Returns new track_id (>0) or 0 on error. */
static int64_t insert_track(quadrature_db_t* db, int64_t album_id,
                             const desired_track_t* d) {
    if (!d->path) return 0;

    char* display = NULL;
    if (d->present_fields & DESIRED_TRACK_ARTISTS)
        display = build_artist_display(d->artists, d->artist_count);

    const char* title = (d->present_fields & DESIRED_TRACK_TITLE) && d->title
                        ? d->title : "Unknown";
    uint16_t track_num = (d->present_fields & DESIRED_TRACK_NUM)  ? d->track_num   : 0;
    uint16_t disc_num  = (d->present_fields & DESIRED_TRACK_DISC) ? d->disc_num    : 1;
    if (disc_num == 0) disc_num = 1;
    uint32_t dur       = (d->present_fields & DESIRED_TRACK_DURATION) ? d->duration_ms : 0;
    uint16_t year      = (d->present_fields & DESIRED_TRACK_YEAR)  ? d->year : 0;
    int64_t  mtime     = (d->present_fields & DESIRED_TRACK_MTIME) ? d->mtime : 0;
    const char* genre  = (d->present_fields & (DESIRED_TRACK_GENRE | DESIRED_TRACK_GENRE_MERGE))
                         ? d->genre : NULL;

    sqlite3_stmt* s = db->reconcile_insert_track;
    sqlite3_reset(s);
    sqlite3_bind_text (s, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, album_id);
    sqlite3_bind_text (s, 3, d->path, -1, SQLITE_STATIC);
    sqlite3_bind_int  (s, 4, (int)dur);
    sqlite3_bind_int  (s, 5, track_num);
    sqlite3_bind_int  (s, 6, disc_num);
    sqlite3_bind_int64(s, 7, mtime);
    sqlite3_bind_int  (s, 8, year);
    if (genre)   sqlite3_bind_text(s, 9,  genre,   -1, SQLITE_STATIC); else sqlite3_bind_null(s, 9);
    if (display) sqlite3_bind_text(s, 10, display, -1, SQLITE_STATIC); else sqlite3_bind_null(s, 10);

    int64_t new_id = 0;
    if (sqlite3_step(s) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->db);
    sqlite3_reset(s);
    g_free(display);
    return new_id;
}

/* Replace the entire track_artists row set for `track_id`. */
static void replace_track_artists(quadrature_db_t* db, int64_t track_id,
                                   const desired_track_artist_t* a, size_t n) {
    sqlite3_reset(db->delete_track_artists);
    sqlite3_bind_int64(db->delete_track_artists, 1, track_id);
    sqlite3_step(db->delete_track_artists);
    sqlite3_reset(db->delete_track_artists);

    for (size_t i = 0; i < n; i++) {
        if (a[i].artist_id <= 0) continue;
        sqlite3_reset(db->insert_track_artist);
        sqlite3_bind_int64(db->insert_track_artist, 1, track_id);
        sqlite3_bind_int64(db->insert_track_artist, 2, a[i].artist_id);
        sqlite3_bind_int  (db->insert_track_artist, 3, a[i].position);
        sqlite3_bind_text (db->insert_track_artist, 4,
            a[i].join_phrase ? a[i].join_phrase : "", -1, SQLITE_STATIC);
        sqlite3_step(db->insert_track_artist);
        sqlite3_reset(db->insert_track_artist);
    }
}

/* ============================================================================
 * Per-album reconcile (runs inside the batch loop; no lock, no txn boundary)
 * ========================================================================== */

static void reconcile_one(quadrature_db_t* db,
                           int64_t album_id,
                           const current_album_t* cur_album,
                           const album_tracks_t* cur_bucket,
                           const desired_album_state_t* desired,
                           const reconcile_policy_t* policy,
                           reconcile_summary_t* summary_out) {
    reconcile_summary_t summary = {0};

    /* Respect-user-edits: TAGS source must not overwrite post-RESOLVED album
     * data. Phase 6 owns MB-derived fields; user's tag-era values are frozen. */
    desired_album_state_t d_local = *desired;
    if (desired->source == RECONCILE_SOURCE_TAGS &&
        policy->respect_user_edits &&
        cur_album->mb_status == MB_STATUS_RESOLVED) {
        d_local.present_fields &= ~(DESIRED_ALBUM_TITLE
                                    | DESIRED_ALBUM_ARTIST_ID
                                    | DESIRED_ALBUM_MB_RELEASE_ID
                                    | DESIRED_ALBUM_MB_RELEASE_GROUP
                                    | DESIRED_ALBUM_MB_STATUS
                                    | DESIRED_ALBUM_MB_RESOLVED_AT);
    }

    bool album_fts_dirty  = false;
    bool tracks_fts_dirty = false;

    /* --- Track updates (matched pairs) --- */
    for (size_t i = 0; i < d_local.track_count; i++) {
        const desired_track_t* dt = &d_local.tracks[i];
        if (!dt->path) continue;

        current_track_t* ct = cur_bucket
            ? g_hash_table_lookup(cur_bucket->by_path, dt->path)
            : NULL;

        if (!ct) {
            int64_t new_id = insert_track(db, album_id, dt);
            if (new_id <= 0) continue;
            if ((dt->present_fields & DESIRED_TRACK_ARTISTS) && dt->artist_count > 0)
                replace_track_artists(db, new_id, dt->artists, dt->artist_count);
            summary.tracks_inserted++;
            tracks_fts_dirty = true;
            continue;
        }

        desired_track_t dt_local = *dt;
        if (desired->source == RECONCILE_SOURCE_TAGS &&
            policy->respect_user_edits &&
            cur_album->mb_status == MB_STATUS_RESOLVED) {
            dt_local.present_fields &= ~(DESIRED_TRACK_TITLE | DESIRED_TRACK_ARTISTS);
        }

        /* Artist display pre-compute (needed to diff even when we won't write it). */
        char* new_display = NULL;
        if (dt_local.present_fields & DESIRED_TRACK_ARTISTS)
            new_display = build_artist_display(dt_local.artists, dt_local.artist_count);

        /* Genre merge (replace-mode goes through as-is). */
        char* merged_genre = NULL;
        if (dt_local.present_fields & DESIRED_TRACK_GENRE_MERGE)
            merged_genre = merge_genre_strings(ct->genre, dt_local.genre);

        bool changed = track_update_canonical(db, ct, &dt_local, policy,
                                               desired->source, new_display,
                                               &merged_genre,
                                               &tracks_fts_dirty,
                                               &summary.track_titles_changed,
                                               &summary.track_positions_changed,
                                               &summary.track_genres_changed);

        /* Artists replacement (diff against pre-loaded current set). */
        if (dt_local.present_fields & DESIRED_TRACK_ARTISTS) {
            if (!artist_credits_equal(ct->artists, dt_local.artists, dt_local.artist_count)) {
                replace_track_artists(db, ct->id, dt_local.artists, dt_local.artist_count);
                summary.track_artists_changed++;
                changed = true;
            }
        }

        g_free(new_display);
        g_free(merged_genre);

        if (changed) summary.tracks_updated++;
    }

    /* --- Prune missing tracks (Phase 2 only) --- */
    if (policy->prune_missing_tracks && cur_bucket) {
        GHashTable* want = g_hash_table_new(g_str_hash, g_str_equal);
        for (size_t i = 0; i < d_local.track_count; i++)
            if (d_local.tracks[i].path)
                g_hash_table_add(want, (gpointer)d_local.tracks[i].path);

        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, cur_bucket->by_path);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            current_track_t* ct = v;
            if (g_hash_table_contains(want, (const char*)k)) continue;
            sqlite3_reset(db->reconcile_delete_track_by_id);
            sqlite3_bind_int64(db->reconcile_delete_track_by_id, 1, ct->id);
            sqlite3_step(db->reconcile_delete_track_by_id);
            sqlite3_reset(db->reconcile_delete_track_by_id);
            summary.tracks_deleted++;
            tracks_fts_dirty = true;
        }
        g_hash_table_destroy(want);
    }

    /* --- Album-level field updates --- */
    summary.album_fields_changed =
        album_update_canonical(db, cur_album, &d_local, &album_fts_dirty);

    /* --- FTS syncs --- */
    if (album_fts_dirty && db->update_album_fts) {
        sqlite3_reset(db->update_album_fts);
        sqlite3_bind_int64(db->update_album_fts, 1, album_id);
        sqlite3_step(db->update_album_fts);
        sqlite3_reset(db->update_album_fts);
    }
    if (tracks_fts_dirty && db->sync_album_tracks_fts) {
        sqlite3_reset(db->sync_album_tracks_fts);
        sqlite3_bind_int64(db->sync_album_tracks_fts, 1, album_id);
        sqlite3_step(db->sync_album_tracks_fts);
        sqlite3_reset(db->sync_album_tracks_fts);
        summary.fts_synced = true;
    }

    if (summary_out) *summary_out = summary;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

quadrature_result_t db_reconcile_albums(quadrature_db_t* db,
                                         const int64_t* album_ids,
                                         const desired_album_state_t* desireds,
                                         size_t count,
                                         const reconcile_policy_t* policy,
                                         reconcile_summary_t* summaries_out) {
    if (!db || !policy) return QUADRATURE_ERROR_INVALID_PARAM;
    if (count == 0) return QUADRATURE_OK;
    if (!album_ids || !desireds) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);

    reconcile_snapshot_t snap = {0};
    snapshot_load(db, album_ids, count, &snap);

    for (size_t i = 0; i < count; i++) {
        int64_t aid = album_ids[i];
        if (aid <= 0) continue;

        current_album_t* cur_album = g_hash_table_lookup(snap.albums, &aid);
        if (!cur_album) {
            if (summaries_out) summaries_out[i] = (reconcile_summary_t){0};
            continue;  /* skip-and-continue on missing rows */
        }

        album_tracks_t* cur_bucket = g_hash_table_lookup(snap.tracks_by_album, &aid);
        reconcile_one(db, aid, cur_album, cur_bucket,
                      &desireds[i], policy,
                      summaries_out ? &summaries_out[i] : NULL);
    }

    snapshot_free(&snap);
    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_delete_album(quadrature_db_t* db, int64_t album_id) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;
    if (!db->in_transaction) return QUADRATURE_ERROR_INTERNAL;

    db_lock(db);

    /* Tracks don't cascade from albums — delete them first.
     * track_artists DOES cascade off tracks. */
    sqlite3_stmt* del_tracks = NULL;
    sqlite3_prepare_v2(db->db, "DELETE FROM tracks WHERE album_id = ?",
                       -1, &del_tracks, NULL);
    sqlite3_bind_int64(del_tracks, 1, album_id);
    sqlite3_step(del_tracks);
    sqlite3_finalize(del_tracks);

    sqlite3_stmt* del_album = NULL;
    sqlite3_prepare_v2(db->db, "DELETE FROM albums WHERE id = ?",
                       -1, &del_album, NULL);
    sqlite3_bind_int64(del_album, 1, album_id);
    sqlite3_step(del_album);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(del_album);

    if (changes > 0) {
        sqlite3_exec(db->db,
            "DELETE FROM tracks_fts WHERE rowid NOT IN (SELECT id FROM tracks)",
            NULL, NULL, NULL);
        sqlite3_exec(db->db,
            "DELETE FROM albums_fts WHERE rowid NOT IN (SELECT id FROM albums)",
            NULL, NULL, NULL);
    }

    db_unlock(db);
    return QUADRATURE_OK;
}

quadrature_result_t db_create_or_get_album_by_path(
    quadrature_db_t* db,
    const char* path,
    const char* title,
    int64_t artist_id,
    uint16_t year,
    int64_t* album_id_out) {
    if (!db || !path || !album_id_out) return QUADRATURE_ERROR_INVALID_PARAM;
    *album_id_out = 0;

    db_lock(db);

    sqlite3_reset(db->select_album_by_path);
    sqlite3_bind_text(db->select_album_by_path, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(db->select_album_by_path) == SQLITE_ROW) {
        *album_id_out = sqlite3_column_int64(db->select_album_by_path, 0);
        sqlite3_reset(db->select_album_by_path);
        db_unlock(db);
        return QUADRATURE_OK;
    }
    sqlite3_reset(db->select_album_by_path);

    sqlite3_reset(db->insert_folder_album);
    sqlite3_bind_text(db->insert_folder_album, 1, title ? title : "", -1, SQLITE_STATIC);
    if (artist_id > 0) sqlite3_bind_int64(db->insert_folder_album, 2, artist_id);
    else               sqlite3_bind_null(db->insert_folder_album, 2);
    sqlite3_bind_text(db->insert_folder_album, 3, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(db->insert_folder_album, 4, year);
    sqlite3_bind_int(db->insert_folder_album, 5, 0);  /* is_compilation */

    int rc = sqlite3_step(db->insert_folder_album);
    sqlite3_reset(db->insert_folder_album);
    if (rc != SQLITE_DONE) {
        db_unlock(db);
        return QUADRATURE_ERROR_INTERNAL;
    }

    *album_id_out = sqlite3_last_insert_rowid(db->db);

    /* Seed albums_fts row */
    if (db->update_album_fts) {
        sqlite3_reset(db->update_album_fts);
        sqlite3_bind_int64(db->update_album_fts, 1, *album_id_out);
        sqlite3_step(db->update_album_fts);
        sqlite3_reset(db->update_album_fts);
    }

    db_unlock(db);
    return QUADRATURE_OK;
}

/* Fast path: no reconciler, no load, just one cached UPDATE.
 * mb_status / mb_resolved_at are not FTS-visible, so no FTS sync is needed. */
quadrature_result_t db_reconcile_album_mb_status(quadrature_db_t* db,
                                                  int64_t album_id,
                                                  int mb_status,
                                                  int64_t mb_resolved_at) {
    if (!db || album_id <= 0) return QUADRATURE_ERROR_INVALID_PARAM;

    db_lock(db);
    sqlite3_stmt* s = db->update_album_mb_status;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, mb_status);
    sqlite3_bind_int64(s, 2, mb_resolved_at);
    sqlite3_bind_int64(s, 3, album_id);
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    db_unlock(db);

    return (rc == SQLITE_DONE) ? QUADRATURE_OK : QUADRATURE_ERROR_INTERNAL;
}
