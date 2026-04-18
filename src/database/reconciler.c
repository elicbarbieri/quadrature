/**
 * Album State Reconciler — the single writer for album/track updates.
 *
 * See include/quadrature/indexer.h for the contract.
 *
 * Implementation notes:
 *   - Loads current state via transient SELECTs (not on hot-path enough to
 *     warrant cached statements here — the expensive work is in producers).
 *   - Diffs field-by-field against desired state. Only emits UPDATE when at
 *     least one tracked field differs.
 *   - Track identity is the album-relative path (matches tracks.path +
 *     UNIQUE(album_id, path) schema invariant).
 *   - Artist credit sets are compared atomically: if anything differs in the
 *     ordered list, the whole set is replaced.
 *   - Genre merge logic (DESIRED_TRACK_GENRE_MERGE) is a case-insensitive
 *     set union on ';'-separated values.
 *   - Emits exactly one sync_album_tracks_fts / albums_fts update when any
 *     FTS-visible field changed.
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
 * Current-state snapshot
 * ========================================================================== */

typedef struct {
    int64_t id;
    char*   path;
    char*   title;
    uint16_t track_num;
    uint16_t disc_num;
    uint32_t duration_ms;
    uint16_t year;
    char*   genre;
    char*   artist_display;
    int64_t mtime;

    /* Artist credits for this track, position-ordered. Loaded on demand. */
    GPtrArray* artists;   /* of db_track_artist_t*, owned */
} current_track_t;

typedef struct {
    int64_t id;
    char*   path;
    char*   title;
    int64_t artist_id;
    bool    is_compilation;
    uint16_t year;
    char*   mb_release_id;
    char*   mb_release_group_id;
    int     mb_status;
    int64_t mb_resolved_at;
} current_album_t;

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

static quadrature_result_t load_current_album(quadrature_db_t* db, int64_t album_id,
                                               current_album_t* out) {
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT id, path, title, artist_id, is_compilation, year, "
        "musicbrainz_release_id, musicbrainz_release_group_id, "
        "mb_status, mb_resolved_at "
        "FROM albums WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return QUADRATURE_ERROR_INTERNAL;

    sqlite3_bind_int64(stmt, 1, album_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    out->id                  = sqlite3_column_int64(stmt, 0);
    const char* p            = (const char*)sqlite3_column_text(stmt, 1);
    const char* t            = (const char*)sqlite3_column_text(stmt, 2);
    out->path                = p ? g_strdup(p) : NULL;
    out->title               = t ? g_strdup(t) : NULL;
    out->artist_id           = sqlite3_column_int64(stmt, 3);
    out->is_compilation      = sqlite3_column_int(stmt, 4) != 0;
    out->year                = (uint16_t)sqlite3_column_int(stmt, 5);
    const char* rid          = (const char*)sqlite3_column_text(stmt, 6);
    const char* rgid         = (const char*)sqlite3_column_text(stmt, 7);
    out->mb_release_id       = rid  ? g_strdup(rid)  : NULL;
    out->mb_release_group_id = rgid ? g_strdup(rgid) : NULL;
    out->mb_status           = sqlite3_column_int(stmt, 8);
    out->mb_resolved_at      = sqlite3_column_int64(stmt, 9);

    sqlite3_finalize(stmt);
    return QUADRATURE_OK;
}

static void current_album_free_fields(current_album_t* a) {
    if (!a) return;
    g_free(a->path);
    g_free(a->title);
    g_free(a->mb_release_id);
    g_free(a->mb_release_group_id);
}

/* Returns GHashTable<char* path, current_track_t*>, caller destroys. */
static GHashTable* load_current_tracks(quadrature_db_t* db, int64_t album_id) {
    GHashTable* by_path = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, current_track_free);

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT id, path, title, track_num, disc_num, duration_ms, "
        "year, genre, artist_display, mtime "
        "FROM tracks WHERE album_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return by_path;

    sqlite3_bind_int64(stmt, 1, album_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        current_track_t* t = g_new0(current_track_t, 1);
        t->id                 = sqlite3_column_int64(stmt, 0);
        const char* path      = (const char*)sqlite3_column_text(stmt, 1);
        const char* title     = (const char*)sqlite3_column_text(stmt, 2);
        t->path               = path  ? g_strdup(path)  : g_strdup("");
        t->title              = title ? g_strdup(title) : NULL;
        t->track_num          = (uint16_t)sqlite3_column_int(stmt, 3);
        t->disc_num           = (uint16_t)sqlite3_column_int(stmt, 4);
        t->duration_ms        = (uint32_t)sqlite3_column_int(stmt, 5);
        t->year               = (uint16_t)sqlite3_column_int(stmt, 6);
        const char* genre     = (const char*)sqlite3_column_text(stmt, 7);
        const char* adisplay  = (const char*)sqlite3_column_text(stmt, 8);
        t->genre              = genre    ? g_strdup(genre)    : NULL;
        t->artist_display     = adisplay ? g_strdup(adisplay) : NULL;
        t->mtime              = sqlite3_column_int64(stmt, 9);
        g_hash_table_insert(by_path, g_strdup(t->path), t);
    }
    sqlite3_finalize(stmt);
    return by_path;
}

