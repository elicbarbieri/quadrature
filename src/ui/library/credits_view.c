/**
 * Quadrature Credits View
 *
 * MusicBrainz credits popover builders, role formatting, and info button wiring.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "../internal.h"
#include "quadrature/database.h"
#include <string.h>

static GtkWidget *find_parent_card(GtkWidget *widget) {
    GtkWidget *w = widget;
    while (w) {
        if (g_object_get_data(G_OBJECT(w), "release-mbid"))
            return w;
        if (g_object_get_data(G_OBJECT(w), "album-id") &&
            gtk_widget_has_css_class(w, "album-card"))
            return w;
        w = gtk_widget_get_parent(w);
    }
    return NULL;
}

/** Accumulated unique roles for a single entity (track or album). */
typedef struct {
    int64_t id;                /* track_id or album_id */
    GPtrArray *roles;          /* unique role strings (owned) */
    GHashTable *role_set;      /* for dedup */
} CreditRoleSet;

static void credit_role_set_free(gpointer data) {
    CreditRoleSet *rs = data;
    if (rs) {
        g_ptr_array_unref(rs->roles);
        g_hash_table_unref(rs->role_set);
        g_free(rs);
    }
}

/** Create a new CreditRoleSet for the given entity ID. */
static CreditRoleSet *credit_role_set_new(int64_t id) {
    CreditRoleSet *rs = g_new0(CreditRoleSet, 1);
    rs->id = id;
    rs->roles = g_ptr_array_new_with_free_func(g_free);
    rs->role_set = g_hash_table_new(g_str_hash, g_str_equal);
    return rs;
}

/** Add a role to the set if not already present. Always takes ownership of role. */
static void credit_role_set_add(CreditRoleSet *rs, char *role) {
    if (!role) return;
    if (g_hash_table_contains(rs->role_set, role)) {
        g_free(role);
        return;
    }
    /* roles array owns the string; role_set borrows the pointer */
    g_ptr_array_add(rs->roles, role);
    g_hash_table_add(rs->role_set, role);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared Helpers (used by popover builders and credit row formatting)
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Add a subsection header (smaller than section header). */
static void popover_add_subsection(GtkWidget *container, const char *title) {
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "track-info-subsection-header");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_box_append(GTK_BOX(container), lbl);
}

/** Dismiss the ancestor popover, then navigate to the artist.
 * on_album_card_artist_navigate is defined in detail_view.c (non-static). */
static void on_popover_artist_navigate(GtkButton *btn, gpointer data) {
    GtkWidget *w = GTK_WIDGET(btn);
    while (w && !GTK_IS_POPOVER(w))
        w = gtk_widget_get_parent(w);
    if (w)
        gtk_popover_popdown(GTK_POPOVER(w));

    on_album_card_artist_navigate(btn, data);
}

/** Create a clickable artist button for the popover. */
static GtkWidget *popover_create_artist_button(int64_t artist_id,
                                                const char *name,
                                                UnifiedDetailData *ud) {
    GtkWidget *btn = gtk_button_new_with_label(name);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "artist-btn");
    g_object_set_data(G_OBJECT(btn), "artist-id",
                      GSIZE_TO_POINTER((gsize)artist_id));
    g_signal_connect(btn, "clicked",
                     G_CALLBACK(on_popover_artist_navigate), ud);
    return btn;
}

/** Dismiss the ancestor popover, then navigate to a credit artist (MBID-based).
 * Checks the MBID bridge: if the artist exists in the main DB, navigate to the
 * library artist view; otherwise, navigate to the credits-only meta artist view. */
