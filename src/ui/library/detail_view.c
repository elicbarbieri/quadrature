/**
 * Quadrature Unified Detail View
 *
 * Three-state view for album/artist detail display.
 * Built programmatically - no template dependencies.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "../internal.h"
#include "quadrature/metadata.h"
#include "quadrature/database.h"
#include <string.h>

/* wire_info_buttons, append_credit_rows declared in library/internal.h */

static const char *UNIFIED_DATA_KEY = "unified-detail-data";

static void load_album_state(UnifiedDetailData *ud, int64_t album_id, int64_t select_track_id);
static void sync_spacer_heights(UnifiedDetailData *ud);

static void unified_detail_data_free(UnifiedDetailData *ud) {
    if (!ud) return;
    for (int i = 0; i < ud->nav_depth; i++) {
        g_free(ud->nav_stack[i].view_name);
        g_free(ud->nav_stack[i].meta_artist_mbid);
        g_free(ud->nav_stack[i].meta_artist_name);
        g_free(ud->nav_stack[i].meta_artist_type);
    }
    g_free(ud->meta_artist_mbid);
    g_free(ud->meta_artist_name);
    g_free(ud->about_wiki_url);
    if (ud->banner_cancel) {
        g_cancellable_cancel(ud->banner_cancel);
        g_object_unref(ud->banner_cancel);
    }
    if (ud->bio_bg_cancel) {
        g_cancellable_cancel(ud->bio_bg_cancel);
        g_object_unref(ud->bio_bg_cancel);
    }
    ui_selection_group_free(ud->sel_group);
    if (ud->header_height_signal && ud->back_header)
        g_signal_handler_disconnect(ud->back_header, ud->header_height_signal);
    g_free(ud);
}

/**
 * Find the first background image for an artist across all library roots.
 * Returns a newly-allocated path, or NULL if not found.
 */
static char *find_artist_background(app_settings_t *settings, const char *mbid) {
    if (!settings || !mbid) return NULL;

    for (int i = 0; i < settings->library_count; i++) {
        const char *dp = app_settings_get_library_data_path(settings, i);
        char *path = g_strdup_printf("%s/artwork/artists/%s/background_0.jpg", dp, mbid);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }
    return NULL;
}

/**
 * Load artist bio from the metadata DB.
 * Shows the about section only if both a background image and bio text exist.
 * Synchronous — reads from quadrature-bios.sqlite (written by indexer Phase 8).
 */
/* ─── Async image loading helpers ──────────────────────────────────────── */

typedef struct {
    GdkTexture *texture;
    EdgeColors  edge_colors;
    char       *path;
} AsyncImageResult;

static void async_image_result_free(AsyncImageResult *r) {
    g_clear_object(&r->texture);
    g_free(r->path);
    g_free(r);
}

/* Worker thread: load texture + sample edge colors (both are thread-safe) */
static void load_image_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel) {
    (void)src;
    char *path = data;
    if (g_cancellable_is_cancelled(cancel)) return;

    GFile *file = g_file_new_for_path(path);
    GError *error = NULL;
    GdkTexture *texture = gdk_texture_new_from_file(file, &error);
    g_object_unref(file);

    if (!texture) {
        g_task_return_error(task, error);
        return;
    }

    AsyncImageResult *result = g_new0(AsyncImageResult, 1);
    result->texture = texture;  /* transfer ownership */
    result->edge_colors = sample_edge_colors(texture, 5);
    result->path = g_strdup(path);
    g_task_return_pointer(task, result, (GDestroyNotify)async_image_result_free);
}

/* Callback: apply background image + gradients, then load bio text */
static void on_bio_background_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    UnifiedDetailData *ud = data;
    GError *error = NULL;
    AsyncImageResult *result = g_task_propagate_pointer(G_TASK(res), &error);
    if (!result) {
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("about background load failed: %s", error->message);
        g_clear_error(&error);
        return;
    }

    int tex_w = gdk_texture_get_width(result->texture);
    int tex_h_orig = gdk_texture_get_height(result->texture);
    int proportional_h = (int)((double)tex_h_orig / tex_w * 1000);
    int tex_h = MIN(proportional_h, 600);
    gtk_widget_set_size_request(ud->about_background_image, -1, tex_h);
    gtk_picture_set_paintable(GTK_PICTURE(ud->about_background_image),
                              GDK_PAINTABLE(result->texture));
    quad_gradient_fade_set_color(ud->about_fade_top, &result->edge_colors.top, TRUE);
    quad_gradient_fade_set_color(ud->about_fade_bottom, &result->edge_colors.bottom, FALSE);
    async_image_result_free(result);

    /* Now query bios DB for bio text (fast, stays on main thread) */
    /* Extract MBID from the most recent nav entry */
    const char *mbid = NULL;
    if (ud->nav_depth > 0) {
        NavEntry *top = &ud->nav_stack[ud->nav_depth - 1];
        if (top->meta_artist_mbid) mbid = top->meta_artist_mbid;
    }
    if (!mbid && ud->meta_artist_mbid) mbid = ud->meta_artist_mbid;
    if (!mbid && ud->state == DETAIL_STATE_ARTIST && ud->current_id > 0) {
        /* MBID is universal — resolve from any library, not just the filtered set */
        const library_artist_info_t *a = library_cache_get_artist(ud->cache, ud->current_id, LIBRARY_MASK_ALL);
        if (a) mbid = a->musicbrainz_id;
    }
    if (!mbid) return;

    int lib_count = library_cache_get_library_count(ud->cache);
    for (int i = 0; i < lib_count; i++) {
        int bi = library_cache_get_bitmap_index(ud->cache, i);
        if (!library_cache_get_available(ud->cache, bi)) continue;
        quadrature_bios_db_t *bios_db = library_cache_get_dbs(ud->cache, bi).bios;
        if (!bios_db) continue;

        char *bio_text = NULL;
        char *wiki_url = NULL;
        quadrature_result_t r = db_bios_get(bios_db, mbid, &bio_text, &wiki_url);

        if (r == QUADRATURE_OK && bio_text && bio_text[0]) {
            gtk_label_set_text(GTK_LABEL(ud->about_bio_text), bio_text);
            g_free(ud->about_wiki_url);
            ud->about_wiki_url = wiki_url;
            gtk_widget_set_visible(ud->about_section, TRUE);
            g_free(bio_text);
            return;
        }
        g_free(bio_text);
        g_free(wiki_url);
    }
}

static void load_artist_bio(UnifiedDetailData *ud, const char *mbid) {
    /* Check for background image first — no image means no hero banner */
    char *bg_path = find_artist_background(ud->settings, mbid);
    if (!bg_path) return;

    /* Cancel any in-flight bio background load */
    if (ud->bio_bg_cancel) {
        g_cancellable_cancel(ud->bio_bg_cancel);
        g_object_unref(ud->bio_bg_cancel);
    }
    ud->bio_bg_cancel = g_cancellable_new();

    /* Async: load texture + sample edge colors off main thread */
    GTask *task = g_task_new(NULL, ud->bio_bg_cancel, on_bio_background_loaded, ud);
    g_task_set_task_data(task, bg_path, g_free);
    g_task_run_in_thread(task, load_image_thread);
    g_object_unref(task);
}

/**
 * Find the first banner image for an artist across all library roots.
 * Returns a newly-allocated path, or NULL if not found.
 */
static char *find_artist_banner(app_settings_t *settings, const char *mbid) {
    if (!settings || !mbid) return NULL;

    for (int i = 0; i < settings->library_count; i++) {
        const char *dp = app_settings_get_library_data_path(settings, i);
        char *path = g_strdup_printf("%s/artwork/artists/%s/banner_0.jpg", dp, mbid);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }
    return NULL;
}

/* Callback: apply banner image + gradient */
static void on_banner_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    UnifiedDetailData *ud = data;
    GError *error = NULL;
    AsyncImageResult *result = g_task_propagate_pointer(G_TASK(res), &error);
    if (!result) {
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("artist banner load failed: %s", error->message);
            gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
        }
        g_clear_error(&error);
        return;
    }

    int tex_h = gdk_texture_get_height(result->texture);
    gtk_widget_set_size_request(ud->artist_banner, -1, tex_h);
    gtk_picture_set_paintable(GTK_PICTURE(ud->artist_banner),
                              GDK_PAINTABLE(result->texture));
    quad_gradient_fade_set_color(ud->artist_banner_fade_bottom, &result->edge_colors.bottom, FALSE);
    gtk_widget_set_visible(ud->artist_banner_overlay, TRUE);
    async_image_result_free(result);
}

static void load_artist_banner(UnifiedDetailData *ud, const char *mbid) {
    char *banner_path = find_artist_banner(ud->settings, mbid);
    if (!banner_path) {
        gtk_picture_set_paintable(GTK_PICTURE(ud->artist_banner), NULL);
        gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
        return;
    }

    /* Cancel any in-flight banner load */
    if (ud->banner_cancel) {
        g_cancellable_cancel(ud->banner_cancel);
        g_object_unref(ud->banner_cancel);
    }
    ud->banner_cancel = g_cancellable_new();

    /* Async: load texture + sample edge colors off main thread */
    GTask *task = g_task_new(NULL, ud->banner_cancel, on_banner_loaded, ud);
    g_task_set_task_data(task, banner_path, g_free);
    g_task_run_in_thread(task, load_image_thread);
    g_object_unref(task);
}

