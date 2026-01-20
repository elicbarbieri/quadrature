/**
 * Quadrature UI Row Helpers
 *
 * Shared functions for creating interactive, template-based list rows.
 * All rows are clickable with appropriate handlers attached.
 */

#include "internal.h"
#include <string.h>

void ui_format_duration(uint32_t ms, char *buf, size_t len) {
    uint32_t sec = ms / 1000, min = sec / 60;
    snprintf(buf, len, "%u:%02u", min, sec % 60);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback Storage
 *
 * We store callbacks on widgets to retrieve them in generic handlers.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    LibraryCallbacks cbs;
    quadrature_db_t *db;  /* For album right-click to get track 1 */
} RowCallbackData;

static void row_callback_data_free(gpointer data) {
    g_free(data);
}

static RowCallbackData *row_callback_data_new(const LibraryCallbacks *cbs, quadrature_db_t *db) {
    RowCallbackData *data = g_new0(RowCallbackData, 1);
    if (cbs) data->cbs = *cbs;
    data->db = db;
    return data;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Generic Click Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Artist row left-click: navigate to artist detail */
static void on_artist_row_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GtkWidget *row = gtk_button_get_child(btn);
    if (!row) return;

    RowCallbackData *data = g_object_get_data(G_OBJECT(row), "row-callbacks");
    if (!data || !data->cbs.on_navigate) return;

    int64_t id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "artist-id"));
    if (id > 0) {
        data->cbs.on_navigate(LIBRARY_ITEM_ARTIST, id, data->cbs.user_data);
    }
}

/* Album row left-click: navigate to album detail */
static void on_album_row_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GtkWidget *row = gtk_button_get_child(btn);
    if (!row) return;

    RowCallbackData *data = g_object_get_data(G_OBJECT(row), "row-callbacks");
    if (!data || !data->cbs.on_navigate) return;

    int64_t id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "album-id"));
    if (id > 0) {
        data->cbs.on_navigate(LIBRARY_ITEM_ALBUM, id, data->cbs.user_data);
    }
}

/* Album row right-click: queue track 1 to focused channel */
static void on_album_row_right_click(GtkGestureClick *gesture, int n_press,
                                      double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!row) return;

    RowCallbackData *data = g_object_get_data(G_OBJECT(row), "row-callbacks");
    if (!data || !data->cbs.on_play || !data->db) return;

    int64_t album_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "album-id"));
    if (album_id <= 0) return;

    /* Get first track of album */
    db_track_t *tracks = NULL;
    size_t count = 0;
    if (db_get_tracks_by_album(data->db, album_id, &tracks, &count) != QUADRATURE_OK || count == 0) {
        if (tracks) db_tracks_free(tracks, count);
        return;
    }

    data->cbs.on_play(tracks[0].path, tracks[0].title, tracks[0].artist,
                      tracks[0].album, tracks[0].id, data->cbs.user_data);
    db_tracks_free(tracks, count);
}

/* Track row right-click: queue track to focused channel */
static void on_track_row_right_click(GtkGestureClick *gesture, int n_press,
                                      double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!row) return;

    RowCallbackData *data = g_object_get_data(G_OBJECT(row), "row-callbacks");
    if (!data || !data->cbs.on_play) return;

    int64_t track_id = (int64_t)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "track-id"));
    if (track_id <= 0) return;

    /* Get track data from stored fields */
    const char *path = g_object_get_data(G_OBJECT(row), "track-path");
    const char *title = g_object_get_data(G_OBJECT(row), "track-title");
    const char *artist = g_object_get_data(G_OBJECT(row), "track-artist");
    const char *album = g_object_get_data(G_OBJECT(row), "track-album");

    if (path && title) {
        data->cbs.on_play(path, title, artist ? artist : "",
                          album ? album : "", track_id, data->cbs.user_data);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Creation Functions
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *ui_create_artist_row(const db_artist_t *artist,
                                 gboolean show_art_strip,
                                 const LibraryCallbacks *cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_artist_row.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    GtkWidget *art_strip = GTK_WIDGET(gtk_builder_get_object(builder, "art_strip"));

    g_object_unref(builder);

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), artist->name);
    }

    if (subtitle) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%zu album%s, %zu track%s",
                 artist->album_count, artist->album_count == 1 ? "" : "s",
                 artist->track_count, artist->track_count == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(subtitle), buf);
    }

    if (art_strip) {
        gtk_widget_set_visible(art_strip, show_art_strip);
    }

    /* Store artist ID and callbacks on row */
    g_object_set_data(G_OBJECT(row), "artist-id", GSIZE_TO_POINTER((gsize)artist->id));

    RowCallbackData *cb_data = row_callback_data_new(cbs, NULL);
    g_object_set_data_full(G_OBJECT(row), "row-callbacks", cb_data, row_callback_data_free);

    /* Wrap in clickable button */
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_button_set_child(GTK_BUTTON(btn), row);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_artist_row_clicked), NULL);

    return btn;
}

