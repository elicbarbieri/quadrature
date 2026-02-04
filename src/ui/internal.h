/**
 * Quadrature UI Internal Header
 *
 * Single consolidated header for all UI widgets.
 */

#ifndef QUADRATURE_UI_INTERNAL_H
#define QUADRATURE_UI_INTERNAL_H

#include <gtk/gtk.h>
#include "quadrature/quadrature_audio.h"
#include "quadrature/quadrature_library.h"
#include "quadrature/ui/app_settings.h"

G_BEGIN_DECLS

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_CHANNELS 4
#define SPECTRUM_BARS 24

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared Colors (for 60fps snapshot rendering)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const GdkRGBA UI_COLOR_CYAN     = {0.00f, 0.83f, 1.00f, 1.0f};

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void ui_toggle_css(GtkWidget *w, const char *cls, gboolean on) {
    if (on) gtk_widget_add_css_class(w, cls);
    else    gtk_widget_remove_css_class(w, cls);
}

/* "Various Artists" is a synthetic placeholder for compilations, not a real artist.
 * UI elements referencing it should be inactive (dimmed, non-clickable). */
static inline gboolean ui_is_various_artists(const char *name) {
    return name && g_ascii_strcasecmp(name, "Various Artists") == 0;
}

/* Clear all children from a GtkBox */
static inline void ui_box_clear(GtkBox *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))))
        gtk_box_remove(box, child);
}

/* Populate a GtkBox with genre pill labels. Splits on semicolon,
 * shows at most max_show pills. Hides the box if no genres. */
