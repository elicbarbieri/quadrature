/**
 * Quadrature Main Window
 *
 * GtkApplicationWindow subclass: nav bar + content stack + channel panel.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "library/internal.h"
#include "perf/internal.h"
#include "search/internal.h"
#include "libraries/internal.h"
#include "settings/internal.h"
#include "quadrature/audio.h"
#include "quadrature/database.h"
#include "quadrature/library.h"
#include "quadrature/metadata.h"
#include "quadrature/gpio.h"
#include "../audio/internal.h" /* For audio_devices and perf access */
#include <string.h>

/* View names for content stack pages */
static const char *VIEW_SEARCH = "search";

/* Map view name to display label for back button. */
static const struct {
    const char *view;
    const char *label;
} VIEW_LABELS[] = {
    { "search", "Search" },
    { "artists", "Artists" },
    { "albums", "Albums" },
};

static const char *
view_display_name(const char *view)
{
    for (size_t i = 0; i < G_N_ELEMENTS(VIEW_LABELS); i++) {
        if (g_strcmp0(view, VIEW_LABELS[i].view) == 0)
            return VIEW_LABELS[i].label;
    }
    return VIEW_LABELS[G_N_ELEMENTS(VIEW_LABELS) - 1].label; /* default: Albums */
}

G_DEFINE_FINAL_TYPE(UiWindow, ui_window, GTK_TYPE_APPLICATION_WINDOW)

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_ui(UiWindow *w);
static gboolean hide_toast(gpointer data);

/* Debounced settings save — coalesces rapid changes into a single disk write */
static gboolean
settings_save_tick(gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    w->settings_save_timer = 0;
    if (w->settings)
        app_settings_save(w->settings);
    return G_SOURCE_REMOVE;
}

void
settings_save_debounced(UiWindow *w)
{
    if (w->settings_save_timer)
        g_source_remove(w->settings_save_timer);
    w->settings_save_timer = g_timeout_add(200, settings_save_tick, w);
}

/* Library view callbacks */
static void on_library_navigate(LibraryItemKind kind, int64_t id, gpointer data);
static void on_library_play(const PlaybackIntent *intent, gpointer data);
static void on_library_back(gpointer data);
static void on_track_info(int64_t track_id, gpointer data);
static void on_channel_strip_clicked(UiChannelStrip *strip, int channel_id, gpointer data);
static void
on_channel_strip_mode_changed(UiChannelStrip *strip, int channel_id, int new_mode, gpointer data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
apply_monitor_refresh(UiWindow *w, GdkMonitor *m)
{
    if (!w->pipeline || !m)
        return;
    int mhz = gdk_monitor_get_refresh_rate(m);
    double hz = (mhz > 0) ? (mhz / 1000.0) : 60.0;
    audio_pipeline_set_spectrum_refresh_hz(w->pipeline, hz);
}

static void
on_surface_enter_monitor(GdkSurface *surface, GdkMonitor *monitor, gpointer data)
{
    (void)surface;
    apply_monitor_refresh(UI_WINDOW(data), monitor);
}

static void
on_window_realize(GtkWidget *widget, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (!surface)
        return;

    g_signal_connect(surface, "enter-monitor", G_CALLBACK(on_surface_enter_monitor), w);

    GListModel *monitors = gdk_display_get_monitors(gtk_widget_get_display(widget));
    if (g_list_model_get_n_items(monitors) > 0) {
        GdkMonitor *m = g_list_model_get_item(monitors, 0);
        apply_monitor_refresh(w, m);
        g_object_unref(m);
    }
}

static gboolean
on_update_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    (void)clock;
    UiWindow *w = UI_WINDOW(data);
    if (!w->pipeline)
        return G_SOURCE_CONTINUE;

    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->channels[i] && UI_IS_CHANNEL_STRIP(w->channels[i]))
            ui_channel_strip_update(w->channels[i], w->pipeline);
    }
    return G_SOURCE_CONTINUE;
}

/** Install or remove the per-frame tick callback based on channel state.
 *  Option A: tick runs whenever any channel has a loaded track. */
static void
ensure_update_tick(UiWindow *w)
{
    gboolean need_tick = FALSE;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->channels[i] && ui_channel_strip_has_track(w->channels[i])) {
            need_tick = TRUE;
            break;
        }
    }
    if (need_tick && !w->update_tick_id) {
        w->update_tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(w), on_update_tick, w, NULL);
    } else if (!need_tick && w->update_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w), w->update_tick_id);
        w->update_tick_id = 0;
    }
}

/**
 * Track changed callback - called when audio pipeline auto-advances to next track.
 * Updates the channel strip display with the new track's metadata.
 */
static void
on_track_changed(int player_id, int64_t track_id, void *user_data)
{
    UiWindow *w = UI_WINDOW(user_data);
    if (!w || player_id < 0 || player_id >= MAX_CHANNELS)
        return;
    if (!w->channels[player_id])
        return;

    if (track_id <= 0) {
        /* Playback ended (no next track) */
        g_info("Channel %d: playback ended", player_id + 1);
        return;
    }

    /* Look up track info from library cache */
    if (w->library_cache) {
        const library_track_info_t *track = library_cache_get_track(w->library_cache, track_id);
        if (track) {
            g_info("Channel %d: auto-advanced to \"%s\" by %s",
                   player_id + 1,
                   track->title,
                   track->artist_display);

            /* Update channel strip display with new track metadata.
             * Use update_track_display, not load_track - track is already
             * loaded in engine, we just need to refresh the UI. */
            char *resolved = library_cache_resolve_track_path(w->library_cache, track_id);
            ui_channel_strip_update_track_display(w->channels[player_id],
                                                  &(PlaybackIntent){
                                                      .track_id = track_id,
                                                      .path = resolved,
                                                      .title = track->title,
                                                      .artist = track->artist_display,
                                                      .album = track->album_title,
                                                  });
            g_free(resolved);
        }
    }
}

/**
 * Track decode-failure callback — fired by the audio pipeline (on the main
 * thread) when a track cannot be decoded. Surfaces an error toast and skips to
 * the next track so a single bad file does not stall playback.
 */
static void
on_track_failed(int player_id, int64_t track_id, void *user_data)
{
    UiWindow *w = UI_WINDOW(user_data);
    if (!w || player_id < 0 || player_id >= MAX_CHANNELS)
        return;

    const char *title = NULL, *artist = NULL, *album = NULL;
    if (w->library_cache) {
        const library_track_info_t *track = library_cache_get_track(w->library_cache, track_id);
        if (track) {
            title = track->title;
            artist = track->artist_display;
            album = track->album_title;
        }
    }

    char *msg = g_strdup_printf("Decoding failed for %s by %s on %s",
                                title ? title : "Unknown Track",
                                artist ? artist : "Unknown Artist",
                                album ? album : "Unknown Album");
    g_warning("Channel %d: %s", player_id + 1, msg);
    ui_window_show_toast(w, msg, TOAST_ERROR, 5000);
    g_free(msg);

    /* Skip past the bad track. */
    if (w->channels[player_id])
        ui_channel_strip_next_track(w->channels[player_id]);
}

/* Navigate action handler - triggered by nav bar buttons */
static void
on_navigate_action(GSimpleAction *action, GVariant *param, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    const char *view = g_variant_get_string(param, NULL);

    /* Update action state (auto-updates toggle button :checked states) */
    g_simple_action_set_state(action, param);

    /* Toplevel nav bar click clears any detail view nav history */
    if (w->detail_view)
        library_unified_detail_clear_nav(w->detail_view);

    /* Switch stack page */
    w->current_view = view;
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), view);
}