GtkWidget *ui_create_album_row(const db_album_t *album,
                                ArtworkManager *art_mgr,
                                gboolean show_count,
                                quadrature_db_t *db,
                                const LibraryCallbacks *cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/library_album_row.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    GtkWidget *count = GTK_WIDGET(gtk_builder_get_object(builder, "count"));

    g_object_unref(builder);

    if (art && art_mgr) {
        artwork_manager_load_thumb_into(art_mgr, album->id,
                                         LOAD_PRIORITY_VISIBLE, art, NULL);
    }

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), album->title);
    }

    if (subtitle) {
        char buf[128];
        if (album->year > 0)
            snprintf(buf, sizeof(buf), "%s \u2022 %u", album->artist_name, album->year);
        else
            snprintf(buf, sizeof(buf), "%s", album->artist_name);
        gtk_label_set_text(GTK_LABEL(subtitle), buf);
    }

    if (count) {
        gtk_widget_set_visible(count, show_count);
    }

    /* Store album ID and callbacks on row */
    g_object_set_data(G_OBJECT(row), "album-id", GSIZE_TO_POINTER((gsize)album->id));

    RowCallbackData *cb_data = row_callback_data_new(cbs, db);
    g_object_set_data_full(G_OBJECT(row), "row-callbacks", cb_data, row_callback_data_free);

    /* Add right-click gesture for queuing track 1 */
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_album_row_right_click), NULL);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(right_click));

    /* Wrap in clickable button */
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_button_set_child(GTK_BUTTON(btn), row);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_album_row_clicked), NULL);

    return btn;
}

GtkWidget *ui_create_track_row(const db_track_t *track,
                                ArtworkManager *art_mgr,
                                gboolean show_track_disc,
                                const LibraryCallbacks *cbs) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/song_list_view.ui");
    GtkWidget *row = GTK_WIDGET(gtk_builder_get_object(builder, "row"));
    g_object_ref(row);

    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "art"));
    GtkWidget *title_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *album_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "album"));
    GtkWidget *artist_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "artist"));
    GtkWidget *year_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "year"));
    GtkWidget *duration_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "duration"));
    GtkWidget *track_num_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "track_num"));
    GtkWidget *disc_num_lbl = GTK_WIDGET(gtk_builder_get_object(builder, "disc_num"));

    g_object_unref(builder);

    if (art && art_mgr) {
        artwork_manager_load_thumb_into(art_mgr, track->album_id,
                                         LOAD_PRIORITY_VISIBLE, art, NULL);
    }

    if (title_lbl) gtk_label_set_text(GTK_LABEL(title_lbl), track->title);
    if (album_lbl) gtk_label_set_text(GTK_LABEL(album_lbl), track->album);
    if (artist_lbl) gtk_label_set_text(GTK_LABEL(artist_lbl), track->artist);

    if (year_lbl) {
        if (track->year > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", track->year);
            gtk_label_set_text(GTK_LABEL(year_lbl), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(year_lbl), "");
        }
    }

    if (duration_lbl) {
        char buf[16];
        ui_format_duration(track->duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(duration_lbl), buf);
    }

    if (track_num_lbl) gtk_widget_set_visible(track_num_lbl, show_track_disc);
    if (disc_num_lbl) gtk_widget_set_visible(disc_num_lbl, show_track_disc);

    /* Store track ID and metadata for right-click handler */
    g_object_set_data(G_OBJECT(row), "track-id", GSIZE_TO_POINTER((gsize)track->id));
    g_object_set_data_full(G_OBJECT(row), "track-path", g_strdup(track->path), g_free);
    g_object_set_data_full(G_OBJECT(row), "track-title", g_strdup(track->title), g_free);
    g_object_set_data_full(G_OBJECT(row), "track-artist", g_strdup(track->artist), g_free);
    g_object_set_data_full(G_OBJECT(row), "track-album", g_strdup(track->album), g_free);

    RowCallbackData *cb_data = row_callback_data_new(cbs, NULL);
    g_object_set_data_full(G_OBJECT(row), "row-callbacks", cb_data, row_callback_data_free);

    /* Add right-click gesture for queuing track */
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_track_row_right_click), NULL);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(right_click));

    /* Track rows are NOT wrapped in a button - they just have right-click */
    return row;
}