static void on_popover_credit_navigate(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    const char *artist_mbid = g_object_get_data(G_OBJECT(btn), "artist-mbid");
    const char *artist_name = g_object_get_data(G_OBJECT(btn), "artist-name");
    const char *artist_type = g_object_get_data(G_OBJECT(btn), "artist-type");

    if (!artist_mbid) return;

    /* Dismiss popover */
    GtkWidget *w = GTK_WIDGET(btn);
    while (w && !GTK_IS_POPOVER(w))
        w = gtk_widget_get_parent(w);
    if (w)
        gtk_popover_popdown(GTK_POPOVER(w));

    /* Check MBID bridge: does this artist exist in any library's main DB? */
    int64_t found_artist_id = 0;
    int found_bi = 0;
    int lib_count = library_cache_get_library_count(ud->cache);
    for (int i = 0; i < lib_count && found_artist_id == 0; i++) {
        int bi = library_cache_get_bitmap_index(ud->cache, i);
        if (!library_cache_get_available(ud->cache, bi)) continue;
        quadrature_db_t *lib_db = library_cache_get_dbs(ud->cache, bi).db;
        if (lib_db) {
            db_get_artist_by_mbid(lib_db, artist_mbid, &found_artist_id);
            if (found_artist_id > 0)
                found_bi = bi;
        }
    }

    if (found_artist_id > 0) {
        int64_t global_id = LIBRARY_MAKE_GLOBAL_ID(found_bi, found_artist_id);
        library_unified_detail_navigate_to_artist(ud->container, global_id, NULL);
    } else {
        library_unified_detail_navigate_to_meta_artist(
            ud->container, artist_mbid, artist_name, artist_type);
    }
}

/**
 * Categorize a MusicBrainz link_type_name into a credit bucket.
 * Returns: 0=instrumentalist, 1=vocal, 2=producer, -1=uncategorized (omit).
 */
static int credit_bucket(const char *link_type_name) {
    if (!link_type_name) return -1;
    if (g_strcmp0(link_type_name, "instrument") == 0) return 0;
    if (g_strcmp0(link_type_name, "vocal") == 0) return 1;
    if (g_strcmp0(link_type_name, "producer") == 0) return 2;
    if (g_strcmp0(link_type_name, "co-producer") == 0) return 2;
    if (g_strcmp0(link_type_name, "remixer") == 0) return 2;
    return -1;
}

/**
 * Format a credit role for display.
 * Caller must g_free() the result.
 */
static char *format_credit_role(const char *link_type_name, const char *attributes) {
    if (g_strcmp0(link_type_name, "co-producer") == 0)
        return g_strdup("Co Producer");
    if (g_strcmp0(link_type_name, "remixer") == 0)
        return g_strdup("Remixer");
    if (g_strcmp0(link_type_name, "producer") == 0 && attributes && attributes[0]) {
        if (g_strcmp0(attributes, "co") == 0)
            return g_strdup("Co Producer");
        if (g_strcmp0(attributes, "remixer") == 0)
            return g_strdup("Remixer");
        if (g_strcmp0(attributes, "additional") == 0)
            return g_strdup("Additional Producer");
    }

    if (attributes && attributes[0]) {
        char *display = g_strdup(attributes);
        display[0] = g_ascii_toupper(display[0]);
        return display;
    }

    if (link_type_name && link_type_name[0]) {
        char *display = g_strdup(link_type_name);
        display[0] = g_ascii_toupper(display[0]);
        return display;
    }

    return NULL;
}

/**
 * Collect aggregated MB credit roles per album for a given artist.
 * Returns a GHashTable mapping album_id (GSIZE_TO_POINTER) → GPtrArray<char*>
 * of unique role strings.  The caller owns the returned table; both keys
 * (trivial pointers) and values (GPtrArray with g_free element destructor)
 * are freed when the table is destroyed.  Returns NULL if no credits found.
 */
static void _roles_array_free(gpointer p) { g_ptr_array_unref(p); }

