/**
 * Quadrature Unified Detail View
 *
 * Three-state view for album/artist detail display.
 * Built programmatically - no template dependencies.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "../internal.h"
#include <string.h>

#define MAX_NAV_STACK 24

typedef struct {
    library_cache_t *cache;
    ArtworkManager *art_mgr;
    LibraryCallbacks cbs;

    DetailState state;
    int64_t current_id;        /* Current artist/album ID */
    int64_t album_artist_id;   /* For album state: artist to navigate to */

    /* Navigation stack */
    NavEntry nav_stack[MAX_NAV_STACK];
    int nav_depth;

    /* Widgets */
    GtkWidget *container;
    GtkWidget *back_header;
    GtkWidget *back_button;
    GtkWidget *back_label;
    GtkWidget *content_stack;

    /* Album state widgets */
    GtkWidget *album_card_container;  /* Container that holds the album card */

    /* Artist state widgets */
    GtkWidget *artist_name;
    GtkWidget *artist_stats;
    GtkWidget *artist_albums_container;
    GtkWidget *appears_on_section;
    GtkWidget *appears_on_stack;
    GtkWidget *appears_on_albums;
    GtkWidget *appears_on_tracks;
    GtkWidget *toggle_albums_btn;
    GtkWidget *toggle_tracks_btn;
} UnifiedDetailData;

static const char *UNIFIED_DATA_KEY = "unified-detail-data";

static void format_duration_str(uint32_t ms, char *buf, size_t len) {
    uint32_t sec = ms / 1000, min = sec / 60, hr = min / 60;
    if (hr > 0)
        snprintf(buf, len, "%uh %02um", hr, min % 60);
    else
        snprintf(buf, len, "%u:%02u", min, sec % 60);
}

static void clear_container(GtkWidget *container) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(container)))
        gtk_box_remove(GTK_BOX(container), child);
}