static void
on_channel_strip_clicked(UiChannelStrip *strip, int channel_id, gpointer data)
{
    (void)strip;
    UiWindow *w = UI_WINDOW(data);
    ui_window_set_focused_channel(w, channel_id);
}

static void
on_channel_strip_mode_changed(UiChannelStrip *strip, int channel_id, int new_mode, gpointer data)
{
    (void)strip;
    UiWindow *w = UI_WINDOW(data);

    /* Clear focus if this channel was focused and entered QUEUED/ON_AIR */
    if (w->focused_channel == channel_id) {
        if (new_mode == CHANNEL_MODE_QUEUED || new_mode == CHANNEL_MODE_ON_AIR) {
            ui_window_clear_focus(w);
        }
    }
}

static void
on_channel_album_clicked(UiChannelStrip *strip, int channel_id, int64_t album_id, gpointer data)
{
    (void)strip;
    (void)channel_id;
    UiWindow *w = UI_WINDOW(data);

    if (album_id > 0) {
        gboolean from_detail = (g_strcmp0(w->current_view, "detail") == 0);
        /* Channel strip clicks preserve existing nav stack (no clear_nav).
         * Pass source view name so back button shows where to return to. */
        const char *source = from_detail ? NULL : view_display_name(w->current_view);
        if (!from_detail)
            w->previous_view = w->current_view;
        library_unified_detail_navigate_to_album(w->detail_view, album_id, source, 0);
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
        w->current_view = "detail";
    }
}

static void
on_channel_artist_clicked(UiChannelStrip *strip, int channel_id, int64_t artist_id, gpointer data)
{
    (void)strip;
    (void)channel_id;
    UiWindow *w = UI_WINDOW(data);

    if (artist_id > 0) {
        gboolean from_detail = (g_strcmp0(w->current_view, "detail") == 0);
        /* Channel strip clicks preserve existing nav stack (no clear_nav).
         * Pass source view name so back button shows where to return to. */
        const char *source = from_detail ? NULL : view_display_name(w->current_view);
        if (!from_detail)
            w->previous_view = w->current_view;
        library_unified_detail_navigate_to_artist(w->detail_view, artist_id, source);
        gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
        w->current_view = "detail";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Keyboard Shortcut Actions
 *
 * All keyboard shortcuts are implemented as GActions with accelerators.
 * This provides proper GTK4 integration and works regardless of focus.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declaration */
static void on_load_to_channel(int channel, int64_t track_id, gpointer data);

/* Helper: Get track ID from selected row in a list box */
static int64_t
get_selected_track_id(GtkWidget *list_box)
{
    if (!list_box)
        return 0;

    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_box));
    if (!row)
        return 0;

    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child)
        return 0;

    /* Check for track-id (track rows) or first-track-id (album rows) */
    gpointer p = g_object_get_data(G_OBJECT(child), "track-id");
    if (!p)
        p = g_object_get_data(G_OBJECT(child), "first-track-id");
    if (!p)
        return 0;

    return (int64_t)GPOINTER_TO_SIZE(p);
}

/* Helper: Get selected track from the current view */
static int64_t
get_current_view_selected_track(UiWindow *w)
{
    if (g_strcmp0(w->current_view, "search") == 0) {
        return get_selected_track_id(w->search_results_list);
    } else if (g_strcmp0(w->current_view, "detail") == 0) {
        return get_selected_track_id(library_unified_detail_get_track_list(w->detail_view));
    } else if (g_strcmp0(w->current_view, "albums") == 0 && w->albums_view) {
        return library_view_get_selected_track_id(w->albums_view);
    }
    return 0;
}

/* Action: Load selected track to channel N (1-4 keys) */
static void
on_action_load_channel(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)param;
    UiWindow *w = UI_WINDOW(data);

    /* Extract channel number from action name (load-channel-1 → 0) */
    const char *name = g_action_get_name(G_ACTION(action));
    int ch = name[strlen(name) - 1] - '1';

    if (ch < 0 || ch >= MAX_CHANNELS || !w->channels[ch])
        return;

    /* Check if channel can receive tracks */
    if (!ui_channel_strip_is_active(w->channels[ch])) {
        DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
        ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);
        char msg[64];

        if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
            snprintf(msg, sizeof(msg), "Channel %d not configured", ch + 1);
            ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
        } else if (mode == CHANNEL_MODE_QUEUED) {
            snprintf(msg, sizeof(msg), "Channel %d is queued", ch + 1);
            ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
        } else if (mode == CHANNEL_MODE_ON_AIR) {
            snprintf(msg, sizeof(msg), "Channel %d is on air", ch + 1);
            ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
        }
        return;
    }

    /* Get selected track from current view */
    int64_t track_id = get_current_view_selected_track(w);
    if (track_id <= 0) {
        ui_window_show_toast(w, "No track selected", TOAST_WARNING, 3000);
        return;
    }

    /* Load track to channel */
    on_load_to_channel(ch, track_id, w);
}

/* Action: Focus channel N (Ctrl+1-4 keys) */
static void
on_action_focus_channel(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)param;
    UiWindow *w = UI_WINDOW(data);

    const char *name = g_action_get_name(G_ACTION(action));
    int ch = name[strlen(name) - 1] - '1';

    if (ch < 0 || ch >= MAX_CHANNELS || !w->channels[ch])
        return;

    if (!ui_channel_strip_is_active(w->channels[ch])) {
        DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
        ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);
        char msg[64];

        if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
            snprintf(msg, sizeof(msg), "Channel %d not configured", ch + 1);
            ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
        } else if (mode == CHANNEL_MODE_QUEUED) {
            snprintf(msg, sizeof(msg), "Channel %d is queued", ch + 1);
            ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
        } else if (mode == CHANNEL_MODE_ON_AIR) {
            snprintf(msg, sizeof(msg), "Channel %d is on air", ch + 1);
            ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
        }
        return;
    }

    ui_window_set_focused_channel(w, ch);
}

/* Action: Play channel N (F1-F4 keys) */
static void
on_action_play_channel(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    if (!w->pipeline)
        return;

    const char *name = g_action_get_name(G_ACTION(action));
    int ch = name[strlen(name) - 1] - '1';

    if (ch >= 0 && ch < MAX_CHANNELS)
        audio_pipeline_player_play(w->pipeline, ch);
}

/* Action: Stop channel N (Shift+F1-F4 keys) */
static void
on_action_stop_channel(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    if (!w->pipeline)
        return;

    const char *name = g_action_get_name(G_ACTION(action));
    int ch = name[strlen(name) - 1] - '1';

    if (ch >= 0 && ch < MAX_CHANNELS)
        audio_pipeline_player_stop(w->pipeline, ch);
}

/* Action: Navigate to search (Ctrl+F) */
static void
on_action_search(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    ui_window_navigate_to(w, VIEW_SEARCH);
    set_search_filter(w, 0);
    focus_search_entry(w);
}

/* Action: Navigate to search with Artists filter (Ctrl+A) */
static void
on_action_filter_artists(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    ui_window_navigate_to(w, VIEW_SEARCH);
    set_search_filter(w, 1);
    focus_search_entry(w);
}

/* Action: Navigate to search with Albums filter (Ctrl+B) */
static void
on_action_filter_albums(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    ui_window_navigate_to(w, VIEW_SEARCH);
    set_search_filter(w, 2);
    focus_search_entry(w);
}

