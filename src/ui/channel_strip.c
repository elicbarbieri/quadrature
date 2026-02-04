/**
 * Quadrature Channel Strip Widget
 *
 * Per-channel display: level meters, spectrum, transport controls, metadata.
 * Uses GTK4 composite template for UI structure.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include "quadrature/quadrature_library.h"
#include "../audio/internal.h"  /* For audio_cache_load() */
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MARQUEE_SPEED_PPS 50.0   /* Pixels per second */
#define MARQUEE_PAUSE_SEC 2.0    /* Pause at start before scrolling */
#define MARQUEE_SEPARATOR "   •   "  /* Separator between text copies */
#define TIME_WARN_SEC 30
#define TIME_CAUTION_SEC 90

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
    gboolean seek_dragging;  /* TRUE when user is dragging the seek bar */

    /* Layered state model */
    DeviceState device_state;    /* Hardware availability */
    ChannelMode mode;            /* Operational mode */
    gboolean focused;            /* Target for song loading */

    /* Queue double-click detection */
    gint64 last_queue_click_time;

    /* Template children */
    GtkWidget *main_box;
    GtkWidget *channel_label;
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
    GtkWidget *seek_bar;
    GtkWidget *right_column;
    GtkWidget *info_box;

    /* Seek bar time labels */
    GtkWidget *seek_row;
    GtkWidget *seek_times_row;
    GtkWidget *seek_time_start;
    GtkWidget *seek_time_end;

    /* Metrics column labels */
    GtkWidget *metrics_column;
    GtkWidget *time_elapsed_label;
    GtkWidget *next_track_duration_label;

    /* Dynamic widgets (not in template) */
    GtkWidget *spectrum;
    GtkWidget *status_scroll;  /* Scrolled window for status_label */
    GtkWidget *status_label;

    /* Gesture controllers (must be stored for proper cleanup) */
    GtkGesture *seek_gesture;
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
    /* seek_bar: file + device + disabled in both modes */
    SENS_RULE(seek_bar,     SENS_NEEDS_FILE | SENS_NEEDS_DEVICE | SENS_DISABLED_QUEUED | SENS_DISABLED_ON_AIR),
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
static double marquee_setup_label(GtkWidget *scroll, GtkWidget *label, const char *text) {
    if (!scroll || !label || !text || !text[0]) {
        if (label) gtk_label_set_text(GTK_LABEL(label), text ? text : "");
        return 0;
    }

    /* First set the original text to measure it */
    gtk_label_set_text(GTK_LABEL(label), text);

    /* Force layout calculation */
    gtk_widget_measure(label, GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);

    int text_width = measure_text_width(GTK_LABEL(label), text);
    int container_width = gtk_widget_get_width(scroll);

    /* If text fits, no duplication needed */
    if (container_width <= 0 || text_width <= container_width) {
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
}

static void setup_marquee_for_scroll(GtkWidget *scroll) {
    if (!scroll) return;

    /* Remove existing tick callback if any */
    guint tick_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(scroll), "marquee-tick-id"));
    if (tick_id > 0) {
        gtk_widget_remove_tick_callback(scroll, tick_id);
    }

    /* Add new tick callback */
    tick_id = gtk_widget_add_tick_callback(scroll, marquee_tick_callback, scroll, NULL);
    g_object_set_data(G_OBJECT(scroll), "marquee-tick-id", GUINT_TO_POINTER(tick_id));
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
        ui_channel_strip_set_mode(s, CHANNEL_MODE_ON_AIR);
        quadrature_result_t res = audio_pipeline_player_play(s->pipeline, s->channel_id);
        if (res == QUADRATURE_OK) {
            const char *title = gtk_label_get_text(GTK_LABEL(s->title_label));
            const char *artist = gtk_label_get_text(GTK_LABEL(s->artist_label));
            g_info("Playback → Channel %d: Start playback (QUEUED → ON_AIR) - '%s' by %s",
                   s->channel_id + 1,
                   title && *title ? title : "<no track>",
                   artist && *artist ? artist : "<unknown>");
        } else {
            g_warning("Playback FAILED → Channel %d: result=%d", s->channel_id + 1, res);
        }
        return;
    }

    /* In ON_AIR mode, play/pause is disabled (handled by button sensitivity) */
    if (s->mode == CHANNEL_MODE_ON_AIR) {
        g_debug("on_play_pause: ignored, mode is ON_AIR");
        return;
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

    /* Stop is disabled in QUEUED and ON_AIR modes */
    if (s->mode == CHANNEL_MODE_QUEUED || s->mode == CHANNEL_MODE_ON_AIR) return;

    if (s->pipeline) {
        const char *title = gtk_label_get_text(GTK_LABEL(s->title_label));
        g_info("Playback → Channel %d: Stop - '%s'", s->channel_id + 1,
               title && *title ? title : "<no track>");
        audio_pipeline_player_stop(s->pipeline, s->channel_id);
    }
}

