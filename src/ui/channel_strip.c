/**
 * Quadrature Channel Strip Widget
 *
 * Per-channel display: level meters, spectrum, transport controls, metadata.
 * Uses GTK4 composite template for UI structure.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/library.h"
#include "../audio/internal.h"  /* For audio_cache_load() */
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MARQUEE_SPEED_PPS 50.0   /* Pixels per second */
#define MARQUEE_PAUSE_SEC 2.0    /* Pause at start before scrolling */
#define MARQUEE_SEPARATOR "   •   "  /* Separator between text copies */
#define TIME_WARN_SEC 30
#define TIME_CAUTION_SEC 90
#define SPECTRUM_DECAY_FRAMES 120  /* ~2s at 60fps: matches cava's slow release curve */

/* Countdown color states */
typedef enum {
    TIME_STATE_NONE,     /* No track loaded or no time */
    TIME_STATE_SAFE,     /* > 1:30 - green */
    TIME_STATE_CAUTION,  /* 1:30 to 0:30 - yellow/orange */
    TIME_STATE_WARNING   /* < 0:30 - flashing red */
} time_state_t;

struct _UiChannelStrip {
    GtkWidget parent;

    int channel_id;
    audio_pipeline_t *pipeline;
    library_cache_t *library;    /* For track navigation queries */
    gboolean show_spectrum;
    time_state_t time_state;  /* Cached to avoid repeated CSS updates */

    /* Layered state model */
    DeviceState device_state;    /* Hardware availability */
    ChannelMode mode;            /* Operational mode */
    gboolean focused;            /* Target for song loading */

    /* Queue double-click detection */
    gint64 last_queue_click_time;

    /* Template children */
    GtkWidget *main_box;
    GtkWidget *display_panel;
    GtkWidget *title_scroll;
    GtkWidget *title_label;
    GtkWidget *artist_label;
    GtkWidget *time_label;
    GtkWidget *play_btn;
    GtkWidget *stop_btn;
    GtkWidget *repeat_btn;
    GtkWidget *autoplay_btn;
    GtkWidget *preview_btn;
    GtkWidget *queue_btn;
    GtkWidget *right_column;
    GtkWidget *info_box;

    /* Seek bar */
    GtkWidget *seek_row;
    GtkWidget *waveform;
    int64_t waveform_track_id;  /* Track whose loudness is currently displayed */

    /* Metrics column labels */
    GtkWidget *metrics_column;
    GtkWidget *time_elapsed_label;
    GtkWidget *next_track_duration_label;

    /* Dynamic widgets (not in template) */
    GtkWidget *spectrum;
    GtkWidget *status_scroll;  /* Scrolled window for status_label */
    GtkWidget *status_label;

    /* Gesture controllers (must be stored for proper cleanup) */
    GtkGesture *channel_click_gesture;
    GtkGesture *shuttle_click_gesture;
    GtkGesture *shuttle_drag_gesture;

    /* Shuttle drag state */
    double shuttle_drag_start_value;

    /* Skip/Shuttle controls (template children) */
    GtkWidget *skip_back_15;
    GtkWidget *skip_back_5;
    GtkWidget *skip_fwd_5;
    GtkWidget *skip_fwd_15;
    GtkWidget *shuttle_scale;
    GtkWidget *shuttle_label;
    GtkWidget *shuttle_mode_btn;
    shuttle_mode_t shuttle_mode;  /* OFF, KEYLOCK, PITCHED */

    /* Track info */
    char *filepath;
    char *title;
    char *artist;
    char *album;
    int64_t current_track_id;      /* Current track for LibraryCache queries */

    /* Track end detection */
    channel_state_t prev_player_state;

    /* New template children for album display */
    GtkWidget *album_scroll;
    GtkWidget *album_label;
    GtkWidget *artist_scroll;
    GtkWidget *track_position_label;
    GtkWidget *next_track_scroll;
    GtkWidget *next_track_label;
    GtkWidget *prev_track_btn;
    GtkWidget *next_track_btn;

    /* Gesture controllers for clickable labels */
    GtkGesture *album_click_gesture;
    GtkGesture *artist_click_gesture;
    GtkGesture *next_track_click_gesture;

    /* Device name (for invalid state message) */
    char *device_name;

    /* Cached for seek */
    uint32_t sample_rate;
    uint64_t length_samples;

    /* UI-estimated length from cache duration_ms (used while decode pending) */
    uint64_t ui_length_samples;
    uint64_t deferred_seek_pos;   /* Queued seek position (applied when buffer ready) */
    gboolean has_deferred_seek;

    /* Cached time display strings — avoid per-frame snprintf + gtk_label_set_text */
    char cached_time_remaining[20];
    char cached_time_elapsed[20];

    /* Spectrum decay: frames remaining before we stop updating the spectrum.
     * Reset to SPECTRUM_DECAY_FRAMES when playing; counts down when stopped/paused
     * so the bars animate to zero before we go fully idle. */
    int spectrum_decay_frames;
    uint32_t last_spectrum_gen;  /* Cached generation counter — skip redundant copies */
};

