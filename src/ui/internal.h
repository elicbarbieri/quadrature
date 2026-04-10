/**
 * Quadrature UI Internal Header
 *
 * Single consolidated header for all UI widgets.
 */

#ifndef QUADRATURE_UI_INTERNAL_H
#define QUADRATURE_UI_INTERNAL_H

#include <gtk/gtk.h>
#include "quadrature/audio.h"
#include "quadrature/library.h"
#include "quadrature/indexer.h"
#include "quadrature/metadata.h"
#include "quadrature/settings.h"
#include "quadrature/gpio.h"
#include "quadrature/ui.h"

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

/* Find a widget by its GtkBuildable ID in the widget tree (recursive). */
GtkWidget *find_widget_by_name(GtkWidget *parent, const char *name);

/* Clear all children from a GtkBox */
static inline void ui_box_clear(GtkBox *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))))
        gtk_box_remove(box, child);
}

/**
 * Load a widget tree from a GResource .ui template.
 * Returns the root widget (ref'd) and the builder (caller must g_object_unref).
 * Typical usage:
 *   GtkBuilder *b;
 *   GtkWidget *row = ui_builder_load("/org/quadrature/ui/foo.ui", "row", &b);
 *   GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(b, "title"));
 *   g_object_unref(b);
 */
static inline GtkWidget *ui_builder_load(const char *resource_path,
                                          const char *root_id,
                                          GtkBuilder **builder_out) {
    GtkBuilder *builder = gtk_builder_new_from_resource(resource_path);
    GtkWidget *root = GTK_WIDGET(gtk_builder_get_object(builder, root_id));
    g_object_ref(root);
    *builder_out = builder;
    return root;
}