/* Action: Navigate to search with Tracks filter (Ctrl+T) */
static void
on_action_filter_tracks(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    ui_window_navigate_to(w, VIEW_SEARCH);
    set_search_filter(w, 3);
    focus_search_entry(w);
}

/* Action: Toggle metadata search mode (Ctrl+M) */
static void
on_action_toggle_metadata(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    /* In search view: toggle the metadata button */
    if (w->filter_metadata_btn && g_strcmp0(w->current_view, "search") == 0) {
        gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->filter_metadata_btn));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->filter_metadata_btn), !active);
    }
}

/* Action: Clear filters in current view (Ctrl+R) */
static void
on_action_clear_filters(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);
    if (g_strcmp0(w->current_view, "search") == 0)
        clear_search_view_filters(w);
    else if (w->artists_view && g_strcmp0(w->current_view, "artists") == 0)
        library_view_clear_filters(w->artists_view);
    else if (w->albums_view && g_strcmp0(w->current_view, "albums") == 0)
        library_view_clear_filters(w->albums_view);
}

/* Action: Escape key - context-dependent behavior */
static void
on_action_close_errors(GSimpleAction *action, GVariant *param, gpointer data)
{
    (void)action;
    (void)param;
    UiWindow *w = UI_WINDOW(data);

    /* Priority 1: In detail view, navigate back */
    if (g_strcmp0(w->current_view, "detail") == 0) {
        if (!library_unified_detail_go_back(w->detail_view)) {
            if (w->previous_view)
                ui_window_navigate_to(w, w->previous_view);
        }
        return;
    }

    /* Priority 3: In search view, return focus to search entry */
    if (g_strcmp0(w->current_view, "search") == 0) {
        focus_search_entry(w);
    }
}

static gboolean
on_window_key_pressed(GtkEventControllerKey *, guint, guint, GdkModifierType, gpointer);

/* Setup all keyboard shortcut actions */
static void
setup_keyboard_actions(UiWindow *w)
{
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w));
    g_assert(app != NULL);

    /* Action entries for window */
    static const GActionEntry entries[] = {
        { "load-channel-1", on_action_load_channel, NULL, NULL, NULL, { 0 } },
        { "load-channel-2", on_action_load_channel, NULL, NULL, NULL, { 0 } },
        { "load-channel-3", on_action_load_channel, NULL, NULL, NULL, { 0 } },
        { "load-channel-4", on_action_load_channel, NULL, NULL, NULL, { 0 } },
        { "focus-channel-1", on_action_focus_channel, NULL, NULL, NULL, { 0 } },
        { "focus-channel-2", on_action_focus_channel, NULL, NULL, NULL, { 0 } },
        { "focus-channel-3", on_action_focus_channel, NULL, NULL, NULL, { 0 } },
        { "focus-channel-4", on_action_focus_channel, NULL, NULL, NULL, { 0 } },
        { "play-channel-1", on_action_play_channel, NULL, NULL, NULL, { 0 } },
        { "play-channel-2", on_action_play_channel, NULL, NULL, NULL, { 0 } },
        { "play-channel-3", on_action_play_channel, NULL, NULL, NULL, { 0 } },
        { "play-channel-4", on_action_play_channel, NULL, NULL, NULL, { 0 } },
        { "stop-channel-1", on_action_stop_channel, NULL, NULL, NULL, { 0 } },
        { "stop-channel-2", on_action_stop_channel, NULL, NULL, NULL, { 0 } },
        { "stop-channel-3", on_action_stop_channel, NULL, NULL, NULL, { 0 } },
        { "stop-channel-4", on_action_stop_channel, NULL, NULL, NULL, { 0 } },
        { "search", on_action_search, NULL, NULL, NULL, { 0 } },
        { "filter-artists", on_action_filter_artists, NULL, NULL, NULL, { 0 } },
        { "filter-albums", on_action_filter_albums, NULL, NULL, NULL, { 0 } },
        { "filter-tracks", on_action_filter_tracks, NULL, NULL, NULL, { 0 } },
        { "toggle-metadata", on_action_toggle_metadata, NULL, NULL, NULL, { 0 } },
        { "clear-filters", on_action_clear_filters, NULL, NULL, NULL, { 0 } },
        { "close-errors", on_action_close_errors, NULL, NULL, NULL, { 0 } },
    };

    g_action_map_add_action_entries(G_ACTION_MAP(w), entries, G_N_ELEMENTS(entries), w);

    /* Set accelerators (1-4 handled by window key controller, not global accels) */
    gtk_application_set_accels_for_action(
        app, "win.focus-channel-1", (const char *[]){ "<Control>1", NULL });
    gtk_application_set_accels_for_action(
        app, "win.focus-channel-2", (const char *[]){ "<Control>2", NULL });
    gtk_application_set_accels_for_action(
        app, "win.focus-channel-3", (const char *[]){ "<Control>3", NULL });
    gtk_application_set_accels_for_action(
        app, "win.focus-channel-4", (const char *[]){ "<Control>4", NULL });

    gtk_application_set_accels_for_action(
        app, "win.play-channel-1", (const char *[]){ "F1", NULL });
    gtk_application_set_accels_for_action(
        app, "win.play-channel-2", (const char *[]){ "F2", NULL });
    gtk_application_set_accels_for_action(
        app, "win.play-channel-3", (const char *[]){ "F3", NULL });
    gtk_application_set_accels_for_action(
        app, "win.play-channel-4", (const char *[]){ "F4", NULL });

    gtk_application_set_accels_for_action(
        app, "win.stop-channel-1", (const char *[]){ "<Shift>F1", NULL });
    gtk_application_set_accels_for_action(
        app, "win.stop-channel-2", (const char *[]){ "<Shift>F2", NULL });
    gtk_application_set_accels_for_action(
        app, "win.stop-channel-3", (const char *[]){ "<Shift>F3", NULL });
    gtk_application_set_accels_for_action(
        app, "win.stop-channel-4", (const char *[]){ "<Shift>F4", NULL });

    gtk_application_set_accels_for_action(
        app, "win.search", (const char *[]){ "<Control>f", NULL });
    gtk_application_set_accels_for_action(
        app, "win.filter-artists", (const char *[]){ "<Control>a", NULL });
    gtk_application_set_accels_for_action(
        app, "win.filter-albums", (const char *[]){ "<Control>b", NULL });
    gtk_application_set_accels_for_action(
        app, "win.filter-tracks", (const char *[]){ "<Control>t", NULL });
    gtk_application_set_accels_for_action(
        app, "win.toggle-metadata", (const char *[]){ "<Control>m", NULL });
    gtk_application_set_accels_for_action(
        app, "win.clear-filters", (const char *[]){ "<Control>r", NULL });
    gtk_application_set_accels_for_action(
        app, "win.close-errors", (const char *[]){ "Escape", NULL });

    /* Window-level key controller for 1-4 (bubble phase: text entries get first crack) */
    GtkEventController *win_key_ctl = gtk_event_controller_key_new();
    g_signal_connect(win_key_ctl, "key-pressed", G_CALLBACK(on_window_key_pressed), w);
    gtk_widget_add_controller(GTK_WIDGET(w), win_key_ctl);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared Row Handlers
 *
 * Used by all library views (search, detail, lists). Match RowCallbacks signature.
 * Selection handled by GTK automatically. These handle activate and queue actions.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Track: activate navigates to album detail with track pre-selected */