static void update_visual_state(UiChannelStrip *s);  /* Forward declaration */
static void update_album_display(UiChannelStrip *s); /* Forward declaration */

static void on_preview(GtkToggleButton *btn, gpointer data) {
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    gboolean on = gtk_toggle_button_get_active(btn);

    /* Cannot enable preview if QUEUED or ON_AIR */
    if (on && (s->mode == CHANNEL_MODE_QUEUED || s->mode == CHANNEL_MODE_ON_AIR)) {
        gtk_toggle_button_set_active(btn, FALSE);
        return;
    }

    ui_channel_strip_set_mode(s, on ? CHANNEL_MODE_PREVIEW : CHANNEL_MODE_IDLE);
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
    case CHANNEL_MODE_IDLE:
    case CHANNEL_MODE_PREVIEW: {
        /* If already playing, go straight to ON_AIR; otherwise QUEUED */
        gboolean playing = s->pipeline &&
            (audio_pipeline_get_player_state(s->pipeline, s->channel_id) == CHANNEL_PLAYING);
        if (playing) {
            ui_channel_strip_set_mode(s, CHANNEL_MODE_ON_AIR);
        } else {
            ui_channel_strip_set_mode(s, CHANNEL_MODE_QUEUED);
        }
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

static void on_seek_drag_begin(GtkGestureDrag *g, double start_x, double start_y, gpointer data) {
    (void)g; (void)start_x; (void)start_y;
    if (!UI_IS_CHANNEL_STRIP(data)) return;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    s->seek_dragging = TRUE;
}

static void on_seek_drag_end(GtkGestureDrag *g, double offset_x, double offset_y, gpointer data) {
    (void)g; (void)offset_x; (void)offset_y;

    if (!UI_IS_CHANNEL_STRIP(data)) return;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    s->seek_dragging = FALSE;

    if (!s->seek_bar || !s->pipeline || s->length_samples == 0)
        return;

    /* Seek to final position from seek bar */
    double val = gtk_range_get_value(GTK_RANGE(s->seek_bar));
    uint64_t target_pos = (uint64_t)(val * (double)s->length_samples);
    audio_pipeline_player_seek(s->pipeline, s->channel_id, target_pos);
}

/* ===============================================================================
 * Skip/Shuttle Callbacks
 * =============================================================================== */

/* Speed range constants */
#define RUBBERBAND_SPEED_MAX  4.0f   /* Pitch-preserved mode: 0.5x-4.0x */
#define RUBBERBAND_SPEED_MIN  0.5f   /* Pitch-preserved mode min */
#define PITCHED_SPEED_MAX     1.5f   /* Pitched mode: symmetric 0.5x-1.5x */
#define PITCHED_SPEED_MIN     0.5f   /* Pitched mode min (symmetric: 1 - 0.5 = 0.5) */
#define SLIDER_MAX_RUBBERBAND 3.0f   /* Slider upper bound for rubberband mode */
#define SLIDER_MIN_RUBBERBAND -2.0f  /* Slider lower bound for rubberband mode */
#define SLIDER_MAX_PITCHED    1.0f   /* Slider upper bound for pitched mode (symmetric) */
#define SLIDER_MIN_PITCHED    -1.0f  /* Slider lower bound for pitched mode (symmetric) */

/**
 * Convert linear shuttle slider value to speed using quadratic curves.
 *
 * Mode-dependent ranges:
 *   Rubberband (default): slider -2 to +3, speed 0.5x to 4.0x
 *   Pitched (turntable):  slider -1 to +1, speed 0.5x to 1.5x (symmetric)
 *
 * Uses quadratic curves with zero derivative at center for fine control.
 */
static float shuttle_value_to_speed(double slider_value, shuttle_mode_t mode) {
    /* In OFF mode, always return 1.0x */
    if (mode == SHUTTLE_MODE_OFF) return 1.0f;

    gboolean use_pitched_range = (mode == SHUTTLE_MODE_PITCHED);

    if (slider_value >= 0.0) {
        /* Forward: 1.0x to max with quadratic curve */
        float speed_max = use_pitched_range ? PITCHED_SPEED_MAX : RUBBERBAND_SPEED_MAX;
        float slider_max = use_pitched_range ? SLIDER_MAX_PITCHED : SLIDER_MAX_RUBBERBAND;
        float range = speed_max - 1.0f;
        float normalized = (float)slider_value / slider_max;
        return 1.0f + range * normalized * normalized;
    } else {
        /* Backward: 1.0x down to min with quadratic curve */
        float speed_min = use_pitched_range ? PITCHED_SPEED_MIN : RUBBERBAND_SPEED_MIN;
        float slider_min = use_pitched_range ? SLIDER_MIN_PITCHED : SLIDER_MIN_RUBBERBAND;
        float range = 1.0f - speed_min;
        float normalized = (float)slider_value / slider_min;  /* Both negative, result positive */
        return 1.0f - range * normalized * normalized;
    }
}

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
    float speed = shuttle_value_to_speed(slider_value, s->shuttle_mode);

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
        float speed = shuttle_value_to_speed(slider_value, s->shuttle_mode);
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

    /* Adjust shuttle slider range and sensitivity based on mode */
    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(s->shuttle_scale));

    if (s->shuttle_mode == SHUTTLE_MODE_OFF) {
        /* OFF mode: disable slider, reset to center */
        gtk_widget_set_sensitive(s->shuttle_scale, FALSE);
        gtk_range_set_value(GTK_RANGE(s->shuttle_scale), 0.0);
    } else {
        gtk_widget_set_sensitive(s->shuttle_scale, TRUE);
        gboolean use_pitched_range = (s->shuttle_mode == SHUTTLE_MODE_PITCHED);
        double new_lower = use_pitched_range ? SLIDER_MIN_PITCHED : SLIDER_MIN_RUBBERBAND;
        double new_upper = use_pitched_range ? SLIDER_MAX_PITCHED : SLIDER_MAX_RUBBERBAND;
        gtk_adjustment_set_lower(adj, new_lower);
        gtk_adjustment_set_upper(adj, new_upper);

        /* Clamp current value if outside new range */
        double current = gtk_range_get_value(GTK_RANGE(s->shuttle_scale));
        if (current > new_upper) {
            gtk_range_set_value(GTK_RANGE(s->shuttle_scale), new_upper);
        } else if (current < new_lower) {
            gtk_range_set_value(GTK_RANGE(s->shuttle_scale), new_lower);
        }
    }

    /* Update label and speed with new mapping */
    double current = gtk_range_get_value(GTK_RANGE(s->shuttle_scale));
    update_shuttle_label(s, current);
    if (s->pipeline) {
        float speed = shuttle_value_to_speed(current, s->shuttle_mode);
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

/**
 * Format duration in milliseconds to MM:SS string.
 */
static void format_duration_ms(uint64_t ms, char *buf, size_t buf_size) {
    uint64_t total_sec = ms / 1000;
    unsigned long total_min = (unsigned long)(total_sec / 60);
    unsigned long sec = (unsigned long)(total_sec % 60);
    snprintf(buf, buf_size, "%lu:%02lu", total_min, sec);
}

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
    const GPtrArray *album_tracks = NULL;

    if (s->library && s->current_track_id > 0) {
        track = library_cache_get_track(s->library, s->current_track_id);
        if (track) {
            album = library_cache_get_album(s->library, track->album_id);
            album_tracks = library_cache_get_tracks_by_album(s->library, track->album_id);
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
                format_duration_ms(track->duration_ms, dur_buf, sizeof(dur_buf));
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
                        format_duration_ms(next_track->duration_ms, dur_buf, sizeof(dur_buf));
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

static void on_artist_clicked(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    UiChannelStrip *s = UI_CHANNEL_STRIP(data);
    if (!s->library || s->current_track_id <= 0) return;

    const library_track_info_t *track = library_cache_get_track(s->library, s->current_track_id);
    if (!track || track->album_id <= 0) return;

    /* Get artist_id from album (track's artist_id not populated by db_get_track) */
    const library_album_info_t *album = library_cache_get_album(s->library, track->album_id);
    if (album && album->artist_id > 0) {
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
    remove_marquee_tick_callback(s->title_scroll);
    remove_marquee_tick_callback(s->artist_scroll);
    remove_marquee_tick_callback(s->album_scroll);
    remove_marquee_tick_callback(s->next_track_scroll);

    /* Disconnect gesture signal handlers before widget destruction */
    if (s->seek_gesture) {
        g_signal_handlers_disconnect_by_data(s->seek_gesture, s);
        s->seek_gesture = NULL;
    }
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

    /* Load template from resource */
    gtk_widget_class_set_template_from_resource(wc, "/org/quadrature/ui/channel_strip.ui");

    /* Bind template children */
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, main_box);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, channel_label);
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
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, seek_bar);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, right_column);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, info_box);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, seek_row);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, seek_times_row);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, seek_time_start);
    gtk_widget_class_bind_template_child(wc, UiChannelStrip, seek_time_end);

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