static inline void ui_populate_genre_pills(GtkBox *box, const char *genres, guint max_show) {
    ui_box_clear(box);

    if (!genres || !genres[0]) {
        gtk_widget_set_visible(GTK_WIDGET(box), FALSE);
        return;
    }

    gchar **parts = g_strsplit(genres, ";", 0);
    guint n = g_strv_length(parts);
    guint shown = 0;

    for (guint i = 0; i < n && shown < max_show; i++) {
        g_strstrip(parts[i]);
        if (!parts[i][0]) continue;

        GtkWidget *pill = gtk_label_new(parts[i]);
        gtk_label_set_ellipsize(GTK_LABEL(pill), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(pill), 18);
        gtk_widget_add_css_class(pill, "genre-pill");
        gtk_box_append(box, pill);
        shown++;
    }

    g_strfreev(parts);
    gtk_widget_set_visible(GTK_WIDGET(box), shown > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SpectrumDisplay Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_SPECTRUM (ui_spectrum_get_type())
G_DECLARE_FINAL_TYPE(UiSpectrum, ui_spectrum, UI, SPECTRUM, GtkWidget)

GtkWidget *ui_spectrum_new(int num_bars);
void ui_spectrum_set_bars(UiSpectrum *s, const float *left, const float *right, int count);

/* ═══════════════════════════════════════════════════════════════════════════
 * ChannelStrip Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_CHANNEL_STRIP (ui_channel_strip_get_type())
G_DECLARE_FINAL_TYPE(UiChannelStrip, ui_channel_strip, UI, CHANNEL_STRIP, GtkWidget)

/* Device hardware state - tracks whether output device is available */
typedef enum {
    DEVICE_STATE_VALID = 0,      /* Device configured and available */
    DEVICE_STATE_UNCONFIGURED,   /* No output device assigned */
    DEVICE_STATE_INVALID         /* Device was configured but is now missing */
} DeviceState;

/* Operational mode - broadcast automation states */
typedef enum {
    CHANNEL_MODE_IDLE = 0,       /* Normal operation */
    CHANNEL_MODE_PREVIEW,        /* PFL active */
    CHANNEL_MODE_QUEUED,         /* Ready for on-air */
    CHANNEL_MODE_ON_AIR          /* Live broadcast */
} ChannelMode;

GtkWidget *ui_channel_strip_new(int channel_id, audio_pipeline_t *pipeline, library_cache_t *library);
void ui_channel_strip_update(UiChannelStrip *strip, audio_pipeline_t *pipeline);
void ui_channel_strip_set_spectrum_visible(UiChannelStrip *strip, gboolean visible);
void ui_channel_strip_set_device_name(UiChannelStrip *strip, const char *device_name);
int ui_channel_strip_get_channel_id(UiChannelStrip *strip);
quadrature_result_t ui_channel_strip_load_track(UiChannelStrip *strip,
                                                 int64_t track_id,
                                                 const char *path,
                                                 const char *title,
                                                 const char *artist,
                                                 const char *album);
void ui_channel_strip_update_track_display(UiChannelStrip *strip,
                                            int64_t track_id,
                                            const char *path,
                                            const char *title,
                                            const char *artist,
                                            const char *album);

void ui_channel_strip_set_device_state(UiChannelStrip *strip, DeviceState state);
DeviceState ui_channel_strip_get_device_state(UiChannelStrip *strip);
void ui_channel_strip_set_mode(UiChannelStrip *strip, ChannelMode mode);
ChannelMode ui_channel_strip_get_mode(UiChannelStrip *strip);
void ui_channel_strip_set_focused(UiChannelStrip *strip, gboolean focused);
gboolean ui_channel_strip_get_focused(UiChannelStrip *strip);
gboolean ui_channel_strip_is_active(UiChannelStrip *strip);

/* Track context query - uses LibraryCache for album context */
int64_t ui_channel_strip_get_current_track_id(UiChannelStrip *strip);

/* Track navigation */
gboolean ui_channel_strip_previous_track(UiChannelStrip *strip);
gboolean ui_channel_strip_next_track(UiChannelStrip *strip);
gboolean ui_channel_strip_can_go_previous(UiChannelStrip *strip);
gboolean ui_channel_strip_can_go_next(UiChannelStrip *strip);

/* ═══════════════════════════════════════════════════════════════════════════
 * TransportBar Widget
 *
 * Global transport controls: skip, prev/next, play/pause, shuttle slider.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_TRANSPORT_BAR (ui_transport_bar_get_type())
G_DECLARE_FINAL_TYPE(UiTransportBar, ui_transport_bar, UI, TRANSPORT_BAR, GtkWidget)

GtkWidget *ui_transport_bar_new(audio_pipeline_t *pipeline);
void ui_transport_bar_set_pipeline(UiTransportBar *bar, audio_pipeline_t *pipeline);
void ui_transport_bar_set_focused_channel(UiTransportBar *bar, int channel);
void ui_transport_bar_update(UiTransportBar *bar);
int ui_transport_bar_get_focused_channel(UiTransportBar *bar);


/* ═══════════════════════════════════════════════════════════════════════════
 * Main Window
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_WINDOW (ui_window_get_type())
G_DECLARE_FINAL_TYPE(UiWindow, ui_window, UI, WINDOW, GtkApplicationWindow)

GtkWidget *ui_window_new(GtkApplication *app,
                         audio_pipeline_t *pipeline,
                         library_cache_t *library_cache,
                         app_settings_t *settings);

void ui_window_navigate_to(UiWindow *win, const char *view);
void ui_window_set_spectrum_visible(UiWindow *win, gboolean visible);
void ui_window_set_focused_channel(UiWindow *win, int channel);
int ui_window_get_focused_channel(UiWindow *win);
void ui_window_clear_focus(UiWindow *win);

/* Toast notifications */
void ui_window_show_toast(UiWindow *win, const char *message);

/* ═══════════════════════════════════════════════════════════════════════════
 * MetadataDialog Widget
 *
 * Shows detailed track metadata with Copy JSON functionality.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_METADATA_DIALOG (ui_metadata_dialog_get_type())
G_DECLARE_FINAL_TYPE(UiMetadataDialog, ui_metadata_dialog, UI, METADATA_DIALOG, GtkWindow)

GtkWidget *ui_metadata_dialog_new(GtkWindow *parent, const library_track_info_t *track);

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Module (see library/internal.h for full API)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "library/internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Helpers (row_helpers.c)
 *
 * Functions for creating consistent list rows from LibraryCache data.
 * All row creation functions store entity IDs for handler access.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Size groups for column alignment across multiple rows in a list.
 * Pass NULL for any group to skip alignment for that column. */
typedef struct {
    GtkSizeGroup *col1;  /* Left content column (title/name) */
    GtkSizeGroup *col2;  /* Right content column (metadata/art strip) */
} UiRowSizeGroups;

/* Attach click handlers to a row. Handlers receive entity ID from row data. */
void ui_row_attach_handlers(GtkWidget *row, RowCallbacks *callbacks);

/* GtkListBox row-activated handler. Connect to "row-activated" signal for Enter key support.
 * Reads callbacks from child's "row-handler-data" (set by ui_row_attach_handlers). */
void ui_list_box_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Creation Helpers
 *
 * Create rows from LibraryCache data. Rows are stateless widgets.
 * Click handlers attached separately via ui_row_attach_handlers().
 *
 * Each row stores entity data via g_object_set_data():
 *   - Artist rows: "artist-id"
 *   - Album rows: "album-id"
 *   - Track rows: "track-id", "track-path", "album-id"
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Format duration in milliseconds to "M:SS" string */
void ui_format_duration(uint32_t ms, char *buf, size_t len);

/* Create artist row with name and album/track counts.
 * Optionally shows album art strip (up to 6 thumbnails).
 * size_groups: optional size groups for column alignment (NULL to skip) */
GtkWidget *ui_create_artist_row(const library_artist_info_t *artist,
                                 library_cache_t *cache,
                                 ArtworkManager *art_mgr,
                                 gboolean show_art_strip,
                                 UiRowSizeGroups *size_groups);

/* Create album row with art, title, artist, year, track count.
 * cache: library cache for resolving first track (NULL to skip first-track-id)
 * artist_cbs: optional callbacks for artist name click (navigate to artist)
 * size_groups: optional size groups for column alignment (NULL to skip)
 * Stores: "album-id", "first-track-id" (if cache provided and album has tracks) */
GtkWidget *ui_create_album_row(const library_album_info_t *album,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_count,
                                RowCallbacks *artist_cbs,
                                UiRowSizeGroups *size_groups);

/* Create track row with art, title, album, artist buttons, year, duration.
 * show_album_info controls visibility of album column.
 * artist_cbs: callbacks for artist button clicks (navigate to artist)
 * album_cbs: callbacks for album button click (navigate to album)
 * size_groups: optional size groups for column alignment (NULL to skip)
 * Stores: "track-id", "track-path", "track-artists" */
GtkWidget *ui_create_track_row(const library_track_info_t *track,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_album_info,
                                RowCallbacks *artist_cbs,
                                RowCallbacks *album_cbs,
                                UiRowSizeGroups *size_groups);

/* Create album detail track item (compact row for album detail view).
 * Shows track number, title, featuring artists, info button, duration.
 * artist_cbs: callbacks for featuring artist button clicks
 * Stores: "track-id", "track-path" */
GtkWidget *ui_create_album_detail_track_item(const library_track_info_t *track,
                                               library_cache_t *cache,
                                               RowCallbacks *artist_cbs);

/* Create disc separator header for multi-disc albums.
 * Shows "DISC N" label with subtle styling. */
GtkWidget *ui_create_disc_header(uint16_t disc_num);

/* Create album detail card for artist detail view.
 * Contains: album art, metadata, preview track list with automatic disc headers.
 * Stores: "album-id" on the card widget.
 * max_preview_tracks: limit of tracks to show (0 for all)
 * track_cbs: optional callbacks for track rows (NULL to skip handler attachment)
 * artist_cbs: optional callbacks for artist buttons in track rows */
GtkWidget *ui_create_album_detail_card(const library_album_info_t *album,
                                        const GPtrArray *tracks,
                                        library_cache_t *cache,
                                        ArtworkManager *art_mgr,
                                        guint max_preview_tracks,
                                        RowCallbacks *track_cbs,
                                        RowCallbacks *artist_cbs);

/* ═══════════════════════════════════════════════════════════════════════════
 * List View Loading States
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_list_view_set_loading(GtkWidget *list, gboolean loading);
void ui_list_view_set_empty(GtkWidget *list, const char *message);
void ui_list_view_set_error(GtkWidget *list, const char *message, GCallback retry_cb);

G_END_DECLS

#endif /* QUADRATURE_UI_INTERNAL_H */
