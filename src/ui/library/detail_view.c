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
    ui_selection_group_free(ud->sel_group);
    g_clear_object(&ud->header_height_group);
    g_free(ud);
}

/**
 * Find the first background image for an artist across all library roots.
 * Returns a newly-allocated path, or NULL if not found.
 */
static char *find_artist_background(app_settings_t *settings, const char *mbid) {
    if (!settings || !mbid) return NULL;

    for (int i = 0; i < settings->library_path_count; i++) {
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
static void load_artist_bio(UnifiedDetailData *ud, const char *mbid) {
    /* Check for background image first — no image means no hero banner */
    char *bg_path = find_artist_background(ud->settings, mbid);
    if (!bg_path) return;

    /* Load background image into GtkPicture */
    GFile *bg_file = g_file_new_for_path(bg_path);
    GError *bg_error = NULL;
    GdkTexture *bg_texture = gdk_texture_new_from_file(bg_file, &bg_error);
    if (bg_texture) {
        int tex_w = gdk_texture_get_width(bg_texture);
        int tex_h_orig = gdk_texture_get_height(bg_texture);
        /* Compute proportional height at ~1000px display width, capped at 600px.
         * This preserves aspect ratio so cover-fit doesn't clip excessively. */
        int proportional_h = (int)((double)tex_h_orig / tex_w * 1000);
        int tex_h = MIN(proportional_h, 600);
        g_debug("about background loaded: %s (%dx%d, proportional_h=%d, display_h=%d)",
                      bg_path, tex_w, tex_h_orig, proportional_h, tex_h);
        gtk_widget_set_size_request(ud->about_background_image, -1, tex_h);
        gtk_picture_set_paintable(GTK_PICTURE(ud->about_background_image),
                                  GDK_PAINTABLE(bg_texture));

        /* Sample edge colors and update gradient widgets */
        EdgeColors ec = sample_edge_colors(bg_texture, 5);
        quad_gradient_fade_set_color(ud->about_fade_top, &ec.top, TRUE);
        quad_gradient_fade_set_color(ud->about_fade_bottom, &ec.bottom, FALSE);

        g_object_unref(bg_texture);
    } else {
        g_warning("about background load failed: %s: %s",
                     bg_path, bg_error->message);
        g_error_free(bg_error);
        g_object_unref(bg_file);
        g_free(bg_path);
        return;  /* No image → skip bio entirely */
    }
    g_object_unref(bg_file);
    g_free(bg_path);

    /* Query bios DB for bio text */
    for (int i = 0; i < ud->settings->library_path_count; i++) {
        quadrature_bios_db_t *bios_db = NULL;
        const char *dp = app_settings_get_library_data_path(ud->settings, i);
        quadrature_result_t res = db_bios_open_readonly(dp, &bios_db);
        if (res != QUADRATURE_OK) continue;

        char *bio_text = NULL;
        char *wiki_url = NULL;
        res = db_bios_get(bios_db, mbid, &bio_text, &wiki_url);
        db_bios_close(bios_db);

        if (res == QUADRATURE_OK && bio_text && bio_text[0]) {
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

/**
 * Find the first banner image for an artist across all library roots.
 * Returns a newly-allocated path, or NULL if not found.
 */
static char *find_artist_banner(app_settings_t *settings, const char *mbid) {
    if (!settings || !mbid) return NULL;

    for (int i = 0; i < settings->library_path_count; i++) {
        const char *dp = app_settings_get_library_data_path(settings, i);
        char *path = g_strdup_printf("%s/artwork/artists/%s/banner_0.jpg", dp, mbid);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }
    return NULL;
}

/**
 * Load the artist banner image. Shows if file exists, hides if not.
 */
static void load_artist_banner(UnifiedDetailData *ud, const char *mbid) {
    char *banner_path = find_artist_banner(ud->settings, mbid);
    if (banner_path) {
        GFile *file = g_file_new_for_path(banner_path);
        GError *error = NULL;
        GdkTexture *texture = gdk_texture_new_from_file(file, &error);
        if (texture) {
            int tex_h = gdk_texture_get_height(texture);
            g_debug("artist banner loaded: %s (%dx%d)",
                          banner_path,
                          gdk_texture_get_width(texture), tex_h);
            gtk_widget_set_size_request(ud->artist_banner, -1, tex_h);
            gtk_picture_set_paintable(GTK_PICTURE(ud->artist_banner),
                                      GDK_PAINTABLE(texture));

            /* Sample bottom edge color for gradient fade */
            EdgeColors ec = sample_edge_colors(texture, 5);
            quad_gradient_fade_set_color(ud->artist_banner_fade_bottom, &ec.bottom, FALSE);

            gtk_widget_set_visible(ud->artist_banner_overlay, TRUE);
            g_object_unref(texture);
        } else {
            g_warning("artist banner load failed: %s: %s",
                         banner_path, error->message);
            g_error_free(error);
            gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
        }
        g_object_unref(file);
        g_free(banner_path);
    } else {
        gtk_picture_set_paintable(GTK_PICTURE(ud->artist_banner), NULL);
        gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
    }
}

/* Wikipedia link button handler */
static void on_wiki_link_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UnifiedDetailData *ud = data;
    if (ud->about_wiki_url) {
        GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(ud->container));
        if (GTK_IS_WINDOW(toplevel)) {
            gtk_show_uri(GTK_WINDOW(toplevel), ud->about_wiki_url, GDK_CURRENT_TIME);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Navigation Stack
 * ═══════════════════════════════════════════════════════════════════════════ */

static void nav_push(UnifiedDetailData *ud, NavEntryType type, int64_t id, const char *view_name) {
    if (ud->nav_depth >= MAX_NAV_STACK) return;
    NavEntry *e = &ud->nav_stack[ud->nav_depth++];
    e->type = type;
    e->id = id;
    e->view_name = view_name ? g_strdup(view_name) : NULL;
    e->scroll_pos = 0;
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
        const library_artist_info_t *artist = library_cache_get_artist(ud->cache, ud->current_id);
        return artist ? g_strdup_printf("Artist Detail - %s", artist->name)
                      : g_strdup("Artist Detail");
    } else if (ud->state == DETAIL_STATE_META_ARTIST) {
        return ud->meta_artist_name
            ? g_strdup_printf("Credits - %s", ud->meta_artist_name)
            : g_strdup("Credits");
    } else {
        const library_album_info_t *album = library_cache_get_album(ud->cache, ud->current_id);
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

static void on_album_card_navigate(GtkButton *btn, gpointer data);
static void on_unified_artist_link_clicked(GtkButton *btn, gpointer data);
/* Defined in this file, shared with credits_view.c (declared in library/internal.h) */
void on_album_card_artist_navigate(GtkButton *btn, gpointer data);
static void load_meta_artist_state(UnifiedDetailData *ud, const char *artist_mbid,
                                    const char *artist_name, const char *artist_type);

/* ═══════════════════════════════════════════════════════════════════════════
 * Album Card Signal Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Selection deconfliction ──────────────────────────────────────────────
 * On the artist page, each album card has its own GtkListBox for tracks,
 * plus the appears-on-tracks and appears-on-albums lists.  GTK manages
 * selection per-list, so clicking a track in Album B leaves Album A's
 * selection intact.  These helpers ensure only one list has a selection
 * at a time.
 */

static void attach_album_card_handlers(GtkWidget *card, UnifiedDetailData *ud, int64_t album_id) {
    GtkWidget *child = gtk_widget_get_first_child(card);
    while (child) {
        if (GTK_IS_BOX(child)) {
            GtkWidget *inner = gtk_widget_get_first_child(child);
            while (inner) {
                if (GTK_IS_BUTTON(inner)) {
                    const char *tooltip = gtk_widget_get_tooltip_text(inner);
                    if (tooltip && strstr(tooltip, "View album")) {
                        g_object_set_data(G_OBJECT(inner), "album-id",
                                          GSIZE_TO_POINTER((gsize)album_id));
                        g_object_set_data(G_OBJECT(inner), "unified-data", ud);
                        g_signal_connect(inner, "clicked", G_CALLBACK(on_album_card_navigate), ud);
                    }
                }
                inner = gtk_widget_get_next_sibling(inner);
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * State Loading
 * ═══════════════════════════════════════════════════════════════════════════ */

static void load_album_state(UnifiedDetailData *ud, int64_t album_id, int64_t select_track_id) {
    ud->state = DETAIL_STATE_ALBUM;
    ud->current_id = album_id;

    /* Clear artist name from header bar — album view does not show it */
    gtk_label_set_text(GTK_LABEL(ud->header_artist_name), "");

    /* Get album info and tracks */
    const library_album_info_t *album = library_cache_get_album(ud->cache, album_id);
    if (!album)
        return;

    const GPtrArray *tracks = library_cache_get_tracks_by_album(ud->cache, album_id);
    if (!tracks || tracks->len == 0)
        return;

    /* Prefetch audio files for instant playback */
    if (ud->cache && tracks->len > 0) {
        int64_t *track_ids = g_new(int64_t, tracks->len);
        for (guint i = 0; i < tracks->len; i++) {
            const library_track_info_t *t = g_ptr_array_index(tracks, i);
            track_ids[i] = t->track_id;
        }
        library_cache_prefetch_audio_files(ud->cache, track_ids, tracks->len);
        g_free(track_ids);
    }

    /* Query metadata DB for enriched release info (non-fatal) */
    db_meta_release_t *meta_release = NULL;
    if (album->musicbrainz_release_id && album->musicbrainz_release_id[0] &&
        ud->settings && album->library_index >= 0 &&
        album->library_index < ud->settings->library_path_count) {
        const char *lib_root = app_settings_get_library_data_path(ud->settings, album->library_index);
        quadrature_meta_db_t *meta_db = NULL;
        if (db_meta_open_readonly(lib_root, &meta_db) == QUADRATURE_OK && meta_db) {
            db_meta_get_release(meta_db, album->musicbrainz_release_id, &meta_release);
            db_meta_close(meta_db);
        }
    }

    /* Clear inner container and create album card */
    ui_box_clear(GTK_BOX(ud->album_card_inner));

    GtkWidget *card = ui_create_album_detail_card(album, tracks, ud->cache, ud->art_mgr, 0,
                                                   (RowCallbacks *)&ud->cbs.album_track_cbs,
                                                   (RowCallbacks *)&ud->cbs.artist_cbs,
                                                   meta_release);
    db_meta_release_free(meta_release);
    gtk_box_append(GTK_BOX(ud->album_card_inner), card);

    /* Wire info button handlers for track rows */
    wire_info_buttons(card, ud);

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

    gtk_stack_set_visible_child_name(GTK_STACK(ud->content_stack), "album");
    gtk_widget_set_visible(ud->back_header, TRUE);
}


static void load_artist_state(UnifiedDetailData *ud, int64_t artist_id) {
    ud->state = DETAIL_STATE_ARTIST;
    ud->current_id = artist_id;

    /* Reset sections */
    gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
    gtk_widget_set_visible(ud->appears_on_section, FALSE);
    gtk_widget_set_visible(ud->about_section, FALSE);

    const GPtrArray *albums = library_cache_get_albums_by_artist(ud->cache, artist_id);
    const GPtrArray *appearance_albums = library_cache_get_artist_appearances(ud->cache, artist_id);
    const GPtrArray *appearance_tracks = library_cache_get_artist_appearance_tracks(ud->cache, artist_id);

    gboolean has_albums = albums && albums->len > 0;
    gboolean has_appearances = (appearance_albums && appearance_albums->len > 0) ||
                               (appearance_tracks && appearance_tracks->len > 0);

    /* Check if artist may have MB credits (don't bail early) */
    const library_artist_info_t *artist_check = library_cache_get_artist(ud->cache, artist_id);
    g_debug("artist detail: id=%" G_GINT64_FORMAT " mbid=%s",
            artist_id,
            artist_check ? (artist_check->musicbrainz_id ?: "(null)") : "(no artist)");
    gboolean may_have_credits = artist_check && artist_check->musicbrainz_id && ud->settings;

    if (!has_albums && !has_appearances && !may_have_credits)
        return;

    /* Resolve artist name: prefer own albums, fall back to cache lookup */
    const char *artist_name = NULL;
    if (has_albums) {
        const library_album_info_t *first_album = g_ptr_array_index(albums, 0);
        artist_name = first_album->artist_name;
    } else {
        const library_artist_info_t *artist = library_cache_get_artist(ud->cache, artist_id);
        if (artist)
            artist_name = artist->name;
    }
    gtk_label_set_text(GTK_LABEL(ud->header_artist_name), artist_name ? artist_name : "Unknown Artist");

    /* Prefetch audio files from first own album for instant playback */
    if (has_albums && ud->cache) {
        const library_album_info_t *first_album = g_ptr_array_index(albums, 0);
        const GPtrArray *first_tracks = library_cache_get_tracks_by_album(ud->cache, first_album->album_id);
        if (first_tracks && first_tracks->len > 0) {
            int64_t *track_ids = g_new(int64_t, first_tracks->len);
            for (guint i = 0; i < first_tracks->len; i++) {
                const library_track_info_t *t = g_ptr_array_index(first_tracks, i);
                track_ids[i] = t->track_id;
            }
            library_cache_prefetch_audio_files(ud->cache, track_ids, first_tracks->len);
            g_free(track_ids);
        }
    }

    guint album_count = has_albums ? albums->len : 0;

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
        const GPtrArray *tracks = library_cache_get_tracks_by_album(ud->cache, album->album_id);
        if (tracks && tracks->len > 0) {
            GtkWidget *card = ui_create_album_detail_card(album, tracks, ud->cache, ud->art_mgr, 0,
                                                           (RowCallbacks *)&ud->cbs.album_track_cbs,
                                                           (RowCallbacks *)&ud->cbs.artist_cbs,
                                                           NULL);

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

            attach_album_card_handlers(card, ud, album->album_id);
            wire_info_buttons(card, ud);

            /* Add track list to selection group for mutual-exclusion */
            GtkWidget *tl = find_widget_by_name(card, "track_list");
            if (tl)
                ui_selection_group_add(ud->sel_group, GTK_LIST_BOX(tl));

            gtk_box_append(GTK_BOX(ud->artist_albums_container), card);
        }
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
        const GPtrArray *atracks = library_cache_get_tracks_by_album(ud->cache, album->album_id);
        if (atracks) {
            for (guint j = 0; j < atracks->len; j++) {
                const library_track_info_t *t = g_ptr_array_index(atracks, j);
                g_hash_table_add(skip_track_ids, GSIZE_TO_POINTER((gsize)t->track_id));
            }
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

    /* Populate normal appearance album rows */
    if (appearance_albums && appearance_albums->len > 0) {
        for (guint i = 0; i < appearance_albums->len; i++) {
            const library_album_info_t *album = g_ptr_array_index(appearance_albums, i);
            GtkWidget *row = ui_create_album_row(album, ud->cache, ud->art_mgr, TRUE,
                                                   (RowCallbacks *)&ud->cbs.artist_cbs,
                                                   &album_groups, NULL);
            ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.album_cbs);
            gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_albums), row);
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

    /* Append MB credit rows (both tracks and albums) */
    guint credit_count = 0;
    const library_artist_info_t *artist_info = library_cache_get_artist(ud->cache, artist_id);
    if (artist_info && artist_info->musicbrainz_id && ud->settings) {
        credit_count = append_credit_rows(ud, artist_info->musicbrainz_id,
                                           artist_name ? artist_name : "Unknown Artist",
                                           artist_id, skip_track_ids, skip_album_ids,
                                           &track_groups, &album_groups);
    }

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

static void on_album_card_navigate(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    int64_t album_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "album-id"));
    if (album_id > 0) {
        library_unified_detail_navigate_to_album(ud->container, album_id, NULL, 0);
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
static void on_revealer_hidden(GtkRevealer *revealer, GParamSpec *pspec, gpointer data) {
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

    /* Invisible spacer: joins the shared header_height_group so its height
     * always matches back_header, pushing album art below the overlay.
     * Lives in the outer container so ui_box_clear(album_card_inner) cannot remove it. */
    GtkWidget *header_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_size_group_add_widget(ud->header_height_group, header_spacer);
    gtk_box_append(GTK_BOX(ud->album_card_container), header_spacer);

    /* Inner container: cleared and repopulated on each album load. */
    ud->album_card_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(ud->album_card_container), ud->album_card_inner);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ud->album_card_container);
    
    /* Enable scroll-to-focus on viewport */
    GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    if (GTK_IS_VIEWPORT(viewport)) {
        gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), TRUE);
    }
    
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

    /* Store references to widgets we need to update dynamically */
    ud->artist_name = GTK_WIDGET(gtk_builder_get_object(builder, "artist_name"));
    ud->artist_stats = GTK_WIDGET(gtk_builder_get_object(builder, "artist_stats"));
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

    /* Invisible spacer: joins the shared header_height_group so its height
     * always matches back_header, pushing the banner (and all content below
     * it) clear of the opaque overlay header. */
    GtkWidget *artist_page_content = GTK_WIDGET(gtk_builder_get_object(builder, "artist_page_content"));
    g_assert(artist_page_content != NULL);
    GtkWidget *header_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_size_group_add_widget(ud->header_height_group, header_spacer);
    gtk_box_prepend(GTK_BOX(artist_page_content), header_spacer);

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
                                    const char *artist_name, const char *artist_type) {
    ud->state = DETAIL_STATE_META_ARTIST;
    ud->current_id = 0;

    /* Reset sections */
    gtk_widget_set_visible(ud->artist_banner_overlay, FALSE);
    gtk_widget_set_visible(ud->appears_on_section, FALSE);
    gtk_widget_set_visible(ud->about_section, FALSE);

    /* Store current meta artist info */
    g_free(ud->meta_artist_mbid);
    g_free(ud->meta_artist_name);
    ud->meta_artist_mbid = g_strdup(artist_mbid);
    ud->meta_artist_name = g_strdup(artist_name);

    /* Set name in the full-width header bar; hide albums (meta artist has no own albums) */
    gtk_label_set_text(GTK_LABEL(ud->header_artist_name),
                       artist_name ? artist_name : "Unknown Artist");
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

    /* Shared VERTICAL size group: back_header + one spacer per page.
     * GTK propagates back_header's natural height to every spacer automatically,
     * so content on both the album and artist pages always starts below the
     * opaque header overlay — no hardcoded pixel values, no callbacks. */
    ud->header_height_group = gtk_size_group_new(GTK_SIZE_GROUP_VERTICAL);
    gtk_size_group_add_widget(ud->header_height_group, ud->back_header);

    /* Build and add detail pages to stack */
    gtk_stack_add_named(GTK_STACK(ud->content_stack), build_album_page(ud), "album");
    gtk_stack_add_named(GTK_STACK(ud->content_stack), build_artist_page(ud), "artist");

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
    const library_artist_info_t *artist = library_cache_get_artist(ud->cache, artist_id);
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
    const library_album_info_t *album = library_cache_get_album(ud->cache, album_id);
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

    switch (prev->type) {
    case NAV_ENTRY_VIEW:
        /* Exiting detail view - caller will navigate to previous main view */
        g_info("Navigate ← Back to %s", prev->view_name ? prev->view_name : "previous view");
        g_free(prev->view_name);
        return FALSE;
    case NAV_ENTRY_ARTIST:
        {
            const library_artist_info_t *artist = library_cache_get_artist(ud->cache, prev->id);
            const char *artist_name = artist ? artist->name : "<unknown>";
            g_info("Navigate ← Back to Artist: '%s' (artist_id=%" G_GINT64_FORMAT ")",
                   artist_name, prev->id);
        }
        load_artist_state(ud, prev->id);
        break;
    case NAV_ENTRY_ALBUM:
        {
            const library_album_info_t *album = library_cache_get_album(ud->cache, prev->id);
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

    update_back_label(ud);

    g_free(prev->view_name);
    g_free(prev->meta_artist_mbid);
    g_free(prev->meta_artist_name);
    g_free(prev->meta_artist_type);
    return TRUE;
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

void library_unified_detail_clear_nav(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    if (ud)
        nav_clear(ud);
}

DetailState library_unified_detail_get_state(GtkWidget *view) {
    UnifiedDetailData *ud = g_object_get_data(G_OBJECT(view), UNIFIED_DATA_KEY);
    return ud ? ud->state : DETAIL_STATE_ALBUM;
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