/* Helper: Find widget by builder ID in widget tree */
static GtkWidget *find_widget_by_name(GtkWidget *parent, const char *name) {
    const char *widget_name = gtk_buildable_get_buildable_id(GTK_BUILDABLE(parent));
    if (widget_name && g_strcmp0(widget_name, name) == 0)
        return parent;
    
    GtkWidget *child = gtk_widget_get_first_child(parent);
    while (child) {
        GtkWidget *found = find_widget_by_name(child, name);
        if (found) return found;
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
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
    for (int i = 0; i < ud->nav_depth; i++)
        g_free(ud->nav_stack[i].view_name);
    ud->nav_depth = 0;
}

/* Build a display label for the current detail state (e.g., "Artist Detail - Daft Punk").
 * Caller must g_free() the result. */
static char *build_current_detail_label(UnifiedDetailData *ud) {
    if (ud->state == DETAIL_STATE_ARTIST) {
        const library_artist_info_t *artist = library_cache_get_artist(ud->cache, ud->current_id);
        return artist ? g_strdup_printf("Artist Detail - %s", artist->name)
                      : g_strdup("Artist Detail");
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
static void on_album_card_artist_navigate(GtkButton *btn, gpointer data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Album Card Signal Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

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

    /* Clear container and create album card */
    clear_container(ud->album_card_container);
    
    GtkWidget *card = ui_create_album_detail_card(album, tracks, ud->cache, ud->art_mgr, 0,
                                                   (RowCallbacks *)&ud->cbs.album_track_cbs,
                                                   (RowCallbacks *)&ud->cbs.artist_cbs);
    gtk_box_append(GTK_BOX(ud->album_card_container), card);
    
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

    const GPtrArray *albums = library_cache_get_albums_by_artist(ud->cache, artist_id);
    const GPtrArray *appearance_albums = library_cache_get_artist_appearances(ud->cache, artist_id);
    const GPtrArray *appearance_tracks = library_cache_get_artist_appearance_tracks(ud->cache, artist_id);

    gboolean has_albums = albums && albums->len > 0;
    gboolean has_appearances = (appearance_albums && appearance_albums->len > 0) ||
                               (appearance_tracks && appearance_tracks->len > 0);

    if (!has_albums && !has_appearances)
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
    gtk_label_set_text(GTK_LABEL(ud->artist_name), artist_name ? artist_name : "Unknown Artist");

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

    /* Artist stats (own albums only) */
    uint32_t total_tracks = 0;
    uint32_t total_ms = 0;
    guint album_count = has_albums ? albums->len : 0;
    for (guint i = 0; i < album_count; i++) {
        const library_album_info_t *album = g_ptr_array_index(albums, i);
        total_tracks += album->track_count;
        total_ms += album->total_duration_ms;
    }

    char buf[64], dur_buf[16];
    format_duration_str(total_ms, dur_buf, sizeof(dur_buf));
    snprintf(buf, sizeof(buf), "%u album%s - %u track%s - %s",
             album_count, album_count == 1 ? "" : "s",
             total_tracks, total_tracks == 1 ? "" : "s", dur_buf);
    gtk_label_set_text(GTK_LABEL(ud->artist_stats), buf);

    /* Populate own albums */
    clear_container(ud->artist_albums_container);

    for (guint i = 0; i < album_count; i++) {
        const library_album_info_t *album = g_ptr_array_index(albums, i);
        const GPtrArray *tracks = library_cache_get_tracks_by_album(ud->cache, album->album_id);
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

            attach_album_card_handlers(card, ud, album->album_id);
            gtk_box_append(GTK_BOX(ud->artist_albums_container), card);
        }
    }

    /* Populate "Appears On" section */
    gtk_list_box_remove_all(GTK_LIST_BOX(ud->appears_on_albums));
    gtk_list_box_remove_all(GTK_LIST_BOX(ud->appears_on_tracks));

    if (has_appearances) {
        /* Populate album rows with size groups for column alignment */
        if (appearance_albums && appearance_albums->len > 0) {
            UiRowSizeGroups album_groups = {
                .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
                .col2 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)
            };

            for (guint i = 0; i < appearance_albums->len; i++) {
                const library_album_info_t *album = g_ptr_array_index(appearance_albums, i);
                GtkWidget *row = ui_create_album_row(album, ud->cache, ud->art_mgr, TRUE,
                                                       (RowCallbacks *)&ud->cbs.artist_cbs,
                                                       &album_groups);
                ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.album_cbs);
                gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_albums), row);
            }

            g_object_unref(album_groups.col1);
            g_object_unref(album_groups.col2);
        }

        /* Populate track rows with size groups for column alignment */
        if (appearance_tracks && appearance_tracks->len > 0) {
            UiRowSizeGroups track_groups = {
                .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
                .col2 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)
            };

            for (guint i = 0; i < appearance_tracks->len; i++) {
                const library_track_info_t *track = g_ptr_array_index(appearance_tracks, i);
                GtkWidget *row = ui_create_track_row(track, ud->cache, ud->art_mgr, TRUE,
                                                       (RowCallbacks *)&ud->cbs.artist_cbs,
                                                       (RowCallbacks *)&ud->cbs.album_cbs,
                                                       &track_groups);
                ui_row_attach_handlers(row, (RowCallbacks *)&ud->cbs.track_cbs);
                gtk_list_box_append(GTK_LIST_BOX(ud->appears_on_tracks), row);
            }

            g_object_unref(track_groups.col1);
            g_object_unref(track_groups.col2);
        }

        gtk_widget_set_visible(ud->appears_on_section, TRUE);
    } else {
        gtk_widget_set_visible(ud->appears_on_section, FALSE);
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

static void on_album_card_artist_navigate(GtkButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    int64_t artist_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "artist-id"));
    if (artist_id > 0) {
        library_unified_detail_navigate_to_artist(ud->container, artist_id, NULL);
    }
}

static void on_toggle_albums(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_tracks_btn), FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(ud->appears_on_stack), "albums");
    }
}

static void on_toggle_tracks(GtkToggleButton *btn, gpointer data) {
    UnifiedDetailData *ud = data;
    if (gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ud->toggle_albums_btn), FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(ud->appears_on_stack), "tracks");
    }
}



/* ═══════════════════════════════════════════════════════════════════════════
 * List Navigation Helpers (modular, reusable)
 * ═══════════════════════════════════════════════════════════════════════════ */

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
 * Detail View Key Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Key handler for the detail view container.
 * Handles:
 *   - Tab: toggle views and snap to first item
 *   - Down: select first track when nothing selected
 */