/* Signal IDs */
enum {
    SIGNAL_CLICKED,
    SIGNAL_MODE_CHANGED,
    SIGNAL_ALBUM_CLICKED,    /* (channel_id, album_id) */
    SIGNAL_ARTIST_CLICKED,   /* (channel_id, artist_id) */
    SIGNAL_TRACK_CHANGED,    /* (channel_id, track_id) - emitted on auto-advance */
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Double-click threshold in microseconds (500ms) */
#define DOUBLE_CLICK_THRESHOLD_US 500000

G_DEFINE_FINAL_TYPE(UiChannelStrip, ui_channel_strip, GTK_TYPE_WIDGET)

/* ===============================================================================
 * Table-Driven Button Sensitivity
 * =============================================================================== */

typedef enum {
    SENS_NEEDS_FILE      = 1 << 0,  /* Requires filepath != NULL */
    SENS_NEEDS_DEVICE    = 1 << 1,  /* Requires device_state == VALID */
    SENS_DISABLED_QUEUED = 1 << 2,  /* Disabled when mode == QUEUED */
    SENS_DISABLED_ON_AIR = 1 << 3,  /* Disabled when mode == ON_AIR */
    SENS_NEEDS_PLAYING   = 1 << 4,  /* Requires player state != STOPPED */
} SensitivityFlags;

typedef struct {
    size_t widget_offset;  /* offsetof(UiChannelStrip, member) */
    SensitivityFlags flags;
} SensitivityRule;

#define SENS_RULE(member, f) { offsetof(UiChannelStrip, member), (f) }

static const SensitivityRule sensitivity_rules[] = {
    /* play_btn: file + device, disabled only in ON_AIR (QUEUED allows play -> ON_AIR transition) */
    SENS_RULE(play_btn,     SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_ON_AIR),
    /* stop_btn: file + device + must be playing + disabled in both modes */
    SENS_RULE(stop_btn,     SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR | SENS_NEEDS_PLAYING),
    /* repeat_btn: file + device (always available when track loaded, even in QUEUED/ON_AIR) */
    SENS_RULE(repeat_btn,   SENS_NEEDS_FILE | SENS_NEEDS_DEVICE),
    /* autoplay_btn: file + device (always available when track loaded, even in QUEUED/ON_AIR) */
    SENS_RULE(autoplay_btn, SENS_NEEDS_FILE | SENS_NEEDS_DEVICE),
    /* waveform seek bar: file + device + disabled in both modes */
    SENS_RULE(waveform,     SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    /* preview_btn: file + device + disabled in both modes */
    SENS_RULE(preview_btn,  SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    /* queue_btn: only needs device (no file required) */
    SENS_RULE(queue_btn,    SENS_NEEDS_DEVICE),
    /* skip buttons: file + device + disabled in both modes */
    SENS_RULE(skip_back_15, SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    SENS_RULE(skip_back_5,  SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    SENS_RULE(skip_fwd_5,   SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    SENS_RULE(skip_fwd_15,  SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    /* shuttle_scale: file + device + disabled in both modes */
    SENS_RULE(shuttle_scale, SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
    /* shuttle_mode_btn: always enabled (not affected by channel state) */
};

static void update_button_sensitivity(UiChannelStrip *s, channel_state_t player_state) {
    gboolean has_file = (s->filepath != NULL);
    gboolean device_valid = (s->device_state == DEVICE_STATE_VALID);
    gboolean in_queued = (s->mode == CHANNEL_MODE_QUEUED);
    gboolean in_on_air = (s->mode == CHANNEL_MODE_ON_AIR);
    gboolean is_playing = (player_state != CHANNEL_STOPPED);

    for (size_t i = 0; i < G_N_ELEMENTS(sensitivity_rules); i++) {
        const SensitivityRule *r = &sensitivity_rules[i];
        GtkWidget **widget_ptr = (GtkWidget **)((char *)s + r->widget_offset);
        gboolean sensitive = TRUE;

        if ((r->flags & SENS_NEEDS_FILE) && !has_file) sensitive = FALSE;
        if ((r->flags & SENS_NEEDS_DEVICE) && !device_valid) sensitive = FALSE;
        if ((r->flags & SENS_DISABLED_QUEUED) && in_queued) sensitive = FALSE;
        if ((r->flags & SENS_DISABLED_ON_AIR) && in_on_air) sensitive = FALSE;
        if ((r->flags & SENS_NEEDS_PLAYING) && !is_playing) sensitive = FALSE;

        gtk_widget_set_sensitive(*widget_ptr, sensitive);
    }
}

/* ===============================================================================
 * Callbacks (bound via template)
 * =============================================================================== */

static void on_channel_clicked(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    g_signal_emit(s, signals[SIGNAL_CLICKED], 0, s->channel_id);
}

/* Marquee state stored per scroll widget */
typedef struct {
    double pos;           /* Current scroll position in pixels */
    double pause_time;    /* Accumulated pause time in seconds */
    double loop_point;    /* Position at which to seamlessly loop (original text + separator width) */
    gint64 last_frame;    /* Last frame time for delta calculation */
} marquee_state_t;

/**
 * Measure the pixel width of text using the label's Pango context.
 */
static int measure_text_width(GtkLabel *label, const char *text) {
    PangoLayout *layout = gtk_label_get_layout(label);
    PangoContext *context = pango_layout_get_context(layout);
    PangoFontDescription *font = pango_context_get_font_description(context);

    PangoLayout *temp = pango_layout_new(context);
    pango_layout_set_font_description(temp, font);
    pango_layout_set_text(temp, text, -1);

    int width;
    pango_layout_get_pixel_size(temp, &width, NULL);
    g_object_unref(temp);
    return width;
}

/**
 * Set up a label for seamless marquee scrolling.
 * If text is wider than container, duplicates it with a separator.
 * Returns the loop point (original width + separator), or 0 if no scrolling needed.
 */
static void on_marquee_map(GtkWidget *scroll, gpointer label);
static void marquee_update_loop_point(GtkWidget *scroll, double loop_point);

static double marquee_setup_label(GtkWidget *scroll, GtkWidget *label, const char *text) {
    if (!scroll || !label || !text || !text[0]) {
        if (label) gtk_label_set_text(GTK_LABEL(label), text ? text : "");
        g_object_set_data(G_OBJECT(scroll), "marquee-pending-text", NULL);
        return 0;
    }

    /* Measure text width without setting label (avoids single-frame flash) */
    int text_width = measure_text_width(GTK_LABEL(label), text);
    int container_width = gtk_widget_get_width(scroll);

    /* Container not yet allocated — store pending text and defer */
    if (container_width <= 0) {
        gtk_label_set_text(GTK_LABEL(label), text);
        g_object_set_data_full(G_OBJECT(scroll), "marquee-pending-text",
                               g_strdup(text), g_free);
        if (!g_object_get_data(G_OBJECT(scroll), "marquee-map-connected")) {
            g_signal_connect(scroll, "map", G_CALLBACK(on_marquee_map), label);
            g_object_set_data(G_OBJECT(scroll), "marquee-map-connected",
                              GINT_TO_POINTER(1));
        }
        return 0;
    }

    /* Clear any pending deferred setup */
    g_object_set_data(G_OBJECT(scroll), "marquee-pending-text", NULL);

    /* Text fits — set directly, no scrolling needed */
    if (text_width <= container_width) {
        gtk_label_set_text(GTK_LABEL(label), text);
        return 0;
    }

    /* Duplicate text with separator for seamless loop */
    int sep_width = measure_text_width(GTK_LABEL(label), MARQUEE_SEPARATOR);
    char *duplicated = g_strconcat(text, MARQUEE_SEPARATOR, text, NULL);
    gtk_label_set_text(GTK_LABEL(label), duplicated);
    g_free(duplicated);

    /* Loop point: when we've scrolled past original text + separator,
     * the view shows the duplicate which looks identical to position 0 */
    return (double)(text_width + sep_width);
}

/**
 * Deferred marquee setup: called when a ScrolledWindow is mapped after
 * marquee_setup_label() was called before the widget had a valid width.
 */
static void on_marquee_map(GtkWidget *scroll, gpointer label) {
    const char *text = g_object_get_data(G_OBJECT(scroll), "marquee-pending-text");
    if (!text) return;
    double loop = marquee_setup_label(scroll, GTK_WIDGET(label), text);
    marquee_update_loop_point(scroll, loop);
}

/**
 * Seamless marquee using frame-synchronized tick callback.
 *
 * Pattern: pause at start → scroll continuously → seamless loop (invisible reset)
 * Text is duplicated: "Title  •  Title" so when we scroll past the first copy,
 * resetting to 0 is invisible because the duplicate looks identical.
 */
static gboolean marquee_tick_callback(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget;
    GtkWidget *scroll = GTK_WIDGET(data);

    if (!GTK_IS_SCROLLED_WINDOW(scroll)) return G_SOURCE_REMOVE;

    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
    double max = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);

    if (max <= 0) return G_SOURCE_CONTINUE;  /* Content fits, no scrolling needed */

    /* Get or create marquee state */
    marquee_state_t *state = g_object_get_data(G_OBJECT(scroll), "marquee-state");
    if (!state) {
        state = g_new0(marquee_state_t, 1);
        state->pos = 0;
        state->pause_time = 0;
        state->loop_point = 0;
        state->last_frame = 0;
        g_object_set_data_full(G_OBJECT(scroll), "marquee-state", state, g_free);
    }

    /* If no loop point set, scrolling not configured yet */
    if (state->loop_point <= 0) return G_SOURCE_CONTINUE;

    /* Calculate delta time */
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double dt = 0;
    if (state->last_frame > 0) {
        dt = (now - state->last_frame) / 1000000.0;  /* Convert µs to seconds */
        if (dt > 0.1) dt = 0.1;  /* Cap delta to avoid jumps after pause */
    }
    state->last_frame = now;

    /* State machine: pause at start, then scroll continuously */
    if (state->pause_time < MARQUEE_PAUSE_SEC) {
        state->pause_time += dt;
        gtk_adjustment_set_value(adj, 0);
        return G_SOURCE_CONTINUE;
    }

    /* Scrolling: advance position based on velocity */
    state->pos += MARQUEE_SPEED_PPS * dt;

    /* Seamless loop: when we reach the duplicate text, reset to 0 */
    if (state->pos >= state->loop_point) {
        state->pos = 0;
        state->pause_time = 0;  /* Pause again at start */
    }

    gtk_adjustment_set_value(adj, CLAMP(state->pos, 0, max));
    return G_SOURCE_CONTINUE;
}

/**
 * Update marquee state when label text changes.
 * Manages tick callback lifecycle: installs when scrolling needed, removes when not.
 */
static void marquee_update_loop_point(GtkWidget *scroll, double loop_point) {
    if (!scroll) return;

    marquee_state_t *state = g_object_get_data(G_OBJECT(scroll), "marquee-state");
    if (!state) {
        state = g_new0(marquee_state_t, 1);
        g_object_set_data_full(G_OBJECT(scroll), "marquee-state", state, g_free);
    }

    state->loop_point = loop_point;
    state->pos = 0;
    state->pause_time = 0;
    state->last_frame = 0;

    /* Reset scroll position immediately to avoid a single frame where new text
     * renders at the old scroll offset (causes 1px artifact on track change) */
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
    if (adj) gtk_adjustment_set_value(adj, 0);

    guint tick_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(scroll), "marquee-tick-id"));

    if (loop_point > 0 && tick_id == 0) {
        /* Scrolling needed — install tick callback */
        tick_id = gtk_widget_add_tick_callback(scroll, marquee_tick_callback, scroll, NULL);
        g_object_set_data(G_OBJECT(scroll), "marquee-tick-id", GUINT_TO_POINTER(tick_id));
    } else if (loop_point <= 0 && tick_id > 0) {
        /* Text fits — remove tick callback */
        gtk_widget_remove_tick_callback(scroll, tick_id);
        g_object_set_data(G_OBJECT(scroll), "marquee-tick-id", NULL);
    }
}

static void on_play_pause(GtkButton *btn, gpointer data) {
    (void)btn;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);

    g_debug("on_play_pause: channel=%d, pipeline=%p, mode=%d, filepath=%s",
            s->channel_id, (void *)s->pipeline, s->mode,
            s->filepath ? s->filepath : "(none)");

    if (!s->pipeline) {
        g_warning("on_play_pause: no pipeline for channel %d", s->channel_id);
        return;
    }

    /* In QUEUED mode, play transitions to ON_AIR */
    if (s->mode == CHANNEL_MODE_QUEUED) {
        ui_channel_strip_play(s);  /* Now uses unified function */
        const char *title = gtk_label_get_text(GTK_LABEL(s->title_label));
        const char *artist = gtk_label_get_text(GTK_LABEL(s->artist_label));
        g_info("Playback → Channel %d: Start playback (QUEUED → ON_AIR) - '%s' by %s",
               s->channel_id + 1,
               title && *title ? title : "<no track>",
               artist && *artist ? artist : "<unknown>");
        return;
    }

    /* In ON_AIR mode, play/pause is disabled (handled by button sensitivity) */
    if (s->mode == CHANNEL_MODE_ON_AIR) {
        g_debug("on_play_pause: ignored, mode is ON_AIR");
        return;
    }

    /* If the strip has a track but the pipeline player doesn't (e.g. device was
     * inactive when the track was loaded), retry set_player_track now. */
    if (s->current_track_id > 0 && s->pipeline->cache) {
        int64_t pipeline_track = audio_pipeline_get_player_track_id(s->pipeline, s->channel_id);
        if (pipeline_track <= 0) {
            gboolean streams = audio_pipeline_player_streams_active(s->pipeline, s->channel_id);
            g_info("Playback → Channel %d: retrying set_player_track for %" G_GINT64_FORMAT
                   " (streams_active=%d, device_state=%d)",
                   s->channel_id + 1, s->current_track_id, streams, s->device_state);
            audio_cache_load(s->pipeline->cache, s->current_track_id);
            quadrature_result_t retry = audio_pipeline_set_player_track(
                s->pipeline, s->channel_id, s->current_track_id);
            if (retry != QUADRATURE_OK) {
                g_warning("Playback → Channel %d: retry set_player_track FAILED - result=%d",
                          s->channel_id + 1, retry);
                return;
            }
        }
    }

    /* Toggle play/pause atomically */
    quadrature_result_t res = audio_pipeline_player_toggle_play(s->pipeline, s->channel_id);
    if (res == QUADRATURE_OK) {
        const char *title = gtk_label_get_text(GTK_LABEL(s->title_label));
        g_info("Playback → Channel %d: Toggle play/pause - '%s'", s->channel_id + 1,
               title && *title ? title : "<no track>");
    } else {
        g_warning("Playback → Channel %d: Toggle play/pause FAILED - result=%d",
                  s->channel_id + 1, res);
    }
}

static void on_stop(GtkButton *btn, gpointer data) {
    (void)btn;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    ui_channel_strip_stop(s);  /* Now uses unified function with mode transition */
}

static void update_visual_state(UiChannelStrip *s);  /* Forward declaration */
static void update_album_display(UiChannelStrip *s); /* Forward declaration */

static void on_preview(GtkToggleButton *btn, gpointer data) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    gboolean on = gtk_toggle_button_get_active(btn);

    /* Use public API functions which handle signal blocking */
    if (on) {
        ui_channel_strip_preview_on(s);
    } else {
        ui_channel_strip_preview_off(s);
    }
}

static void on_repeat(GtkToggleButton *btn, gpointer data) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    gboolean on = gtk_toggle_button_get_active(btn);

    if (s->pipeline) {
        audio_pipeline_player_set_repeat(s->pipeline, s->channel_id, on);

        /* Repeat and autoplay are mutually exclusive */
        if (on && audio_pipeline_player_get_autoplay(s->pipeline, s->channel_id)) {
            audio_pipeline_player_set_autoplay(s->pipeline, s->channel_id, false);
            if (s->autoplay_btn) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->autoplay_btn), FALSE);
            }
        }
    }

    /* Update next track label when repeat changes */
    update_album_display(s);
}