static void ui_channel_strip_init(UiChannelStrip *s) {
    s->channel_id = 0;
    s->pipeline = NULL;
    s->show_spectrum = TRUE;
    s->time_state = TIME_STATE_NONE;
    s->seek_dragging = FALSE;
    s->sample_rate = 0;
    s->length_samples = 0;
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

    /* Add gesture controller for seek end detection (not template-able) */
    /* Use GtkGestureDrag instead of GtkGestureClick to avoid intercepting
     * press events before GtkScale can handle its native drag */
    s->seek_gesture = GTK_GESTURE(gtk_gesture_drag_new());
    g_signal_connect(s->seek_gesture, "drag-begin", G_CALLBACK(on_seek_drag_begin), s);
    g_signal_connect(s->seek_gesture, "drag-end", G_CALLBACK(on_seek_drag_end), s);
    gtk_widget_add_controller(s->seek_bar, GTK_EVENT_CONTROLLER(s->seek_gesture));

    /* Add click gesture to channel label for selection */
    s->channel_click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->channel_click_gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect(s->channel_click_gesture, "released", G_CALLBACK(on_channel_clicked), s);
    gtk_widget_add_controller(s->channel_label, GTK_EVENT_CONTROLLER(s->channel_click_gesture));

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
    if (s->album_label) {
        s->album_click_gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->album_click_gesture), GDK_BUTTON_PRIMARY);
        g_signal_connect(s->album_click_gesture, "released", G_CALLBACK(on_album_clicked), s);
        gtk_widget_add_controller(s->album_label, GTK_EVENT_CONTROLLER(s->album_click_gesture));
    }

    if (s->artist_label) {
        s->artist_click_gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->artist_click_gesture), GDK_BUTTON_PRIMARY);
        g_signal_connect(s->artist_click_gesture, "released", G_CALLBACK(on_artist_clicked), s);
        gtk_widget_add_controller(s->artist_label, GTK_EVENT_CONTROLLER(s->artist_click_gesture));
    }

    if (s->next_track_label) {
        s->next_track_click_gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(s->next_track_click_gesture), GDK_BUTTON_PRIMARY);
        g_signal_connect(s->next_track_click_gesture, "released", G_CALLBACK(on_next_track_label_clicked), s);
        gtk_widget_add_controller(s->next_track_label, GTK_EVENT_CONTROLLER(s->next_track_click_gesture));
    }
}