/* Wikipedia link button handler */
static void on_wiki_link_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UnifiedDetailData *ud = data;
    if (ud->about_wiki_url) {
        GtkUriLauncher *launcher = gtk_uri_launcher_new(ud->about_wiki_url);
        GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(ud->container));
        gtk_uri_launcher_launch(launcher,
                                GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL,
                                NULL, NULL, NULL);
        g_object_unref(launcher);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Navigation Stack
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nav_push(UnifiedDetailData *ud, NavEntryType type, int64_t id, const char *view_name) {
    if (ud->nav_depth >= MAX_NAV_STACK) {
        g_warning("nav_push: navigation stack overflow (depth=%d)", ud->nav_depth);
        return;
    }
    NavEntry *e = &ud->nav_stack[ud->nav_depth++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->id = id;
    e->view_name = view_name ? g_strdup(view_name) : NULL;
}

static NavEntry *nav_pop(UnifiedDetailData *ud) {
    if (ud->nav_depth <= 0) return NULL;
    return &ud->nav_stack[--ud->nav_depth];
}

static void nav_clear(UnifiedDetailData *ud) {
    for (int i = 0; i < ud->nav_depth; i++) {
        g_free(ud->nav_stack[i].view_name);
        g_free(ud->nav_stack[i].meta_artist_mbid);
        g_free(ud->nav_stack[i].meta_artist_name);
        g_free(ud->nav_stack[i].meta_artist_type);
    }
    ud->nav_depth = 0;
}

/* Build a display label for the current detail state (e.g., "Artist Detail - Daft Punk").
 * Caller must g_free() the result. */
static char *build_current_detail_label(UnifiedDetailData *ud) {
    if (ud->state == DETAIL_STATE_ARTIST) {
        const library_artist_info_t *artist = library_cache_get_artist(ud->cache, ud->current_id, ud->library_mask);
        return artist ? g_strdup_printf("Artist Detail - %s", artist->name)
                      : g_strdup("Artist Detail");
    } else if (ud->state == DETAIL_STATE_META_ARTIST) {
        return ud->meta_artist_name
            ? g_strdup_printf("Credits - %s", ud->meta_artist_name)
            : g_strdup("Credits");
    } else {
        const library_album_info_t *album = library_cache_get_album(ud->cache, ud->current_id, ud->library_mask);
        return album ? g_strdup_printf("Album Detail - %s", album->title)
                     : g_strdup("Album Detail");
    }
}

/* Update the back button label from the current top of the nav stack. */
static void update_back_label(UnifiedDetailData *ud) {
    if (ud->nav_depth > 0) {
        NavEntry *top = &ud->nav_stack[ud->nav_depth - 1];
        if (top->view_name) {
            char *label = g_strdup_printf("Back to %s", top->view_name);
            gtk_label_set_text(GTK_LABEL(ud->back_label), label);
            g_free(label);
        } else {
            gtk_label_set_text(GTK_LABEL(ud->back_label), "Back");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_unified_artist_link_clicked(GtkButton *btn, gpointer data);
/* Defined in this file, shared with credits_view.c (declared in library/internal.h) */
void on_album_card_artist_navigate(GtkButton *btn, gpointer data);
static void load_meta_artist_state(UnifiedDetailData *ud, const char *artist_mbid,
                                    const char *artist_name, const char *artist_type);

/* ═══════════════════════════════════════════════════════════════════════════
 * Album Card Signal Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * State Loading
 * ═══════════════════════════════════════════════════════════════════════════ */

static void load_artist_state(UnifiedDetailData *ud, int64_t artist_id);
static void populate_library_toggles(UnifiedDetailData *ud, GtkWidget *toggles_box,
                                      const library_album_info_t *album,
                                      int64_t active_album_id,
                                      GCallback toggle_cb, int64_t artist_id);

/* Callback for library toggle buttons in the single-album detail view. */
static void on_album_library_toggle(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (!gtk_toggle_button_get_active(btn))
        return;

    int64_t target_id = (int64_t)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(btn), "album-id"));
    if (target_id == ud->current_id)
        return;

    load_album_state(ud, target_id, 0);
}

/* Rebuild a single album card in-place within the artist detail view. */
static void rebuild_artist_album_card(UnifiedDetailData *ud,
                                       GtkWidget *card,
                                       int64_t new_album_id);

/* Callback for library toggle buttons on album cards inside the artist detail view.
 * Swaps the card in-place without navigating away from the artist view. */
static void on_artist_card_library_toggle(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (!gtk_toggle_button_get_active(btn))
        return;

    int64_t target_id = (int64_t)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(btn), "album-id"));

    /* Walk up to the album_card widget */
    GtkWidget *card = GTK_WIDGET(btn);
    while (card && gtk_widget_get_parent(card) != ud->artist_albums_container)
        card = gtk_widget_get_parent(card);
    if (!card) return;

    /* Check if already showing this version */
    int64_t current_card_id = (int64_t)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(card), "album-id"));
    if (current_card_id == target_id)
        return;

    rebuild_artist_album_card(ud, card, target_id);
}

/* In-place update of an album card's library-specific data.
 * No widgets are created or destroyed — just text, IDs, and artwork. */
static void rebuild_artist_album_card(UnifiedDetailData *ud,
                                       GtkWidget *card,
                                       int64_t new_album_id) {
    const library_album_info_t *album = library_cache_get_album(
        ud->cache, new_album_id, ud->library_mask);
    if (!album) return;

    /* Build (disc_num, track_num) → new track lookup */
    GPtrArray *new_tracks = library_cache_get_tracks_by_album(
        ud->cache, new_album_id, LIBRARY_MASK_ALL);

    GHashTable *track_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (new_tracks) {
        for (guint i = 0; i < new_tracks->len; i++) {
            const library_track_info_t *t = g_ptr_array_index(new_tracks, i);
            uint32_t key = ((uint32_t)t->disc_num << 16) | t->track_num;
            g_hash_table_insert(track_map, GUINT_TO_POINTER(key), (gpointer)t);
        }
    }

    /* Update track rows in-place */
    GtkWidget *track_list = find_widget_by_name(card, "track_list");
    if (track_list) {
        for (GtkWidget *row = gtk_widget_get_first_child(track_list);
             row; row = gtk_widget_get_next_sibling(row)) {
            if (!GTK_IS_LIST_BOX_ROW(row)) continue;
            GtkWidget *content = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
            if (!content) continue;

            guint disc = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(content), "disc-num"));
            guint tnum = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(content), "track-num"));
            uint32_t key = ((uint32_t)disc << 16) | tnum;

            const library_track_info_t *new_t = g_hash_table_lookup(
                track_map, GUINT_TO_POINTER(key));

            GtkWidget *dur_w = find_widget_by_name(content, "duration");
            GtkWidget *info_w = find_widget_by_name(content, "info_btn");
            GtkWidget *artists_w = find_widget_by_name(content, "artists_box");

            if (new_t) {
                /* Update IDs and library index */
                g_object_set_data(G_OBJECT(content), "track-id",
                                  GSIZE_TO_POINTER((gsize)new_t->track_id));
                g_object_set_data(G_OBJECT(content), "library-index",
                                  GSIZE_TO_POINTER((gsize)new_t->library_index));
                gtk_widget_remove_css_class(content, "missing-track");

                /* Restore duration text */
                if (dur_w) {
                    char buf[16];
                    ui_format_duration(new_t->duration_ms, buf, sizeof(buf));
                    gtk_label_set_text(GTK_LABEL(dur_w), buf);
                }
                if (info_w) gtk_widget_set_sensitive(info_w, TRUE);
                if (artists_w) gtk_widget_set_visible(artists_w, TRUE);
            } else {
                /* Track missing in this library version */
                g_object_set_data(G_OBJECT(content), "track-id", GSIZE_TO_POINTER(0));
                gtk_widget_add_css_class(content, "missing-track");

                if (dur_w)
                    gtk_label_set_text(GTK_LABEL(dur_w), "—");
                if (info_w) gtk_widget_set_sensitive(info_w, FALSE);
                if (artists_w) gtk_widget_set_visible(artists_w, FALSE);
            }
        }
    }

    g_hash_table_destroy(track_map);
    g_clear_pointer(&new_tracks, g_ptr_array_unref);

    /* Update album art */
    GtkWidget *art = find_widget_by_name(card, "card_art");
    if (art && ud->art_mgr)
        artwork_manager_get_fullsize_album_art(ud->art_mgr, new_album_id, art);

    /* Update album path button (per-library: different library roots) */
    GtkWidget *path_btn = find_widget_by_name(card, "card_path_btn");
    if (path_btn && album->first_track_id) {
        char *full_path = library_cache_resolve_track_path(ud->cache, album->first_track_id);
        char *dir = full_path ? g_path_get_dirname(full_path) : NULL;
        g_free(full_path);
        if (dir) {
            GtkWidget *path_label = gtk_button_get_child(GTK_BUTTON(path_btn));
            if (path_label)
                gtk_label_set_text(GTK_LABEL(path_label), dir);
            gtk_widget_set_tooltip_text(path_btn, dir);
            g_object_set_data_full(G_OBJECT(path_btn), "dir-path", dir, g_free);
            gtk_widget_set_visible(path_btn, TRUE);
        }
    }

    /* Update release info and label (per-library: different metadata DBs) */
    db_meta_release_t *meta_release = NULL;
    int lib_idx = album->library_index;
    if (lib_idx < 0) lib_idx = LIBRARY_GLOBAL_ID_LIB(album->album_id);
    if (album->musicbrainz_release_id && album->musicbrainz_release_id[0] && lib_idx >= 0) {
        quadrature_meta_db_t *meta_db = library_cache_get_dbs(ud->cache, lib_idx).meta;
        if (meta_db)
            db_meta_get_release(meta_db, album->musicbrainz_release_id, &meta_release);
    }

    GtkWidget *release_info = find_widget_by_name(card, "card_release_info");
    if (release_info) {
        const char *type = (meta_release && meta_release->release_type &&
                            meta_release->release_type[0])
                           ? meta_release->release_type : NULL;
        char *date_str = NULL;
        if (meta_release && meta_release->release_date)
            date_str = ui_format_release_date(meta_release->release_date);
        if (!date_str && album->year > 0)
            date_str = g_strdup_printf("%u", album->year);

        if (type && date_str) {
            char *combined = g_strdup_printf("%s \u00b7 %s", type, date_str);
            gtk_label_set_text(GTK_LABEL(release_info), combined);
            gtk_widget_set_visible(release_info, TRUE);
            g_free(combined);
        } else if (type) {
            gtk_label_set_text(GTK_LABEL(release_info), type);
            gtk_widget_set_visible(release_info, TRUE);
        } else if (date_str) {
            gtk_label_set_text(GTK_LABEL(release_info), date_str);
            gtk_widget_set_visible(release_info, TRUE);
        } else {
            gtk_widget_set_visible(release_info, FALSE);
        }
        g_free(date_str);
    }

    GtkWidget *label_w = find_widget_by_name(card, "card_label");
    if (label_w) {
        if (meta_release && meta_release->label && meta_release->label[0]) {
            char *label_text = g_strdup_printf("Label: %s", meta_release->label);
            gtk_label_set_text(GTK_LABEL(label_w), label_text);
            gtk_widget_set_visible(label_w, TRUE);
            g_free(label_text);
        } else {
            gtk_widget_set_visible(label_w, FALSE);
        }
    }

    db_meta_release_free(meta_release);

    /* Update track count stats (per-library: may have different tracks) */
    GtkWidget *stats = find_widget_by_name(card, "card_stats");
    if (stats) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u track%s",
                 album->track_count, album->track_count == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(stats), buf);
    }

    /* Update stored album-id on card */
    g_object_set_data(G_OBJECT(card), "album-id",
                      GSIZE_TO_POINTER((gsize)new_album_id));

    /* Re-populate library toggles to reflect the new active album */
    GtkWidget *card_toggles = find_widget_by_name(card, "card_library_toggles");
    if (card_toggles) {
        int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(
            g_object_get_data(G_OBJECT(card_toggles), "artist-id"));
        populate_library_toggles(ud, card_toggles, album, new_album_id,
                                  G_CALLBACK(on_artist_card_library_toggle), artist_id);
    }
}