static void on_autoplay(GtkToggleButton *btn, gpointer data) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    gboolean on = gtk_toggle_button_get_active(btn);

    if (s->pipeline) {
        audio_pipeline_player_set_autoplay(s->pipeline, s->channel_id, on);

        /* Repeat and autoplay are mutually exclusive */
        if (on && audio_pipeline_player_get_repeat(s->pipeline, s->channel_id)) {
            audio_pipeline_player_set_repeat(s->pipeline, s->channel_id, false);
            if (s->repeat_btn) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->repeat_btn), FALSE);
            }
        }
    }
}

static void on_queue(GtkButton *btn, gpointer data) {
    (void)btn;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);

    /* Cannot queue if device is not valid */
    if (s->device_state != DEVICE_STATE_VALID) return;

    gint64 now = g_get_monotonic_time();
    gboolean is_double_click = (now - s->last_queue_click_time) < DOUBLE_CLICK_THRESHOLD_US;
    s->last_queue_click_time = now;

    switch (s->mode) {
    case CHANNEL_MODE_PREVIEW:
        /* Exit preview first (clears mode and button state) */
        ui_channel_strip_preview_off(s);
        /* FALLTHROUGH - now in IDLE, same logic as IDLE case */
        
    case CHANNEL_MODE_IDLE: {
        /* If already playing, go straight to ON_AIR; otherwise QUEUED */
        gboolean playing = s->pipeline &&
            (audio_pipeline_get_player_state(s->pipeline, s->channel_id) == CHANNEL_PLAYING);
        if (playing) {
            ui_channel_strip_set_mode(s, CHANNEL_MODE_ON_AIR);
        } else {
            ui_channel_strip_set_mode(s, CHANNEL_MODE_QUEUED);
        }
        /* Reset so the next click in the new state starts a fresh window */
        s->last_queue_click_time = 0;
        break;
    }

    case CHANNEL_MODE_QUEUED:
        if (is_double_click) {
            /* Double-click: exit QUEUED, return to IDLE */
            ui_channel_strip_set_mode(s, CHANNEL_MODE_IDLE);
        }
        /* Single click: do nothing (wait for play or double-click) */
        break;

    case CHANNEL_MODE_ON_AIR:
        if (is_double_click) {
            /* Double-click: exit ON_AIR, return to IDLE (keeps playing) */
            ui_channel_strip_set_mode(s, CHANNEL_MODE_IDLE);
        }
        /* Single click: do nothing */
        break;
    }
}

/* Seek signal handler — connected to UiWaveformSeekBar's "seek" signal */
static void on_seek(UiWaveformSeekBar *waveform, double value, gpointer data) {
    (void)waveform;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    if (s->length_samples > 0) {
        uint64_t target = (uint64_t)(value * (double)s->length_samples);
        audio_pipeline_player_seek(s->pipeline, s->channel_id, target);
    } else if (s->ui_length_samples > 0) {
        s->deferred_seek_pos = (uint64_t)(value * (double)s->ui_length_samples);
        s->has_deferred_seek = TRUE;
    }
}

/* ===============================================================================
 * Skip/Shuttle Callbacks
 * =============================================================================== */

/* Speed range constants.
 * Both KEYLOCK and PITCHED share the same slider range (-2 to +3) so the
 * scale/thumb position is visually consistent across modes.  The difference
 * is entirely in the speed mapping curve: KEYLOCK covers a wide range
 * (0.5x-4.0x) while PITCHED is more conservative (0.5x-1.5x, symmetric)
 * with a much flatter quadratic curve spread across the same slider travel. */
/* shuttle_value_to_speed → ui_ui_shuttle_value_to_speed() in ui_math.c */

static void do_skip(UiChannelStrip *s, int seconds) {
    if (!s->pipeline) return;

    uint64_t pos = audio_pipeline_get_player_position(s->pipeline, s->channel_id);
    uint64_t len = audio_pipeline_get_player_length(s->pipeline, s->channel_id);
    uint32_t rate = audio_pipeline_get_sample_rate(s->pipeline);
    if (rate == 0 || len == 0) return;

    int64_t delta = (int64_t)seconds * rate;
    int64_t new_pos = (int64_t)pos + delta;

    if (new_pos < 0) new_pos = 0;
    if ((uint64_t)new_pos >= len) new_pos = (int64_t)len - 1;

    audio_pipeline_player_seek(s->pipeline, s->channel_id, (uint64_t)new_pos);
}

static void on_skip_clicked(GtkButton *btn, gpointer data) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    /* Skip disabled in QUEUED and ON_AIR modes */
    if (s->mode == CHANNEL_MODE_QUEUED || s->mode == CHANNEL_MODE_ON_AIR) return;
    int seconds = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "skip-seconds"));
    do_skip(s, seconds);
}

static void update_shuttle_label(UiChannelStrip *s, double slider_value) {
    char buf[16];
    float speed = ui_shuttle_value_to_speed(slider_value, s->shuttle_mode);

    /* Show 2 decimal places between 0.5x and 1.5x for fine control visibility */
    if (speed >= 0.5f && speed <= 1.5f) {
        snprintf(buf, sizeof(buf), "%.2fx", speed);
    } else {
        /* Outside 0.5-1.5 range: show 1 decimal place */
        snprintf(buf, sizeof(buf), "%.1fx", speed);
    }

    gtk_label_set_text(GTK_LABEL(s->shuttle_label), buf);
}

static void on_shuttle_value_changed(GtkRange *r, gpointer data) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    double slider_value = gtk_range_get_value(r);

    /* Update label with mapped speed */
    update_shuttle_label(s, slider_value);

    /* Convert slider value to mapped speed and set playback speed */
    if (s->pipeline) {
        float speed = ui_shuttle_value_to_speed(slider_value, s->shuttle_mode);
        audio_pipeline_player_set_speed(s->pipeline, s->channel_id, speed);
    }
}

/**
 * Update shuttle mode button appearance.
 */
static void update_shuttle_mode_button(UiChannelStrip *s) {
    const char *label;
    const char *css_class;

    /* Remove all mode classes first */
    gtk_widget_remove_css_class(s->shuttle_mode_btn, "shuttle-mode-off");
    gtk_widget_remove_css_class(s->shuttle_mode_btn, "shuttle-mode-keylock");
    gtk_widget_remove_css_class(s->shuttle_mode_btn, "shuttle-mode-pitched");

    switch (s->shuttle_mode) {
    case SHUTTLE_MODE_KEYLOCK:
        label = "KEY";
        css_class = "shuttle-mode-keylock";
        break;
    case SHUTTLE_MODE_PITCHED:
        label = "PITCH";
        css_class = "shuttle-mode-pitched";
        break;
    case SHUTTLE_MODE_OFF:
    default:
        label = "OFF";
        css_class = "shuttle-mode-off";
        break;
    }

    gtk_button_set_label(GTK_BUTTON(s->shuttle_mode_btn), label);
    gtk_widget_add_css_class(s->shuttle_mode_btn, css_class);
}

/**
 * Shuttle mode toggle callback.
 * Cycles: OFF → KEYLOCK → PITCHED → OFF
 */
static void on_shuttle_mode_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);

    /* Cycle to next mode */
    switch (s->shuttle_mode) {
    case SHUTTLE_MODE_OFF:
        s->shuttle_mode = SHUTTLE_MODE_KEYLOCK;
        break;
    case SHUTTLE_MODE_KEYLOCK:
        s->shuttle_mode = SHUTTLE_MODE_PITCHED;
        break;
    case SHUTTLE_MODE_PITCHED:
    default:
        s->shuttle_mode = SHUTTLE_MODE_OFF;
        break;
    }

    /* Update button appearance */
    update_shuttle_mode_button(s);

    /* Update audio pipeline */
    if (s->pipeline) {
        audio_pipeline_player_set_shuttle_mode(s->pipeline, s->channel_id, s->shuttle_mode);
    }

    /* OFF mode: reset slider to center and disable it.
     * KEYLOCK/PITCHED: enable slider (unless QUEUED/ON_AIR overrides).
     * Slider range is always -2..+3 — modes only differ in speed mapping. */
    if (s->shuttle_mode == SHUTTLE_MODE_OFF) {
        gtk_range_set_value(GTK_RANGE(s->shuttle_scale), 0.0);
        gtk_widget_set_sensitive(s->shuttle_scale, FALSE);
    } else {
        /* Only enable if not in a locked broadcast state */
        gboolean locked = (s->mode == CHANNEL_MODE_QUEUED ||
                           s->mode == CHANNEL_MODE_ON_AIR);
        gtk_widget_set_sensitive(s->shuttle_scale, !locked);
    }

    /* Update label and speed with new mapping */
    double current = gtk_range_get_value(GTK_RANGE(s->shuttle_scale));
    update_shuttle_label(s, current);
    if (s->pipeline) {
        float speed = ui_shuttle_value_to_speed(current, s->shuttle_mode);
        audio_pipeline_player_set_speed(s->pipeline, s->channel_id, speed);
    }
}

/**
 * Shuttle click handler in CAPTURE phase.
 * - Right-click: reset shuttle to 1.0x speed and claim event
 * - Left-click: don't claim here, let drag gesture handle it
 */
static void on_shuttle_pressed(GtkGestureClick *gesture, int n_press,
                                double x, double y, gpointer data) {
    (void)n_press; (void)x; (void)y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);

    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    if (button == GDK_BUTTON_SECONDARY) {
        /* Right-click: reset shuttle to center (speed = 1.0) */
        gtk_range_set_value(GTK_RANGE(s->shuttle_scale), 0.0);

        if (s->pipeline) {
            audio_pipeline_player_set_speed(s->pipeline, s->channel_id, 1.0f);
        }

        update_shuttle_label(s, 0.0);

        /* Claim only for right-click */
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }
    /* Left-click: don't claim, let the grouped drag gesture handle it */
}