GHashTable *collect_credit_album_roles(UnifiedDetailData *ud,
                                       const char *artist_mbid,
                                       const char *artist_name,
                                       int64_t viewed_artist_id,
                                       GHashTable *skip_track_ids) {
    if (!artist_mbid || !ud->settings) return NULL;

    /* release_mbid → CreditRoleSet* (internal, freed at end). Keying by MBID
     * collapses the same logical album across libraries into one set. */
    GHashTable *album_roles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, credit_role_set_free);

    int lib_count = library_cache_get_library_count(ud->cache);
    for (int li = 0; li < lib_count; li++) {
        int bi = library_cache_get_bitmap_index(ud->cache, li);
        /* Skip libraries not in the active library mask */
        if (!(ud->library_mask & (1u << bi))) continue;
        if (!library_cache_get_available(ud->cache, bi)) continue;
        library_cache_dbs_t dbs = library_cache_get_dbs(ud->cache, bi);
        if (!dbs.meta || !dbs.db) continue;

        db_meta_artist_credit_t *credits = NULL;
        size_t credit_count = 0;
        quadrature_result_t res = db_meta_get_credits_by_artist(
            dbs.meta, artist_mbid, NULL, &credits, &credit_count);
        if (res != QUADRATURE_OK || credit_count == 0) {
            db_meta_artist_credits_free(credits, credit_count);
            continue;
        }

        for (size_t i = 0; i < credit_count; i++) {
            db_meta_artist_credit_t *c = &credits[i];
            if (!c->release_mbid) continue;

            char *role = format_credit_role(c->link_type_name, c->attributes);
            if (!role) continue;

            /* Resolve to local track_id → global */
            int64_t local_tid = 0;
            if (db_get_track_by_position(dbs.db, c->release_mbid,
                    c->disc_num, c->track_num, &local_tid) != QUADRATURE_OK) {
                g_free(role);
                continue;
            }
            int64_t track_id = LIBRARY_MAKE_GLOBAL_ID(bi, local_tid);

            /* Skip tracks from own albums */
            if (skip_track_ids &&
                g_hash_table_contains(skip_track_ids,
                                      GSIZE_TO_POINTER((gsize)track_id))) {
                g_free(role);
                continue;
            }

            /* Skip if viewed artist is already a track artist */
            {
                const GPtrArray *track_artists =
                    library_cache_get_track_artists(ud->cache, track_id);
                gboolean already_credited = FALSE;
                if (track_artists && artist_name) {
                    for (guint j = 0; j < track_artists->len; j++) {
                        const library_track_artist_t *a =
                            g_ptr_array_index(track_artists, j);
                        if (a->artist_id == viewed_artist_id ||
                            (a->name && g_ascii_strcasecmp(a->name, artist_name) == 0)) {
                            already_credited = TRUE;
                            break;
                        }
                    }
                }
                if (already_credited) { g_free(role); continue; }
            }

            const library_track_info_t *track = library_cache_get_track(ud->cache, track_id);
            if (!track || track->album_id <= 0) { g_free(role); continue; }

            /* Skip own-album by artist name match */
            {
                const library_album_info_t *al = library_cache_get_album(ud->cache, track->album_id, ud->library_mask);
                if (al && al->artist_name && artist_name &&
                    g_ascii_strcasecmp(al->artist_name, artist_name) == 0) {
                    g_free(role);
                    continue;
                }
            }

            /* Accumulate per-album roles, keyed by MBID */
            CreditRoleSet *ars = g_hash_table_lookup(album_roles, c->release_mbid);
            if (!ars) {
                ars = credit_role_set_new(track->album_id);
                g_hash_table_insert(album_roles, g_strdup(c->release_mbid), ars);
            }
            credit_role_set_add(ars, role);  /* ownership transferred */
        }

        db_meta_artist_credits_free(credits, credit_count);
    }

    if (g_hash_table_size(album_roles) == 0) {
        g_hash_table_destroy(album_roles);
        return NULL;
    }

    /* Convert CreditRoleSet → GPtrArray<char*> in output table, keyed by MBID */
    GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, _roles_array_free);
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, album_roles);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        const char *mbid = k;
        CreditRoleSet *ars = v;
        GPtrArray *roles = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < ars->roles->len; i++)
            g_ptr_array_add(roles, g_strdup(g_ptr_array_index(ars->roles, i)));
        g_hash_table_insert(out, g_strdup(mbid), roles);
    }

    g_hash_table_destroy(album_roles);
    return out;
}

/**
 * Collect MB credit tracks for an artist and append to appears_on lists.
 * - Track rows go into appears_on_tracks (with per-track credit annotation)
 * - Album rows go into appears_on_albums (with aggregated role pills)
 * Skips tracks in skip_track_ids (own albums, existing appearances).
 * When multiple credits exist for the same track, keeps the highest-priority role.
 * Returns the number of credit tracks appended.
 */