/** Format a year into a label. No-op if year is 0. */
static inline void ui_set_year_label(GtkWidget *label, uint16_t year) {
    if (!label) return;
    if (year > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", year);
        gtk_label_set_text(GTK_LABEL(label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(label), "");
    }
}

/** Format a track count (1 → "Single", N → "NN Tracks"). (ui_math.c) */
void ui_format_track_count(char *buf, size_t len, uint32_t count);

/** Create a QuadOverflowBox pre-populated with GENRE_PILL_MAX label slots
 *  + 1 overflow "…" label.  Overflow planning happens inside the box's
 *  own size_allocate — no external signals or callbacks needed. */
GtkWidget *ui_genre_pills_new(int spacing);

/** Bind genre text into pre-allocated pill slots.
 *  Splits genres on semicolons, sets label text.  The QuadOverflowBox
 *  handles overflow during its next size_allocate automatically. */
void ui_genre_pills_bind(GtkWidget *genres_box, const char *genres);

/* ═══════════════════════════════════════════════════════════════════════════
 * Popover Shortcut Passthrough
 *
 * Install on any autohide popover so navigation hotkeys (Ctrl+F, Ctrl+A,
 * Ctrl+B, Ctrl+T, Ctrl+R, Escape) dismiss the popover and activate the
 * corresponding window action. Call once after popover creation.
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_popover_install_shortcuts(GtkPopover *popover);

/* ═══════════════════════════════════════════════════════════════════════════
 * SpectrumDisplay Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_SPECTRUM (ui_spectrum_get_type())
G_DECLARE_FINAL_TYPE(UiSpectrum, ui_spectrum, UI, SPECTRUM, GtkWidget)

GtkWidget *ui_spectrum_new(int num_bars);
void ui_spectrum_set_bars(UiSpectrum *s, const float *left, const float *right, int count);

/* ═══════════════════════════════════════════════════════════════════════════
 * WaveformSeekBar Widget
 *
 * Loudness-over-time bar visualization that renders behind the seek bar.
 * Bars before playback position are blue (played), after are gray (unplayed).
 * Grow-out animation when loudness data first becomes available.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_WAVEFORM_SEEK_BAR (ui_waveform_seek_bar_get_type())
G_DECLARE_FINAL_TYPE(UiWaveformSeekBar, ui_waveform_seek_bar, UI, WAVEFORM_SEEK_BAR, GtkWidget)

GtkWidget      *ui_waveform_seek_bar_new(void);
GtkAdjustment  *ui_waveform_seek_bar_get_adjustment(UiWaveformSeekBar *w);
gboolean        ui_waveform_seek_bar_is_dragging(UiWaveformSeekBar *w);
void            ui_waveform_seek_bar_set_playback_position(UiWaveformSeekBar *w, double value);
void            ui_waveform_seek_bar_set_loudness(UiWaveformSeekBar *w, const float *bins, int count);
void            ui_waveform_seek_bar_clear(UiWaveformSeekBar *w);
gboolean        ui_waveform_seek_bar_is_animating(UiWaveformSeekBar *w);

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

/* Note: ChannelMode is now defined in quadrature/ui.h (public API) */

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

gboolean ui_channel_strip_has_track(UiChannelStrip *strip);

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
typedef enum {
    TOAST_INFO = 0,   /* Neutral -- gray border (default) */
    TOAST_SUCCESS,    /* Green -- completion, saved, etc. */
    TOAST_WARNING,    /* Amber -- non-fatal issues */
    TOAST_ERROR,      /* Red   -- failures */
} ToastVariant;

/* Show a plain-text toast. duration_ms controls auto-hide (0 = 2000ms default). */
void ui_window_show_toast(UiWindow *win, const char *message, ToastVariant variant, guint duration_ms);

/* Show a Pango-markup toast (bold, spans, etc.). Same parameters as above. */
void ui_window_show_toast_markup(UiWindow *win, const char *markup, ToastVariant variant, guint duration_ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * MetadataDialog Widget
 *
 * Shows detailed track metadata with Copy JSON functionality.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UI_TYPE_METADATA_DIALOG (ui_metadata_dialog_get_type())
G_DECLARE_FINAL_TYPE(UiMetadataDialog, ui_metadata_dialog, UI, METADATA_DIALOG, GtkWindow)

GtkWidget *ui_metadata_dialog_new(GtkWindow *parent, const library_track_info_t *track,
                                  const char *resolved_path);

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Module (see library/internal.h for full API)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "library/internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * IndexerController
 *
 * GTK wrapper around the indexer that supports parallel scanning of multiple
 * library roots. Each library root gets its own indexer_t instance.
 *
 * GSignals (all emitted on the main thread):
 *   started(library_path: string)
 *   progress(library_path: string, progress: pointer to indexer_progress_t)
 *   library-updated(library_path: string, progress: pointer)
 *   artwork-updated(library_path: string, progress: pointer)
 *   completed(library_path: string, ok: boolean, progress: pointer)
 *   all-completed
 *
 * GObject properties (read-only):
 *   running  - TRUE while any library is being scanned
 * ═══════════════════════════════════════════════════════════════════════════ */

#define INDEXER_TYPE_CONTROLLER (indexer_controller_get_type())
G_DECLARE_FINAL_TYPE(IndexerController, indexer_controller, INDEXER, CONTROLLER, GObject)

IndexerController* indexer_controller_new(void);
void indexer_controller_set_thread_count(IndexerController* self, int thread_count);
void indexer_controller_set_process_artwork(IndexerController* self, gboolean enable);
void indexer_controller_set_art_size(IndexerController* self, int size);
void indexer_controller_set_max_concurrent(IndexerController* self, int max_concurrent);
gboolean indexer_controller_start(IndexerController* self,
                                   const char** library_roots,
                                   const char** data_roots,
                                   gsize path_count);
void indexer_controller_cancel(IndexerController* self);
void indexer_controller_cancel_library(IndexerController* self, const char* library_path);
gboolean indexer_controller_is_running(IndexerController* self);
void indexer_controller_set_musicbrainz_resolve(IndexerController* self, gboolean enable);
void indexer_controller_set_pg_conninfo(IndexerController* self, const char* conninfo);
void indexer_controller_set_mb_solr_url(IndexerController* self, const char* url);
void indexer_controller_set_acoustid_pg_conninfo(IndexerController* self, const char* conninfo);
void indexer_controller_set_acoustid_index_url(IndexerController* self, const char* url);
void indexer_controller_set_fanart_api_key(IndexerController* self, const char* api_key);

/* ═══════════════════════════════════════════════════════════════════════════
 * LibraryMonitor — detects mount/unmount of library paths
 *
 * Signals:
 *   availability-changed(lib_idx: int, available: gboolean)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LIBRARY_TYPE_MONITOR (library_monitor_get_type())
G_DECLARE_FINAL_TYPE(LibraryMonitor, library_monitor, LIBRARY, MONITOR, GObject)

LibraryMonitor *library_monitor_new(library_cache_t *cache, app_settings_t *settings);
void            library_monitor_start(LibraryMonitor *self);
void            library_monitor_stop(LibraryMonitor *self);
void            library_monitor_check_now(LibraryMonitor *self);

/* ═══════════════════════════════════════════════════════════════════════════
 * ProportionalBox Widget
 *
 * Horizontal container with four named slots: art, left, right, meta.
 * art and meta receive their natural widths; left and right split the
 * remaining flexible space at left_ratio : (1 - left_ratio).
 *
 * Sizing runs inside GTK's own size_allocate pass, so children receive
 * their final widths before the first frame is drawn -- no tick callback,
 * no post-allocation correction, no one-frame layout pop.
 *
 * GtkBuilder slot assignment via child type= attribute:
 *   <child type="art">  ... </child>
 *   <child type="left"> ... </child>
 *   <child type="right">... </child>
 *   <child type="meta"> ... </child>
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Pre-allocate callback — invoked with the exact pixel budget for a
 *  ProportionalBox slot BEFORE its gtk_widget_size_allocate runs.
 *  @param child  The slot widget (col_left or col_right)
 *  @param width  Available width in pixels (integer, final) */
typedef void (*PBoxPreAllocate)(GtkWidget *child, int width, gpointer user_data);

#define QUADRATURE_TYPE_PROPORTIONAL_BOX (proportional_box_get_type())
G_DECLARE_FINAL_TYPE(ProportionalBox, proportional_box,
                     QUADRATURE, PROPORTIONAL_BOX, GtkWidget)

/** Register a pre-allocate callback for a flexible slot ("left" or "right").
 *  The callback fires every size_allocate with the integer pixel budget
 *  before the child's own vfunc lays out its children. */
void proportional_box_set_pre_allocate(ProportionalBox *self,
                                        const char *slot,
                                        PBoxPreAllocate callback,
                                        gpointer user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 * QuadOverflowBox Widget
 *
 * Wrapping container that flows children left→right, wrapping to the next
 * row when they don't fit.  The LAST child is the overflow indicator ("…"),
 * shown only when items exceed max-rows.
 *
 * Properties:
 *   spacing      — horizontal gap between items (default 0)
 *   row-spacing  — vertical gap between rows (default 0)
 *   max-rows     — maximum visible rows; overflow on last row (default 1)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define QUADRATURE_TYPE_OVERFLOW_BOX (quad_overflow_box_get_type())
G_DECLARE_FINAL_TYPE(QuadOverflowBox, quad_overflow_box,
                     QUADRATURE, OVERFLOW_BOX, GtkWidget)

void quad_overflow_box_append(QuadOverflowBox *self, GtkWidget *child);

/** Set the number of populated item children (excludes overflow indicator).
 *  Unpopulated pre-allocated slots are hidden during layout. */
void quad_overflow_box_set_item_count(QuadOverflowBox *self, guint count);

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Button Helpers (artist_buttons.c)
 *
 * Clickable artist/album buttons with overflow popover support.
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget* create_artist_button(int64_t artist_id, const char* name, RowCallbacks* callbacks);
GtkWidget* create_artist_overflow_button(const GPtrArray* artists, RowCallbacks* callbacks);

void on_artist_button_clicked(GtkButton *button, gpointer user_data);
void on_album_button_clicked(GtkButton *button, gpointer user_data);
void on_credit_mbid_navigate(GtkButton *button, gpointer user_data);

void populate_artist_buttons(GtkWidget* box,
                              GtkWidget* constraint_widget,
                              double constraint_fraction,
                              library_cache_t *cache,
                              int64_t track_id,
                              library_artist_role_t role,
                              RowCallbacks* callbacks,
                              gboolean add_feat_prefix);

void populate_artist_buttons_combined(GtkWidget *box,
                                       GtkWidget *constraint_widget,
                                       double constraint_fraction,
                                       library_cache_t *cache,
                                       int64_t track_id,
                                       RowCallbacks *callbacks,
                                       gboolean show_primary);

/* Width-aware credit role pill layout.
 * Fills credit_annotation box with pills that fit the available width,
 * collapsing excess pills into a static "+N more" overflow indicator.
 * Roles are deep-copied; constraint_widget drives re-layout on resize.
 * first_child_width is the pre-measured width of the artist button already
 * in the box (pills are appended after it). */
void populate_credit_pills(GtkWidget *credit_annotation,
                            GtkWidget *constraint_widget,
                            double constraint_fraction,
                            const char *const *roles,
                            guint role_count,
                            int first_child_width);

/* ═══════════════════════════════════════════════════════════════════════════
 * SelectionGroup (row_helpers.c)
 *
 * Mutual-exclusion selection across multiple GtkListBox widgets.
 * When a row is selected in any member, all other members are deselected.
 * Reusable by any view (detail view, search results, etc.).
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _SelectionGroup SelectionGroup;

SelectionGroup *ui_selection_group_new(void);
void ui_selection_group_add(SelectionGroup *group, GtkListBox *list);
void ui_selection_group_remove(SelectionGroup *group, GtkListBox *list);
void ui_selection_group_free(SelectionGroup *group);

/* ═══════════════════════════════════════════════════════════════════════════
 * Overflow Box — Generic Width-Aware Pill/Badge Layout (row_helpers.c)
 *
 * Populates a GtkBox with as many widgets as fit within a width budget,
 * collapsing the rest into a caller-provided overflow indicator.
 * Re-lays out automatically when the constraint widget resizes.
 *
 * create_item(index, user_data)  → floating GtkWidget for item #index
 * create_overflow(first_hidden_index, total_count, user_data)
 *                                → overflow indicator widget (or NULL to just stop)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef GtkWidget* (*UiOverflowCreateItem)(guint index, gpointer user_data);
typedef GtkWidget* (*UiOverflowCreateOverflow)(guint first_hidden_index,
                                                guint total_count,
                                                gpointer user_data);

typedef struct {
    GtkWidget  *box;                  /* Target horizontal GtkBox */
    GtkWidget  *constraint_widget;    /* Widget whose width constrains layout (or NULL) */
    double      constraint_fraction;  /* Fraction of constraint width to use */
    int         default_max_width;    /* Fallback when no constraint (default: 300) */
    int         pinned_children;      /* Leading children to preserve (0 = clear all) */

    UiOverflowCreateItem     create_item;
    UiOverflowCreateOverflow create_overflow;  /* NULL = just stop adding */
    guint       item_count;

    gpointer       user_data;
    GDestroyNotify user_data_destroy;  /* Frees user_data when box is destroyed */
} UiOverflowBoxParams;

/** Set up width-aware overflow layout. Wires map + notify::width signals.
 *  Stores lifecycle data as GObject data on the box. */
void ui_overflow_box_setup(const UiOverflowBoxParams *params);

/** Pure layout planner — no GTK dependency (overflow_plan.c).
 *  Returns how many items to show. Sets *needs_overflow if not all fit.
 *  spacing: gap between each item (same as GtkBox spacing). */
guint ui_overflow_box_plan_layout(int budget,
                                  const int *item_widths,
                                  guint item_count,
                                  int overflow_width,
                                  int spacing,
                                  gboolean *needs_overflow);

/* ═══════════════════════════════════════════════════════════════════════════
 * Pure Math Helpers (ui_math.c) — no GTK dependency, unit-testable.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Quadratic shuttle slider → playback speed mapping. */
float ui_shuttle_value_to_speed(double slider_value, shuttle_mode_t mode);

/** Piecewise linear log-scale normalization (0–100% → 0–1). */
double ui_log_pct_norm(double pct);

/** Fill LUT with bell-curve weights: lut[i] = exp(-(d²)/(2σ²)). */
void ui_bell_curve_lut(float *lut, int n, double sigma);

/** Attach smooth-scroll interceptor to a GtkScrolledWindow.
 *  Converts discrete mouse-wheel events into animated ease-out scrolling.
 *  Touchpad/smooth-scroll input passes through unmodified. */
void ui_smooth_scroll_attach(GtkScrolledWindow *sw);

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Helpers (row_helpers.c)
 *
 * Functions for creating consistent list rows from LibraryCache data.
 * All row creation functions store entity IDs for handler access.
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *ui_create_library_badge(library_cache_t *cache, int library_index);

typedef enum {
    BADGE_ENTITY_ARTIST,
    BADGE_ENTITY_ALBUM,
    BADGE_ENTITY_TRACK,   /* No MBID cross-library lookup; source library only */
} badge_entity_type_t;

/** Populate a badges box with one label per library this entity belongs to.
 *  Uses MBID index lookup for artists/albums to show cross-library presence.
 *  Hides the box if ≤1 library or no badges.
 *  @param constraint_widget  Widget whose width constrains badge overflow
 *                            layout (typically col_right). NULL uses fixed
 *                            default_max_width. */
void ui_populate_library_badges(GtkWidget *badges_box,
                                 library_cache_t *cache,
                                 int64_t entity_global_id,
                                 badge_entity_type_t entity_type,
                                 GtkWidget *constraint_widget);

/* Size groups for column alignment across multiple rows in a list.
 * Pass NULL for any group to skip alignment for that column.
 *   col1: title/name column -- used by artist, album, and track rows
 *   col2: right content column -- used by artist rows (art strip) and album
 *         rows only; track rows omit it (ProportionalBox already enforces
 *         uniform column widths, so a size group here would fight layout) */
typedef struct UiRowSizeGroups {
    GtkSizeGroup *col1;
    GtkSizeGroup *col2;
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
 *   - Track rows: "track-id", "album-id"
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Format duration in milliseconds to "M:SS" string */
void ui_format_duration(uint32_t ms, char *buf, size_t len);

/* Format a release date ("YYYY-MM-DD", "YYYY-MM", or "YYYY") to human-readable.
 * Returns a newly allocated string (caller must g_free), or NULL on invalid input. */
char *ui_format_release_date(const char *date);

/* ── Shell / Rebind for GtkListView factory recycling ──
 * Shell creates the widget tree once (factory setup).
 * Rebind populates data into the existing tree (factory bind).
 * This avoids re-parsing GtkBuilder XML on every scroll recycle. */

GtkWidget *ui_create_artist_row_shell(void);
void ui_rebind_artist_row(GtkWidget *row,
                           const library_artist_info_t *artist,
                           library_cache_t *cache,
                           ArtworkManager *art_mgr,
                           uint32_t library_mask);

GtkWidget *ui_create_album_row_shell(void);
void ui_rebind_album_row(GtkWidget *row,
                          const library_album_info_t *album,
                          library_cache_t *cache,
                          ArtworkManager *art_mgr,
                          gboolean show_count,
                          RowCallbacks *artist_cbs);

/* Create artist row with name and album/track counts.
 * Optionally shows album art strip (up to 6 thumbnails).
 * size_groups: optional size groups for column alignment (NULL to skip) */
GtkWidget *ui_create_artist_row(const library_artist_info_t *artist,
                                 library_cache_t *cache,
                                 ArtworkManager *art_mgr,
                                 gboolean show_art_strip,
                                 UiRowSizeGroups *size_groups,
                                 uint32_t library_mask);

/* Optional credit annotation for album rows.
 * When non-NULL, displays a third row showing the credited artist name
 * as a clickable button followed by role pills (e.g. [Guitarist][Vocals]).
 * Used in appears-on album view for MB-credited artists. */
typedef struct {
    const char *artist_name;   /* Credited artist name */
    int64_t artist_id;         /* Library artist ID for navigation (0 if MB-only) */
    const char *const *roles;  /* NULL-terminated array of role strings */
    guint role_count;          /* Number of roles */
} UiAlbumCreditInfo;

/* Create album row with art, title, artist, year, track count.
 * cache: library cache for resolving first track (NULL to skip first-track-id)
 * artist_cbs: optional callbacks for artist name click (navigate to artist)
 * size_groups: optional size groups for column alignment (NULL to skip)
 * credit: optional credit annotation (NULL to hide credit row)
 * Stores: "album-id", "first-track-id" (if cache provided and album has tracks) */
GtkWidget *ui_create_album_row(const library_album_info_t *album,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_count,
                                RowCallbacks *artist_cbs,
                                UiRowSizeGroups *size_groups,
                                const UiAlbumCreditInfo *credit);

/* Optional credit annotation for track rows.
 * When non-NULL, displays a third line under the artist buttons showing
 * role pills (purple) and artist name. Used in artist detail views
 * and credit search results. Matches UiAlbumCreditInfo multi-role layout. */
typedef struct {
    const char *const *roles;  /* NULL-terminated array of role strings */
    guint role_count;          /* Number of roles */
    const char *artist_name;   /* e.g. "John Smith" */
    int64_t artist_id;         /* Library artist ID for navigation (0 if MB-only) */
    const char *artist_mbid;   /* MusicBrainz ID for MBID-based navigation when artist_id==0 */
    const char *artist_type;   /* MusicBrainz artist type (Person/Group/etc.), NULL if unknown */
} UiTrackCreditInfo;

/* Create track row with art, title, album, artist buttons, year, duration.
 * show_album_info controls visibility of album column.
 * artist_cbs: callbacks for artist button clicks (navigate to artist)
 * album_cbs: callbacks for album button click (navigate to album)
 * size_groups: optional size groups for column alignment (NULL to skip)
 * credit: optional credit annotation (NULL to hide third line)
 * Stores: "track-id" */
GtkWidget *ui_create_track_row(const library_track_info_t *track,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_album_info,
                                RowCallbacks *artist_cbs,
                                RowCallbacks *album_cbs,
                                UiRowSizeGroups *size_groups,
                                const UiTrackCreditInfo *credit);

/* Create album detail track item (compact row for album detail view).
 * Shows track number, title, featuring artists, info button, duration.
 * artist_cbs: callbacks for featuring artist button clicks
 * album_artist_id: the album's primary artist — when a track's primary artist
 *   matches this, the primary pill is suppressed (only "ft" shown if present).
 *   Pass 0 to always show primary artists.
 * Stores: "track-id" */
GtkWidget *ui_create_album_detail_track_item(const library_track_info_t *track,
                                               library_cache_t *cache,
                                               RowCallbacks *artist_cbs,
                                               int64_t album_artist_id);

/* Create a "── TITLE ──────────" section header for use with
 * gtk_list_box_row_set_header() or standalone. Pango markup + snapshot-based
 * separator line — bypasses GTK4 CSS cascade issues in header contexts. */
GtkWidget *ui_make_section_header(const char *title);

/* Create album detail card (used in both album and artist detail views).
 * Contains: album art, metadata (incl. MusicBrainz label/date), genre pills,
 * and track list with automatic disc headers.
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

/* ═══════════════════════════════════════════════════════════════════════════
 * UiWindow Struct Definition (shared with search/, libraries/, settings/)
 *
 * Placed at end of header so all types it references (IndexerController,
 * ArtworkManager, FilterBarState, etc.) are already declared.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Per-phase state machine — consolidates widgets, timing, and rate tracking */
typedef enum {
    PHASE_WAITING,    /* dim, bar at 0, "Waiting" */
    PHASE_STARTUP,    /* active CSS, bar pulses, "Connecting..." — unknown-duration init */
    PHASE_ACTIVE,     /* cyan, bar pulses or fills, live rate */
    PHASE_COMPLETE,   /* green, bar at 1.0, item count + elapsed */
    PHASE_SKIPPED,    /* muted, bar at 0, "Up to date" */
    PHASE_ERROR,      /* red, bar at 0 */
} PhaseState;

typedef struct {
    /* Widgets */
    GtkWidget *container;
    GtkWidget *title;
    GtkWidget *bar;
    GtkWidget *label;
    GtkWidget *rate_label;
    /* State */
    PhaseState state;
    int64_t start_us;       /* g_get_monotonic_time() when activated */
    int64_t end_us;         /* when completed; 0 = not yet */
    /* Per-phase rate tracking */
    size_t  prev_count;
    int64_t prev_time;
    double  rate_ema;       /* items/sec, EMA α=0.3 */
} PhaseRow;

/* Library entry */
typedef struct {
    int64_t id;              /* Index into settings->libraries[] */
    char *path;
    char *data_path;         /* Where DB + artwork live (NULL = same as path) */
    char *name;
    /* Stats loaded from the library DB */
    size_t tracks;
    size_t albums;
    size_t artists;
    int64_t last_indexed_time;   /* Unix timestamp, 0 = never */
    size_t errors;
    /* Card root widget */
    GtkWidget *card;
    /* Reorder / lock controls */
    GtkWidget *move_up_btn;
    GtkWidget *move_down_btn;
    GtkWidget *lock_btn;
    /* Edit revealer + settings */
    GtkWidget *edit_btn;
    GtkWidget *edit_revealer;
    GtkWidget *edit_name_entry;
    GtkWidget *card_name_label;
    /* 3-state toggle buttons (Default / On / Off) per integration */
    GtkWidget *mb_toggles[3];       /* [0]=Default [1]=On [2]=Off */
    GtkWidget *acoustid_toggles[3];
    GtkWidget *fanart_toggles[3];
    GtkWidget *wikipedia_toggles[3];
    /* Progress revealer — slides down below stats during scan */
    GtkWidget *progress_revealer;
    /* Stats panel labels (updated in-place after indexing) */
    GtkWidget *stat_tracks;
    GtkWidget *stat_albums;
    GtkWidget *stat_artists;
    GtkWidget *stat_last_scanned;
    GtkWidget *stat_errors_btn;  /* Hidden when errors == 0 */
    /* Per-card phase rows (state machine) */
    PhaseRow phases[7];
    guint pulse_timer;  /* 100ms scan pulse */
    guint hide_timer;   /* 5s delay before crossfading back to stats */
    gboolean shown_initial_load_toast;  /* TRUE after first library-updated toast */
    gboolean pending_load_toast;        /* Deferred toast — shown when async stats arrive */
    gboolean available;                 /* Mirrors library_cache availability flag */
    gboolean locked;                    /* Mirrors settings->libraries[id].locked */
} LibEntry;

struct _UiWindow {
    GtkApplicationWindow parent;

    audio_pipeline_t *pipeline;
    app_settings_t *settings;
    quadrature_db_t *errors_lib_db;
    IndexerController *indexer;
    LibraryMonitor *lib_monitor;
    library_cache_t *library_cache;
    ArtworkManager *artwork_mgr;

    /* Template-bound layout widgets */
    GtkWidget *main_box;
    GtkWidget *content_stack;
    GtkWidget *channel_strips_box;
    GtkWidget *toast_overlay;
    GtkWidget *toast_label;

    GtkWidget *library_bar;

    /* Library filter (global, persists across view switches) */
    uint32_t library_mask;
    GtkToggleButton **library_toggles;  /* Array of toggle buttons (one per library) */
    int library_toggle_count;
    gboolean library_toggle_updating;   /* Prevents re-entrancy during programmatic updates */

    /* Runtime layout */
    GtkWidget *nav_bar;
    GSimpleAction *navigate_action;
    GtkWidget *stack;  /* Alias for content_stack */
    UiChannelStrip *channels[MAX_CHANNELS];
    int focused_channel;
    gboolean show_spectrum;
    const char *current_view;

    /* Toast timer */
    guint toast_timer;

    /* Errors */
    char *errors_library_path;

    /* Library views */
    GtkWidget *artists_view;
    GtkWidget *albums_view;
    GtkWidget *detail_view;     /* Unified detail view */
    const char *previous_view;  /* For back navigation */

    /* Search */
    GtkWidget *search_entry;
    GtkWidget *filter_btns[4];       /* All/Artists/Albums/Songs (radio group) */
    GtkWidget *filter_metadata_btn;  /* Metadata mode toggle (Ctrl+M), independent */
    int filter_active;
    guint search_debounce_timer;
    GCancellable *credit_search_cancel;  /* cancels in-flight async credit search */
    char *last_search_query;
    GtkWidget *search_results_list;  /* Unified GtkListBox for keyboard nav */
    GtkWidget *search_empty_label;

    /* Shared filter bar for search view (genre/year/advanced/credit) */
    FilterBarState search_filter_bar;

    /* Shared library callbacks */
    LibraryCallbacks lib_cbs;

    /* Settings - devices */
    GtkWidget *device_drops[MAX_CHANNELS];
    gulong device_drop_handler_ids[MAX_CHANNELS];
    GtkWidget *exclusive_checks[MAX_CHANNELS];
    GtkWidget *format_drops[MAX_CHANNELS];
    GtkWidget *quantum_drops[MAX_CHANNELS];
    GtkWidget *gpio_entries[MAX_CHANNELS];
    GtkStringList *device_models[MAX_CHANNELS];  /* Per-channel (filtered) */
    int device_model_map[MAX_CHANNELS][64];      /* model index → device_names index (-1 = "None") */

    /* Axia GPIO handlers (one per channel) */
    axia_gpio_t *gpio_handlers[MAX_CHANNELS];
    GtkStringList *format_model;
    GtkStringList *quantum_model;
    char **device_names;
    char **device_descs;  /* Human-readable descriptions (parallel to device_names) */
    int device_count;
    gboolean settings_initializing;  /* Prevents spurious saves during init */

    /* Libraries */
    LibEntry *libs;
    size_t lib_count;
    GtkWidget *libs_box;
    GtkWidget *libs_empty;

    guint update_tick_id;
    guint settings_save_timer;      /* Debounced settings save (200ms) */
    guint device_hotplug_timer_id;  /* Debounced PW device topology change (300ms) */
    guint device_rebuild_idle_id;   /* Deferred rebuild_device_models (avoids re-entrancy during notify::selected) */
    GtkCssProvider *css;
};

/* Debounced settings save — coalesces rapid changes into a single disk write */
void settings_save_debounced(UiWindow *w);

G_END_DECLS

#endif /* QUADRATURE_UI_INTERNAL_H */