/* Populate library toggle buttons for albums with same MBRID across libraries.
 * toggle_cb selects the behavior: on_album_library_toggle for album detail view,
 * on_artist_card_library_toggle for in-place swap in artist detail view.
 * artist_id is stored on each button for the artist card handler (ignored otherwise). */
static void populate_library_toggles(UnifiedDetailData *ud, GtkWidget *toggles_box,
                                      const library_album_info_t *album,
                                      int64_t active_album_id,
                                      GCallback toggle_cb,
                                      int64_t artist_id) {
    /* Clear any previous toggle buttons */
    GtkWidget *child = gtk_widget_get_first_child(toggles_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(toggles_box), child);
        child = next;
    }

    GPtrArray *versions = library_cache_get_albums(ud->cache, album->album_id,
                                                    ud->library_mask, -1);
    if (!versions || versions->len <= 1) {
        g_clear_pointer(&versions, g_ptr_array_unref);
        gtk_widget_set_visible(toggles_box, FALSE);
        return;
    }

    GtkToggleButton *group = NULL;
    for (guint i = 0; i < versions->len; i++) {
        const library_album_info_t *ver = g_ptr_array_index(versions, i);
        int lib_idx = ver->library_index;
        const char *name = library_cache_get_library_name(ud->cache, lib_idx);
        if (!name) continue;

        GtkWidget *btn = gtk_toggle_button_new_with_label(name);
        gtk_widget_add_css_class(btn, "library-toggle-btn");

        g_object_set_data(G_OBJECT(btn), "album-id",
                          GSIZE_TO_POINTER((gsize)ver->album_id));
        g_object_set_data(G_OBJECT(btn), "artist-id",
                          GSIZE_TO_POINTER((gsize)artist_id));

        if (ver->album_id == active_album_id)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);

        if (group)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), group);
        else
            group = GTK_TOGGLE_BUTTON(btn);

        g_signal_connect(btn, "toggled", toggle_cb, ud);
        gtk_box_append(GTK_BOX(toggles_box), btn);
    }

    /* Store artist_id on the container for rebuild_artist_album_card retrieval */
    g_object_set_data(G_OBJECT(toggles_box), "artist-id",
                      GSIZE_TO_POINTER((gsize)artist_id));

    gtk_widget_set_visible(toggles_box, TRUE);
    g_ptr_array_unref(versions);
}

static void load_album_state(UnifiedDetailData *ud, int64_t album_id, int64_t select_track_id) {
    ud->state = DETAIL_STATE_ALBUM;
    ud->current_id = album_id;

    /* No artist suppression in album view */
    ud->cbs.artist_cbs.suppress_id = 0;
    ud->cbs.artist_cbs.suppress_mbid = NULL;

    /* Clear artist name from header bar — album view does not show it */
    gtk_label_set_text(GTK_LABEL(ud->header_artist_name), "");
    sync_spacer_heights(ud);

    /* Get album info and tracks */
    const library_album_info_t *album = library_cache_get_album(ud->cache, album_id, ud->library_mask);
    if (!album)
        return;

    ud->merged_rep_album_id = album_id;

    GPtrArray *tracks = library_cache_get_tracks_by_album(ud->cache, album_id, LIBRARY_MASK_ALL);
    if (!tracks || tracks->len == 0) {
        g_clear_pointer(&tracks, g_ptr_array_unref);
        return;
    }

    /* Prefetch audio files for instant playback */
    if (tracks->len > 0) {
        int64_t *track_ids = g_new(int64_t, tracks->len);
        for (guint i = 0; i < tracks->len; i++) {
            const library_track_info_t *t = g_ptr_array_index(tracks, i);
            track_ids[i] = t->track_id;
        }
        library_cache_prefetch_audio_files(ud->cache, track_ids, tracks->len);
        g_free(track_ids);
    }

    /* Clear inner container and create album card */
    ui_box_clear(GTK_BOX(ud->album_card_inner));

    GtkWidget *card = ui_create_album_detail_card(album, tracks, ud->cache, ud->art_mgr, 0,
                                                   (RowCallbacks *)&ud->cbs.album_track_cbs,
                                                   (RowCallbacks *)&ud->cbs.artist_cbs);
    gtk_box_append(GTK_BOX(ud->album_card_inner), card);

    /* Wire info button handlers for track rows */
    wire_info_buttons(card, ud);

    /* Populate library toggles for merged albums */
    GtkWidget *toggles_box = find_widget_by_name(card, "card_library_toggles");
    if (toggles_box)
        populate_library_toggles(ud, toggles_box, album, album_id,
                                  G_CALLBACK(on_album_library_toggle), 0);

    /* Find and connect artist link button */
    GtkWidget *artist_link = find_widget_by_name(card, "card_artist_link");
    if (artist_link) {
        if (ui_is_various_artists(album->artist_name)) {
            gtk_widget_set_sensitive(artist_link, FALSE);
        } else {
            ud->album_artist_id = album->artist_id;
            g_signal_connect(artist_link, "clicked", G_CALLBACK(on_unified_artist_link_clicked), ud);
        }
    }

    /* Find track list for selection handling */
    GtkWidget *track_list = find_widget_by_name(card, "track_list");
    if (track_list && select_track_id > 0) {
        /* Find and select the requested track */
        GtkWidget *child = gtk_widget_get_first_child(track_list);
        while (child) {
            if (GTK_IS_LIST_BOX_ROW(child)) {
                GtkWidget *content = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
                if (content) {
                    int64_t track_id = (int64_t)GPOINTER_TO_SIZE(
                        g_object_get_data(G_OBJECT(content), "track-id"));
                    if (track_id == select_track_id) {
                        gtk_list_box_select_row(GTK_LIST_BOX(track_list), GTK_LIST_BOX_ROW(child));
                        gtk_widget_grab_focus(child);
                        break;
                    }
                }
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }

    g_ptr_array_unref(tracks);

    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "album");
    gtk_widget_set_visible(ud->back_header, TRUE);
}


/* Callback for library toggle buttons in artist detail view. */
static void on_artist_library_toggle_clicked(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (!gtk_toggle_button_get_active(btn))
        return;

    int64_t target_id = (int64_t)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(btn), "artist-id"));
    if (target_id == ud->current_id)
        return;

    load_artist_state(ud, target_id);
}

/* Populate library toggle buttons for artists with same MBID across libraries. */
static void populate_artist_library_toggles(UnifiedDetailData *ud, int64_t active_artist_id) {
    GtkWidget *toggles_box = ud->artist_library_toggles;

    /* Clear previous toggle buttons */
    GtkWidget *child = gtk_widget_get_first_child(toggles_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(toggles_box), child);
        child = next;
    }

    GPtrArray *versions = library_cache_get_artists(ud->cache, active_artist_id,
                                                     ud->library_mask, -1);
    if (!versions || versions->len <= 1) {
        g_clear_pointer(&versions, g_ptr_array_unref);
        gtk_widget_set_visible(toggles_box, FALSE);
        return;
    }

    GtkToggleButton *group = NULL;
    for (guint i = 0; i < versions->len; i++) {
        const library_artist_info_t *ver = g_ptr_array_index(versions, i);
        int lib_idx = ver->library_index;
        const char *name = library_cache_get_library_name(ud->cache, lib_idx);
        if (!name) continue;

        GtkWidget *btn = gtk_toggle_button_new_with_label(name);
        gtk_widget_add_css_class(btn, "library-toggle-btn");

        g_object_set_data(G_OBJECT(btn), "artist-id",
                          GSIZE_TO_POINTER((gsize)ver->artist_id));

        if (ver->artist_id == active_artist_id)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);

        if (group)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), group);
        else
            group = GTK_TOGGLE_BUTTON(btn);

        g_signal_connect(btn, "toggled", G_CALLBACK(on_artist_library_toggle_clicked), ud);
        gtk_box_append(GTK_BOX(toggles_box), btn);
    }

    gtk_widget_set_visible(toggles_box, TRUE);
    g_ptr_array_unref(versions);
}