static void
on_track_activate(int64_t track_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (!w->library_cache)
        return;

    const library_track_info_t *track = library_cache_get_track(w->library_cache, track_id);
    if (!track)
        return;

    /* Navigate to album detail with this track selected */
    const char *source = NULL;
    if (g_strcmp0(w->current_view, "detail") != 0) {
        source = view_display_name(w->current_view);
        w->previous_view = w->current_view;
        library_unified_detail_clear_nav(w->detail_view);
    }
    library_unified_detail_navigate_to_album(w->detail_view, track->album_id, source, track_id);
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
    w->current_view = "detail";
}

static void
on_track_secondary(int64_t track_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (!w->library_cache)
        return;

    const library_track_info_t *track = library_cache_get_track(w->library_cache, track_id);
    if (!track)
        return;

    char *resolved = library_cache_resolve_track_path(w->library_cache, track_id);
    on_library_play(
        &(PlaybackIntent){
            .track_id = track->track_id,
            .path = resolved,
            .title = track->title,
            .artist = track->artist_display,
            .album = track->album_title,
        },
        w);
    g_free(resolved);
}

/* Album: activate navigates to album detail, right-click queues track_1 */
static void
on_album_activate(int64_t album_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    on_library_navigate(LIBRARY_ITEM_ALBUM, album_id, w);
}

static void
on_album_secondary(int64_t album_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (!w->library_cache)
        return;

    GPtrArray *tracks
        = library_cache_get_tracks_by_album(w->library_cache, album_id, LIBRARY_MASK_ALL);
    if (!tracks || tracks->len == 0) {
        g_clear_pointer(&tracks, g_ptr_array_unref);
        return;
    }

    const library_track_info_t *track = g_ptr_array_index(tracks, 0);
    char *resolved = library_cache_resolve_track_path(w->library_cache, track->track_id);
    on_library_play(
        &(PlaybackIntent){
            .track_id = track->track_id,
            .path = resolved,
            .title = track->title,
            .artist = track->artist_display,
            .album = track->album_title,
        },
        w);
    g_free(resolved);
    g_ptr_array_unref(tracks);
}

/* Artist: activate navigates to artist detail, no right-click action */
static void
on_artist_activate(int64_t artist_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    on_library_navigate(LIBRARY_ITEM_ARTIST, artist_id, w);
}

static void
on_artist_mbid_navigate(const char *mbid, const char *name, const char *type, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);

    /* Try MBID bridge: check if this artist exists in any library's main DB */
    int64_t found_id = 0;
    if (w->settings) {
        for (int i = 0; i < w->settings->library_count && found_id == 0; i++) {
            const char *dp = app_settings_get_library_data_path(w->settings, i);
            char *db_path = g_build_filename(dp, "quadrature.sqlite", NULL);
            quadrature_db_t *lib_db = NULL;
            if (db_open(db_path, true, &lib_db) == QUADRATURE_OK) {
                db_get_artist_by_mbid(lib_db, mbid, &found_id);
                db_close(lib_db);
            }
            g_free(db_path);
        }
    }

    gboolean from_detail = (g_strcmp0(w->current_view, "detail") == 0);
    if (!from_detail) {
        const char *source = view_display_name(w->current_view);
        w->previous_view = w->current_view;
        if (found_id > 0)
            library_unified_detail_navigate_to_artist(w->detail_view, found_id, source);
        else
            library_unified_detail_navigate_to_meta_artist(w->detail_view, mbid, name, type);
    } else {
        if (found_id > 0)
            library_unified_detail_navigate_to_artist(w->detail_view, found_id, NULL);
        else
            library_unified_detail_navigate_to_meta_artist(w->detail_view, mbid, name, type);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
    w->current_view = "detail";
}

/* Window-level 1-4 key handler (bubble phase — text entries get first crack) */
static gboolean
on_window_key_pressed(
    GtkEventControllerKey *ctl, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
    (void)ctl;
    (void)keycode;
    UiWindow *w = UI_WINDOW(data);

    /* Let editable widgets handle bare keypresses */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(w));
    if (focus && GTK_IS_EDITABLE(focus))
        return GDK_EVENT_PROPAGATE;

    /* Only bare 1-4 (no modifiers except NumLock/CapsLock) */
    GdkModifierType significant = state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK);
    if (significant != 0)
        return GDK_EVENT_PROPAGATE;

    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_4) {
        char action_name[32];
        snprintf(
            action_name, sizeof(action_name), "load-channel-%d", (int)(keyval - GDK_KEY_1) + 1);
        g_action_group_activate_action(G_ACTION_GROUP(w), action_name, NULL);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library View Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
on_library_navigate(LibraryItemKind kind, int64_t id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    gboolean from_detail = (g_strcmp0(w->current_view, "detail") == 0);
    const char *source = from_detail ? NULL : view_display_name(w->current_view);

    /* Toplevel navigation clears stale detail history */
    if (!from_detail) {
        w->previous_view = w->current_view;
        library_unified_detail_clear_nav(w->detail_view);
    }

    if (kind == LIBRARY_ITEM_ARTIST) {
        g_info("Artist selected: id=%" G_GINT64_FORMAT, id);
        library_unified_detail_navigate_to_artist(w->detail_view, id, source);
    } else if (kind == LIBRARY_ITEM_ALBUM) {
        g_info("Album selected: id=%" G_GINT64_FORMAT, id);
        library_unified_detail_navigate_to_album(w->detail_view, id, source, 0);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), "detail");
    w->current_view = "detail";
}

static void
on_library_play(const PlaybackIntent *intent, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    g_return_if_fail(intent != NULL);

    /* Reject tracks from disconnected libraries */
    if (intent->track_id > 0 && w->library_cache) {
        int lib_idx = LIBRARY_GLOBAL_ID_LIB(intent->track_id);
        if (!library_cache_get_available(w->library_cache, lib_idx)) {
            ui_window_show_toast(w, "Library disconnected", TOAST_WARNING, 3000);
            return;
        }
    }

    /* Check if a channel is focused */
    if (w->focused_channel < 0) {
        ui_window_show_toast(w,
                             "No channel focused \u2014 click a channel or press Ctrl+1\u20114",
                             TOAST_WARNING,
                             3000);
        return;
    }

    int ch = w->focused_channel;

    /* Check if target channel can receive tracks */
    if (ch >= 0 && ch < MAX_CHANNELS && w->channels[ch]) {
        if (!ui_channel_strip_is_active(w->channels[ch])) {
            DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
            ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);
            char msg[64];

            if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
                snprintf(msg, sizeof(msg), "Channel %d not configured", ch + 1);
                ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
            } else if (mode == CHANNEL_MODE_QUEUED) {
                snprintf(msg, sizeof(msg), "Channel %d is queued", ch + 1);
                ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
            } else if (mode == CHANNEL_MODE_ON_AIR) {
                snprintf(msg, sizeof(msg), "Channel %d is on air", ch + 1);
                ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
            }
            return;
        }
    }

    g_info("Load Track → Channel %d: '%s' by %s from '%s' (track_id=%" G_GINT64_FORMAT ")",
           ch + 1,
           intent->title,
           intent->artist,
           intent->album,
           intent->track_id);

    if (!w->channels[ch])
        return;

    /* Load the track - album context is resolved via LibraryCache in channel_strip */
    ui_channel_strip_load_track(w->channels[ch], intent);
    ensure_update_tick(w);
}

static void
on_library_back(gpointer data)
{
    UiWindow *w = UI_WINDOW(data);

    /* on_back is only called when detail view's internal nav is exhausted
     * (back button handler already called library_unified_detail_go_back).
     * Just navigate to previous main view. */
    if (w->previous_view) {
        ui_window_navigate_to(w, w->previous_view);
    }
}