guint append_credit_rows(UnifiedDetailData *ud,
                                 const char *artist_mbid,
                                 const char *artist_name,
                                 int64_t viewed_artist_id,
                                 GHashTable *skip_track_ids,
                                 GHashTable *skip_album_mbids,
                                 UiRowSizeGroups *track_groups,
                                 UiRowSizeGroups *album_groups) {
    if (!artist_mbid || !ud->settings) return 0;

    /* Pass 1: collect all unique roles per track position + per album.
     * Albums are keyed by release_mbid so entries for the same logical album
     * from multiple libraries collapse into one row. */
    /* track key "release:disc:track" → CreditRoleSet */
    GHashTable *track_roles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, credit_role_set_free);
    /* release_mbid → CreditRoleSet (stores a representative album_id for display) */
    GHashTable *album_roles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, credit_role_set_free);

    int lib_count = library_cache_get_library_count(ud->cache);
    for (int li = 0; li < lib_count; li++) {
        int bi = library_cache_get_bitmap_index(ud->cache, li);
        /* Skip libraries not in the active library mask */
        if (!(ud->library_mask & (1u << bi))) continue;
        if (!library_cache_get_available(ud->cache, bi)) continue;
        library_cache_dbs_t dbs = library_cache_get_dbs(ud->cache, bi);
        if (!dbs.meta) continue;

        db_meta_artist_credit_t *credits = NULL;
        size_t credit_count = 0;
        quadrature_result_t res = db_meta_get_credits_by_artist(
            dbs.meta, artist_mbid, NULL, &credits, &credit_count);

        if (res != QUADRATURE_OK || credit_count == 0) {
            db_meta_artist_credits_free(credits, credit_count);
            continue;
        }

        if (!dbs.db) {
            db_meta_artist_credits_free(credits, credit_count);
            continue;
        }

        for (size_t i = 0; i < credit_count; i++) {
            db_meta_artist_credit_t *c = &credits[i];
            if (!c->release_mbid) continue;

            /* Format role for this credit */
            char *role = format_credit_role(c->link_type_name, c->attributes);

            /* Resolve to local track_id, then convert to global */
            int64_t local_tid = 0;
            if (db_get_track_by_position(dbs.db, c->release_mbid,
                    c->disc_num, c->track_num, &local_tid) != QUADRATURE_OK) {
                g_free(role);
                continue;
            }
            int64_t track_id = LIBRARY_MAKE_GLOBAL_ID(bi, local_tid);

            /* Skip tracks from own albums or already shown */
            if (skip_track_ids &&
                g_hash_table_contains(skip_track_ids,
                                      GSIZE_TO_POINTER((gsize)track_id))) {
                g_free(role);
                continue;
            }

            /* Skip if the viewed artist is already primary/featuring.
             * Compare by name (case-insensitive) rather than ID because the
             * same artist can have different global IDs across libraries due
             * to cross-library merging. */
            {
                const GPtrArray *track_artists =
                    library_cache_get_track_artists(ud->cache, track_id);
                gboolean already_credited = FALSE;
                if (track_artists && artist_name) {
                    for (guint j = 0; j < track_artists->len; j++) {
                        const library_track_artist_t *a =
                            g_ptr_array_index(track_artists, j);
                        if (a->artist_id == viewed_artist_id ||
                            (a->name && g_ascii_strcasecmp(a->name, artist_name) == 0)) {
                            already_credited = TRUE;
                            break;
                        }
                    }
                }
                if (already_credited) { g_free(role); continue; }
            }

            const library_track_info_t *track = library_cache_get_track(ud->cache, track_id);

            /* Skip if the album artist name matches the viewed artist name.
             * This catches "appears on own album" cases that the ID check
             * can miss across merged libraries. */
            if (track && track->album_id > 0) {
                const library_album_info_t *al = library_cache_get_album(ud->cache, track->album_id, ud->library_mask);
                if (al && al->artist_name && artist_name &&
                    g_ascii_strcasecmp(al->artist_name, artist_name) == 0) {
                    g_free(role);
                    continue;
                }
            }
            if (!track) { g_free(role); continue; }

            /* Accumulate per-album roles (keyed by MBID, not album_id, so
             * the same logical album in multiple libraries collapses). Also
             * skip albums already shown as cache appearances — compared by
             * MBID since global album_ids diverge across libraries. */
            if (role && track->album_id > 0 && c->release_mbid) {
                if (skip_album_mbids &&
                    g_hash_table_contains(skip_album_mbids, c->release_mbid)) {
                    /* already shown via appearance_albums pass */
                } else {
                    CreditRoleSet *ars = g_hash_table_lookup(album_roles, c->release_mbid);
                    if (!ars) {
                        ars = credit_role_set_new(track->album_id);
                        g_hash_table_insert(album_roles, g_strdup(c->release_mbid), ars);
                    }
                    credit_role_set_add(ars, g_strdup(role));
                }
            }

            /* Accumulate per-track roles (all unique roles, no priority) */
            char *key = g_strdup_printf("%s:%d:%d",
                c->release_mbid, c->disc_num, c->track_num);
            CreditRoleSet *trs = g_hash_table_lookup(track_roles, key);
            if (!trs) {
                trs = credit_role_set_new(track_id);
                g_hash_table_insert(track_roles, key, trs);
            } else {
                g_free(key);
            }
            credit_role_set_add(trs, role);  /* ownership transferred */
        }

        db_meta_artist_credits_free(credits, credit_count);
    }

    /* Pass 2a: create track rows */
    guint added = 0;
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, track_roles);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        CreditRoleSet *trs = v;
        const library_track_info_t *track = library_cache_get_track(ud->cache, trs->id);
        if (!track) continue;

        /* Build NULL-terminated roles array */
        g_ptr_array_add(trs->roles, NULL);  /* sentinel */
        UiTrackCreditInfo credit = {
            .roles = (const char *const *)trs->roles->pdata,
            .role_count = trs->roles->len - 1,  /* exclude sentinel */
            .artist_name = artist_name,
            .artist_id = viewed_artist_id,
        };

        GtkWidget *row = ui_create_track_row(track, ud->cache, ud->art_mgr, TRUE,
                                               (RowCallbacks *)&ud->cbs.artist_cbs,
                                               (RowCallbacks *)&ud->cbs.album_cbs,
                                               track_groups, &credit);
        ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.track_cbs);
        gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_tracks), row);
        added++;
    }

    /* Pass 2b: create album rows with aggregated role pills.
     * Skip against skip_album_mbids already applied during accumulation. */
    g_hash_table_iter_init(&iter, album_roles);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        CreditRoleSet *ars = v;

        const library_album_info_t *album = library_cache_get_album(ud->cache, ars->id, ud->library_mask);
        if (!album) continue;

        /* Build NULL-terminated roles array for UiAlbumCreditInfo */
        g_ptr_array_add(ars->roles, NULL);  /* sentinel */
        UiAlbumCreditInfo acredit = {
            .artist_name = artist_name,
            .artist_id = viewed_artist_id,
            .roles = (const char *const *)ars->roles->pdata,
            .role_count = ars->roles->len - 1,  /* exclude sentinel */
        };

        GtkWidget *row = ui_create_album_row(album, ud->cache, ud->art_mgr, TRUE,
                                               (RowCallbacks *)&ud->cbs.artist_cbs,
                                               album_groups, &acredit);
        ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.album_cbs);
        gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_albums), row);
    }

    g_hash_table_destroy(track_roles);
    g_hash_table_destroy(album_roles);

    return added;
}