/**
 * Shuttle drag begin - save starting value and claim to block click-to-jump.
 */
static void on_shuttle_drag_begin(GtkGestureDrag *gesture, double start_x,
                                   double start_y, gpointer data) {
    (void)start_x; (void)start_y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    s->shuttle_drag_start_value = gtk_range_get_value(GTK_RANGE(s->shuttle_scale));

    /* Claim sequence to prevent GtkScale's default click-to-jump */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/**
 * Shuttle drag update - calculate new value from drag offset.
 */
static void on_shuttle_drag_update(GtkGestureDrag *gesture, double offset_x,
                                    double offset_y, gpointer data) {
    (void)gesture; (void)offset_y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);

    int width = gtk_widget_get_width(s->shuttle_scale);
    if (width <= 0) return;

    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(s->shuttle_scale));
    double lower = gtk_adjustment_get_lower(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double range = upper - lower;

    /* Convert pixel offset to value delta */
    double delta = (offset_x / (double)width) * range;
    double new_value = CLAMP(s->shuttle_drag_start_value + delta, lower, upper);

    gtk_range_set_value(GTK_RANGE(s->shuttle_scale), new_value);
}

/* ===============================================================================
 * Album Context Helpers (using LibraryCache)
 * =============================================================================== */

static gboolean can_go_previous(UiChannelStrip *s) {
    if (!s->library || s->current_track_id <= 0) return FALSE;
    return library_cache_get_prev_track_id(s->library, s->current_track_id) > 0;
}

static gboolean can_go_next(UiChannelStrip *s) {
    if (!s->library || s->current_track_id <= 0) return FALSE;
    return library_cache_get_next_track_id(s->library, s->current_track_id) > 0;
}

/**
 * Update album display elements using LibraryCache.
 */
static void update_album_display(UiChannelStrip *s) {
    if (!s->album_label || !s->track_position_label || !s->next_track_label)
        return;

    /* Get current track info from LibraryCache */
    const library_track_info_t *track = NULL;
    const library_album_info_t *album = NULL;
    GPtrArray *album_tracks = NULL;

    if (s->library && s->current_track_id > 0) {
        track = library_cache_get_track(s->library, s->current_track_id);
        if (track) {
            album = library_cache_get_album(s->library, track->album_id, LIBRARY_MASK_ALL);
            album_tracks = library_cache_get_tracks_by_album(s->library, track->album_id, LIBRARY_MASK_ALL);
        }
    }

    double album_loop = 0, next_loop = 0;

    if (track && album && album_tracks && album_tracks->len > 0) {
        /* Album name with seamless marquee */
        const char *album_text = album->title ? album->title : "";
        album_loop = marquee_setup_label(s->album_scroll, s->album_label, album_text);

        /* Find current track index in album */
        int current_index = -1;
        for (guint i = 0; i < album_tracks->len; i++) {
            const library_track_info_t *t = g_ptr_array_index(album_tracks, i);
            if (t->track_id == s->current_track_id) {
                current_index = (int)i;
                break;
            }
        }

        /* Track position "3/12" */
        if (current_index >= 0) {
            char pos[16];
            snprintf(pos, sizeof(pos), "%d/%u", current_index + 1, album_tracks->len);
            gtk_label_set_text(GTK_LABEL(s->track_position_label), pos);
        } else {
            gtk_label_set_text(GTK_LABEL(s->track_position_label), "");
        }

        /* Next track preview - query engine for repeat state */
        gboolean repeat = s->pipeline ?
            audio_pipeline_player_get_repeat(s->pipeline, s->channel_id) : FALSE;

        if (repeat) {
            next_loop = marquee_setup_label(s->next_track_scroll, s->next_track_label, "Repeating");
            gtk_widget_set_sensitive(s->next_track_label, FALSE);
            /* Show current track duration when repeating */
            if (s->next_track_duration_label) {
                char dur_buf[16];
                ui_format_duration(track->duration_ms, dur_buf, sizeof(dur_buf));
                gtk_label_set_text(GTK_LABEL(s->next_track_duration_label), dur_buf);
            }
        } else {
            /* Get next track from LibraryCache */
            int64_t next_id = library_cache_get_next_track_id(s->library, s->current_track_id);
            if (next_id > 0) {
                const library_track_info_t *next_track = library_cache_get_track(s->library, next_id);
                if (next_track) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Next: %s", next_track->title ? next_track->title : "");
                    next_loop = marquee_setup_label(s->next_track_scroll, s->next_track_label, buf);
                    gtk_widget_set_sensitive(s->next_track_label, TRUE);
                    /* Show next track duration */
                    if (s->next_track_duration_label) {
                        char dur_buf[16];
                        ui_format_duration(next_track->duration_ms, dur_buf, sizeof(dur_buf));
                        gtk_label_set_text(GTK_LABEL(s->next_track_duration_label), dur_buf);
                    }
                }
            } else {
                next_loop = marquee_setup_label(s->next_track_scroll, s->next_track_label, "Next: —");
                gtk_widget_set_sensitive(s->next_track_label, FALSE);
                if (s->next_track_duration_label) {
                    gtk_label_set_text(GTK_LABEL(s->next_track_duration_label), "");
                }
            }
        }

        /* Update navigation button sensitivity */
        if (s->prev_track_btn) {
            gboolean can_prev = can_go_previous(s) &&
                s->mode != CHANNEL_MODE_QUEUED && s->mode != CHANNEL_MODE_ON_AIR;
            gtk_widget_set_sensitive(s->prev_track_btn, can_prev);
        }
        if (s->next_track_btn) {
            gboolean can_next = can_go_next(s) &&
                s->mode != CHANNEL_MODE_QUEUED && s->mode != CHANNEL_MODE_ON_AIR;
            gtk_widget_set_sensitive(s->next_track_btn, can_next);
        }
    } else {
        /* No track loaded or no library - clear album display */
        const char *album_text = s->album ? s->album : "";
        album_loop = marquee_setup_label(s->album_scroll, s->album_label, album_text);
        gtk_label_set_text(GTK_LABEL(s->track_position_label), "");
        next_loop = marquee_setup_label(s->next_track_scroll, s->next_track_label, "");
        gtk_widget_set_sensitive(s->next_track_label, FALSE);
        if (s->next_track_duration_label) {
            gtk_label_set_text(GTK_LABEL(s->next_track_duration_label), "");
        }

        if (s->prev_track_btn)
            gtk_widget_set_sensitive(s->prev_track_btn, FALSE);
        if (s->next_track_btn)
            gtk_widget_set_sensitive(s->next_track_btn, FALSE);
    }

    g_clear_pointer(&album_tracks, g_ptr_array_unref);

    /* Update marquee loop points */
    marquee_update_loop_point(s->album_scroll, album_loop);
    marquee_update_loop_point(s->next_track_scroll, next_loop);
}

/* ===============================================================================
 * Helpers
 * =============================================================================== */

static void update_display(UiChannelStrip *s) {
    const char *t = (s->title && s->title[0]) ? s->title :
                    (s->filepath ? strrchr(s->filepath, '/') : NULL);
    if (t && t[0] == '/') t++;

    /* Set up title with seamless marquee if needed */
    const char *title_text = t ? t : "No file loaded";
    double title_loop = marquee_setup_label(s->title_scroll, s->title_label, title_text);
    marquee_update_loop_point(s->title_scroll, title_loop);

    /* Set up artist with seamless marquee if needed */
    const char *artist_text = s->artist ? s->artist : "";
    double artist_loop = marquee_setup_label(s->artist_scroll, s->artist_label, artist_text);
    marquee_update_loop_point(s->artist_scroll, artist_loop);

    /* Update album display */
    update_album_display(s);
}

static void update_play_icon(UiChannelStrip *s, gboolean playing) {
    gtk_button_set_icon_name(GTK_BUTTON(s->play_btn),
        playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
}

/* CSS classes for visual states (in priority order: highest first) */
static const char *visual_state_classes[] = {
    "channel-strip-invalid",       /* DEVICE_STATE_INVALID */
    "channel-strip-unconfigured",  /* DEVICE_STATE_UNCONFIGURED */
    "channel-strip-on-air",        /* CHANNEL_MODE_ON_AIR */
    "channel-strip-queued",        /* CHANNEL_MODE_QUEUED */
    "channel-strip-preview",       /* CHANNEL_MODE_PREVIEW */
    "channel-strip-focused",       /* focused == TRUE */
    NULL                           /* IDLE / default */
};

/* Remove all visual state CSS classes */
static void clear_visual_classes(UiChannelStrip *s) {
    for (int i = 0; visual_state_classes[i] != NULL; i++) {
        gtk_widget_remove_css_class(GTK_WIDGET(s), visual_state_classes[i]);
    }
}

/* Update visual state based on priority hierarchy */
static void update_visual_state(UiChannelStrip *s) {
    clear_visual_classes(s);

    const char *css_class = NULL;

    /* Priority order (highest first) */
    if (s->device_state == DEVICE_STATE_INVALID) {
        css_class = "channel-strip-invalid";
    } else if (s->device_state == DEVICE_STATE_UNCONFIGURED) {
        css_class = "channel-strip-unconfigured";
    } else if (s->mode == CHANNEL_MODE_ON_AIR) {
        css_class = "channel-strip-on-air";
    } else if (s->mode == CHANNEL_MODE_QUEUED) {
        css_class = "channel-strip-queued";
    } else if (s->mode == CHANNEL_MODE_PREVIEW) {
        css_class = "channel-strip-preview";
    } else if (s->focused) {
        css_class = "channel-strip-focused";
    }

    if (css_class)
        gtk_widget_add_css_class(GTK_WIDGET(s), css_class);

    /* Update status display for error states */
    if (!s->status_scroll || !s->status_label || !s->info_box) return;

    if (s->device_state == DEVICE_STATE_UNCONFIGURED) {
        gtk_label_set_text(GTK_LABEL(s->status_label), "No Output Device Set");
        gtk_widget_remove_css_class(s->status_label, "status-invalid");
        gtk_widget_add_css_class(s->status_label, "status-unconfigured");
        gtk_widget_set_visible(s->status_scroll, TRUE);
        gtk_widget_set_visible(s->info_box, FALSE);
    } else if (s->device_state == DEVICE_STATE_INVALID) {
        char msg[512];
        if (s->device_name && s->device_name[0])
            snprintf(msg, sizeof(msg), "Audio Device \"%s\" Not Available", s->device_name);
        else
            snprintf(msg, sizeof(msg), "Audio Device Not Available");
        gtk_label_set_text(GTK_LABEL(s->status_label), msg);
        gtk_widget_remove_css_class(s->status_label, "status-unconfigured");
        gtk_widget_add_css_class(s->status_label, "status-invalid");
        gtk_widget_set_visible(s->status_scroll, TRUE);
        gtk_widget_set_visible(s->info_box, FALSE);
    } else {
        gtk_widget_set_visible(s->status_scroll, FALSE);
        gtk_widget_set_visible(s->info_box, TRUE);
    }
}

/* ===============================================================================
 * Album/Artist/Track Click Handlers
 * =============================================================================== */

static void on_album_clicked(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    if (!s->library || s->current_track_id <= 0) return;

    const library_track_info_t *track = library_cache_get_track(s->library, s->current_track_id);
    if (track && track->album_id > 0) {
        g_signal_emit(s, signals[SIGNAL_ALBUM_CLICKED], 0, s->channel_id, track->album_id);
    }
}

/* Data passed to each artist button inside the popover */
typedef struct {
    UiChannelStrip *strip;
    int64_t         artist_id;
} ArtistPopoverData;

static void on_artist_popover_btn_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    ArtistPopoverData *d = data;
    /* Dismiss first so navigation doesn't fight the open popover */
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    g_signal_emit(d->strip, signals[SIGNAL_ARTIST_CLICKED], 0,
                  d->strip->channel_id, d->artist_id);
}