static void
on_track_info(int64_t track_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (!w->library_cache || track_id <= 0)
        return;

    const library_track_info_t *track = library_cache_get_track(w->library_cache, track_id);
    if (!track)
        return;

    char *resolved = library_cache_resolve_track_path(w->library_cache, track_id);
    GtkWidget *dialog = ui_metadata_dialog_new(GTK_WINDOW(w), track, resolved);
    g_free(resolved);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void
on_load_to_channel(int channel, int64_t track_id, gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (!w->library_cache || track_id <= 0)
        return;

    /* Reject tracks from disconnected libraries */
    int lib_idx = LIBRARY_GLOBAL_ID_LIB(track_id);
    if (!library_cache_get_available(w->library_cache, lib_idx)) {
        ui_window_show_toast(w, "Library disconnected", TOAST_WARNING, 3000);
        return;
    }

    /* Validate channel */
    if (channel < 0 || channel >= MAX_CHANNELS || !w->channels[channel])
        return; /* Fail silently */

    /* Check if target channel can receive tracks - fail silently if not */
    if (!ui_channel_strip_is_active(w->channels[channel]))
        return;

    /* Look up track info */
    const library_track_info_t *track = library_cache_get_track(w->library_cache, track_id);
    if (!track)
        return;

    g_info("Keyboard Shortcut → Load Track to Channel %d: '%s' by %s from '%s' "
           "(track_id=%" G_GINT64_FORMAT ")",
           channel + 1,
           track->title,
           track->artist_display,
           track->album_title,
           track_id);

    char *resolved = library_cache_resolve_track_path(w->library_cache, track_id);
    ui_channel_strip_load_track(w->channels[channel],
                                &(PlaybackIntent){
                                    .track_id = track_id,
                                    .path = resolved,
                                    .title = track->title,
                                    .artist = track->artist_display,
                                    .album = track->album_title,
                                });
    g_free(resolved);
    ensure_update_tick(w);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UI Building
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
load_css(UiWindow *w)
{
    w->css = gtk_css_provider_new();

    GBytes *bytes = g_resources_lookup_data(
        "/org/quadrature/ui/quadrature.css", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    if (bytes) {
        gsize size;
        const char *data = g_bytes_get_data(bytes, &size);
        char *str = g_strndup(data, size);
        gtk_css_provider_load_from_string(w->css, str);
        g_free(str);
        g_bytes_unref(bytes);
    }

    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(w->css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Bar (global multi-toggle filter)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
sync_library_toggles(UiWindow *w)
{
    w->library_toggle_updating = TRUE;
    for (int i = 0; i < w->library_toggle_count; i++) {
        int bi = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w->library_toggles[i]), "lib-idx"));
        gboolean active = (w->library_mask & (1u << bi)) != 0;
        gtk_toggle_button_set_active(w->library_toggles[i], active);
    }
    w->library_toggle_updating = FALSE;
}

static void
set_library_mask(UiWindow *w, uint32_t mask)
{
    if (mask == w->library_mask)
        return; /* No change — skip reflow */
    w->library_mask = mask;
    sync_library_toggles(w);
    refresh_library_views(w);
}

static void
on_library_left_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
    (void)n_press;
    (void)x;
    (void)y;
    UiWindow *w = UI_WINDOW(data);

    /* Claim the gesture so GtkToggleButton's default handler doesn't fire */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    int lib_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "lib-idx"));

    GdkModifierType mods
        = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    uint32_t new_mask;
    if (mods & GDK_SHIFT_MASK) {
        /* Shift+click → solo this library */
        new_mask = library_mask_solo(lib_idx);
    } else {
        /* Plain click → toggle this library */
        new_mask = library_mask_after_toggle(w->library_mask, lib_idx);
    }

    set_library_mask(w, new_mask);
}

static void
on_library_right_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
    (void)gesture;
    (void)n_press;
    (void)x;
    (void)y;
    UiWindow *w = UI_WINDOW(data);
    set_library_mask(w, LIBRARY_MASK_ALL);
}

static void
build_library_bar(UiWindow *w)
{
    int lib_count = library_cache_get_library_count(w->library_cache);
    if (lib_count <= 1) {
        gtk_widget_set_visible(w->library_bar, FALSE);
        return;
    }

    /* Label */
    GtkWidget *label = gtk_label_new("Libraries:");
    gtk_widget_add_css_class(label, "library-bar-label");
    gtk_box_append(GTK_BOX(w->library_bar), label);

    /* Toggle buttons */
    w->library_toggle_count = lib_count;
    w->library_toggles = g_new0(GtkToggleButton *, lib_count);

    for (int i = 0; i < lib_count; i++) {
        int bi = library_cache_get_bitmap_index(w->library_cache, i);
        const char *name = library_cache_get_library_name(w->library_cache, bi);
        GtkWidget *btn = gtk_toggle_button_new_with_label(name ? name : "Library");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
        gtk_widget_add_css_class(btn, "library-toggle");
        g_object_set_data(G_OBJECT(btn), "lib-idx", GINT_TO_POINTER(bi));
        gtk_widget_set_tooltip_text(btn, "Click: toggle\nShift+Click: solo\nRight-click: show all");

        /* Left-click (with modifier detection) */
        GtkGesture *lc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lc), 1);
        g_signal_connect(lc, "pressed", G_CALLBACK(on_library_left_click), w);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(lc));

        /* Right-click → select all libraries */
        GtkGesture *rc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), 3);
        g_signal_connect(rc, "pressed", G_CALLBACK(on_library_right_click), w);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(rc));

        gtk_box_append(GTK_BOX(w->library_bar), btn);
        w->library_toggles[i] = GTK_TOGGLE_BUTTON(btn);
    }

    /* Spacer for future right-side info */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(w->library_bar), spacer);

    gtk_widget_set_visible(w->library_bar, TRUE);
}

/* ═══════════════════════════════════════════════════════════════════════════ */