/* Sort albums by year descending (latest first), title as tiebreaker. */
static int detail_album_year_desc_cmp(gconstpointer a, gconstpointer b) {
    const library_album_info_t *aa = *(const library_album_info_t *const *)a;
    const library_album_info_t *bb = *(const library_album_info_t *const *)b;
    int cmp = (int)bb->year - (int)aa->year;
    return cmp != 0 ? cmp : g_utf8_collate(aa->title, bb->title);
}

static void load_artist_state(UnifiedDetailData *ud, int64_t artist_id) {
    ud->state = DETAIL_STATE_ARTIST;
    ud->current_id = artist_id;
    ud->merged_rep_album_id = 0;

    /* Suppress buttons for the artist we're viewing */
    ud->cbs.artist_cbs.suppress_id = artist_id;
    ud->cbs.artist_cbs.suppress_mbid = NULL;

    /* Reset sections */
    gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
    gtk_widget_set_visible(ud->appears_on_section, FALSE);
    gtk_widget_set_visible(ud->about_section, FALSE);

    GPtrArray *albums = library_cache_get_albums_by_artist(ud->cache, artist_id, ud->library_mask);
    if (albums && albums->len > 1)
        g_ptr_array_sort(albums, (GCompareFunc)detail_album_year_desc_cmp);
    GPtrArray *appearance_albums = library_cache_get_artist_appearances(ud->cache, artist_id, ud->library_mask);
    GPtrArray *appearance_tracks = library_cache_get_artist_appearance_tracks(ud->cache, artist_id, ud->library_mask);

    gboolean has_albums = albums && albums->len > 0;
    gboolean has_appearances = (appearance_albums && appearance_albums->len > 0) ||
                               (appearance_tracks && appearance_tracks->len > 0);

    /* MBID lookup for art/bio/credits — resolve from any library (MBID is universal) */
    const library_artist_info_t *artist_check = library_cache_get_artist(ud->cache, artist_id, LIBRARY_MASK_ALL);
    gboolean may_have_credits = artist_check && artist_check->musicbrainz_id && ud->settings;

    if (!has_albums && !has_appearances && !may_have_credits) {
        g_clear_pointer(&albums, g_ptr_array_unref);
        g_clear_pointer(&appearance_albums, g_ptr_array_unref);
        g_clear_pointer(&appearance_tracks, g_ptr_array_unref);
        return;
    }

    /* Resolve artist name: prefer own albums, fall back to cache lookup */
    const char *artist_name = NULL;
    if (has_albums) {
        const library_album_info_t *first_album = g_ptr_array_index(albums, 0);
        artist_name = first_album->artist_name;
    } else {
        const library_artist_info_t *artist = library_cache_get_artist(ud->cache, artist_id, ud->library_mask);
        if (artist)
            artist_name = artist->name;
    }
    gtk_label_set_text(GTK_LABEL(ud->header_artist_name), artist_name ? artist_name : "Unknown Artist");
    sync_spacer_heights(ud);

    /* Library toggles for multi-library artists */
    populate_artist_library_toggles(ud, artist_id);

    /* Prefetch audio files from first own album for instant playback */
    if (has_albums && ud->cache) {
        const library_album_info_t *first_album = g_ptr_array_index(albums, 0);
        GPtrArray *first_tracks = library_cache_get_tracks_by_album(ud->cache, first_album->album_id, LIBRARY_MASK_ALL);
        if (first_tracks && first_tracks->len > 0) {
            int64_t *track_ids = g_new(int64_t, first_tracks->len);
            for (guint i = 0; i < first_tracks->len; i++) {
                const library_track_info_t *t = g_ptr_array_index(first_tracks, i);
                track_ids[i] = t->track_id;
            }
            library_cache_prefetch_audio_files(ud->cache, track_ids, first_tracks->len);
            g_free(track_ids);
        }
        g_clear_pointer(&first_tracks, g_ptr_array_unref);
    }

    guint album_count = has_albums ? albums->len : 0;

    /* Compute stats: total tracks and duration across own albums */
    {
        guint total_tracks = 0;
        int64_t total_ms = 0;
        for (guint i = 0; i < album_count; i++) {
            const library_album_info_t *al = g_ptr_array_index(albums, i);
            GPtrArray *trks = library_cache_get_tracks_by_album(ud->cache, al->album_id, LIBRARY_MASK_ALL);
            if (!trks) continue;
            total_tracks += trks->len;
            for (guint j = 0; j < trks->len; j++) {
                const library_track_info_t *t = g_ptr_array_index(trks, j);
                total_ms += t->duration_ms;
            }
            g_ptr_array_unref(trks);
        }
        char stats[128];
        int total_secs = (int)(total_ms / 1000);
        int hours = total_secs / 3600;
        int mins  = (total_secs % 3600) / 60;
        if (album_count > 0 && hours > 0)
            snprintf(stats, sizeof(stats), "%u album%s \u00b7 %u track%s \u00b7 %dh %dm",
                     album_count, album_count == 1 ? "" : "s",
                     total_tracks, total_tracks == 1 ? "" : "s",
                     hours, mins);
        else if (album_count > 0)
            snprintf(stats, sizeof(stats), "%u album%s \u00b7 %u track%s \u00b7 %dm",
                     album_count, album_count == 1 ? "" : "s",
                     total_tracks, total_tracks == 1 ? "" : "s",
                     mins);
        else
            stats[0] = '\0';
        gtk_label_set_text(GTK_LABEL(ud->artist_stats), stats);
        gtk_widget_set_visible(ud->artist_stats, stats[0] != '\0');
    }

    /* Load artist banner and bio if artist has MBID */
    if (artist_check && artist_check->musicbrainz_id) {
        load_artist_banner(ud, artist_check->musicbrainz_id);
        load_artist_bio(ud, artist_check->musicbrainz_id);
    }

    /* Reset selection group before clearing widgets (stale pointers) */
    ui_selection_group_free(ud->sel_group);
    ud->sel_group = ui_selection_group_new();

    /* Populate own albums */
    ui_box_clear(GTK_BOX(ud->artist_albums_container));
    gtk_widget_set_visible(ud->albums_section, album_count > 0);

    for (guint i = 0; i < album_count; i++) {
        const library_album_info_t *album = g_ptr_array_index(albums, i);
        GPtrArray *tracks = library_cache_get_tracks_by_album(ud->cache, album->album_id, LIBRARY_MASK_ALL);
        if (tracks && tracks->len > 0) {
            GtkWidget *card = ui_create_album_detail_card(album, tracks, ud->cache, ud->art_mgr, 0,
                                                           (RowCallbacks *)&ud->cbs.album_track_cbs,
                                                           (RowCallbacks *)&ud->cbs.artist_cbs);

            /* Find artist link and configure it */
            GtkWidget *artist_link = find_widget_by_name(card, "card_artist_link");
            if (artist_link && (album->artist_id == artist_id ||
                                ui_is_various_artists(album->artist_name))) {
                /* Current artist or synthetic "Various Artists" - make button inactive */
                gtk_widget_set_sensitive(artist_link, FALSE);
            } else if (artist_link) {
                /* Different artist - connect navigation handler */
                g_signal_connect(artist_link, "clicked", G_CALLBACK(on_album_card_artist_navigate), ud);
            }

            /* Populate library toggles for multi-library albums */
            GtkWidget *card_toggles = find_widget_by_name(card, "card_library_toggles");
            if (card_toggles)
                populate_library_toggles(ud, card_toggles, album, album->album_id,
                                          G_CALLBACK(on_artist_card_library_toggle), artist_id);

            g_object_set_data(G_OBJECT(card), "album-id",
                              GSIZE_TO_POINTER((gsize)album->album_id));
            wire_info_buttons(card, ud);

            /* Add track list to selection group for mutual-exclusion */
            GtkWidget *tl = find_widget_by_name(card, "track_list");
            if (tl)
                ui_selection_group_add(ud->sel_group, GTK_LIST_BOX(tl));

            gtk_box_append(GTK_BOX(ud->artist_albums_container), card);
        }
        g_clear_pointer(&tracks, g_ptr_array_unref);
    }

    /* Populate "Appears On" section */
    gtk_list_box_remove_all(GTK_LIST_BOX(ud->appears_on_albums));
    gtk_list_box_remove_all(GTK_LIST_BOX(ud->appears_on_tracks));

    /* Build skip sets for credit dedup */
    GHashTable *skip_track_ids = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *skip_album_ids = g_hash_table_new(g_direct_hash, g_direct_equal);

    /* Skip all tracks from own albums */
    for (guint i = 0; i < album_count; i++) {
        const library_album_info_t *album = g_ptr_array_index(albums, i);
        g_hash_table_add(skip_album_ids, GSIZE_TO_POINTER((gsize)album->album_id));
        GPtrArray *atracks = library_cache_get_tracks_by_album(ud->cache, album->album_id, LIBRARY_MASK_ALL);
        if (atracks) {
            for (guint j = 0; j < atracks->len; j++) {
                const library_track_info_t *t = g_ptr_array_index(atracks, j);
                g_hash_table_add(skip_track_ids, GSIZE_TO_POINTER((gsize)t->track_id));
            }
            g_ptr_array_unref(atracks);
        }
    }

    /* Skip existing appearance tracks/albums */
    if (appearance_tracks) {
        for (guint i = 0; i < appearance_tracks->len; i++) {
            const library_track_info_t *t = g_ptr_array_index(appearance_tracks, i);
            g_hash_table_add(skip_track_ids, GSIZE_TO_POINTER((gsize)t->track_id));
        }
    }
    if (appearance_albums) {
        for (guint i = 0; i < appearance_albums->len; i++) {
            const library_album_info_t *a = g_ptr_array_index(appearance_albums, i);
            g_hash_table_add(skip_album_ids, GSIZE_TO_POINTER((gsize)a->album_id));
        }
    }

    /* Shared size groups across normal + credit rows */
    UiRowSizeGroups album_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
        .col2 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)
    };
    UiRowSizeGroups track_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
    };

    /* Pre-collect MB credit roles per album so cache appearance rows can
     * be annotated with role pills (Vocal, Producer, etc.) in one pass. */
    const library_artist_info_t *artist_info = library_cache_get_artist(ud->cache, artist_id, LIBRARY_MASK_ALL);
    GHashTable *credit_album_roles = NULL;  /* album_id → GPtrArray<char*> */
    if (artist_info && artist_info->musicbrainz_id && ud->settings) {
        credit_album_roles = collect_credit_album_roles(
            ud, artist_info->musicbrainz_id,
            artist_name ? artist_name : "Unknown Artist",
            artist_id, skip_track_ids);
    }

    /* Populate appearance album rows, annotated with credit roles if available */
    if (appearance_albums && appearance_albums->len > 0) {
        for (guint i = 0; i < appearance_albums->len; i++) {
            const library_album_info_t *album = g_ptr_array_index(appearance_albums, i);

            /* Look up credit roles for this album */
            GPtrArray *roles = credit_album_roles
                ? g_hash_table_lookup(credit_album_roles,
                                      GSIZE_TO_POINTER((gsize)album->album_id))
                : NULL;

            /* Build credit annotation if roles found */
            UiAlbumCreditInfo acredit;
            const UiAlbumCreditInfo *credit_ptr = NULL;
            if (roles && roles->len > 0) {
                g_ptr_array_add(roles, NULL);  /* sentinel for roles array */
                acredit = (UiAlbumCreditInfo){
                    .artist_name = artist_name,
                    .artist_id   = artist_id,
                    .roles       = (const char *const *)roles->pdata,
                    .role_count  = roles->len - 1,
                };
                credit_ptr = &acredit;
            }

            GtkWidget *row = ui_create_album_row(album, ud->cache, ud->art_mgr, TRUE,
                                                   (RowCallbacks *)&ud->cbs.artist_cbs,
                                                   &album_groups, credit_ptr);
            ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.album_cbs);
            gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_albums), row);

            /* Album is already shown — add to skip set */
        }
    }

    /* Populate normal appearance track rows */
    if (appearance_tracks && appearance_tracks->len > 0) {
        for (guint i = 0; i < appearance_tracks->len; i++) {
            const library_track_info_t *track = g_ptr_array_index(appearance_tracks, i);
            GtkWidget *row = ui_create_track_row(track, ud->cache, ud->art_mgr, TRUE,
                                                   (RowCallbacks *)&ud->cbs.artist_cbs,
                                                   (RowCallbacks *)&ud->cbs.album_cbs,
                                                   &track_groups, NULL);
            ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.track_cbs);
            gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_tracks), row);
        }
    }

    /* Append MB credit rows for albums/tracks NOT already shown */
    guint credit_count = 0;
    if (artist_info && artist_info->musicbrainz_id && ud->settings) {
        credit_count = append_credit_rows(ud, artist_info->musicbrainz_id,
                                           artist_name ? artist_name : "Unknown Artist",
                                           artist_id, skip_track_ids, skip_album_ids,
                                           &track_groups, &album_groups);
    }

    if (credit_album_roles)
        g_hash_table_destroy(credit_album_roles);
    g_object_unref(track_groups.col1);
    g_object_unref(album_groups.col1);
    g_object_unref(album_groups.col2);
    g_hash_table_destroy(skip_track_ids);
    g_hash_table_destroy(skip_album_ids);

    gboolean has_appear_tracks = (appearance_tracks && appearance_tracks->len > 0) ||
                                  credit_count > 0;
    /* Check if any album rows were added (normal + credit) */
    gboolean has_appear_albums = (appearance_albums && appearance_albums->len > 0) ||
                                  (gtk_widget_get_first_child(ud->appears_on_albums) != NULL);
    has_appearances = has_appear_albums || has_appear_tracks;

    if (has_appearances) {
        /* If only tracks (no albums), switch directly to tracks view */
        if (!has_appear_albums) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_tracks_btn), TRUE);
        }
    }

    /* Show appears_on if we have content */
    if (has_appearances)
        gtk_widget_set_visible(ud->appears_on_section, TRUE);

    /* Add appears-on tracks to the selection group (not albums — album lists
     * are not track lists, so hotkeys and selection deconfliction don't apply) */
    ui_selection_group_add(ud->sel_group, GTK_LIST_BOX(ud->appears_on_tracks));

    /* Clear any leftover selections from a previous visit so GTK does not
     * auto-focus a list row and scroll away from the top. */
    GtkWidget *album_child = gtk_widget_get_first_child(ud->artist_albums_container);
    while (album_child) {
        GtkWidget *tl = find_widget_by_name(album_child, "track_list");
        if (tl) gtk_list_box_unselect_all(GTK_LIST_BOX(tl));
        album_child = gtk_widget_get_next_sibling(album_child);
    }
    gtk_list_box_unselect_all(GTK_LIST_BOX(ud->appears_on_albums));
    gtk_list_box_unselect_all(GTK_LIST_BOX(ud->appears_on_tracks));

    /* Scroll to top and hand focus to the scroll container (not a row) */
    GtkWidget *artist_scroll = gtk_stack_get_child_by_name(GTK_STACK(ud->content_stack), "artist");
    if (artist_scroll) {
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(
                                     GTK_SCROLLED_WINDOW(artist_scroll)), 0.0);
        gtk_widget_grab_focus(artist_scroll);
    }

    g_clear_pointer(&albums, g_ptr_array_unref);
    g_clear_pointer(&appearance_albums, g_ptr_array_unref);
    g_clear_pointer(&appearance_tracks, g_ptr_array_unref);

    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "artist");
    gtk_widget_set_visible(ud->back_header, TRUE);

}