static void on_artist_popover_closed(GtkPopover *popover, gpointer data) {
    (void)data;
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void on_artist_clicked(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    if (!s->library || s->current_track_id <= 0) return;

    const GPtrArray *track_artists = library_cache_get_track_artists(s->library, s->current_track_id);

    /* Multiple artists — show popover so the user can choose */
    if (track_artists && track_artists->len > 1) {
        GtkWidget *popover = gtk_popover_new();
        gtk_widget_set_parent(popover, s->artist_scroll);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_popover_set_child(GTK_POPOVER(popover), box);

        for (guint i = 0; i < track_artists->len; i++) {
            const library_track_artist_t *a = g_ptr_array_index(track_artists, i);
            if (!a->name || a->artist_id <= 0) continue;

            GtkWidget *btn = gtk_button_new_with_label(a->name);
            gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
            gtk_widget_add_css_class(btn, "artist-btn");
            gtk_widget_set_halign(btn, GTK_ALIGN_START);

            ArtistPopoverData *d = g_new(ArtistPopoverData, 1);
            d->strip     = s;
            d->artist_id = a->artist_id;
            g_object_set_data_full(G_OBJECT(btn), "artist-data", d, g_free);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_artist_popover_btn_clicked), d);

            gtk_box_append(GTK_BOX(box), btn);
        }

        g_signal_connect(popover, "closed", G_CALLBACK(on_artist_popover_closed), NULL);
        gtk_popover_popup(GTK_POPOVER(popover));
        return;
    }

    /* Single artist — navigate directly */
    if (track_artists && track_artists->len == 1) {
        const library_track_artist_t *primary = g_ptr_array_index(track_artists, 0);
        if (primary->artist_id > 0) {
            g_signal_emit(s, signals[SIGNAL_ARTIST_CLICKED], 0, s->channel_id, primary->artist_id);
            return;
        }
    }

    /* Fallback: album artist (standard albums where track artists aren't cached) */
    const library_track_info_t *track = library_cache_get_track(s->library, s->current_track_id);
    if (!track || track->album_id <= 0) return;
    const library_album_info_t *album = library_cache_get_album(s->library, track->album_id, LIBRARY_MASK_ALL);
    if (album && album->artist_id > 0 && !ui_is_various_artists(album->artist_name)) {
        g_signal_emit(s, signals[SIGNAL_ARTIST_CLICKED], 0, s->channel_id, album->artist_id);
    }
}

static void on_next_track_label_clicked(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);

    /* Don't navigate if in broadcast modes or no next track */
    if (s->mode == CHANNEL_MODE_QUEUED || s->mode == CHANNEL_MODE_ON_AIR)
        return;
    if (!can_go_next(s))
        return;

    /* Check if repeat is on (query engine) */
    gboolean repeat = s->pipeline ?
        audio_pipeline_player_get_repeat(s->pipeline, s->channel_id) : FALSE;
    if (repeat)
        return;

    ui_channel_strip_next_track(s);
}

static void on_prev_track_btn_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    ui_channel_strip_previous_track(s);
}

static void on_next_track_btn_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    ui_channel_strip_next_track(s);
}

/* ===============================================================================
 * GObject Implementation
 * =============================================================================== */

static void remove_marquee_tick_callback(GtkWidget *scroll) {
    if (!scroll) return;
    guint tick_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(scroll), "marquee-tick-id"));
    if (tick_id > 0) {
        gtk_widget_remove_tick_callback(scroll, tick_id);
        g_object_set_data(G_OBJECT(scroll), "marquee-tick-id", NULL);
    }
}

static void ui_channel_strip_dispose(GObject *obj) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(obj);

    /* Remove marquee tick callbacks */
    GtkWidget *marquee_scrolls[] = {
        s->title_scroll, s->artist_scroll, s->album_scroll, s->next_track_scroll
    };
    for (size_t i = 0; i < G_N_ELEMENTS(marquee_scrolls); i++)
        remove_marquee_tick_callback(marquee_scrolls[i]);

    /* Disconnect gesture signal handlers before widget destruction */
    if (s->channel_click_gesture) {
        g_signal_handlers_disconnect_by_data(s->channel_click_gesture, s);
        s->channel_click_gesture = NULL;
    }
    if (s->shuttle_click_gesture) {
        g_signal_handlers_disconnect_by_data(s->shuttle_click_gesture, s);
        s->shuttle_click_gesture = NULL;
    }
    if (s->shuttle_drag_gesture) {
        g_signal_handlers_disconnect_by_data(s->shuttle_drag_gesture, s);
        s->shuttle_drag_gesture = NULL;
    }
    if (s->album_click_gesture) {
        g_signal_handlers_disconnect_by_data(s->album_click_gesture, s);
        s->album_click_gesture = NULL;
    }
    if (s->artist_click_gesture) {
        g_signal_handlers_disconnect_by_data(s->artist_click_gesture, s);
        s->artist_click_gesture = NULL;
    }
    if (s->next_track_click_gesture) {
        g_signal_handlers_disconnect_by_data(s->next_track_click_gesture, s);
        s->next_track_click_gesture = NULL;
    }

    g_clear_pointer(&s->filepath, g_free);
    g_clear_pointer(&s->title, g_free);
    g_clear_pointer(&s->artist, g_free);
    g_clear_pointer(&s->album, g_free);
    g_clear_pointer(&s->device_name, g_free);
    g_clear_pointer(&s->main_box, gtk_widget_unparent);

    G_OBJECT_CLASS(ui_channel_strip_parent_class)->dispose(obj);
}

static void ui_channel_strip_class_init(UiChannelStripClass *klass) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

    oc->dispose = ui_channel_strip_dispose;
    gtk_widget_class_set_layout_manager_type(wc, GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(wc, "channel-strip");

    /* Signals */
    signals[SIGNAL_CLICKED] = g_signal_new("clicked",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_INT);

    signals[SIGNAL_MODE_CHANGED] = g_signal_new("mode-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);  /* channel_id, new_mode */

    signals[SIGNAL_ALBUM_CLICKED] = g_signal_new("album-clicked",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT64);  /* channel_id, album_id */

    signals[SIGNAL_ARTIST_CLICKED] = g_signal_new("artist-clicked",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT64);  /* channel_id, artist_id */

    signals[SIGNAL_TRACK_CHANGED] = g_signal_new("track-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT64);  /* channel_id, track_id */

    /* Ensure custom child types are registered before template parsing */
    g_type_ensure(UI_TYPE_WAVEFORM_SEEK_BAR);

    /* Load template from resource */
    gtk_widget_class_set_template_from_resource(wc, "/org/quadrature/ui/channel_strip.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, main_box);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, display_panel);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, title_scroll);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, title_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, artist_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, time_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, play_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, stop_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, repeat_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, autoplay_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, preview_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, queue_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, right_column);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, info_box);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, seek_row);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, waveform);

    /* Bind album context template children */
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, album_scroll);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, album_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, artist_scroll);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, track_position_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, next_track_scroll);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, next_track_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, prev_track_btn);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, next_track_btn);

    /* Bind metrics column template children */
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, metrics_column);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, time_elapsed_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, next_track_duration_label);

    /* Bind skip/shuttle template children */
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, skip_back_15);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, skip_back_5);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, skip_fwd_5);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, skip_fwd_15);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, shuttle_scale);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, shuttle_label);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, shuttle_mode_btn);

    /* Bind template callbacks */
    gtk_widget_class_bind_template_callback(wc, on_play_pause);
    gtk_widget_class_bind_template_callback(wc, on_stop);
    gtk_widget_class_bind_template_callback(wc, on_repeat);
    gtk_widget_class_bind_template_callback(wc, on_autoplay);
    gtk_widget_class_bind_template_callback(wc, on_preview);
    gtk_widget_class_bind_template_callback(wc, on_queue);

    /* Bind skip/shuttle callbacks */
    gtk_widget_class_bind_template_callback(wc, on_skip_clicked);
    gtk_widget_class_bind_template_callback(wc, on_shuttle_value_changed);
    gtk_widget_class_bind_template_callback(wc, on_shuttle_mode_clicked);
}

/** Create a click gesture on a label and store the gesture pointer. */
static void setup_label_click(GtkWidget *label, GtkGesture **out_gesture,
                               GCallback callback, gpointer data) {
    if (!label) return;
    *out_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(*out_gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect(*out_gesture, "released", callback, data);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(*out_gesture));
}