/* ===============================================================================
 * Public API
 * =============================================================================== */

GtkWidget *ui_channel_strip_new(int channel_id, audio_pipeline_t *pipeline, library_cache_t *library) {
    UiChannelStrip *s = g_object_new(UI_TYPE_CHANNEL_STRIP, NULL);
    s->channel_id = channel_id;
    s->pipeline = pipeline;
    s->library = library;

    /* Update channel label */
    char badge[4];
    snprintf(badge, sizeof(badge), "%d", channel_id + 1);
    gtk_label_set_text(GTK_LABEL(s->channel_label), badge);

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

    /* Add status scroll (containing status_label) to display inner (sibling of info_box) */
    GtkWidget *display_inner = gtk_frame_get_child(GTK_FRAME(s->display_panel));
    if (display_inner && s->status_scroll) {
        gtk_box_prepend(GTK_BOX(display_inner), s->status_scroll);
    }

    /* Add spectrum if enabled (full height) */
    if (s->show_spectrum) {
        s->spectrum = ui_spectrum_new(SPECTRUM_BARS);
        gtk_widget_add_css_class(s->spectrum, "spectrum-display");
        gtk_widget_set_vexpand(s->spectrum, TRUE);
        gtk_box_append(GTK_BOX(s->right_column), s->spectrum);
    }

    /* Setup frame-synchronized marquee scrolling for each scroll widget */
    setup_marquee_for_scroll(s->title_scroll);
    setup_marquee_for_scroll(s->artist_scroll);
    setup_marquee_for_scroll(s->album_scroll);
    setup_marquee_for_scroll(s->next_track_scroll);

    return GTK_WIDGET(s);
}