static void
build_ui(UiWindow *w)
{
    /* Template already set window title and size, load CSS */
    load_css(w);

    /* Errors popover is created fresh per-click in on_errors() */

    /* Create navigate action (stateful - state is current view name) */
    w->navigate_action = g_simple_action_new_stateful(
        "navigate", G_VARIANT_TYPE_STRING, g_variant_new_string("search"));
    g_signal_connect(w->navigate_action, "activate", G_CALLBACK(on_navigate_action), w);
    g_action_map_add_action(G_ACTION_MAP(w), G_ACTION(w->navigate_action));

    /* Load nav bar from template and prepend to main_box */
    GtkBuilder *nav_builder = gtk_builder_new_from_resource("/org/quadrature/ui/nav_bar.ui");
    w->nav_bar = GTK_WIDGET(gtk_builder_get_object(nav_builder, "nav_bar"));
    gtk_widget_set_size_request(w->nav_bar, 56, -1);
    gtk_box_prepend(GTK_BOX(w->main_box), w->nav_bar);
    g_object_unref(nav_builder);

    /* Add views to content stack */
    gtk_stack_add_named(GTK_STACK(w->stack), make_search_view(w), "search");

    w->lib_cbs = (LibraryCallbacks){
        .on_navigate = on_library_navigate,
        .on_play = on_library_play,
        .on_back = on_library_back,
        .on_track_info = on_track_info,
        .on_load_to_channel = on_load_to_channel,
        /* Track rows in search/lists: activate navigates to album */
        .track_cbs
        = { .on_activate = on_track_activate, .on_secondary = on_track_secondary, .user_data = w },
        /* Track rows in album detail: no on_activate (already viewing album) */
        .album_track_cbs = { .on_secondary = on_track_secondary, .user_data = w },
        .album_cbs
        = { .on_activate = on_album_activate, .on_secondary = on_album_secondary, .user_data = w },
        .artist_cbs = { .on_activate = on_artist_activate,
                        .on_mbid_navigate = on_artist_mbid_navigate,
                        .user_data = w },
        .user_data = w
    };

    w->artists_view = library_view_new(
        LIBRARY_ITEM_ARTIST, w->library_cache, w->artwork_mgr, &w->lib_cbs, w->settings);
    gtk_stack_add_named(GTK_STACK(w->stack), w->artists_view, "artists");

    w->albums_view = library_view_new(
        LIBRARY_ITEM_ALBUM, w->library_cache, w->artwork_mgr, &w->lib_cbs, w->settings);
    gtk_stack_add_named(GTK_STACK(w->stack), w->albums_view, "albums");

    gtk_stack_add_named(GTK_STACK(w->stack), make_libraries_view(w), "libraries");
    gtk_stack_add_named(GTK_STACK(w->stack), make_settings_view(w), "settings");
    gtk_stack_add_named(
        GTK_STACK(w->stack), perf_view_new(w->pipeline, w->library_cache, w->artwork_mgr), "perf");
    gtk_stack_add_named(GTK_STACK(w->stack), make_help_view(), "help");

    /* Unified detail view */
    w->detail_view = library_unified_detail_view_new(
        w->library_cache, w->artwork_mgr, &w->lib_cbs, w->settings);
    gtk_stack_add_named(GTK_STACK(w->stack), w->detail_view, "detail");

    /* Build library bar (hidden for single library) */
    build_library_bar(w);

    /* Create channel strips and add to template container */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        GtkWidget *strip = ui_channel_strip_new(i, w->pipeline, w->library_cache);
        w->channels[i] = UI_CHANNEL_STRIP(strip);
        gtk_box_append(GTK_BOX(w->channel_strips_box), strip);

        /* Connect signals */
        g_signal_connect(strip, "clicked", G_CALLBACK(on_channel_strip_clicked), w);
        g_signal_connect(strip, "mode-changed", G_CALLBACK(on_channel_strip_mode_changed), w);
        g_signal_connect(strip, "album-clicked", G_CALLBACK(on_channel_album_clicked), w);
        g_signal_connect(strip, "artist-clicked", G_CALLBACK(on_channel_artist_clicked), w);

        /* Connect mode-changed signal for GPIO LED feedback */
        g_signal_connect(strip, "mode-changed", G_CALLBACK(on_channel_mode_changed), w);
    }

    /* Set fixed width on channels panel - CSS min/max-width alone isn't reliable */
    gtk_widget_set_size_request(w->channel_strips_box, 720, -1);

    /* Setup keyboard shortcut actions */
    setup_keyboard_actions(w);

    /* Update timer: demand-driven, installed when first track loads */
    w->update_tick_id = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
ui_window_dispose(GObject *obj)
{
    UiWindow *w = UI_WINDOW(obj);

    /* Library bar cleanup */
    g_free(w->library_toggles);
    w->library_toggles = NULL;

    /* ── 1. Deregister external callbacks that reference this window ─────── */

    /* Stop all GPIO handlers */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->gpio_handlers[i]) {
            axia_gpio_stop(w->gpio_handlers[i]);
            axia_gpio_destroy(w->gpio_handlers[i]);
            w->gpio_handlers[i] = NULL;
        }
    }

    /* Stop the PW device monitor before nulling the pipeline pointer.
     * This prevents on_pw_device_changed() from being called with a
     * stale UiWindow pointer after disposal. */
    teardown_device_monitor(w);

    /* Clear pipeline's track-changed callback so the 50ms advance timer
     * can't invoke on_track_changed on our disposed widget */
    if (w->pipeline) {
        audio_pipeline_set_track_changed_callback(w->pipeline, NULL, NULL);
        audio_pipeline_set_track_failed_callback(w->pipeline, NULL, NULL);
    }

    /* Clear cache ready callback so warming-complete idle can't call us */
    if (w->library_cache) {
        library_cache_set_ready_callback(w->library_cache, NULL, NULL);
    }

    /* Stop library monitor before anything else — prevents availability
     * callbacks firing during teardown */
    if (w->lib_monitor) {
        g_signal_handlers_disconnect_by_data(w->lib_monitor, w);
        library_monitor_stop(w->lib_monitor);
        g_clear_object(&w->lib_monitor);
    }

    /* Cancel running indexers before unreffing — indexer_controller_dispose
     * will wait for threads but we must disconnect our signal handlers first
     * so no idle callbacks fire against this dead window */
    if (w->indexer) {
        g_signal_handlers_disconnect_by_data(w->indexer, w);
        indexer_controller_cancel(w->indexer);
    }

    /* ── 2. Remove GLib timers / tick callbacks ─────────────────────────── */

    if (w->update_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w), w->update_tick_id);
        w->update_tick_id = 0;
    }
    if (w->search_debounce_timer) {
        g_source_remove(w->search_debounce_timer);
        w->search_debounce_timer = 0;
    }
    if (w->credit_search_cancel) {
        g_cancellable_cancel(w->credit_search_cancel);
        g_clear_object(&w->credit_search_cancel);
    }
    if (w->toast_timer) {
        g_source_remove(w->toast_timer);
        w->toast_timer = 0;
    }
    if (w->device_hotplug_timer_id) {
        g_source_remove(w->device_hotplug_timer_id);
        w->device_hotplug_timer_id = 0;
    }
    if (w->device_rebuild_idle_id) {
        g_source_remove(w->device_rebuild_idle_id);
        w->device_rebuild_idle_id = 0;
    }
    indexer_bridge_cancel_pending_refreshes();
    /* Flush any pending debounced settings save before shutdown */
    if (w->settings_save_timer) {
        g_source_remove(w->settings_save_timer);
        w->settings_save_timer = 0;
        if (w->settings)
            app_settings_save(w->settings);
    }

    /* ── 3. Free UI-owned resources ─────────────────────────────────────── */

    g_clear_object(&w->css);
    g_free(w->last_search_query);
    w->last_search_query = NULL;

    if (w->device_names) {
        for (int i = 0; i < w->device_count; i++) {
            g_free(w->device_names[i]);
            g_free(w->device_descs[i]);
        }
        g_free(w->device_names);
        g_free(w->device_descs);
        w->device_names = NULL;
        w->device_descs = NULL;
    }
    for (int i = 0; i < MAX_CHANNELS; i++)
        g_clear_object(&w->device_models[i]);
    g_clear_object(&w->format_model);
    g_clear_object(&w->quantum_model);

    libs_free(w);
    g_clear_object(&w->indexer);

    g_free(w->errors_library_path);
    w->errors_library_path = NULL;
    filter_bar_destroy(&w->search_filter_bar);

    if (w->artwork_mgr) {
        artwork_manager_free(w->artwork_mgr);
        w->artwork_mgr = NULL;
    }
    if (w->errors_lib_db) {
        db_close(w->errors_lib_db);
        w->errors_lib_db = NULL;
    }

    /* ── 4. Clear borrowed pointers (owned by main.c / on_shutdown) ─────── */

    w->pipeline = NULL;
    w->library_cache = NULL;
    w->settings = NULL;

    G_OBJECT_CLASS(ui_window_parent_class)->dispose(obj);
}