static void ui_channel_strip_init(UiChannelStrip *s) {
    s->channel_id = 0;
    s->pipeline = NULL;
    s->show_spectrum = TRUE;
    s->time_state = TIME_STATE_NONE;
    s->sample_rate = 0;
    s->length_samples = 0;
    s->ui_length_samples = 0;
    s->deferred_seek_pos = 0;
    s->has_deferred_seek = FALSE;
    s->spectrum = NULL;
    s->status_scroll = NULL;
    s->status_label = NULL;
    s->device_name = NULL;

    /* New layered state */
    s->device_state = DEVICE_STATE_VALID;
    s->mode = CHANNEL_MODE_IDLE;
    s->focused = FALSE;
    s->last_queue_click_time = 0;

    /* Track context init */
    s->current_track_id = 0;
    s->prev_player_state = CHANNEL_STOPPED;

    /* Gesture init */
    s->album_click_gesture = NULL;
    s->artist_click_gesture = NULL;
    s->next_track_click_gesture = NULL;

    gtk_widget_init_template(GTK_WIDGET(s));

    /* Waveform seek bar is now a template child — just connect the seek signal */
    s->waveform_track_id = 0;
    g_signal_connect(s->waveform, "seek", G_CALLBACK(on_seek), s);

    /* Click gesture scoped to the display panel — clicks on interactive children
     * (album/artist/next-track labels) claim the sequence in bubble phase; background
     * clicks inside the LCD panel bubble up here to toggle focus. Clicks on transport
     * buttons, seek bar, or the gap between the panel and those widgets do NOT toggle. */
    s->channel_click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->channel_click_gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect(s->channel_click_gesture, "released", G_CALLBACK(on_channel_clicked), s);
    gtk_widget_add_controller(s->display_panel, GTK_EVENT_CONTROLLER(s->channel_click_gesture));

    /* Shuttle scale gestures:
     * 1. Click gesture in CAPTURE phase - handles right-click reset only
     * 2. Drag gesture in CAPTURE phase - handles left-click dragging, blocks click-to-jump
     * Both gestures are grouped so they can cooperate on event sequences */
    s->shuttle_click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->shuttle_click_gesture), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(s->shuttle_click_gesture), GTK_PHASE_CAPTURE);
    g_signal_connect(s->shuttle_click_gesture, "pressed", G_CALLBACK(on_shuttle_pressed), s);
    gtk_widget_add_controller(s->shuttle_scale, GTK_EVENT_CONTROLLER(s->shuttle_click_gesture));

    s->shuttle_drag_gesture = GTK_GESTURE(gtk_gesture_drag_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->shuttle_drag_gesture), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(s->shuttle_drag_gesture), GTK_PHASE_CAPTURE);
    g_signal_connect(s->shuttle_drag_gesture, "drag-begin", G_CALLBACK(on_shuttle_drag_begin), s);
    g_signal_connect(s->shuttle_drag_gesture, "drag-update", G_CALLBACK(on_shuttle_drag_update), s);
    gtk_widget_add_controller(s->shuttle_scale, GTK_EVENT_CONTROLLER(s->shuttle_drag_gesture));

    /* Group gestures so they can share sequences without cancelling each other */
    gtk_gesture_group(s->shuttle_click_gesture, s->shuttle_drag_gesture);

    /* Initialize shuttle in OFF mode - slider disabled by default */
    s->shuttle_mode = SHUTTLE_MODE_OFF;
    gtk_widget_set_sensitive(s->shuttle_scale, FALSE);
    update_shuttle_mode_button(s);

    /* Create status label for error states (added dynamically, shown when needed) */
    /* Wrap in scrolled window for long error messages */
    s->status_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->status_scroll),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_widget_set_hexpand(s->status_scroll, TRUE);
    gtk_widget_set_vexpand(s->status_scroll, TRUE);
    gtk_widget_set_visible(s->status_scroll, FALSE);

    s->status_label = gtk_label_new("");
    gtk_widget_add_css_class(s->status_label, "channel-status");
    gtk_label_set_xalign(GTK_LABEL(s->status_label), 0.0);
    gtk_widget_set_valign(s->status_label, GTK_ALIGN_CENTER);
    gtk_label_set_single_line_mode(GTK_LABEL(s->status_label), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s->status_scroll), s->status_label);

    /* Album/Artist/Next track label click gestures */
    setup_label_click(s->album_label, &s->album_click_gesture, G_CALLBACK(on_album_clicked), s);
    setup_label_click(s->artist_label, &s->artist_click_gesture, G_CALLBACK(on_artist_clicked), s);
    setup_label_click(s->next_track_label, &s->next_track_click_gesture, G_CALLBACK(on_next_track_label_clicked), s);
}

/* ===============================================================================
 * Public API
 * =============================================================================== */

GtkWidget *ui_channel_strip_new(int channel_id, audio_pipeline_t *pipeline, library_cache_t *library) {
    UiChannelStrip *s = g_object_new(UI_TYPE_CHANNEL_STRIP, NULL);
    s->channel_id = channel_id;
    s->pipeline = pipeline;
    s->library = library;

    /* Set skip amounts on skip buttons */
    g_object_set_data(G_OBJECT(s->skip_back_15), "skip-seconds", GINT_TO_POINTER(-15));
    g_object_set_data(G_OBJECT(s->skip_back_5), "skip-seconds", GINT_TO_POINTER(-5));
    g_object_set_data(G_OBJECT(s->skip_fwd_5), "skip-seconds", GINT_TO_POINTER(5));
    g_object_set_data(G_OBJECT(s->skip_fwd_15), "skip-seconds", GINT_TO_POINTER(15));

    /* Connect track navigation buttons */
    if (s->prev_track_btn) {
        g_signal_connect(s->prev_track_btn, "clicked", G_CALLBACK(on_prev_track_btn_clicked), s);
    }
    if (s->next_track_btn) {
        g_signal_connect(s->next_track_btn, "clicked", G_CALLBACK(on_next_track_btn_clicked), s);
    }

    /* Add status scroll (containing status_label) to display panel (sibling of info_box) */
    if (s->display_panel && s->status_scroll) {
        gtk_box_prepend(GTK_BOX(s->display_panel), s->status_scroll);
    }

    /* Add spectrum if enabled (full height) */
    if (s->show_spectrum) {
        s->spectrum = ui_spectrum_new(SPECTRUM_BARS);
        gtk_widget_add_css_class(s->spectrum, "spectrum-display");
        gtk_widget_set_vexpand(s->spectrum, TRUE);
        gtk_box_append(GTK_BOX(s->right_column), s->spectrum);
    }

    /* Marquee tick callbacks are installed on-demand by marquee_update_loop_point()
     * when text actually overflows the container — no eagerly running callbacks. */

    return GTK_WIDGET(s);
}

/** Update a label only if the text changed. Avoids Pango relayout per frame. */
static inline void update_cached_label(GtkWidget *label, char *cache, size_t cache_size,
                                        const char *text) {
    if (!label) return;
    if (strcmp(text, cache) != 0) {
        memcpy(cache, text, MIN(strlen(text) + 1, cache_size));
        gtk_label_set_text(GTK_LABEL(label), text);
    }
}