void ui_channel_strip_update(UiChannelStrip *s, audio_pipeline_t *pipeline) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));
    g_assert(pipeline != NULL);  /* Caller must provide valid pipeline */

    channel_state_t st = audio_pipeline_get_player_state(pipeline, s->channel_id);
    uint64_t len = audio_pipeline_get_player_length(pipeline, s->channel_id);

    s->sample_rate = audio_pipeline_get_sample_rate(pipeline);
    s->length_samples = len;
    s->prev_player_state = st;

    /* Play state - update icon every frame (GTK4 is idempotent) */
    gboolean playing = (st == CHANNEL_PLAYING);
    update_play_icon(s, playing);

    /* Get interpolated position for smooth display */
    float speed = 1.0f;
    double display_pos_d = audio_pipeline_get_player_position_smooth(pipeline, s->channel_id, &speed);

    /* Override with seek bar position if dragging */
    if (s->seek_dragging && s->seek_bar && len > 0) {
        double val = gtk_range_get_value(GTK_RANGE(s->seek_bar));
        display_pos_d = val * (double)len;
    }

    if (s->sample_rate > 0 && len > 0) {
        /* Remaining time - floating point throughout */
        double rem_samples = (double)len - display_pos_d;
        if (rem_samples < 0.0) rem_samples = 0.0;

        float abs_speed = fabsf(speed);
        if (abs_speed < 0.01f) abs_speed = 1.0f;

        double rem_sec_d = rem_samples / (double)s->sample_rate / (double)abs_speed;
        unsigned long total_rem_min = (unsigned long)(rem_sec_d / 60.0);
        unsigned long rem_sec = (unsigned long)fmod(rem_sec_d, 60.0);
        unsigned long rem_cs = (unsigned long)(fmod(rem_sec_d, 1.0) * 100.0);

        char buf[64];
        snprintf(buf, sizeof(buf), "-%lu:%02lu.%02lu", total_rem_min, rem_sec, rem_cs);
        gtk_label_set_text(GTK_LABEL(s->time_label), buf);

        /* Time color state (using floating-point seconds for thresholds) */
        time_state_t new_state = TIME_STATE_NONE;
        if (!s->seek_dragging && rem_sec_d > 0) {
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

        if (s->time_elapsed_label) {
            snprintf(buf, sizeof(buf), "%lu:%02lu.%02lu", total_elapsed_min, elapsed_sec, elapsed_cs);
            gtk_label_set_text(GTK_LABEL(s->time_elapsed_label), buf);
        }

        /* Seek bar time labels (simpler format without centiseconds) */
        if (s->seek_time_start) {
            snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(elapsed_sec_d / 60.0), (unsigned long)fmod(elapsed_sec_d, 60.0));
            gtk_label_set_text(GTK_LABEL(s->seek_time_start), buf);
        }

        /* Total time */
        if (s->seek_time_end) {
            uint64_t total_sec = len / s->sample_rate;
            snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60));
            gtk_label_set_text(GTK_LABEL(s->seek_time_end), buf);
        }
    } else {
        gtk_label_set_text(GTK_LABEL(s->time_label), "-0:00.00");
        if (s->time_elapsed_label) {
            gtk_label_set_text(GTK_LABEL(s->time_elapsed_label), "0:00.00");
        }
        if (s->seek_time_start) {
            gtk_label_set_text(GTK_LABEL(s->seek_time_start), "0:00");
        }
        if (s->seek_time_end) {
            gtk_label_set_text(GTK_LABEL(s->seek_time_end), "--:--");
        }
    }

    /* Seek bar - use interpolated position when not dragging */
    if (!s->seek_dragging && len > 0) {
        gtk_range_set_value(GTK_RANGE(s->seek_bar), display_pos_d / (double)len);
    }

    /* Table-driven button sensitivity */
    update_button_sensitivity(s, st);

    /* Spectrum (stereo: left channel on left, right channel on right) */
    if (s->spectrum) {
        float left[SPECTRUM_BARS], right[SPECTRUM_BARS];
        audio_pipeline_get_player_spectrum(pipeline, s->channel_id, left, right, SPECTRUM_BARS);
        ui_spectrum_set_bars(UI_SPECTRUM(s->spectrum), left, right, SPECTRUM_BARS);
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
                                                 int64_t track_id,
                                                 const char *path,
                                                 const char *title,
                                                 const char *artist,
                                                 const char *album) {
    g_return_val_if_fail(UI_IS_CHANNEL_STRIP(s), QUADRATURE_ERROR_INVALID_PARAM);
    g_return_val_if_fail(track_id > 0, QUADRATURE_ERROR_INVALID_PARAM);

    /* Update UI metadata FIRST (instant feedback) */
    s->current_track_id = track_id;
    g_free(s->filepath); s->filepath = g_strdup(path);
    g_free(s->title);    s->title = g_strdup(title);
    g_free(s->artist);   s->artist = g_strdup(artist);
    g_free(s->album);    s->album = g_strdup(album);
    update_display(s);

    if (s->pipeline && s->pipeline->cache) {
        /* Load track into cache FIRST (non-blocking, starts decode) */
        audio_cache_load(s->pipeline->cache, track_id);

        /* Then set player track (will lock, not wait for decode) */
        quadrature_result_t res = audio_pipeline_set_player_track(s->pipeline, s->channel_id, track_id);
        if (res != QUADRATURE_OK) return res;

        s->length_samples = audio_pipeline_get_player_length(s->pipeline, s->channel_id);
        s->sample_rate = audio_pipeline_get_sample_rate(s->pipeline);
    }

    return QUADRATURE_OK;
}