static void
ui_window_class_init(UiWindowClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    G_OBJECT_CLASS(klass)->dispose = ui_window_dispose;

    /* Set up composite template */
    gtk_widget_class_set_template_from_resource(widget_class,
                                                "/org/quadrature/ui/quadrature_window.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(widget_class, UiWindow, main_box);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, content_stack);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, library_bar);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, channel_strips_box);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, UiWindow, toast_label);
}

static void
ui_window_init(UiWindow *w)
{
    /* Initialize template - this populates all bound children */
    gtk_widget_init_template(GTK_WIDGET(w));

    /* Set alias for stack */
    w->stack = w->content_stack;
    w->library_mask = LIBRARY_MASK_ALL;

    /* Initialize non-template fields */
    w->pipeline = NULL;
    w->settings = NULL;
    w->errors_lib_db = NULL;
    w->indexer = NULL;
    w->library_cache = NULL;
    w->artwork_mgr = NULL;
    w->focused_channel = -1;
    w->show_spectrum = TRUE;
    w->current_view = VIEW_SEARCH;
    w->previous_view = VIEW_SEARCH;
    w->filter_active = 0;
    memset(w->device_models, 0, sizeof(w->device_models));
    w->format_model = NULL;
    w->device_names = NULL;
    w->device_count = 0;
    w->settings_initializing = FALSE;
    w->libs = NULL;
    w->lib_count = 0;
    w->update_tick_id = 0;
    w->css = NULL;

    /* Toast */
    w->toast_timer = 0;

    /* Errors */
    w->errors_library_path = NULL;

    /* Search */
    w->search_debounce_timer = 0;
    w->last_search_query = NULL;
    w->search_results_list = NULL;
    w->search_empty_label = NULL;
    /* Library views */
    w->artists_view = NULL;
    w->albums_view = NULL;
    w->detail_view = NULL;

    for (int i = 0; i < MAX_CHANNELS; i++) {
        w->channels[i] = NULL;
        w->device_drops[i] = NULL;
        w->format_drops[i] = NULL;
        w->quantum_drops[i] = NULL;
        w->gpio_entries[i] = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean
init_devices_idle(gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    populate_devices_async(w);
    setup_device_monitor(w);
    return G_SOURCE_REMOVE;
}

/** Build a temporary array of data root paths from settings. Caller must g_free(). */
/* =============================================================================
 * Library availability changed — drive mounted/unmounted
 * ============================================================================= */

static void
on_library_availability_changed(LibraryMonitor *mon,
                                int bitmap_idx,
                                gboolean available,
                                gpointer data)
{
    (void)mon;
    UiWindow *w = UI_WINDOW(data);

    /* Map bitmap_index → settings array index */
    int si = -1;
    if (w->settings) {
        for (int i = 0; i < w->settings->library_count; i++) {
            if (w->settings->libraries[i].library_index == bitmap_idx) {
                si = i;
                break;
            }
        }
    }

    /* 1. Toast notification */
    char *name = (si >= 0) ? app_settings_get_library_name(w->settings, si) : NULL;
    char *msg = g_strdup_printf(
        "Library \"%s\" %s", name ? name : "Unknown", available ? "reconnected" : "disconnected");
    ui_window_show_toast(
        w, msg, available ? TOAST_SUCCESS : TOAST_WARNING, available ? 3000 : 5000);
    g_free(msg);
    g_free(name);

    if (available) {
        /* Rewarm slot (data may have changed while disconnected) */
        library_cache_clear_slot(w->library_cache, bitmap_idx);
        library_cache_warm_slot(w->library_cache, bitmap_idx);
        /* on_cache_ready fires refresh_library_views when warming completes */

        /* Auto-rescan to detect file changes */
        if (w->indexer && si >= 0 && w->settings->auto_scan_on_startup) {
            const char *paths[] = { w->settings->libraries[si].path };
            const char *dp = app_settings_get_library_data_path(w->settings, si);
            const char *dpaths[] = { dp };
            indexer_controller_start(w->indexer, paths, dpaths, 1);
        }
    } else {
        /* Cancel any running indexer for this library */
        if (w->indexer && si >= 0)
            indexer_controller_cancel_library(w->indexer, w->settings->libraries[si].path);

        /* If detail view is showing entity from this library, navigate back */
        if (w->current_view && g_strcmp0(w->current_view, "detail") == 0 && w->detail_view) {
            int64_t eid = library_unified_detail_get_current_entity_id(w->detail_view);
            if (eid > 0 && LIBRARY_GLOBAL_ID_LIB(eid) == bitmap_idx) {
                const char *back_to = w->previous_view ? w->previous_view : "artists";
                gtk_stack_set_visible_child_name(GTK_STACK(w->stack), back_to);
                w->current_view = back_to;
            }
        }
    }

    /* Update library card state (libs[] is parallel to settings array) */
    if (si >= 0)
        update_lib_card_availability(w, si, available);

    /* Refresh all browse/search views */
    refresh_library_views(w);
}

static gboolean
auto_scan_idle(gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (w->indexer && w->settings && w->settings->auto_scan_on_startup
        && w->settings->library_count > 0) {
        /* Build arrays of only available libraries */
        int total = w->settings->library_count;
        const char **paths = g_new(const char *, total);
        const char **dpaths = g_new(const char *, total);
        gsize count = 0;
        for (int i = 0; i < total; i++) {
            if (!library_cache_get_available(w->library_cache,
                                             w->settings->libraries[i].library_index))
                continue;
            paths[count] = w->settings->libraries[i].path;
            dpaths[count] = app_settings_get_library_data_path(w->settings, i);
            count++;
        }
        if (count > 0)
            indexer_controller_start(w->indexer, paths, dpaths, count);
        g_free(paths);
        g_free(dpaths);
    }
    return G_SOURCE_REMOVE;
}

GtkWidget *
ui_window_new(GtkApplication *app,
              audio_pipeline_t *pipeline,
              library_cache_t *library_cache,
              app_settings_t *settings)
{
    UiWindow *w = g_object_new(UI_TYPE_WINDOW, "application", app, NULL);
    w->pipeline = pipeline;           /* borrowed from main.c */
    w->library_cache = library_cache; /* borrowed from main.c */
    w->settings = settings;           /* borrowed from main.c */

    g_signal_connect(w, "realize", G_CALLBACK(on_window_realize), w);

    if (settings)
        w->show_spectrum = settings->show_spectrum;

    /* Create indexer (DB is opened per-library by the worker thread) */
    w->indexer = indexer_controller_new();
    if (w->indexer && settings) {
        indexer_controller_set_thread_count(w->indexer, settings->indexer_thread_count);
        indexer_controller_set_process_artwork(w->indexer, settings->process_artwork);
        indexer_controller_set_art_size(w->indexer, settings->art_thumb_size);
        indexer_controller_set_max_concurrent(w->indexer, settings->max_concurrent_library_scans);
        indexer_controller_set_musicbrainz_resolve(w->indexer, settings->musicbrainz_resolve);
        indexer_controller_set_pg_conninfo(w->indexer, settings->musicbrainz_pg_conninfo);
        indexer_controller_set_mb_solr_url(w->indexer, settings->mb_solr_url);
        indexer_controller_set_acoustid_pg_conninfo(w->indexer, settings->acoustid_pg_conninfo);
        indexer_controller_set_acoustid_index_url(w->indexer, settings->acoustid_index_url);
        indexer_controller_set_fanart_api_key(w->indexer, settings->fanart_api_key);
    }

    /* Create artwork manager — sources use bitmap_index for stable library addressing,
     * mirroring library_cache_source_t. */
    int thumb_size = settings ? settings->art_thumb_size : 96;
    artwork_manager_source_t *art_sources = NULL;
    int art_source_count = 0;
    if (settings && settings->library_count > 0) {
        art_source_count = settings->library_count;
        art_sources = g_new0(artwork_manager_source_t, art_source_count);
        for (int i = 0; i < art_source_count; i++) {
            art_sources[i].bitmap_index = settings->libraries[i].library_index;
            art_sources[i].data_root = app_settings_get_library_data_path(settings, i);
            art_sources[i].music_root = settings->libraries[i].path;
        }
    }
    w->artwork_mgr
        = artwork_manager_new(w->library_cache, art_sources, art_source_count, thumb_size, 0);
    g_free(art_sources);

    build_ui(w);

    /* Start background cache warming (after build_ui so views exist for the
     * ready callback, and the sync fallback in populate_artists/albums has
     * already completed before the warming thread can replace all_artists) */
    if (w->library_cache) {
        library_cache_set_ready_callback(w->library_cache, on_cache_ready, w);
        library_cache_start_warming(w->library_cache);
    }

    /* Register track changed callback for auto-advance notification */
    if (pipeline) {
        audio_pipeline_set_track_changed_callback(pipeline, on_track_changed, w);
        audio_pipeline_set_track_failed_callback(pipeline, on_track_failed, w);
    }

    if (w->indexer) {
        g_signal_connect(w->indexer, "started", G_CALLBACK(on_indexer_started), w);
        g_signal_connect(w->indexer, "progress", G_CALLBACK(on_indexer_progress), w);
        g_signal_connect(w->indexer, "library-updated", G_CALLBACK(on_indexer_library_updated), w);
        g_signal_connect(w->indexer, "artwork-updated", G_CALLBACK(on_indexer_artwork_updated), w);
        g_signal_connect(w->indexer, "completed", G_CALLBACK(on_indexer_done), w);
    }

    /* Library availability monitor (GVolumeMonitor + stat() heartbeat) */
    w->lib_monitor = library_monitor_new(w->library_cache, w->settings);
    g_signal_connect(
        w->lib_monitor, "availability-changed", G_CALLBACK(on_library_availability_changed), w);
    library_monitor_start(w->lib_monitor);

    g_idle_add(init_devices_idle, w);

    if (w->indexer && settings && settings->auto_scan_on_startup)
        g_idle_add(auto_scan_idle, w);

    return GTK_WIDGET(w);
}

void
ui_window_navigate_to(UiWindow *w, const char *view)
{
    g_return_if_fail(UI_IS_WINDOW(w));
    g_return_if_fail(view != NULL);

    w->current_view = view;
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), view);

    /* Update action state to sync nav bar toggle buttons */
    if (w->navigate_action)
        g_simple_action_set_state(w->navigate_action, g_variant_new_string(view));
}

void
ui_window_set_spectrum_visible(UiWindow *w, gboolean visible)
{
    g_return_if_fail(UI_IS_WINDOW(w));
    w->show_spectrum = visible;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (w->channels[i])
            ui_channel_strip_set_spectrum_visible(w->channels[i], visible);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Focus Management (New API)
 * ═══════════════════════════════════════════════════════════════════════════ */

void
ui_window_set_focused_channel(UiWindow *w, int ch)
{
    g_return_if_fail(UI_IS_WINDOW(w));

    /* Check if the channel can be focused */
    if (ch >= 0 && ch < MAX_CHANNELS && w->channels[ch]) {
        if (!ui_channel_strip_is_active(w->channels[ch])) {
            /* Cannot focus this channel - show toast and return */
            DeviceState ds = ui_channel_strip_get_device_state(w->channels[ch]);
            ChannelMode mode = ui_channel_strip_get_mode(w->channels[ch]);
            char msg[64];

            if (ds == DEVICE_STATE_UNCONFIGURED || ds == DEVICE_STATE_INVALID) {
                snprintf(msg, sizeof(msg), "Channel %d not configured", ch + 1);
                ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
            } else if (mode == CHANNEL_MODE_QUEUED) {
                snprintf(msg, sizeof(msg), "Channel %d is queued", ch + 1);
                ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
            } else if (mode == CHANNEL_MODE_ON_AIR) {
                snprintf(msg, sizeof(msg), "Channel %d is on air", ch + 1);
                ui_window_show_toast(w, msg, TOAST_WARNING, 3000);
            }
            return;
        }
    }

    /* Toggle off if clicking the same channel */
    if (ch == w->focused_channel) {
        ui_window_clear_focus(w);
        return;
    }

    /* Clear previous focus */
    if (w->focused_channel >= 0 && w->focused_channel < MAX_CHANNELS
        && w->channels[w->focused_channel])
        ui_channel_strip_set_focused(w->channels[w->focused_channel], FALSE);

    w->focused_channel = ch;

    if (ch >= 0 && ch < MAX_CHANNELS && w->channels[ch])
        ui_channel_strip_set_focused(w->channels[ch], TRUE);
}

int
ui_window_get_focused_channel(UiWindow *w)
{
    g_return_val_if_fail(UI_IS_WINDOW(w), -1);
    return w->focused_channel;
}

void
ui_window_clear_focus(UiWindow *w)
{
    g_return_if_fail(UI_IS_WINDOW(w));

    if (w->focused_channel >= 0 && w->focused_channel < MAX_CHANNELS
        && w->channels[w->focused_channel])
        ui_channel_strip_set_focused(w->channels[w->focused_channel], FALSE);

    w->focused_channel = -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Toast Notifications
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean
hide_toast(gpointer data)
{
    UiWindow *w = UI_WINDOW(data);
    if (w->toast_overlay)
        gtk_widget_set_visible(w->toast_overlay, FALSE);
    w->toast_timer = 0;
    return G_SOURCE_REMOVE;
}

/* Apply/remove variant CSS class on the toast box */
static void
apply_toast_variant(GtkWidget *toast_box, ToastVariant variant)
{
    static const char *classes[] = { NULL, "toast-success", "toast-warning", "toast-error" };
    for (int i = 1; i <= 3; i++)
        gtk_widget_remove_css_class(toast_box, classes[i]);
    if (variant >= 1 && variant <= 3)
        gtk_widget_add_css_class(toast_box, classes[variant]);
}

static void
show_toast_impl(
    UiWindow *w, const char *content, gboolean is_markup, ToastVariant variant, guint duration_ms)
{
    g_assert(w->toast_overlay != NULL);
    g_assert(w->toast_label != NULL);

    if (is_markup)
        gtk_label_set_markup(GTK_LABEL(w->toast_label), content);
    else
        gtk_label_set_text(GTK_LABEL(w->toast_label), content);

    apply_toast_variant(w->toast_overlay, variant);
    gtk_widget_set_visible(w->toast_overlay, TRUE);

    if (w->toast_timer)
        g_source_remove(w->toast_timer);
    w->toast_timer = g_timeout_add(duration_ms ? duration_ms : 3000, hide_toast, w);
}

void
ui_window_show_toast(UiWindow *w, const char *message, ToastVariant variant, guint duration_ms)
{
    g_return_if_fail(UI_IS_WINDOW(w));
    g_return_if_fail(message != NULL);
    show_toast_impl(w, message, FALSE, variant, duration_ms);
}

void
ui_window_show_toast_markup(UiWindow *w,
                            const char *markup,
                            ToastVariant variant,
                            guint duration_ms)
{
    g_return_if_fail(UI_IS_WINDOW(w));
    g_return_if_fail(markup != NULL);
    show_toast_impl(w, markup, TRUE, variant, duration_ms);
}