/* Populate t->artists with current track_artists rows, position-ordered. */
static void load_current_track_artists(quadrature_db_t* db, current_track_t* t) {
    if (t->artists) return;
    t->artists = g_ptr_array_new_with_free_func(track_artist_free_gp);

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT ta.artist_id, a.name, ta.join_phrase, ta.position "
        "FROM track_artists ta LEFT JOIN artists a ON a.id = ta.artist_id "
        "WHERE ta.track_id = ? ORDER BY ta.position",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return;

    sqlite3_bind_int64(stmt, 1, t->id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        db_track_artist_t* a = g_new0(db_track_artist_t, 1);
        a->artist_id   = sqlite3_column_int64(stmt, 0);
        const char* nm = (const char*)sqlite3_column_text(stmt, 1);
        const char* jp = (const char*)sqlite3_column_text(stmt, 2);
        a->name        = nm ? g_strdup(nm) : NULL;
        a->join_phrase = jp ? g_strdup(jp) : g_strdup("");
        a->position    = sqlite3_column_int(stmt, 3);
        g_ptr_array_add(t->artists, a);
    }
    sqlite3_finalize(stmt);
}

/* ============================================================================
 * Diff helpers
 * ========================================================================== */

static bool str_equal_nullsafe(const char* a, const char* b) {
    if (a == b) return true;
    if (!a) return !b || !b[0];
    if (!b) return !a || !a[0];
    return strcmp(a, b) == 0;
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

/* Compute the "artist_display" string from an artist-credit list. */
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

/* Case-insensitive ';'-separated set union; returns newly-allocated string. */
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
 * Write helpers
 * ========================================================================== */

static int album_field_updates(quadrature_db_t* db,
                                int64_t album_id,
                                const current_album_t* cur,
                                const desired_album_state_t* d,
                                const reconcile_policy_t* policy,
                                bool* album_fts_dirty) {
    (void)policy;
    GString* setclause = g_string_new(NULL);
    int change_count = 0;

    /* Build SET list for fields that are (a) present in desired and (b)
     * different from current. */
    #define BIND_IF_DIFFERS_STR(bit, field_name, desired_val, current_val) do { \
        if ((d->present_fields & (bit)) && !str_equal_nullsafe((desired_val), (current_val))) { \
            if (change_count > 0) g_string_append_c(setclause, ','); \
            g_string_append(setclause, " " field_name " = ?"); \
            change_count++; \
        } \
    } while (0)

    #define BIND_IF_DIFFERS_INT(bit, field_name, desired_val, current_val) do { \
        if ((d->present_fields & (bit)) && (int64_t)(desired_val) != (int64_t)(current_val)) { \
            if (change_count > 0) g_string_append_c(setclause, ','); \
            g_string_append(setclause, " " field_name " = ?"); \
            change_count++; \
        } \
    } while (0)

    bool title_diff    = (d->present_fields & DESIRED_ALBUM_TITLE)
                          && !str_equal_nullsafe(d->title, cur->title);
    bool artist_diff   = (d->present_fields & DESIRED_ALBUM_ARTIST_ID)
                          && d->artist_id != cur->artist_id;
    bool comp_diff     = (d->present_fields & DESIRED_ALBUM_COMPILATION)
                          && d->is_compilation != cur->is_compilation;
    bool year_diff     = (d->present_fields & DESIRED_ALBUM_YEAR)
                          && d->year != cur->year;
    bool rid_diff      = (d->present_fields & DESIRED_ALBUM_MB_RELEASE_ID)
                          && !str_equal_nullsafe(d->musicbrainz_release_id, cur->mb_release_id);
    bool rgid_diff     = (d->present_fields & DESIRED_ALBUM_MB_RELEASE_GROUP)
                          && !str_equal_nullsafe(d->musicbrainz_release_group_id,
                                                  cur->mb_release_group_id);
    bool status_diff   = (d->present_fields & DESIRED_ALBUM_MB_STATUS)
                          && d->mb_status != cur->mb_status;
    bool resat_diff    = (d->present_fields & DESIRED_ALBUM_MB_RESOLVED_AT)
                          && d->mb_resolved_at != cur->mb_resolved_at;

    if (title_diff)  { g_string_append(setclause, " title = ?");                         change_count++; }
    if (artist_diff) { g_string_append(setclause, change_count > 0 ? ", artist_id = ?" : " artist_id = ?"); change_count++; }
    if (comp_diff)   { g_string_append(setclause, change_count > 0 ? ", is_compilation = ?" : " is_compilation = ?"); change_count++; }
    if (year_diff)   { g_string_append(setclause, change_count > 0 ? ", year = ?" : " year = ?"); change_count++; }
    if (rid_diff)    { g_string_append(setclause, change_count > 0 ? ", musicbrainz_release_id = ?" : " musicbrainz_release_id = ?"); change_count++; }
    if (rgid_diff)   { g_string_append(setclause, change_count > 0 ? ", musicbrainz_release_group_id = ?" : " musicbrainz_release_group_id = ?"); change_count++; }
    if (status_diff) { g_string_append(setclause, change_count > 0 ? ", mb_status = ?" : " mb_status = ?"); change_count++; }
    if (resat_diff)  { g_string_append(setclause, change_count > 0 ? ", mb_resolved_at = ?" : " mb_resolved_at = ?"); change_count++; }

    #undef BIND_IF_DIFFERS_STR
    #undef BIND_IF_DIFFERS_INT

    if (change_count == 0) {
        g_string_free(setclause, TRUE);
        return 0;
    }

    char* sql = g_strdup_printf("UPDATE albums SET%s WHERE id = ?", setclause->str);
    g_string_free(setclause, TRUE);

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    g_free(sql);
    if (rc != SQLITE_OK) return 0;

    int idx = 1;
    if (title_diff)  sqlite3_bind_text(stmt, idx++, d->title, -1, SQLITE_STATIC);
    if (artist_diff) sqlite3_bind_int64(stmt, idx++, d->artist_id);
    if (comp_diff)   sqlite3_bind_int(stmt, idx++, d->is_compilation ? 1 : 0);
    if (year_diff)   sqlite3_bind_int(stmt, idx++, d->year);
    if (rid_diff) {
        if (d->musicbrainz_release_id) sqlite3_bind_text(stmt, idx++, d->musicbrainz_release_id, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, idx++);
    }
    if (rgid_diff) {
        if (d->musicbrainz_release_group_id) sqlite3_bind_text(stmt, idx++, d->musicbrainz_release_group_id, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, idx++);
    }
    if (status_diff) sqlite3_bind_int(stmt, idx++, d->mb_status);
    if (resat_diff)  sqlite3_bind_int64(stmt, idx++, d->mb_resolved_at);

    sqlite3_bind_int64(stmt, idx, album_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (title_diff || artist_diff) *album_fts_dirty = true;
    return change_count;
}