/* ═══════════════════════════════════════════════════════════════════════════
 * Click Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_unified_back_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UnifiedDetailData *ud = data;
    if (!library_unified_detail_go_back(ud->container)) {
        /* Internal nav exhausted - notify caller to handle navigation */
        if (ud->cbs.on_back)
            ud->cbs.on_back(ud->cbs.user_data);
    }
}

static void on_unified_artist_link_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UnifiedDetailData *ud = data;
    if (ud->album_artist_id > 0) {
        library_unified_detail_navigate_to_artist(ud->container, ud->album_artist_id, NULL);
    }
}


void on_album_card_artist_navigate(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "artist-id"));
    if (artist_id > 0) {
        library_unified_detail_navigate_to_artist(ud->container, artist_id, NULL);
    }
}

/* Called when a revealer finishes its hide animation.
 * Chains: hide completes → set slow duration on target → reveal target. */
static void on_revealer_hidden(GtkRevealer *revealer, GParamSpec *pspec G_GNUC_UNUSED, gpointer data) {
    if (gtk_revealer_get_child_revealed(revealer))
        return;  /* Only act on hide completion */

    UnifiedDetailData *ud = data;
    /* Determine which revealer to show */
    GtkWidget *target = (GTK_WIDGET(revealer) == ud->appears_on_albums_revealer)
        ? ud->appears_on_tracks_revealer
        : ud->appears_on_albums_revealer;

    /* Restore slow duration for the reveal */
    gtk_revealer_set_transition_duration(GTK_REVEALER(target), 250);
    gtk_revealer_set_reveal_child(GTK_REVEALER(target), TRUE);
}

static void on_toggle_albums(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_tracks_btn), FALSE);
        /* If tracks revealer is currently shown, hide it fast — callback will show albums */
        if (gtk_revealer_get_child_revealed(GTK_REVEALER(ud->appears_on_tracks_revealer))) {
            gtk_revealer_set_transition_duration(GTK_REVEALER(ud->appears_on_tracks_revealer), 150);
            gtk_revealer_set_reveal_child(GTK_REVEALER(ud->appears_on_tracks_revealer), FALSE);
        } else {
            /* Tracks already hidden (e.g. initial state), just show albums */
            gtk_revealer_set_transition_duration(GTK_REVEALER(ud->appears_on_albums_revealer), 250);
            gtk_revealer_set_reveal_child(GTK_REVEALER(ud->appears_on_albums_revealer), TRUE);
        }
    }
}

static void on_toggle_tracks(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_albums_btn), FALSE);
        /* If albums revealer is currently shown, hide it fast — callback will show tracks */
        if (gtk_revealer_get_child_revealed(GTK_REVEALER(ud->appears_on_albums_revealer))) {
            gtk_revealer_set_transition_duration(GTK_REVEALER(ud->appears_on_albums_revealer), 150);
            gtk_revealer_set_reveal_child(GTK_REVEALER(ud->appears_on_albums_revealer), FALSE);
        } else {
            /* Albums already hidden, just show tracks */
            gtk_revealer_set_transition_duration(GTK_REVEALER(ud->appears_on_tracks_revealer), 250);
            gtk_revealer_set_reveal_child(GTK_REVEALER(ud->appears_on_tracks_revealer), TRUE);
        }
    }
}

/**
 * Select and focus the first selectable row in a GtkListBox.
 * Skips non-selectable rows (e.g., section headers, disc headers).
 * Returns TRUE if a row was selected.
 */