static void populate_mb_credits(GtkWidget *credits_box, UnifiedDetailData *ud,
                                 const char *release_mbid,
                                 int disc_num, int track_num, int library_index,
                                 char **rec_mbid_out) {
    if (rec_mbid_out) *rec_mbid_out = NULL;

    if (!release_mbid || library_index < 0)
        return;

    quadrature_meta_db_t *meta_db = library_cache_get_dbs(ud->cache, library_index).meta;

    if (meta_db) {
        char *rec_mbid = NULL;
        quadrature_result_t res = db_meta_get_recording_mbid(meta_db, release_mbid, disc_num, track_num, &rec_mbid);

        if (res == QUADRATURE_OK && rec_mbid) {
            if (rec_mbid_out)
                *rec_mbid_out = g_strdup(rec_mbid);

            db_meta_link_t *links = NULL;
            size_t link_count = 0;
            res = db_meta_get_links(meta_db, rec_mbid, &links, &link_count);

            if (res == QUADRATURE_OK && link_count > 0) {
                /* Bucket links into categories */
                GPtrArray *buckets[3] = {
                    g_ptr_array_new(), /* instrumentalists */
                    g_ptr_array_new(), /* vocals */
                    g_ptr_array_new(), /* producers */
                };

                for (size_t i = 0; i < link_count; i++) {
                    int b = credit_bucket(links[i].link_type_name);
                    if (b >= 0)
                        g_ptr_array_add(buckets[b], &links[i]);
                }

                static const char *bucket_titles[] = {
                    "Instrumentalists", "Vocals", "Producers"
                };

                gboolean has_any = FALSE;
                for (int b = 0; b < 3; b++) {
                    if (buckets[b]->len > 0) has_any = TRUE;
                }

                if (has_any) {
                    gtk_widget_set_visible(credits_box, TRUE);

                    /* SizeGroup ensures all artist buttons align across rows */
                    GtkSizeGroup *sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

                    /* Load credit row template ONCE — reuse for each row */
                    GBytes *tmpl_bytes = g_resources_lookup_data(
                        "/org/quadrature/ui/credit_row.ui", 0, NULL);

                    for (int b = 0; b < 3; b++) {
                        if (buckets[b]->len == 0) continue;
                        popover_add_subsection(credits_box, bucket_titles[b]);

                        for (guint j = 0; j < buckets[b]->len; j++) {
                            const db_meta_link_t *link = g_ptr_array_index(buckets[b], j);

                            /* Create fresh builder from cached bytes (avoids re-parsing resource) */
                            GtkBuilder *cr_builder = gtk_builder_new();
                            gtk_builder_add_from_string(cr_builder,
                                g_bytes_get_data(tmpl_bytes, NULL),
                                g_bytes_get_size(tmpl_bytes), NULL);
                            GtkWidget *row = GTK_WIDGET(
                                gtk_builder_get_object(cr_builder, "credit_row"));
                            GtkWidget *btn = GTK_WIDGET(
                                gtk_builder_get_object(cr_builder, "credit_artist_btn"));
                            GtkWidget *artist_label = GTK_WIDGET(
                                gtk_builder_get_object(cr_builder, "credit_artist_label"));
                            GtkWidget *role_label = GTK_WIDGET(
                                gtk_builder_get_object(cr_builder, "credit_role_label"));

                            /* Set artist name */
                            const char *display_name = link->entity0_credit
                                ? link->entity0_credit : link->artist_name;
                            gtk_label_set_text(GTK_LABEL(artist_label), display_name);

                            /* Set role/instrument text */
                            char *display_role = format_credit_role(
                                link->link_type_name, link->attributes);
                            if (display_role) {
                                if (b == 0) {
                                    /* Instruments: show bare name */
                                    gtk_label_set_text(GTK_LABEL(role_label), display_role);
                                } else {
                                    /* Vocals/Producers: show in parens */
                                    char *parens = g_strdup_printf("(%s)", display_role);
                                    gtk_label_set_text(GTK_LABEL(role_label), parens);
                                    g_free(parens);
                                }
                                g_free(display_role);
                            }

                            /* Store MBID/name/type on button for navigation */
                            g_object_set_data_full(G_OBJECT(btn), "artist-mbid",
                                                   g_strdup(link->artist_mbid), g_free);
                            g_object_set_data_full(G_OBJECT(btn), "artist-name",
                                                   g_strdup(link->artist_name), g_free);
                            if (link->artist_type)
                                g_object_set_data_full(G_OBJECT(btn), "artist-type",
                                                       g_strdup(link->artist_type), g_free);

                            /* Suppress if this credit is the artist we're viewing */
                            if (link->artist_mbid && ud->meta_artist_mbid &&
                                g_strcmp0(link->artist_mbid, ud->meta_artist_mbid) == 0) {
                                gtk_widget_set_sensitive(btn, FALSE);
                            } else {
                                g_signal_connect(btn, "clicked",
                                                 G_CALLBACK(on_popover_credit_navigate), ud);
                            }

                            /* Add button to size group for column alignment */
                            gtk_size_group_add_widget(sg, btn);

                            /* row is floating, gtk_box_append sinks it */
                            gtk_box_append(GTK_BOX(credits_box), row);
                            g_object_unref(cr_builder);
                        }
                    }

                    g_bytes_unref(tmpl_bytes);
                    g_object_unref(sg);
                }

                for (int b = 0; b < 3; b++)
                    g_ptr_array_free(buckets[b], TRUE);
            }

            db_meta_links_free(links, link_count);
            g_free(rec_mbid);
        }

    }
}