static gboolean on_detail_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                       guint keycode, GdkModifierType state,
                                       gpointer data) {
    (void)ctl; (void)keycode; (void)state;
    UnifiedDetailData *ud = data;

    /* Tab: toggle views and snap selection */
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        if (ud->state == DETAIL_STATE_ALBUM) {
            /* In album view: Tab snaps to first track */
            GtkWidget *track_list = library_unified_detail_get_track_list(ud->container);
            if (track_list) {
                list_box_select_first(GTK_LIST_BOX(track_list));
                return TRUE;
            }
        } else if (ud->state == DETAIL_STATE_ARTIST) {
            /* In artist view: Tab toggles Appears On Albums/Tracks */
            if (gtk_widget_get_visible(ud->appears_on_section)) {
                gboolean albums_active = gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(ud->toggle_albums_btn));
                if (albums_active) {
                    gtk_toggle_button_set_active(
                        GTK_TOGGLE_BUTTON(ud->toggle_tracks_btn), TRUE);
                } else {
                    gtk_toggle_button_set_active(
                        GTK_TOGGLE_BUTTON(ud->toggle_albums_btn), TRUE);
                }
                return TRUE;
            }
        }
        return FALSE;
    }

    /* Down arrow: select first track when nothing selected (album view) */
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (ud->state == DETAIL_STATE_ALBUM) {
            GtkWidget *track_list = library_unified_detail_get_track_list(ud->container);
            if (track_list) {
                GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(track_list));
                if (selected == NULL) {
                    list_box_select_first(GTK_LIST_BOX(track_list));
                    return TRUE;
                }
            }
        }
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
    
    /* Create container that will hold the album card */
    ud->album_card_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(ud->album_card_container, 24);
    gtk_widget_set_margin_end(ud->album_card_container, 24);
    gtk_widget_set_margin_top(ud->album_card_container, 16);
    gtk_widget_set_margin_bottom(ud->album_card_container, 24);
    
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
    /* Load template from resources */
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/artist_detail_page.ui");

    /* Get the root scroll window and all child widgets */
    GtkWidget *scroll = GTK_WIDGET(gtk_builder_get_object(builder, "artist_page_scroll"));
    g_assert(scroll != NULL);  /* Template must exist */

    /* Store references to widgets we need to update dynamically */
    ud->artist_name = GTK_WIDGET(gtk_builder_get_object(builder, "artist_name"));
    ud->artist_stats = GTK_WIDGET(gtk_builder_get_object(builder, "artist_stats"));
    ud->artist_albums_container = GTK_WIDGET(gtk_builder_get_object(builder, "artist_albums_container"));
    ud->appears_on_section = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_section"));
    ud->appears_on_stack = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_stack"));
    ud->appears_on_albums = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_albums"));
    ud->appears_on_tracks = GTK_WIDGET(gtk_builder_get_object(builder, "appears_on_tracks"));
    ud->toggle_albums_btn = GTK_WIDGET(gtk_builder_get_object(builder, "toggle_albums_btn"));
    ud->toggle_tracks_btn = GTK_WIDGET(gtk_builder_get_object(builder, "toggle_tracks_btn"));

    /* Sanity check: all widgets must be present in template */
    g_assert(ud->artist_name != NULL);
    g_assert(ud->artist_stats != NULL);
    g_assert(ud->artist_albums_container != NULL);
    g_assert(ud->appears_on_section != NULL);
    g_assert(ud->appears_on_stack != NULL);
    g_assert(ud->appears_on_albums != NULL);
    g_assert(ud->appears_on_tracks != NULL);
    g_assert(ud->toggle_albums_btn != NULL);
    g_assert(ud->toggle_tracks_btn != NULL);

    /* Connect row-activated signals for appears-on list boxes */
    g_signal_connect(ud->appears_on_albums, "row-activated",
                     G_CALLBACK(ui_list_box_row_activated), NULL);
    g_signal_connect(ud->appears_on_tracks, "row-activated",
                     G_CALLBACK(ui_list_box_row_activated), NULL);

    /* Enable scroll-to-focus on viewport - CANNOT be done in .ui template
     * Reason: Viewport is created implicitly by GtkScrolledWindow, not accessible in template */
    GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    if (GTK_IS_VIEWPORT(viewport)) {
        gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), TRUE);
    }

    /* Take ownership of root widget and release builder */
    g_object_ref(scroll);
    g_object_unref(builder);

    return scroll;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *library_unified_detail_view_new(library_cache_t *cache,
                                            ArtworkManager *art_mgr,
                                            const LibraryCallbacks *cbs) {
    UnifiedDetailData *ud = g_new0(UnifiedDetailData, 1);
    ud->cache = cache;
    ud->art_mgr = art_mgr;
    if (cbs) ud->cbs = *cbs;
    ud->state = DETAIL_STATE_ALBUM;  /* Default state - will be set when navigated to */

    /* Load unified detail view template */
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/unified_detail_view.ui");

    /* Get widget references from template */
    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "unified_detail_container"));
    g_object_ref(view);
    ud->container = view;

    ud->back_header = GTK_WIDGET(gtk_builder_get_object(builder, "back_header"));
    ud->back_button = GTK_WIDGET(gtk_builder_get_object(builder, "back_button"));
    ud->back_label = GTK_WIDGET(gtk_builder_get_object(builder, "back_label"));
    ud->content_stack = GTK_WIDGET(gtk_builder_get_object(builder, "content_stack"));

    /* Sanity check: all widgets must be present */
    g_assert(view != NULL);
    g_assert(ud->back_header != NULL);
    g_assert(ud->back_button != NULL);
    g_assert(ud->back_label != NULL);
    g_assert(ud->content_stack != NULL);

    g_object_unref(builder);

    /* Build and add detail pages to stack */
    gtk_stack_add_named(GTK_STACK(ud->content_stack), build_album_page(ud), "album");
    gtk_stack_add_named(GTK_STACK(ud->content_stack), build_artist_page(ud), "artist");

    /* Connect signals - CANNOT be done in template (requires C callbacks) */
    g_signal_connect(ud->back_button, "clicked", G_CALLBACK(on_unified_back_clicked), ud);
    /* Note: album artist link is connected dynamically in load_album_state() */
    g_signal_connect(ud->toggle_albums_btn, "toggled", G_CALLBACK(on_toggle_albums), ud);
    g_signal_connect(ud->toggle_tracks_btn, "toggled", G_CALLBACK(on_toggle_tracks), ud);

    /* Store data and set initial state */
    g_object_set_data_full(G_OBJECT(view), UNIFIED_DATA_KEY, ud, g_free);
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
        /* Named source view (toplevel or channel strip) - push view entry.
         * Caller is responsible for calling clear_nav() first if they want
         * to reset the stack (toplevel nav does; channel strip does not). */
        nav_push(ud, NAV_ENTRY_VIEW, 0, source_view);
    } else {
        /* Internal detail-to-detail - push current detail state with entity name */
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
    g_assert(ud != NULL);  /* View must have been created via library_unified_detail_view_new */

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
        /* Named source view (toplevel or channel strip) - push view entry.
         * Caller is responsible for calling clear_nav() first if they want
         * to reset the stack (toplevel nav does; channel strip does not). */
        nav_push(ud, NAV_ENTRY_VIEW, 0, source_view);
    } else {
        /* Internal detail-to-detail - push current detail state with entity name */
        NavEntryType type = (ud->state == DETAIL_STATE_ARTIST) ? NAV_ENTRY_ARTIST : NAV_ENTRY_ALBUM;
        char *detail_label = build_current_detail_label(ud);
        nav_push(ud, type, ud->current_id, detail_label);
        g_free(detail_label);
    }

    update_back_label(ud);
    load_album_state(ud, album_id, select_track_id);
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
    }

    update_back_label(ud);

    g_free(prev->view_name);
    return TRUE;
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
    if (ud->state == DETAIL_STATE_ALBUM && ud->album_card_container) {
        GtkWidget *card = gtk_widget_get_first_child(ud->album_card_container);
        if (card) {
            GtkWidget *track_list = find_widget_by_name(card, "track_list");
            if (track_list)
                return track_list;
        }
    }

    /* Artist state: find which album card has a selected track */
    if (ud->state == DETAIL_STATE_ARTIST && ud->artist_albums_container) {
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
    }

    return NULL;
}