static gboolean list_box_select_first(GtkListBox *list_box) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list_box));
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child) &&
            gtk_list_box_row_get_selectable(GTK_LIST_BOX_ROW(child))) {
            gtk_list_box_select_row(list_box, GTK_LIST_BOX_ROW(child));
            gtk_widget_grab_focus(child);
            return TRUE;
        }
        child = gtk_widget_get_next_sibling(child);
    }
    return FALSE;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Page Album Navigation Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Returns the track_list GtkListBox for the nth album card (0-based) in
 * artist_albums_container, or NULL if the index is out of range. */
static GtkListBox *artist_get_album_track_list(UnifiedDetailData *ud, int index) {
    GtkWidget *child = gtk_widget_get_first_child(ud->artist_albums_container);
    int i = 0;
    while (child) {
        GtkWidget *tl = find_widget_by_name(child, "track_list");
        if (tl) {
            if (i == index) return GTK_LIST_BOX(tl);
            i++;
        }
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
}

/* Returns the 0-based index of the album whose track_list has a selected row,
 * or -1 if no album track list has a selection. */
static int artist_get_selected_album_index(UnifiedDetailData *ud) {
    GtkWidget *child = gtk_widget_get_first_child(ud->artist_albums_container);
    int i = 0;
    while (child) {
        GtkWidget *tl = find_widget_by_name(child, "track_list");
        if (tl) {
            if (gtk_list_box_get_selected_row(GTK_LIST_BOX(tl)) != NULL)
                return i;
            i++;
        }
        child = gtk_widget_get_next_sibling(child);
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Detail View Key Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Key handler for the detail view container.
 *
 * Album view:
 *   Down        → enter first track if nothing selected
 *
 * Artist view:
 *   Down        → enter first track of first album if nothing selected
 *   Ctrl+Down   → first track of next album (wraps)
 *   Ctrl+Up     → first track of previous album (wraps)
 *
 * Tab is intentionally not handled here — GTK uses it to navigate
 * between focusable buttons within the currently selected row.
 */
static gboolean on_detail_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                       guint keycode, GdkModifierType state,
                                       gpointer data) {
    (void)ctl; (void)keycode;
    UnifiedDetailData *ud = data;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;

    /* Down (no modifier): enter first track if nothing is selected */
    if ((keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) && !ctrl) {
        if (ud->state == DETAIL_STATE_ALBUM) {
            GtkWidget *track_list = library_unified_detail_get_track_list(ud->container);
            if (track_list &&
                gtk_list_box_get_selected_row(GTK_LIST_BOX(track_list)) == NULL) {
                list_box_select_first(GTK_LIST_BOX(track_list));
                return TRUE;
            }
        } else if (ud->state == DETAIL_STATE_ARTIST) {
            if (artist_get_selected_album_index(ud) < 0) {
                GtkListBox *tl = artist_get_album_track_list(ud, 0);
                if (tl) {
                    list_box_select_first(tl);
                    return TRUE;
                }
            }
        }
        return FALSE;
    }

    /* Ctrl+Down / Ctrl+Up: jump between albums on artist page */
    if (ctrl && (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down ||
                 keyval == GDK_KEY_Up   || keyval == GDK_KEY_KP_Up)) {
        if (ud->state == DETAIL_STATE_ARTIST) {
            gboolean going_down = (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down);
            int current = artist_get_selected_album_index(ud);
            int next = current + (going_down ? 1 : -1);
            GtkListBox *tl = artist_get_album_track_list(ud, next);
            if (!tl) {
                /* Wrap: past the end → album 0, before the start → last album */
                if (going_down) {
                    tl = artist_get_album_track_list(ud, 0);
                } else {
                    for (int i = 0; ; i++) {
                        GtkListBox *candidate = artist_get_album_track_list(ud, i);
                        if (!candidate) break;
                        tl = candidate;
                    }
                }
            }
            if (tl) {
                list_box_select_first(tl);
                return TRUE;
            }
        }
        return FALSE;
    }

    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Widget Builders
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Sync spacer heights to back_header.  Called:
 *  1. Proactively after every header text change (before GTK's measure pass)
 *  2. Reactively via notify::height (catches allocation-driven changes)
 * Uses gtk_widget_measure to predict height from current text, so the
 * spacer's size request is correct DURING measure, not one frame late. */
static void sync_spacer_heights(UnifiedDetailData *ud) {
    int min_h, nat_h;
    gtk_widget_measure(ud->back_header, GTK_ORIENTATION_VERTICAL, -1,
                       &min_h, &nat_h, NULL, NULL);
    if (nat_h <= 0) return;
    if (ud->album_header_spacer)
        gtk_widget_set_size_request(ud->album_header_spacer, -1, nat_h);
    if (ud->artist_header_spacer)
        gtk_widget_set_size_request(ud->artist_header_spacer, -1, nat_h);
}

static void on_back_header_height_changed(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)obj; (void)pspec;
    sync_spacer_heights(data);
}

static GtkWidget *build_album_page(UnifiedDetailData *ud) {
    /* Create scrolled window programmatically */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    
    /* Outer container: holds the spacer + inner card box. Never cleared. */
    ud->album_card_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ud->album_card_container, "album-card-container");

    /* Invisible spacer: height synced to back_header via notify::height,
     * pushing album art below the overlay.
     * Lives in the outer container so ui_box_clear(album_card_inner) cannot remove it. */
    ud->album_header_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(ud->album_card_container), ud->album_header_spacer);

    /* Inner container: cleared and repopulated on each album load. */
    ud->album_card_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(ud->album_card_container), ud->album_card_inner);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ud->album_card_container);
    
    /* Enable scroll-to-focus on viewport */
    GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    if (GTK_IS_VIEWPORT(viewport)) {
        gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), TRUE);
    }
    
    /* Smooth scroll for album detail */
    ui_smooth_scroll_attach(GTK_SCROLLED_WINDOW(scroll));

    /* Key controller for navigation */
    GtkEventController *key_ctl = gtk_event_controller_key_new();
    g_signal_connect(key_ctl, "key-pressed", G_CALLBACK(on_detail_key_pressed), ud);
    gtk_widget_add_controller(scroll, key_ctl);
    
    return scroll;
}

static GtkWidget *build_artist_page(UnifiedDetailData *ud) {
    /* Ensure custom types are registered before builder parses the template */
    g_type_ensure(quad_gradient_fade_get_type());

    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/artist_detail_page.ui");

    /* Get the root scroll window and all child widgets */
    GtkWidget *scroll = GTK_WIDGET(gtk_builder_get_object(builder, "artist_page_scroll"));
    g_assert(scroll != NULL);  /* Template must exist */

    /* Smooth scroll for artist detail */
    ui_smooth_scroll_attach(GTK_SCROLLED_WINDOW(scroll));

    /* Store references to widgets we need to update dynamically */
    ud->artist_name = GTK_WIDGET(gtk_builder_get_object(builder, "artist_name"));
    ud->artist_stats = GTK_WIDGET(gtk_builder_get_object(builder, "artist_stats"));
    ud->artist_library_toggles = GTK_WIDGET(gtk_builder_get_object(builder, "artist_library_toggles"));
    ud->albums_section = GTK_WIDGET(gtk_builder_get_object(builder, "albums_section"));
    ud->artist_albums_container = GTK_WIDGET(gtk_builder_get_object(builder, "artist_albums_container"));
    ud->appears_on_section = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_section"));
    ud->appears_on_albums_revealer = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_albums_revealer"));
    ud->appears_on_tracks_revealer = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_tracks_revealer"));
    ud->appears_on_albums = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_albums"));
    ud->appears_on_tracks = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_tracks"));
    ud->toggle_albums_btn = GTK_WIDGET(gtk_builder_get_object(builder, "toggle_albums_btn"));
    ud->toggle_tracks_btn = GTK_WIDGET(gtk_builder_get_object(builder, "toggle_tracks_btn"));

    /* About section widgets */
    ud->about_section = GTK_WIDGET(gtk_builder_get_object(builder, "about_section"));
    ud->about_background_image = GTK_WIDGET(gtk_builder_get_object(builder, "about_background_image"));
    ud->about_bio_text = GTK_WIDGET(gtk_builder_get_object(builder, "about_bio_text"));
    ud->about_wiki_link = GTK_WIDGET(gtk_builder_get_object(builder, "about_wiki_link"));

    /* Artist banner (full-width image above header, hidden if no banner art) */
    ud->artist_banner_overlay = GTK_WIDGET(gtk_builder_get_object(builder, "artist_banner_overlay"));
    ud->artist_banner = GTK_WIDGET(gtk_builder_get_object(builder, "artist_banner"));
    ud->artist_banner_fade_bottom = (QuadGradientFade *)gtk_builder_get_object(builder, "artist_banner_fade_bottom");

    /* About section gradient overlays */
    ud->about_fade_top = (QuadGradientFade *)gtk_builder_get_object(builder, "about_fade_top");
    ud->about_fade_bottom = (QuadGradientFade *)gtk_builder_get_object(builder, "about_fade_bottom");

    /* Sanity check: all widgets must be present in template */
    g_assert(ud->artist_name != NULL);
    g_assert(ud->artist_stats != NULL);
    g_assert(ud->albums_section != NULL);
    g_assert(ud->artist_albums_container != NULL);
    g_assert(ud->appears_on_section != NULL);
    g_assert(ud->appears_on_albums_revealer != NULL);
    g_assert(ud->appears_on_tracks_revealer != NULL);
    g_assert(ud->appears_on_albums != NULL);
    g_assert(ud->appears_on_tracks != NULL);
    g_assert(ud->toggle_albums_btn != NULL);
    g_assert(ud->toggle_tracks_btn != NULL);
    g_assert(ud->about_section != NULL);
    g_assert(ud->about_background_image != NULL);
    g_assert(ud->about_bio_text != NULL);
    g_assert(ud->about_wiki_link != NULL);
    g_assert(ud->artist_banner_overlay != NULL);
    g_assert(ud->artist_banner != NULL);
    g_assert(ud->artist_banner_fade_bottom != NULL);
    g_assert(ud->about_fade_top != NULL);
    g_assert(ud->about_fade_bottom != NULL);

    /* Connect about section signals */
    g_signal_connect(ud->about_wiki_link, "clicked", G_CALLBACK(on_wiki_link_clicked), ud);

    /* Connect row-activated signals for appears-on list boxes */
    g_signal_connect(ud->appears_on_albums, "row-activated",
                     G_CALLBACK(ui_list_box_row_activated), NULL);
    g_signal_connect(ud->appears_on_tracks, "row-activated",
                     G_CALLBACK(ui_list_box_row_activated), NULL);

    /* Selection deconfliction is handled dynamically via SelectionGroup —
     * appears_on_tracks is added in load_artist_state, album card track lists
     * are added as cards are created. No static signal connections needed. */

    /* Enable scroll-to-focus on viewport - CANNOT be done in .ui template
     * Reason: Viewport is created implicitly by GtkScrolledWindow, not accessible in template */
    GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    if (GTK_IS_VIEWPORT(viewport)) {
        gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), TRUE);
    }

    /* Invisible spacer: height synced to back_header via notify::height,
     * pushing the banner (and all content below it) clear of the overlay header. */
    GtkWidget *artist_page_content = GTK_WIDGET(gtk_builder_get_object(builder, "artist_page_content"));
    g_assert(artist_page_content != NULL);
    ud->artist_header_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_prepend(GTK_BOX(artist_page_content), ud->artist_header_spacer);

    /* Take ownership of root widget and release builder */
    g_object_ref(scroll);
    g_object_unref(builder);

    return scroll;
}