/** Called when the track info popover is closed — unparent and destroy it. */
static void on_track_info_popover_closed(GtkPopover *popover, gpointer data) {
    (void)data;
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void on_track_info_btn_clicked(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;

    /* Walk up to find the track item grid (row content with track-id data) */
    GtkWidget *content = gtk_widget_get_parent(GTK_WIDGET(btn));
    while (content && !g_object_get_data(G_OBJECT(content), "track-id"))
        content = gtk_widget_get_parent(content);
    if (!content) return;

    /* Get track metadata stored on the row widget */
    int64_t track_id = (int64_t)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(content), "track-id"));
    int disc_num = (int)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(content), "disc-num"));
    int track_num = (int)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(content), "track-num"));
    int library_index = (int)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(content), "library-index"));

    /* Find release MBID from parent card */
    GtkWidget *card = find_parent_card(content);
    const char *release_mbid = card
        ? g_object_get_data(G_OBJECT(card), "release-mbid") : NULL;

    /* Look up track info from cache */
    const library_track_info_t *track = library_cache_get_track(ud->cache, track_id);

    /* ── Load popover structure from template ── */
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/track_metadata_popover.ui");
    GtkWidget *popover = GTK_WIDGET(gtk_builder_get_object(builder, "track_info_popover"));
    GtkWidget *scroll = GTK_WIDGET(gtk_builder_get_object(builder, "track_info_scroll"));

    /* Get named widgets for data binding */
    GtkLabel *track_title_label = GTK_LABEL(gtk_builder_get_object(builder, "track_title_label"));
    GtkWidget *album_row = GTK_WIDGET(gtk_builder_get_object(builder, "album_row"));
    GtkLabel *album_value = GTK_LABEL(gtk_builder_get_object(builder, "album_value"));
    GtkWidget *artist_row = GTK_WIDGET(gtk_builder_get_object(builder, "artist_row"));
    GtkFlowBox *artist_flow_box = GTK_FLOW_BOX(gtk_builder_get_object(builder, "artist_flow_box"));
    GtkWidget *featuring_row = GTK_WIDGET(gtk_builder_get_object(builder, "featuring_row"));
    GtkFlowBox *featuring_flow_box = GTK_FLOW_BOX(gtk_builder_get_object(builder, "featuring_flow_box"));
    GtkWidget *release_date_row = GTK_WIDGET(gtk_builder_get_object(builder, "release_date_row"));
    GtkLabel *release_date_value = GTK_LABEL(gtk_builder_get_object(builder, "release_date_value"));
    GtkWidget *position_row = GTK_WIDGET(gtk_builder_get_object(builder, "position_row"));
    GtkLabel *position_value = GTK_LABEL(gtk_builder_get_object(builder, "position_value"));
    GtkWidget *credits_box = GTK_WIDGET(gtk_builder_get_object(builder, "credits_box"));
    GtkWidget *path_row = GTK_WIDGET(gtk_builder_get_object(builder, "path_row"));
    GtkLabel *path_value = GTK_LABEL(gtk_builder_get_object(builder, "path_value"));
    GtkWidget *duration_row = GTK_WIDGET(gtk_builder_get_object(builder, "duration_row"));
    GtkLabel *duration_value = GTK_LABEL(gtk_builder_get_object(builder, "duration_value"));
    GtkWidget *recording_row = GTK_WIDGET(gtk_builder_get_object(builder, "recording_row"));
    GtkLabel *recording_value = GTK_LABEL(gtk_builder_get_object(builder, "recording_value"));

    g_object_ref(popover);
    g_object_unref(builder);

    ui_popover_install_shortcuts(GTK_POPOVER(popover));

    /* ══ Bind track data to template widgets ══ */
    if (track) {
        gtk_label_set_text(track_title_label, track->title);

        if (track->album_title && track->album_title[0]) {
            gtk_label_set_text(album_value, track->album_title);
            gtk_widget_set_visible(album_row, TRUE);
        }

        /* Artist / Featuring rows with clickable buttons */
        const GPtrArray *artists = library_cache_get_track_artists(ud->cache, track_id);
        if (artists && artists->len > 0) {
            for (guint i = 0; i < artists->len; i++) {
                const library_track_artist_t *a = g_ptr_array_index(artists, i);
                GtkWidget *abtn = popover_create_artist_button(a->artist_id, a->name, ud);

                if (a->role == LIBRARY_ARTIST_ROLE_PRIMARY) {
                    gtk_flow_box_append(artist_flow_box, abtn);
                    gtk_widget_set_visible(artist_row, TRUE);
                } else {
                    gtk_flow_box_append(featuring_flow_box, abtn);
                    gtk_widget_set_visible(featuring_row, TRUE);
                }
            }
        }

        if (track->year > 0) {
            char year_buf[8];
            snprintf(year_buf, sizeof(year_buf), "%u", track->year);
            gtk_label_set_text(release_date_value, year_buf);
            gtk_widget_set_visible(release_date_row, TRUE);
        }

        char pos_buf[32];
        snprintf(pos_buf, sizeof(pos_buf), "Disc %d \u2013 Track %d", disc_num, track_num);
        gtk_label_set_text(position_value, pos_buf);
        gtk_widget_set_visible(position_row, TRUE);
    }

    /* ══ Credits (dynamic — subsection headers + rows appended to credits_box) ══ */
    char *rec_mbid = NULL;
    populate_mb_credits(credits_box, ud, release_mbid, disc_num, track_num,
                        library_index, &rec_mbid);

    /* ══ Track Info section ══ */
    {
        char *resolved_path = library_cache_resolve_track_path(ud->cache, track_id);
        if (resolved_path) {
            gtk_label_set_text(path_value, resolved_path);
            gtk_widget_set_visible(path_row, TRUE);
            g_free(resolved_path);
        }
    }
    if (track) {
        char dur_buf[16];
        ui_format_duration(track->duration_ms, dur_buf, sizeof(dur_buf));
        gtk_label_set_text(duration_value, dur_buf);
        gtk_widget_set_visible(duration_row, TRUE);
    }
    if (rec_mbid) {
        gtk_label_set_text(recording_value, rec_mbid);
        gtk_widget_set_visible(recording_row, TRUE);
        g_free(rec_mbid);
    }

    /* ── Size popover to fit within content_stack ── */
    static const int MARGIN_TOP  = 100;
    static const int MARGIN_SIDE = 100;
    static const int MARGIN_BOT  = 100;

    int stack_w = gtk_widget_get_width(ud->content_stack);
    int stack_h = gtk_widget_get_height(ud->content_stack);
    int pop_w = MAX(300, stack_w - 2 * MARGIN_SIDE);
    int pop_h = MAX(200, stack_h - MARGIN_TOP - MARGIN_BOT);

    gtk_widget_set_size_request(scroll, pop_w, -1);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), pop_h);

    gtk_widget_set_parent(popover, ud->content_stack);
    g_object_unref(popover);  /* parent now owns it */

    /* Anchor below top margin, opening downward */
    GdkRectangle anchor = { stack_w / 2, MARGIN_TOP, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);

    g_signal_connect(popover, "closed", G_CALLBACK(on_track_info_popover_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

/** Wire info button click handlers for all track rows in a card. */
void wire_info_buttons(GtkWidget *card, UnifiedDetailData *ud) {
    GtkWidget *track_list = find_widget_by_name(card, "track_list");
    if (!track_list) return;

    GtkWidget *child = gtk_widget_get_first_child(track_list);
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            GtkWidget *row_content = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
            if (row_content) {
                GtkWidget *info_btn = g_object_get_data(G_OBJECT(row_content), "info-btn");
                if (info_btn)
                    g_signal_connect(info_btn, "clicked",
                                     G_CALLBACK(on_track_info_btn_clicked), ud);
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * List Navigation Helpers (modular, reusable)
 * ═══════════════════════════════════════════════════════════════════════════ */