/* Apply track-level updates for one matched pair.
 *
 * Returns true if the row changed (any SET clause emitted). Sets
 * `*title_or_display_changed` when a title/artist_display change requires
 * FTS re-sync. */
static bool update_track_fields(quadrature_db_t* db,
                                 const current_track_t* cur,
                                 const desired_track_t* d,
                                 const reconcile_policy_t* policy,
                                 reconcile_source_t source,
                                 bool* title_or_display_changed,
                                 int* title_changes,
                                 int* position_changes,
                                 int* genre_changes) {
    GString* set = g_string_new(NULL);
    int n = 0;

    bool want_title    = (d->present_fields & DESIRED_TRACK_TITLE) &&
                         !str_equal_nullsafe(d->title, cur->title);

    bool want_tracknum = false;
    bool want_discnum  = false;
    if (d->present_fields & DESIRED_TRACK_NUM) {
        if (source == RECONCILE_SOURCE_MB) {
            want_tracknum = (d->position_confidence >= policy->mb_position_min_confidence) &&
                            d->track_num != cur->track_num;
        } else {
            want_tracknum = d->track_num != cur->track_num;
        }
    }
    if (d->present_fields & DESIRED_TRACK_DISC) {
        if (source == RECONCILE_SOURCE_MB) {
            want_discnum = (d->position_confidence >= policy->mb_position_min_confidence) &&
                           d->disc_num != cur->disc_num;
        } else {
            want_discnum = d->disc_num != cur->disc_num;
        }
    }

    bool want_duration = (d->present_fields & DESIRED_TRACK_DURATION) &&
                         d->duration_ms != cur->duration_ms;
    bool want_year     = (d->present_fields & DESIRED_TRACK_YEAR) &&
                         d->year != cur->year;
    bool want_mtime    = (d->present_fields & DESIRED_TRACK_MTIME) &&
                         d->mtime != cur->mtime;

    /* Genre: replace vs merge. */
    char* desired_genre = NULL;
    bool  want_genre    = false;
    if (d->present_fields & DESIRED_TRACK_GENRE) {
        desired_genre = d->genre ? g_strdup(d->genre) : NULL;
        want_genre = !str_equal_nullsafe(desired_genre, cur->genre);
    } else if (d->present_fields & DESIRED_TRACK_GENRE_MERGE) {
        desired_genre = merge_genre_strings(cur->genre, d->genre);
        want_genre = !str_equal_nullsafe(desired_genre, cur->genre);
    }

    /* artist_display: recomputed when artists change. */
    char* new_display = NULL;
    bool  want_display = false;
    if (d->present_fields & DESIRED_TRACK_ARTISTS) {
        new_display = build_artist_display(d->artists, d->artist_count);
        want_display = !str_equal_nullsafe(new_display, cur->artist_display);
    }

    if (!want_title && !want_tracknum && !want_discnum && !want_duration &&
        !want_year && !want_mtime && !want_genre && !want_display) {
        g_free(desired_genre);
        g_free(new_display);
        g_string_free(set, TRUE);
        return false;
    }

    #define APPEND(col) do { \
        if (n > 0) g_string_append_c(set, ','); \
        g_string_append(set, " " col " = ?"); \
        n++; \
    } while (0)

    if (want_title)    APPEND("title");
    if (want_tracknum) APPEND("track_num");
    if (want_discnum)  APPEND("disc_num");
    if (want_duration) APPEND("duration_ms");
    if (want_year)     APPEND("year");
    if (want_mtime)    APPEND("mtime");
    if (want_genre)    APPEND("genre");
    if (want_display)  APPEND("artist_display");

    #undef APPEND

    char* sql = g_strdup_printf("UPDATE tracks SET%s WHERE id = ?", set->str);
    g_string_free(set, TRUE);
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    g_free(sql);
    if (rc != SQLITE_OK) {
        g_free(desired_genre);
        g_free(new_display);
        return false;
    }

    int idx = 1;
    if (want_title)    sqlite3_bind_text(stmt, idx++, d->title ? d->title : "", -1, SQLITE_STATIC);
    if (want_tracknum) sqlite3_bind_int(stmt, idx++, d->track_num);
    if (want_discnum)  sqlite3_bind_int(stmt, idx++, d->disc_num);
    if (want_duration) sqlite3_bind_int(stmt, idx++, (int)d->duration_ms);
    if (want_year)     sqlite3_bind_int(stmt, idx++, d->year);
    if (want_mtime)    sqlite3_bind_int64(stmt, idx++, d->mtime);
    if (want_genre) {
        if (desired_genre) sqlite3_bind_text(stmt, idx++, desired_genre, -1, SQLITE_STATIC);
        else sqlite3_bind_null(stmt, idx++);
    }
    if (want_display)  sqlite3_bind_text(stmt, idx++, new_display ? new_display : "", -1, SQLITE_STATIC);

    sqlite3_bind_int64(stmt, idx, cur->id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    g_free(desired_genre);
    g_free(new_display);

    if (want_title || want_display) *title_or_display_changed = true;
    if (want_title) (*title_changes)++;
    if (want_tracknum || want_discnum) (*position_changes)++;
    if (want_genre) (*genre_changes)++;

    return true;
}