void ui_channel_strip_update(UiChannelStrip *s, audio_pipeline_t *pipeline) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    g_assert(pipeline != NULL);  /* Caller must provide valid pipeline */

    channel_state_t st = audio_pipeline_get_player_state(pipeline, s->channel_id);

    /* Fast path: no track loaded and stopped — skip per-frame work for idle channels */
    if (!s->filepath && st == CHANNEL_STOPPED && !s->has_deferred_seek) {
        if (st != s->prev_player_state) {
            update_button_sensitivity(s, st);
            s->prev_player_state = st;
        }
        return;
    }

    uint64_t len = audio_pipeline_get_player_length(pipeline, s->channel_id);
    uint64_t effective_len = (len > 0) ? len : s->ui_length_samples;

    /* Apply deferred seek when real buffer arrives */
    if (len > 0 && s->has_deferred_seek && s->ui_length_samples > 0) {
        double ratio = (double)s->deferred_seek_pos / (double)s->ui_length_samples;
        uint64_t actual_pos = (uint64_t)(ratio * (double)len);
        if (actual_pos >= len) actual_pos = len - 1;
        audio_pipeline_player_seek(s->pipeline, s->channel_id, actual_pos);
        s->has_deferred_seek = FALSE;
    }
    if (len > 0) s->ui_length_samples = 0;

    s->sample_rate = audio_pipeline_get_sample_rate(pipeline);
    s->length_samples = len;

    /* Play state - update icon every frame (GTK4 is idempotent) */
    gboolean playing = (st == CHANNEL_PLAYING);
    update_play_icon(s, playing);

    /* Get interpolated position for smooth display */
    float speed = 1.0f;
    double display_pos_d = audio_pipeline_get_player_position_smooth(pipeline, s->channel_id, &speed);

    /* Override with seek bar position if dragging */
    gboolean seek_dragging = s->waveform &&
        ui_waveform_seek_bar_is_dragging(UI_WAVEFORM_SEEK_BAR(s->waveform));
    if (seek_dragging && effective_len > 0) {
        GtkAdjustment *adj = ui_waveform_seek_bar_get_adjustment(
            UI_WAVEFORM_SEEK_BAR(s->waveform));
        double val = gtk_adjustment_get_value(adj);
        display_pos_d = val * (double)effective_len;
    }

    if (s->sample_rate > 0 && effective_len > 0) {
        /* Remaining time - floating point throughout */
        double rem_samples = (double)effective_len - display_pos_d;
        if (rem_samples < 0.0) rem_samples = 0.0;

        float abs_speed = fabsf(speed);
        if (abs_speed < 0.01f) abs_speed = 1.0f;

        double rem_sec_d = rem_samples / (double)s->sample_rate / (double)abs_speed;
        unsigned long total_rem_min = (unsigned long)(rem_sec_d / 60.0);
        unsigned long rem_sec = (unsigned long)fmod(rem_sec_d, 60.0);
        unsigned long rem_cs = (unsigned long)(fmod(rem_sec_d, 1.0) * 100.0);

        char buf[20];
        snprintf(buf, sizeof(buf), "-%lu:%02lu.%02lu", total_rem_min, rem_sec, rem_cs);
        update_cached_label(s->time_label, s->cached_time_remaining,
                            sizeof(s->cached_time_remaining), buf);

        /* Time color state (using floating-point seconds for thresholds) */
        time_state_t new_state = TIME_STATE_NONE;
        if (!seek_dragging && rem_sec_d > 0) {
            if (rem_sec_d <= TIME_WARN_SEC) {
                new_state = TIME_STATE_WARNING;
            } else if (rem_sec_d <= TIME_CAUTION_SEC) {
                new_state = TIME_STATE_CAUTION;
            } else {
                new_state = TIME_STATE_SAFE;
            }
        }
        if (new_state != s->time_state) {
            /* Remove old state class */
            ui_toggle_css(s->time_label, "time-safe", FALSE);
            ui_toggle_css(s->time_label, "time-caution", FALSE);
            ui_toggle_css(s->time_label, "time-warning", FALSE);
            /* Apply new state class */
            switch (new_state) {
                case TIME_STATE_SAFE:
                    ui_toggle_css(s->time_label, "time-safe", TRUE);
                    break;
                case TIME_STATE_CAUTION:
                    ui_toggle_css(s->time_label, "time-caution", TRUE);
                    break;
                case TIME_STATE_WARNING:
                    ui_toggle_css(s->time_label, "time-warning", TRUE);
                    break;
                case TIME_STATE_NONE:
                default:
                    break;
            }
            s->time_state = new_state;
        }

        /* Elapsed time - floating point with centiseconds */
        double elapsed_sec_d = display_pos_d / (double)s->sample_rate;
        unsigned long total_elapsed_min = (unsigned long)(elapsed_sec_d / 60.0);
        unsigned long elapsed_sec = (unsigned long)fmod(elapsed_sec_d, 60.0);
        unsigned long elapsed_cs = (unsigned long)(fmod(elapsed_sec_d, 1.0) * 100.0);

        snprintf(buf, sizeof(buf), "%lu:%02lu.%02lu", total_elapsed_min, elapsed_sec, elapsed_cs);
        update_cached_label(s->time_elapsed_label, s->cached_time_elapsed,
                            sizeof(s->cached_time_elapsed), buf);

    } else {
        update_cached_label(s->time_label, s->cached_time_remaining,
                            sizeof(s->cached_time_remaining), "-0:00.00");
        update_cached_label(s->time_elapsed_label, s->cached_time_elapsed,
                            sizeof(s->cached_time_elapsed), "0:00.00");
    }

    /* Seek bar - use interpolated position when not dragging */
    double seek_frac = 0.0;
    if (effective_len > 0) {
        seek_frac = display_pos_d / (double)effective_len;
        if (s->waveform) {
            UiWaveformSeekBar *wsb = UI_WAVEFORM_SEEK_BAR(s->waveform);
            /* Always update playback position (dim trail during drag) */
            ui_waveform_seek_bar_set_playback_position(wsb, seek_frac);
            /* Only move the adjustment (playhead) when not dragging */
            if (!seek_dragging) {
                GtkAdjustment *adj = ui_waveform_seek_bar_get_adjustment(wsb);
                gtk_adjustment_set_value(adj, seek_frac);
            }
        }
    }

    /* Waveform seek bar — redraw every frame while playing or animating
     * so bar colors track the playhead position. */
    if (s->waveform) {
        /* Check if loudness data became available for current track.
         * Read the player's buffer pointer directly — it's locked while playing,
         * and loudness data is immutable once loudness_ready is set. */
        if (s->current_track_id > 0 &&
            s->current_track_id != s->waveform_track_id &&
            s->pipeline) {
            audio_buffer_t *buf = atomic_load(&s->pipeline->players[s->channel_id].buffer);
            if (buf && audio_buffer_get_track_id(buf) == s->current_track_id &&
                audio_buffer_is_loudness_ready(buf)) {
                const float *loudness = audio_buffer_get_loudness(buf);
                if (loudness) {
                    ui_waveform_seek_bar_set_loudness(
                        UI_WAVEFORM_SEEK_BAR(s->waveform), loudness, LOUDNESS_BINS);
                    s->waveform_track_id = s->current_track_id;
                }
            }
        }

        /* Redraw waveform every frame during playback (bar colors must track
         * the slider position) and during grow-out animation. */
        if (playing || seek_dragging ||
            ui_waveform_seek_bar_is_animating(UI_WAVEFORM_SEEK_BAR(s->waveform))) {
            gtk_widget_queue_draw(s->waveform);
        }
    }

    /* Table-driven button sensitivity — only on player state transitions.
     * Mode changes and file loads call update_button_sensitivity separately. */
    if (st != s->prev_player_state) {
        update_button_sensitivity(s, st);
        s->prev_player_state = st;
    }

    /* Spectrum (stereo: left channel on left, right channel on right).
     * While playing, keep the decay counter pegged at max. When stopped/paused,
     * count down so bars animate to zero via cava's release curve, then go idle.
     * Generation counter skips redundant copies when display runs faster than cavacore. */
    if (s->spectrum) {
        if (playing) {
            s->spectrum_decay_frames = SPECTRUM_DECAY_FRAMES;
        }
        if (s->spectrum_decay_frames > 0) {
            uint32_t gen = atomic_load(&pipeline->players[s->channel_id].spectrum_generation);
            if (gen != s->last_spectrum_gen || !playing) {
                float left[SPECTRUM_BARS], right[SPECTRUM_BARS];
                audio_pipeline_get_player_spectrum(pipeline, s->channel_id, left, right, SPECTRUM_BARS);
                ui_spectrum_set_bars(UI_SPECTRUM(s->spectrum), left, right, SPECTRUM_BARS);
                s->last_spectrum_gen = gen;
            }
            if (!playing) {
                s->spectrum_decay_frames--;
            }
        }
    }
}

void ui_channel_strip_set_spectrum_visible(UiChannelStrip *s, gboolean visible) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    if (visible == s->show_spectrum) return;

    s->show_spectrum = visible;

    if (visible && !s->spectrum && s->right_column) {
        s->spectrum = ui_spectrum_new(SPECTRUM_BARS);
        gtk_widget_add_css_class(s->spectrum, "spectrum-display");
        gtk_widget_set_vexpand(s->spectrum, TRUE);
        gtk_box_append(GTK_BOX(s->right_column), s->spectrum);
    } else if (!visible && s->spectrum && s->right_column) {
        gtk_box_remove(GTK_BOX(s->right_column), s->spectrum);
        s->spectrum = NULL;
    }
}

quadrature_result_t ui_channel_strip_load_track(UiChannelStrip *s,
                                                 const PlaybackIntent *intent) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), QUADRATURE_ERROR_INVALID_PARAM);
    g_return_val_if_fail(intent && intent->track_id > 0, QUADRATURE_ERROR_INVALID_PARAM);

    /* Update UI metadata FIRST (instant feedback) */
    s->current_track_id = intent->track_id;
    g_free(s->filepath); s->filepath = g_strdup(intent->path);
    g_free(s->title);    s->title = g_strdup(intent->title);
    g_free(s->artist);   s->artist = g_strdup(intent->artist);
    g_free(s->album);    s->album = g_strdup(intent->album);
    update_display(s);
    update_button_sensitivity(s, s->prev_player_state);

    /* Clear waveform for new track (will re-animate when loudness arrives) */
    if (s->waveform)
        ui_waveform_seek_bar_clear(UI_WAVEFORM_SEEK_BAR(s->waveform));
    s->waveform_track_id = 0;

    /* Estimate duration from cache for instant seek bar (before decode completes) */
    s->has_deferred_seek = FALSE;
    if (s->library) {
        const library_track_info_t *ti = library_cache_get_track(s->library, intent->track_id);
        uint32_t rate = s->pipeline ? audio_pipeline_get_sample_rate(s->pipeline) : 0;
        s->ui_length_samples = (rate > 0 && ti && ti->duration_ms > 0)
            ? ((uint64_t)ti->duration_ms * rate) / 1000 : 0;
    }

    if (s->pipeline && s->pipeline->cache) {
        /* Load track into cache FIRST (non-blocking, starts decode) */
        quadrature_result_t res = audio_cache_load(s->pipeline->cache, intent->track_id);
        if (res != QUADRATURE_OK) return res;

        /* Set player track — may fail if no active device yet. That's OK:
         * on_play_pause will retry set_player_track when the user presses play. */
        res = audio_pipeline_set_player_track(s->pipeline, s->channel_id, intent->track_id);
        if (res == QUADRATURE_OK) {
            s->length_samples = audio_pipeline_get_player_length(s->pipeline, s->channel_id);
            s->sample_rate = audio_pipeline_get_sample_rate(s->pipeline);
        } else {
            g_debug("load_track: set_player_track deferred (channel %d, result=%d)",
                    s->channel_id, res);
        }
    }

    return QUADRATURE_OK;
}

void ui_channel_strip_update_track_display(UiChannelStrip *s,
                                            const PlaybackIntent *intent) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    g_return_if_fail(intent != NULL);

    s->current_track_id = intent->track_id;
    g_free(s->filepath); s->filepath = g_strdup(intent->path);
    g_free(s->title);    s->title = g_strdup(intent->title);
    g_free(s->artist);   s->artist = g_strdup(intent->artist);
    g_free(s->album);    s->album = g_strdup(intent->album);

    if (s->pipeline) {
        s->length_samples = audio_pipeline_get_player_length(s->pipeline, s->channel_id);
        s->sample_rate = audio_pipeline_get_sample_rate(s->pipeline);
    }

    update_display(s);
}

void ui_channel_strip_set_device_name(UiChannelStrip *s, const char *device_name) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    g_free(s->device_name);
    s->device_name = device_name ? g_strdup(device_name) : NULL;
    /* Update status display if currently in invalid state */
    if (s->device_state == DEVICE_STATE_INVALID)
        update_visual_state(s);
}

int ui_channel_strip_get_channel_id(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), -1);
    return s->channel_id;
}

gboolean ui_channel_strip_has_track(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    return s->filepath != NULL;
}

/* ===============================================================================
 * New Layered State API
 * =============================================================================== */

void ui_channel_strip_set_device_state(UiChannelStrip *s, DeviceState state) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    if (s->device_state == state) return;
    s->device_state = state;
    update_visual_state(s);
}

DeviceState ui_channel_strip_get_device_state(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), DEVICE_STATE_INVALID);
    return s->device_state;
}

