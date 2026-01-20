/**
 * Quadrature UI Internal Header
 *
 * Single consolidated header for all UI widgets.
 */

#ifndef QUADRATURE_UI_INTERNAL_H
#define QUADRATURE_UI_INTERNAL_H

#include <gtk/gtk.h>
#include "quadrature/audio/audio_pipeline.h"
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

/* ═══════════════════════════════════════════════════════════════════════════
 * SpectrumDisplay Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_SPECTRUM (ui_spectrum_get_type())
G_DECLARE_FINAL_TYPE(UiSpectrum, ui_spectrum, UI, SPECTRUM, GtkWidget)

GtkWidget *ui_spectrum_new(int num_bars);
void ui_spectrum_set_bars(UiSpectrum *s, const float *bars, int count);

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

GtkWidget *ui_channel_strip_new(int channel_id, audio_pipeline_t *pipeline);
void ui_channel_strip_update(UiChannelStrip *strip, audio_pipeline_t *pipeline);
void ui_channel_strip_set_spectrum_visible(UiChannelStrip *strip, gboolean visible);
void ui_channel_strip_set_device_name(UiChannelStrip *strip, const char *device_name);
int ui_channel_strip_get_channel_id(UiChannelStrip *strip);
quadrature_result_t ui_channel_strip_load_track(UiChannelStrip *strip,
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

/* Album context API */
#include "quadrature/database/database.h"
void ui_channel_strip_set_album_context(UiChannelStrip *strip,
                                         int64_t album_id,
                                         const char *album_name,
                                         const db_track_t *tracks,
                                         int track_count,
                                         int current_index);
void ui_channel_strip_clear_album_context(UiChannelStrip *strip);
int64_t ui_channel_strip_get_album_id(UiChannelStrip *strip);
int ui_channel_strip_get_track_index(UiChannelStrip *strip);
int ui_channel_strip_get_track_count(UiChannelStrip *strip);
gboolean ui_channel_strip_has_album_context(UiChannelStrip *strip);

/* Track navigation */
gboolean ui_channel_strip_previous_track(UiChannelStrip *strip);
gboolean ui_channel_strip_next_track(UiChannelStrip *strip);
gboolean ui_channel_strip_can_go_previous(UiChannelStrip *strip);
gboolean ui_channel_strip_can_go_next(UiChannelStrip *strip);

/* Autoplay control */
void ui_channel_strip_set_autoplay(UiChannelStrip *strip, gboolean autoplay);
gboolean ui_channel_strip_get_autoplay(UiChannelStrip *strip);

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

GtkWidget *ui_metadata_dialog_new(GtkWindow *parent);
void ui_metadata_dialog_set_track(UiMetadataDialog *dialog,
                                   const db_track_t *track,
                                   const db_track_metadata_t *metadata);

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Module (see library/internal.h for full API)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "library/internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Creation Helpers
 *
 * Create fully-populated, interactive row widgets from database types.
 * Each row is wrapped in a clickable button with handlers attached.
 * Entity IDs stored via g_object_set_data() for handler access.
 *
 * Click behaviors:
 *   - Artist row: left-click navigates to artist detail
 *   - Album row: left-click navigates to album detail, right-click queues track 1
 *   - Track row: right-click queues track to focused channel
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Format duration in milliseconds to "M:SS" string */
void ui_format_duration(uint32_t ms, char *buf, size_t len);

/* Create clickable artist row. Left-click calls on_navigate(ARTIST, id). */
GtkWidget *ui_create_artist_row(const db_artist_t *artist,
                                 gboolean show_art_strip,
                                 const LibraryCallbacks *cbs);

/* Create clickable album row. Left-click calls on_navigate(ALBUM, id).
 * Right-click calls on_play() with track 1 of the album. */
GtkWidget *ui_create_album_row(const db_album_t *album,
                                ArtworkManager *art_mgr,
                                gboolean show_count,
                                quadrature_db_t *db,
                                const LibraryCallbacks *cbs);

/* Create track row. Right-click calls on_play() with track info. */
GtkWidget *ui_create_track_row(const db_track_t *track,
                                ArtworkManager *art_mgr,
                                gboolean show_track_disc,
                                const LibraryCallbacks *cbs);

G_END_DECLS

#endif /* QUADRATURE_UI_INTERNAL_H */