/* Insert a new track from desired state. Returns new track_id (>0) or 0 on error.
 * Only fields present in `present_fields` are populated; others get defaults. */
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

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO tracks(title, album_id, path, duration_ms, track_num, "
        "disc_num, mtime, year, genre, artist_display) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_free(display);
        return 0;
    }

    sqlite3_bind_text (stmt, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, album_id);
    sqlite3_bind_text (stmt, 3, d->path, -1, SQLITE_STATIC);
    sqlite3_bind_int  (stmt, 4, (int)dur);
    sqlite3_bind_int  (stmt, 5, track_num);
    sqlite3_bind_int  (stmt, 6, disc_num);
    sqlite3_bind_int64(stmt, 7, mtime);
    sqlite3_bind_int  (stmt, 8, year);
    if (genre) sqlite3_bind_text(stmt, 9, genre, -1, SQLITE_STATIC);
    else       sqlite3_bind_null(stmt, 9);
    if (display) sqlite3_bind_text(stmt, 10, display, -1, SQLITE_STATIC);
    else         sqlite3_bind_null(stmt, 10);

    int64_t new_id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->db);
    sqlite3_finalize(stmt);
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
        sqlite3_bind_int(db->insert_track_artist, 3, a[i].position);
        sqlite3_bind_text(db->insert_track_artist, 4,
            a[i].join_phrase ? a[i].join_phrase : "", -1, SQLITE_STATIC);
        sqlite3_step(db->insert_track_artist);
        sqlite3_reset(db->insert_track_artist);
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