void ui_channel_strip_set_mode(UiChannelStrip *s, ChannelMode mode) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    if (s->mode == mode) return;

    s->mode = mode;

    /* Handle mode entry actions */
    if (mode == CHANNEL_MODE_QUEUED) {
        /* Note: Do NOT seek to 0 here - preserve current playback position
         * so queued tracks resume from where they were paused */

        /* Note: Preview audio cleanup is handled by preview_off() function
         * (Button state is managed by preview_on/preview_off functions) */
    }

    /* Note: Preview button state is NOT synced here - that's handled by
     * ui_channel_strip_preview_on/off public API functions to avoid signal loops */

    update_visual_state(s);

    /* Immediately update all button sensitivity for the new mode:
     * - Table-driven buttons (skip, seek, shuttle, play, stop, etc.)
     * - prev/next track buttons (library-dependent, handled by update_album_display) */
    update_button_sensitivity(s, s->prev_player_state);
    update_album_display(s);

    /* Emit mode-changed signal so window can respond (e.g., clear focus) */
    g_signal_emit(s, signals[SIGNAL_MODE_CHANGED], 0, s->channel_id, (int)mode);
}

ChannelMode ui_channel_strip_get_mode(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), CHANNEL_MODE_IDLE);
    return s->mode;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Preview/PFL Control (for GPIO integration)
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_channel_strip_preview_on(UiChannelStrip *s) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    
    /* Cannot enable preview if QUEUED or ON_AIR */
    if (s->mode == CHANNEL_MODE_QUEUED || s->mode == CHANNEL_MODE_ON_AIR) {
        /* Ensure button is off if we're blocked */
        g_signal_handlers_block_by_func(s->preview_btn, on_preview, s);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), FALSE);
        g_signal_handlers_unblock_by_func(s->preview_btn, on_preview, s);
        return;
    }
    
    /* Already in preview mode, just sync button */
    if (s->mode == CHANNEL_MODE_PREVIEW) {
        g_signal_handlers_block_by_func(s->preview_btn, on_preview, s);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), TRUE);
        g_signal_handlers_unblock_by_func(s->preview_btn, on_preview, s);
        return;
    }
    
    /* Block signal to prevent recursion */
    g_signal_handlers_block_by_func(s->preview_btn, on_preview, s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), TRUE);
    g_signal_handlers_unblock_by_func(s->preview_btn, on_preview, s);
    
    ui_channel_strip_set_mode(s, CHANNEL_MODE_PREVIEW);
}

void ui_channel_strip_preview_off(UiChannelStrip *s) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    
    /* Only transition to IDLE if we're actually in PREVIEW mode */
    if (s->mode != CHANNEL_MODE_PREVIEW) {
        /* Still sync button state in case we're out of sync */
        g_signal_handlers_block_by_func(s->preview_btn, on_preview, s);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), FALSE);
        g_signal_handlers_unblock_by_func(s->preview_btn, on_preview, s);
        return;
    }
    
    /* Block signal to prevent recursion */
    g_signal_handlers_block_by_func(s->preview_btn, on_preview, s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), FALSE);
    g_signal_handlers_unblock_by_func(s->preview_btn, on_preview, s);
    
    ui_channel_strip_set_mode(s, CHANNEL_MODE_IDLE);
}

bool ui_channel_strip_get_preview_active(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), false);
    return s->mode == CHANNEL_MODE_PREVIEW;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API - Playback Control (for GPIO integration)
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_channel_strip_play(UiChannelStrip *s) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    
    if (!s->pipeline) return;
    
    channel_state_t state = audio_pipeline_get_player_state(s->pipeline, s->channel_id);
    
    /* If already playing, do nothing */
    if (state == CHANNEL_PLAYING) return;
    
    /* If in QUEUED mode, transition to ON_AIR */
    if (s->mode == CHANNEL_MODE_QUEUED) {
        ui_channel_strip_set_mode(s, CHANNEL_MODE_ON_AIR);
    }
    
    /* Start playback (handles both stopped and paused states) */
    audio_pipeline_player_play(s->pipeline, s->channel_id);
}

void ui_channel_strip_stop(UiChannelStrip *s) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    
    if (!s->pipeline) return;
    
    channel_state_t state = audio_pipeline_get_player_state(s->pipeline, s->channel_id);
    
    /* If already stopped, do nothing */
    if (state == CHANNEL_STOPPED) return;
    
    /* Stop playback */
    audio_pipeline_player_stop(s->pipeline, s->channel_id);
    
    /* If in ON_AIR or QUEUED mode, return to IDLE */
    if (s->mode == CHANNEL_MODE_ON_AIR || s->mode == CHANNEL_MODE_QUEUED) {
        ui_channel_strip_set_mode(s, CHANNEL_MODE_IDLE);
    }
}

channel_state_t ui_channel_strip_get_player_state(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), CHANNEL_STOPPED);
    
    if (!s->pipeline) return CHANNEL_STOPPED;
    
    return audio_pipeline_get_player_state(s->pipeline, s->channel_id);
}

void ui_channel_strip_set_focused(UiChannelStrip *s, gboolean focused) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    if (s->focused == focused) return;

    s->focused = focused;
    update_visual_state(s);
}

gboolean ui_channel_strip_get_focused(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    return s->focused;
}

gboolean ui_channel_strip_is_active(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    return s->device_state == DEVICE_STATE_VALID
        && s->mode != CHANNEL_MODE_QUEUED
        && s->mode != CHANNEL_MODE_ON_AIR;
}

/* ===============================================================================
 * Track Context API
 * =============================================================================== */

int64_t ui_channel_strip_get_current_track_id(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), 0);
    return s->current_track_id;
}

/* ===============================================================================
 * Track Navigation API
 *
 * Handles prev/next logic in UI layer for instant feedback:
 * 1. Get target track ID from library cache
 * 2. Update UI immediately (title, artist, etc.)
 * 3. Load track into audio cache (async)
 * 4. Set player track (non-blocking)
 * =============================================================================== */

gboolean ui_channel_strip_previous_track(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    if (!s->pipeline || !s->library) return FALSE;
    if (s->current_track_id <= 0) return FALSE;

    /* If more than 3 seconds into the track, restart current track instead */
    uint64_t pos = audio_pipeline_get_player_position(s->pipeline, s->channel_id);
    uint32_t rate = audio_pipeline_get_sample_rate(s->pipeline);
    if (rate > 0 && pos > rate * 3) {
        audio_pipeline_player_seek(s->pipeline, s->channel_id, 0);
        return TRUE;
    }

    /* Get previous track ID */
    int64_t prev_id = library_cache_get_prev_track_id(s->library, s->current_track_id);
    if (prev_id <= 0) {
        /* At start of album, restart current track */
        audio_pipeline_player_seek(s->pipeline, s->channel_id, 0);
        return TRUE;
    }

    /* Get track info and update UI immediately */
    const library_track_info_t *track = library_cache_get_track(s->library, prev_id);
    if (!track) return FALSE;

    s->current_track_id = prev_id;
    g_free(s->filepath); s->filepath = library_cache_resolve_track_path(s->library, prev_id);
    g_free(s->title);    s->title = g_strdup(track->title);
    g_free(s->artist);   s->artist = g_strdup(track->artist_display);
    g_free(s->album);    s->album = g_strdup(track->album_title);
    update_display(s);

    /* Clear waveform immediately so stale data isn't visible during decode */
    if (s->waveform)
        ui_waveform_seek_bar_clear(UI_WAVEFORM_SEEK_BAR(s->waveform));
    s->waveform_track_id = 0;

    /* Estimate duration from cache for instant seek bar (reuse rate from above) */
    s->ui_length_samples = (rate > 0 && track->duration_ms > 0)
        ? ((uint64_t)track->duration_ms * rate) / 1000 : 0;
    s->has_deferred_seek = FALSE;

    /* Load and set track (non-blocking) */
    if (s->pipeline->cache) {
        audio_cache_load(s->pipeline->cache, prev_id);
        audio_pipeline_set_player_track(s->pipeline, s->channel_id, prev_id);
        s->length_samples = audio_pipeline_get_player_length(s->pipeline, s->channel_id);
        s->sample_rate = audio_pipeline_get_sample_rate(s->pipeline);
    }

    return TRUE;
}

gboolean ui_channel_strip_next_track(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    if (!s->pipeline || !s->library) return FALSE;
    if (s->current_track_id <= 0) return FALSE;

    /* Check repeat mode */
    gboolean repeat = audio_pipeline_player_get_repeat(s->pipeline, s->channel_id);

    /* Get next track ID */
    int64_t next_id = library_cache_get_next_track_id(s->library, s->current_track_id);
    if (next_id <= 0) {
        /* At end of album */
        if (repeat) {
            /* In repeat mode, restart current track */
            audio_pipeline_player_seek(s->pipeline, s->channel_id, 0);
            return TRUE;
        }
        return FALSE;  /* No next track and not repeating */
    }

    /* Get track info and update UI immediately */
    const library_track_info_t *track = library_cache_get_track(s->library, next_id);
    if (!track) return FALSE;

    s->current_track_id = next_id;
    g_free(s->filepath); s->filepath = library_cache_resolve_track_path(s->library, next_id);
    g_free(s->title);    s->title = g_strdup(track->title);
    g_free(s->artist);   s->artist = g_strdup(track->artist_display);
    g_free(s->album);    s->album = g_strdup(track->album_title);
    update_display(s);

    /* Clear waveform immediately so stale data isn't visible during decode */
    if (s->waveform)
        ui_waveform_seek_bar_clear(UI_WAVEFORM_SEEK_BAR(s->waveform));
    s->waveform_track_id = 0;

    /* Estimate duration from cache for instant seek bar */
    uint32_t rate = audio_pipeline_get_sample_rate(s->pipeline);
    s->ui_length_samples = (rate > 0 && track->duration_ms > 0)
        ? ((uint64_t)track->duration_ms * rate) / 1000 : 0;
    s->has_deferred_seek = FALSE;

    /* Load and set track (non-blocking) */
    if (s->pipeline->cache) {
        audio_cache_load(s->pipeline->cache, next_id);
        audio_pipeline_set_player_track(s->pipeline, s->channel_id, next_id);
        s->length_samples = audio_pipeline_get_player_length(s->pipeline, s->channel_id);
        s->sample_rate = audio_pipeline_get_sample_rate(s->pipeline);
    }

    return TRUE;
}

gboolean ui_channel_strip_can_go_previous(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    return can_go_previous(s);
}

gboolean ui_channel_strip_can_go_next(UiChannelStrip *s) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), FALSE);
    return can_go_next(s);
}