/**
 * Load credits-only meta artist state.
 * Reuses the artist page: sets name/stats, clears albums, populates
 * appears_on_tracks with credit-annotated track rows.
 */
static void load_meta_artist_state(UnifiedDetailData *ud, const char *artist_mbid,
                                    const char *artist_name, const char *artist_type G_GNUC_UNUSED) {
    ud->state = DETAIL_STATE_META_ARTIST;
    ud->current_id = 0;
    ud->merged_rep_album_id = 0;

    /* Suppress buttons for the meta-artist we're viewing */
    ud->cbs.artist_cbs.suppress_id = 0;
    ud->cbs.artist_cbs.suppress_mbid = NULL;  /* Set after g_strdup below */

    /* Reset sections */
    gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
    gtk_widget_set_visible(ud->appears_on_section, FALSE);
    gtk_widget_set_visible(ud->about_section, FALSE);

    /* Store current meta artist info */
    g_free(ud->meta_artist_mbid);
    g_free(ud->meta_artist_name);
    ud->meta_artist_mbid = g_strdup(artist_mbid);
    ud->meta_artist_name = g_strdup(artist_name);

    /* Now safe to set suppress_mbid — points into ud->meta_artist_mbid lifetime */
    ud->cbs.artist_cbs.suppress_mbid = ud->meta_artist_mbid;

    /* Set name in the full-width header bar; hide albums and stats (meta artist has no own albums) */
    gtk_label_set_text(GTK_LABEL(ud->header_artist_name),
                       artist_name ? artist_name : "Unknown Artist");
    sync_spacer_heights(ud);
    gtk_label_set_text(GTK_LABEL(ud->artist_stats), "");
    gtk_widget_set_visible(ud->artist_stats, FALSE);
    gtk_widget_set_visible(ud->albums_section, FALSE);

    /* Reset selection group before clearing widgets (stale pointers) */
    ui_selection_group_free(ud->sel_group);
    ud->sel_group = ui_selection_group_new();

    /* Clear albums (meta artist has no own albums) */
    ui_box_clear(GTK_BOX(ud->artist_albums_container));

    /* Load banner and bio for meta artists too */
    if (artist_mbid) {
        load_artist_banner(ud, artist_mbid);
        load_artist_bio(ud, artist_mbid);
    }

    /* Clear appears on lists */
    gtk_list_box_remove_all(GTK_LIST_BOX(ud->appears_on_albums));
    gtk_list_box_remove_all(GTK_LIST_BOX(ud->appears_on_tracks));

    if (!artist_mbid || !ud->settings) {
        gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "artist");
        gtk_widget_set_visible(ud->back_header, TRUE);
        return;
    }

    /* Populate credit rows into both appears_on lists */
    UiRowSizeGroups album_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
        .col2 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)
    };
    UiRowSizeGroups track_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
    };

    guint num_credits = append_credit_rows(ud, artist_mbid,
                                            artist_name ? artist_name : "Unknown Artist",
                                            0, NULL, NULL,
                                            &track_groups, &album_groups);

    g_object_unref(track_groups.col1);
    g_object_unref(album_groups.col1);
    g_object_unref(album_groups.col2);

    gboolean has_appearances = FALSE;
    if (num_credits > 0) {
        /* Default to albums if any were added */
        gboolean has_albums = gtk_widget_get_first_child(ud->appears_on_albums) != NULL;
        if (has_albums) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_albums_btn), TRUE);
        } else {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_tracks_btn), TRUE);
        }
        has_appearances = TRUE;
    }

    /* Show appears_on if we have content */
    if (has_appearances)
        gtk_widget_set_visible(ud->appears_on_section, TRUE);

    /* Add appears-on tracks to selection group (not albums — not a track list) */
    ui_selection_group_add(ud->sel_group, GTK_LIST_BOX(ud->appears_on_tracks));

    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "artist");
    gtk_widget_set_visible(ud->back_header, TRUE);

}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *library_unified_detail_view_new(library_cache_t *cache,
                                            ArtworkManager *art_mgr,
                                            const LibraryCallbacks *cbs,
                                            app_settings_t *settings) {
    UnifiedDetailData *ud = g_new0(UnifiedDetailData, 1);
    ud->cache = cache;
    ud->art_mgr = art_mgr;
    ud->settings = settings;
    ud->library_mask = LIBRARY_MASK_ALL;
    if (cbs) ud->cbs = *cbs;
    ud->state = DETAIL_STATE_ALBUM;  /* Default state - will be set when navigated to */
    ud->sel_group = ui_selection_group_new();

    /* Load unified detail view template */
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/unified_detail_view.ui");

    /* Get widget references from template */
    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "unified_detail_container"));
    g_object_ref(view);
    ud->container = view;

    ud->back_header = GTK_WIDGET(gtk_builder_get_object(builder, "back_header"));
    ud->back_button = GTK_WIDGET(gtk_builder_get_object(builder, "back_button"));
    ud->back_label = GTK_WIDGET(gtk_builder_get_object(builder, "back_label"));
    ud->header_artist_name = GTK_WIDGET(gtk_builder_get_object(builder, "header_artist_name"));
    ud->content_stack = GTK_WIDGET(gtk_builder_get_object(builder, "content_stack"));

    /* Sanity check: all widgets must be present */
    g_assert(view != NULL);
    g_assert(ud->back_header != NULL);
    g_assert(ud->back_button != NULL);
    g_assert(ud->back_label != NULL);
    g_assert(ud->header_artist_name != NULL);
    g_assert(ud->content_stack != NULL);

    g_object_unref(builder);

    /* Build and add detail pages to stack */
    gtk_stack_add_named(GTK_STACK(ud->content_stack), build_album_page(ud), "album");
    gtk_stack_add_named(GTK_STACK(ud->content_stack), build_artist_page(ud), "artist");

    /* Sync spacer heights: initial seed + reactive notify::height.
     * sync_spacer_heights() is also called proactively after every header
     * text change in load_album_state / load_artist_state / load_meta_artist. */
    sync_spacer_heights(ud);
    ud->header_height_signal = g_signal_connect(ud->back_header, "notify::height",
        G_CALLBACK(on_back_header_height_changed), ud);

    /* Connect signals - CANNOT be done in template (requires C callbacks) */
    g_signal_connect(ud->back_button, "clicked", G_CALLBACK(on_unified_back_clicked), ud);
    /* Note: album artist link is connected dynamically in load_album_state() */
    g_signal_connect(ud->toggle_albums_btn, "toggled", G_CALLBACK(on_toggle_albums), ud);
    g_signal_connect(ud->toggle_tracks_btn, "toggled", G_CALLBACK(on_toggle_tracks), ud);
    g_signal_connect(ud->appears_on_albums_revealer, "notify::child-revealed",
                     G_CALLBACK(on_revealer_hidden), ud);
    g_signal_connect(ud->appears_on_tracks_revealer, "notify::child-revealed",
                     G_CALLBACK(on_revealer_hidden), ud);

    /* Store data and set initial state */
    g_object_set_data_full(G_OBJECT(view), UNIFIED_DATA_KEY, ud,
                           (GDestroyNotify)unified_detail_data_free);
    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "album");

    return view;
}