quadrature_result_t db_reconcile_album(quadrature_db_t* db,
                                        int64_t album_id,
                                        const desired_album_state_t* desired,
                                        const reconcile_policy_t* policy,
                                        reconcile_summary_t* summary_out) {
    if (!db || !desired || !policy || album_id <= 0)
        return QUADRATURE_ERROR_INVALID_PARAM;

    reconcile_summary_t summary = {0};

    db_lock(db);

    current_album_t cur_album = {0};
    quadrature_result_t res = load_current_album(db, album_id, &cur_album);
    if (res != QUADRATURE_OK) {
        db_unlock(db);
        return res;
    }

    /* Respect-user-edits: TAGS source must not overwrite post-RESOLVED album
     * data. Phase 6 owns MB-derived fields; user's tag-era values are frozen. */
    desired_album_state_t d_local = *desired;
    if (desired->source == RECONCILE_SOURCE_TAGS &&
        policy->respect_user_edits &&
        cur_album.mb_status == MB_STATUS_RESOLVED) {
        d_local.present_fields &= ~(DESIRED_ALBUM_TITLE
                                    | DESIRED_ALBUM_ARTIST_ID
                                    | DESIRED_ALBUM_MB_RELEASE_ID
                                    | DESIRED_ALBUM_MB_RELEASE_GROUP
                                    | DESIRED_ALBUM_MB_STATUS
                                    | DESIRED_ALBUM_MB_RESOLVED_AT);
    }

    GHashTable* cur_tracks = load_current_tracks(db, album_id);

    bool album_fts_dirty = false;
    bool tracks_fts_dirty = false;

    /* --- Track updates (matched pairs) --- */
    for (size_t i = 0; i < d_local.track_count; i++) {
        const desired_track_t* dt = &d_local.tracks[i];
        if (!dt->path) continue;

        current_track_t* ct = g_hash_table_lookup(cur_tracks, dt->path);
        if (!ct) {
            /* New track — insert and apply artists. Only TAGS-source producers
             * insert (MB resolver only ever updates matched local tracks). */
            int64_t new_id = insert_track(db, album_id, dt);
            if (new_id <= 0) continue;
            if ((dt->present_fields & DESIRED_TRACK_ARTISTS) && dt->artist_count > 0)
                replace_track_artists(db, new_id, dt->artists, dt->artist_count);
            summary.tracks_inserted++;
            tracks_fts_dirty = true;
            continue;
        }

        /* Respect user edits at track level too. */
        desired_track_t dt_local = *dt;
        if (desired->source == RECONCILE_SOURCE_TAGS &&
            policy->respect_user_edits &&
            cur_album.mb_status == MB_STATUS_RESOLVED) {
            dt_local.present_fields &= ~(DESIRED_TRACK_TITLE | DESIRED_TRACK_ARTISTS);
        }

        bool changed = update_track_fields(db, ct, &dt_local, policy,
                                            desired->source,
                                            &tracks_fts_dirty,
                                            &summary.track_titles_changed,
                                            &summary.track_positions_changed,
                                            &summary.track_genres_changed);

        /* Artists replacement */
        if (dt_local.present_fields & DESIRED_TRACK_ARTISTS) {
            load_current_track_artists(db, ct);
            if (!artist_credits_equal(ct->artists, dt_local.artists, dt_local.artist_count)) {
                replace_track_artists(db, ct->id, dt_local.artists, dt_local.artist_count);
                summary.track_artists_changed++;
                changed = true;
            }
        }

        if (changed) summary.tracks_updated++;
    }

    /* --- Prune missing tracks (Phase 2 only) --- */
    if (policy->prune_missing_tracks) {
        /* Build set of desired paths for fast lookup. */
        GHashTable* want = g_hash_table_new(g_str_hash, g_str_equal);
        for (size_t i = 0; i < d_local.track_count; i++)
            if (d_local.tracks[i].path)
                g_hash_table_add(want, (gpointer)d_local.tracks[i].path);

        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, cur_tracks);
        GArray* to_delete = g_array_new(FALSE, FALSE, sizeof(int64_t));
        while (g_hash_table_iter_next(&it, &k, &v)) {
            const char* p = k;
            current_track_t* ct = v;
            if (!g_hash_table_contains(want, p))
                g_array_append_val(to_delete, ct->id);
        }
        g_hash_table_destroy(want);

        if (to_delete->len > 0) {
            sqlite3_stmt* del = NULL;
            sqlite3_prepare_v2(db->db, "DELETE FROM tracks WHERE id = ?",
                               -1, &del, NULL);
            for (guint i = 0; i < to_delete->len; i++) {
                int64_t id = g_array_index(to_delete, int64_t, i);
                sqlite3_reset(del);
                sqlite3_bind_int64(del, 1, id);
                sqlite3_step(del);
            }
            sqlite3_finalize(del);
            summary.tracks_deleted = (int)to_delete->len;
            tracks_fts_dirty = true;
        }
        g_array_free(to_delete, TRUE);
    }

    /* --- Album-level field updates --- */
    summary.album_fields_changed =
        album_field_updates(db, album_id, &cur_album, &d_local, policy,
                            &album_fts_dirty);

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

    current_album_free_fields(&cur_album);
    g_hash_table_destroy(cur_tracks);

    db_unlock(db);

    if (summary_out) *summary_out = summary;
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