void ui_channel_strip_update_track_display(UiChannelStrip *s,
                                            int64_t track_id,
                                            const char *path,
                                            const char *title,
                                            const char *artist,
                                            const char *album) {
    g_return_if_fail(UI_IS_CHANNEL_STRIP(s));

    s->current_track_id = track_id;
    g_free(s->filepath); s->filepath = g_strdup(path);
    g_free(s->title);    s->title = g_strdup(title);
    g_free(s->artist);   s->artist = g_strdup(artist);
    g_free(s->album);    s->album = g_strdup(album);

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

    ChannelMode old_mode = s->mode;
    s->mode = mode;

    /* Handle mode entry actions */
    if (mode == CHANNEL_MODE_QUEUED) {
        /* Cue to start when entering QUEUED */
        if (s->pipeline)
            audio_pipeline_player_seek(s->pipeline, s->channel_id, 0);

        /* Exit preview if active */
        if (old_mode == CHANNEL_MODE_PREVIEW) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), FALSE);
        }
    }

    /* Sync preview button state */
    gboolean preview = (mode == CHANNEL_MODE_PREVIEW);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->preview_btn)) != preview)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->preview_btn), preview);

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
    g_free(s->filepath); s->filepath = g_strdup(track->path);
    g_free(s->title);    s->title = g_strdup(track->title);
    g_free(s->artist);   s->artist = g_strdup(track->artist_display ? track->artist_display : track->artist_name);
    g_free(s->album);    s->album = g_strdup(track->album_title);
    update_display(s);

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
    g_free(s->filepath); s->filepath = g_strdup(track->path);
    g_free(s->title);    s->title = g_strdup(track->title);
    g_free(s->artist);   s->artist = g_strdup(track->artist_display ? track->artist_display : track->artist_name);
    g_free(s->album);    s->album = g_strdup(track->album_title);
    update_display(s);

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