void library_unified_detail_navigate_to_artist(GtkWidget *view, int64_t artist_id,
                                                const char *source_view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    g_assert(ud != NULL);  /* View must have been created via library_unified_detail_view_new */

    /* Skip duplicate navigation to same artist */
    if (ud->state == DETAIL_STATE_ARTIST && ud->current_id == artist_id && !source_view)
        return;

    /* Get artist name for logging */
    const library_artist_info_t *artist = library_cache_get_artist(ud->cache, artist_id, ud->library_mask);
    const char *artist_name = artist ? artist->name : "<unknown>";
    
    g_info("Navigate → Artist Detail: '%s' (artist_id=%" G_GINT64_FORMAT ")%s%s",
           artist_name, artist_id,
           source_view ? " from " : "",
           source_view ? source_view : "");

    if (source_view) {
        nav_push(ud, NAV_ENTRY_VIEW, 0, source_view);
    } else if (ud->state == DETAIL_STATE_META_ARTIST) {
        char *detail_label = build_current_detail_label(ud);
        NavEntry *e = &ud->nav_stack[ud->nav_depth];
        if (ud->nav_depth < MAX_NAV_STACK) {
            nav_push(ud, NAV_ENTRY_META_ARTIST, 0, detail_label);
            e = &ud->nav_stack[ud->nav_depth - 1];
            e->meta_artist_mbid = g_strdup(ud->meta_artist_mbid);
            e->meta_artist_name = g_strdup(ud->meta_artist_name);
            e->meta_artist_type = NULL;
        }
        g_free(detail_label);
    } else {
        NavEntryType type = (ud->state == DETAIL_STATE_ARTIST) ? NAV_ENTRY_ARTIST : NAV_ENTRY_ALBUM;
        char *detail_label = build_current_detail_label(ud);
        nav_push(ud, type, ud->current_id, detail_label);
        g_free(detail_label);
    }

    update_back_label(ud);
    load_artist_state(ud, artist_id);
}

void library_unified_detail_navigate_to_album(GtkWidget *view, int64_t album_id,
                                               const char *source_view, int64_t select_track_id) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    g_assert(ud != NULL);

    /* Skip duplicate navigation to same album */
    if (ud->state == DETAIL_STATE_ALBUM && ud->current_id == album_id && !source_view)
        return;

    /* Get album name for logging */
    const library_album_info_t *album = library_cache_get_album(ud->cache, album_id, ud->library_mask);
    const char *album_title = album ? album->title : "<unknown>";
    const char *album_artist = album ? album->artist_name : "<unknown>";
    
    if (select_track_id > 0) {
        const library_track_info_t *track = library_cache_get_track(ud->cache, select_track_id);
        const char *track_title = track ? track->title : "<unknown>";
        g_info("Navigate → Album Detail: '%s' by %s (album_id=%" G_GINT64_FORMAT ") → Select track '%s' (track_id=%" G_GINT64_FORMAT ")%s%s",
               album_title, album_artist, album_id, track_title, select_track_id,
               source_view ? " from " : "",
               source_view ? source_view : "");
    } else {
        g_info("Navigate → Album Detail: '%s' by %s (album_id=%" G_GINT64_FORMAT ")%s%s",
               album_title, album_artist, album_id,
               source_view ? " from " : "",
               source_view ? source_view : "");
    }

    if (source_view) {
        nav_push(ud, NAV_ENTRY_VIEW, 0, source_view);
    } else if (ud->state == DETAIL_STATE_META_ARTIST) {
        char *detail_label = build_current_detail_label(ud);
        if (ud->nav_depth < MAX_NAV_STACK) {
            nav_push(ud, NAV_ENTRY_META_ARTIST, 0, detail_label);
            NavEntry *e = &ud->nav_stack[ud->nav_depth - 1];
            e->meta_artist_mbid = g_strdup(ud->meta_artist_mbid);
            e->meta_artist_name = g_strdup(ud->meta_artist_name);
            e->meta_artist_type = NULL;
        }
        g_free(detail_label);
    } else {
        NavEntryType type = (ud->state == DETAIL_STATE_ARTIST) ? NAV_ENTRY_ARTIST : NAV_ENTRY_ALBUM;
        char *detail_label = build_current_detail_label(ud);
        nav_push(ud, type, ud->current_id, detail_label);
        g_free(detail_label);
    }

    update_back_label(ud);
    load_album_state(ud, album_id, select_track_id);
}

void library_unified_detail_navigate_to_meta_artist(GtkWidget *view,
                                                      const char *artist_mbid,
                                                      const char *artist_name,
                                                      const char *artist_type) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    g_assert(ud != NULL);

    /* Skip duplicate navigation to same meta-artist */
    if (ud->state == DETAIL_STATE_META_ARTIST && artist_mbid &&
        ud->meta_artist_mbid && g_strcmp0(ud->meta_artist_mbid, artist_mbid) == 0)
        return;

    g_info("Navigate → Credits: '%s' (mbid=%s)",
           artist_name ? artist_name : "<unknown>",
           artist_mbid ? artist_mbid : "<none>");

    /* Push current state */
    if (ud->state == DETAIL_STATE_META_ARTIST) {
        char *detail_label = build_current_detail_label(ud);
        if (ud->nav_depth < MAX_NAV_STACK) {
            nav_push(ud, NAV_ENTRY_META_ARTIST, 0, detail_label);
            NavEntry *e = &ud->nav_stack[ud->nav_depth - 1];
            e->meta_artist_mbid = g_strdup(ud->meta_artist_mbid);
            e->meta_artist_name = g_strdup(ud->meta_artist_name);
            e->meta_artist_type = NULL;
        }
        g_free(detail_label);
    } else {
        NavEntryType type = (ud->state == DETAIL_STATE_ARTIST) ? NAV_ENTRY_ARTIST : NAV_ENTRY_ALBUM;
        char *detail_label = build_current_detail_label(ud);
        nav_push(ud, type, ud->current_id, detail_label);
        g_free(detail_label);
    }

    update_back_label(ud);
    load_meta_artist_state(ud, artist_mbid, artist_name, artist_type);
}

gboolean library_unified_detail_go_back(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    g_assert(ud != NULL);  /* View must have been created via library_unified_detail_view_new */

    if (ud->nav_depth == 0)
        return FALSE;  /* No internal history - let caller handle navigation */

    NavEntry *prev = nav_pop(ud);
    g_assert(prev != NULL);  /* nav_depth > 0 guarantees nav_pop returns non-NULL */

    gboolean handled = TRUE;

    switch (prev->type) {
    case NAV_ENTRY_VIEW:
        /* Exiting detail view - caller will navigate to previous main view */
        g_info("Navigate ← Back to %s", prev->view_name ? prev->view_name : "previous view");
        handled = FALSE;
        break;
    case NAV_ENTRY_ARTIST:
        {
            const library_artist_info_t *artist = library_cache_get_artist(ud->cache, prev->id, ud->library_mask);
            const char *artist_name = artist ? artist->name : "<unknown>";
            g_info("Navigate ← Back to Artist: '%s' (artist_id=%" G_GINT64_FORMAT ")",
                   artist_name, prev->id);
        }
        load_artist_state(ud, prev->id);
        break;
    case NAV_ENTRY_ALBUM:
        {
            const library_album_info_t *album = library_cache_get_album(ud->cache, prev->id, ud->library_mask);
            const char *album_title = album ? album->title : "<unknown>";
            const char *album_artist = album ? album->artist_name : "<unknown>";
            g_info("Navigate ← Back to Album: '%s' by %s (album_id=%" G_GINT64_FORMAT ")",
                   album_title, album_artist, prev->id);
        }
        load_album_state(ud, prev->id, 0);
        break;
    case NAV_ENTRY_META_ARTIST:
        g_info("Navigate ← Back to Credits: '%s'",
               prev->meta_artist_name ? prev->meta_artist_name : "<unknown>");
        load_meta_artist_state(ud, prev->meta_artist_mbid,
                               prev->meta_artist_name, prev->meta_artist_type);
        break;
    }

    if (handled)
        update_back_label(ud);

    /* Common cleanup for all entry types */
    g_free(prev->view_name);
    g_free(prev->meta_artist_mbid);
    g_free(prev->meta_artist_name);
    g_free(prev->meta_artist_type);
    memset(prev, 0, sizeof(*prev));
    return handled;
}

void library_unified_detail_reload(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (!ud) return;

    switch (ud->state) {
    case DETAIL_STATE_ALBUM:
        load_album_state(ud, ud->current_id, 0);
        break;
    case DETAIL_STATE_ARTIST:
        load_artist_state(ud, ud->current_id);
        break;
    case DETAIL_STATE_META_ARTIST:
        load_meta_artist_state(ud, ud->meta_artist_mbid, ud->meta_artist_name, NULL);
        break;
    }
}

void library_unified_detail_set_library_mask(GtkWidget *view, uint32_t mask) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (ud) ud->library_mask = mask;
}

void library_unified_detail_clear_nav(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (ud)
        nav_clear(ud);
}

DetailState library_unified_detail_get_state(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    return ud ? ud->state : DETAIL_STATE_ALBUM;
}

int64_t library_unified_detail_get_current_entity_id(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    return ud ? ud->current_id : 0;
}

GtkWidget *library_unified_detail_get_track_list(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (!ud)
        return NULL;

    /* Album state: find track list within the card */
    if (ud->state == DETAIL_STATE_ALBUM && ud->album_card_inner) {
        GtkWidget *card = gtk_widget_get_first_child(ud->album_card_inner);
        if (card) {
            GtkWidget *track_list = find_widget_by_name(card, "track_list");
            if (track_list)
                return track_list;
        }
    }

    /* Artist / meta-artist state: find which list has a selected track */
    if ((ud->state == DETAIL_STATE_ARTIST || ud->state == DETAIL_STATE_META_ARTIST) &&
        ud->artist_albums_container) {
        GtkWidget *card = gtk_widget_get_first_child(ud->artist_albums_container);
        while (card) {
            GtkWidget *track_list = find_widget_by_name(card, "track_list");
            if (track_list) {
                GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(track_list));
                if (row)
                    return track_list;
            }
            card = gtk_widget_get_next_sibling(card);
        }

        /* Check appears-on tracks (not albums — album lists are not track lists) */
        if (ud->appears_on_tracks) {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(ud->appears_on_tracks));
            if (row)
                return ud->appears_on_tracks;
        }
    }

    return NULL;
}
