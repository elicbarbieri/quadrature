/**
 * Quadrature UI Meters
 *
 * SpectrumDisplay widget for audio visualization.
 * Uses GtkSnapshot for 60fps rendering performance.
 */

#include "internal.h"
#include <math.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * SpectrumDisplay Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_BARS 64
#define BAR_GAP 2
#define BAR_RADIUS 2.0f
#define MIN_BAR_WIDTH 3.0f

struct _UiSpectrum {
    GtkWidget parent;
    float bars_left[MAX_BARS];
    float bars_right[MAX_BARS];
    float smoothed_left[MAX_BARS];
    float smoothed_right[MAX_BARS];
    int num_bars;
};

G_DEFINE_FINAL_TYPE(UiSpectrum, ui_spectrum, GTK_TYPE_WIDGET)

static void ui_spectrum_snapshot(GtkWidget *widget, GtkSnapshot *snap) {
    UiSpectrum *s = UI_SPECTRUM(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0 || s->num_bars <= 0) return;

    /* Dual-channel: left channel on left side (bass at center),
     * right channel on right side (bass at center), bars extend up and down */
    float cy = h / 2.0f;
    float max_h = fmaxf(cy - 1.0f, 1.0f);

    /* Calculate how many bars fit with minimum bar width */
    int max_total = (int)((w + BAR_GAP) / (MIN_BAR_WIDTH + BAR_GAP));
    int max_half = max_total / 2;
    if (max_half < 1) max_half = 1;
    int num_bars = (max_half < s->num_bars) ? max_half : s->num_bars;

    int total = num_bars * 2;
    float gap_total = BAR_GAP * (total - 1);
    float bw = fmaxf(MIN_BAR_WIDTH, (w - gap_total) / total);

    for (int i = 0; i < num_bars; i++) {
        /* Left side: left audio channel, reversed (bass at center) */
        float lval = fmaxf(s->smoothed_left[i], 0.03f);
        float l_bar_h = max_h * lval;
        float l_alpha = 0.7f + lval * 0.3f;
        GdkRGBA lc = {UI_COLOR_CYAN.red, UI_COLOR_CYAN.green, UI_COLOR_CYAN.blue, l_alpha};

        int li = num_bars - 1 - i;
        float lx = li * (bw + BAR_GAP);
        float ly = cy - l_bar_h;
        float lth = l_bar_h * 2.0f;

        graphene_rect_t lr = GRAPHENE_RECT_INIT(lx, ly, bw, lth);
        GskRoundedRect lrr;
        gsk_rounded_rect_init_from_rect(&lrr, &lr, BAR_RADIUS);
        gtk_snapshot_push_rounded_clip(snap, &lrr);
        gtk_snapshot_append_color(snap, &lc, &lr);
        gtk_snapshot_pop(snap);

        /* Right side: right audio channel, normal (bass at center) */
        float rval = fmaxf(s->smoothed_right[i], 0.03f);
        float r_bar_h = max_h * rval;
        float r_alpha = 0.7f + rval * 0.3f;
        GdkRGBA rc = {UI_COLOR_CYAN.red, UI_COLOR_CYAN.green, UI_COLOR_CYAN.blue, r_alpha};

        float rx = (num_bars + i) * (bw + BAR_GAP);
        float ry = cy - r_bar_h;
        float rth = r_bar_h * 2.0f;

        graphene_rect_t rr = GRAPHENE_RECT_INIT(rx, ry, bw, rth);
        GskRoundedRect rrr;
        gsk_rounded_rect_init_from_rect(&rrr, &rr, BAR_RADIUS);
        gtk_snapshot_push_rounded_clip(snap, &rrr);
        gtk_snapshot_append_color(snap, &rc, &rr);
        gtk_snapshot_pop(snap);
    }
}

static void ui_spectrum_measure(GtkWidget *w, GtkOrientation o, int for_size,
                                int *min, int *nat, int *min_bl, int *nat_bl) {
    UiSpectrum *s = UI_SPECTRUM(w);
    (void)for_size;
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        *min = s->num_bars * 3 + 4;
        *nat = s->num_bars * 6 + 4;
    } else {
        *min = 30;
        *nat = 60;
    }
    *min_bl = *nat_bl = -1;
}

static void ui_spectrum_class_init(UiSpectrumClass *klass) {
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    wc->snapshot = ui_spectrum_snapshot;
    wc->measure = ui_spectrum_measure;
}

static void ui_spectrum_init(UiSpectrum *s) {
    s->num_bars = SPECTRUM_BARS;
    memset(s->bars_left, 0, sizeof(s->bars_left));
    memset(s->bars_right, 0, sizeof(s->bars_right));
    memset(s->smoothed_left, 0, sizeof(s->smoothed_left));
    memset(s->smoothed_right, 0, sizeof(s->smoothed_right));
}

GtkWidget *ui_spectrum_new(int num_bars) {
    UiSpectrum *s = g_object_new(UI_TYPE_SPECTRUM, NULL);
    if (num_bars > 0 && num_bars <= MAX_BARS) {
        s->num_bars = num_bars;
    }
    return GTK_WIDGET(s);
}

void ui_spectrum_set_bars(UiSpectrum *s, const float *left, const float *right, int count) {
    g_return_if_fail(UI_IS_SPECTRUM(s));
    if (!left || !right || count <= 0) return;

    int n = MIN(count, s->num_bars);
    for (int i = 0; i < n; i++) {
        /* Left channel: fast attack, slow release */
        float lt = CLAMP(left[i], 0.0f, 1.0f);
        float lc = s->smoothed_left[i];
        s->smoothed_left[i] = (lt > lc)
            ? lc + (lt - lc) * 0.5f
            : lc + (lt - lc) * 0.15f;
        s->bars_left[i] = lt;

        /* Right channel: fast attack, slow release */
        float rt = CLAMP(right[i], 0.0f, 1.0f);
        float rc = s->smoothed_right[i];
        s->smoothed_right[i] = (rt > rc)
            ? rc + (rt - rc) * 0.5f
            : rc + (rt - rc) * 0.15f;
        s->bars_right[i] = rt;
    }
    gtk_widget_queue_draw(GTK_WIDGET(s));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WaveformSeekBar — self-contained seek widget with loudness visualization
 *
 * Single custom GtkWidget that renders waveform bars + playhead AND handles
 * input (click-to-seek, playhead drag). All positioning uses graphene_rect_t
 * (floats) for true sub-pixel playhead motion — no GtkScale integer rounding.
 *
 * Owns a GtkAdjustment (0.0–1.0). Emits "seek" signal on committed seeks.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Dimensions ──────────────────────────────────────────────────────── */

#define WAVEFORM_MAX_BINS    1024
#define WAVEFORM_BAR_WIDTH   2.0f
#define WAVEFORM_BAR_GAP     1.0f
#define WAVEFORM_BAR_RADIUS  1.0f
#define WAVEFORM_MIN_BAR_H   0.04f   /* minimum bar height as fraction of max */
#define WAVEFORM_ANIM_MS     400.0   /* grow-out animation duration */

/* ─── Playhead ────────────────────────────────────────────────────────── */

#define PLAYHEAD_WIDTH       3.0f
#define PLAYHEAD_EDGE_FADE   1.0f    /* px of soft alpha falloff on each side
                                      * — eliminates sub-pixel luminance wobble
                                      * caused by sRGB-space alpha compositing
                                      * as split_x slides between integer pixels */
#define PLAYHEAD_HEIGHT_FRAC 0.85f   /* height as fraction of half-widget */
#define PLAYHEAD_HIT_RADIUS  8.0     /* px — grab zone for drag / hover detection */
#define PLAYHEAD_LERP_SPEED  0.15f   /* per-frame lerp for playhead color fade */

/* ─── Seek Spotlight ──────────────────────────────────────────────────── */

/* When the cursor hovers over the waveform (but not near the playhead),
 * the bar under the cursor doubles in height and turns white — a "seek
 * preview" showing exactly where a click would land. Height is clamped to
 * the widget's allocated space. Tiny bars get a minimum boost. */
#define SPOTLIGHT_LERP_SPEED 0.18f   /* per-frame fade in/out speed */
#define SPOTLIGHT_SCALE      1.0f    /* center bar grows by 100% (2x) */
#define SPOTLIGHT_MIN_H_FRAC 0.30f   /* minimum bar height during spotlight */

/* ─── Colors ──────────────────────────────────────────────────────────── */

static const GdkRGBA WAVEFORM_PLAYED     = {0.00f, 0.83f, 1.00f, 0.85f};  /* matches spectrum cyan */
static const GdkRGBA WAVEFORM_PLAYED_DIM = {0.00f, 0.55f, 0.68f, 0.65f};  /* muted cyan when disabled */
static const GdkRGBA WAVEFORM_UNPLAYED   = {0.33f, 0.33f, 0.33f, 0.70f};
static const GdkRGBA PLAYHEAD_DEFAULT    = {1.00f, 1.00f, 1.00f, 0.90f};
static const GdkRGBA PLAYHEAD_HOVER      = {0.00f, 0.83f, 1.00f, 0.95f};

/* ─── Instance Data ───────────────────────────────────────────────────── */

struct _UiWaveformSeekBar {
    GtkWidget parent;

    /* Position & input */
    GtkAdjustment *adjustment;      /* owned; range 0.0–1.0 (drag position) */
    GtkGesture    *drag_gesture;    /* GtkGestureDrag for click + drag */
    gboolean       dragging;        /* TRUE while user drags playhead */
    double         drag_start_value;
    double         playback_value;  /* actual playback position, updated even during drag */

    /* Loudness data */
    float    loudness[WAVEFORM_MAX_BINS];
    int      num_bins;
    gboolean has_data;

    /* Grow-out animation (new track loaded) */
    float    anim_progress;         /* 0.0→1.0, ease-out quadratic */
    gint64   anim_start_us;
    gboolean animating;

    /* Playhead hover: cursor within PLAYHEAD_HIT_RADIUS of playhead */
    gboolean playhead_hovered;
    float    playhead_hover_t;      /* lerped 0→1 for playhead white→cyan */
    float    bar_dim_t;             /* lerped 0→1: dims played bars on any hover/drag/disabled */

    /* Seek spotlight: cursor over waveform but NOT near playhead */
    gboolean waveform_hovered;      /* TRUE when cursor is anywhere over widget */
    float    spotlight_t;           /* lerped 0→1 for smooth fade in/out */
    double   cursor_x;             /* last known cursor X in widget coords */

    /* Tick callback for leave fade-out (0 = inactive) */
    guint    leave_tick_id;
};

G_DEFINE_FINAL_TYPE(UiWaveformSeekBar, ui_waveform_seek_bar, GTK_TYPE_WIDGET)

static guint waveform_seek_signal_id = 0;

/* ─── Input: Drag (click-to-seek + playhead drag) ─────────────────────── */

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
    (void)g; (void)y;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);
    int width = gtk_widget_get_width(GTK_WIDGET(w));
    if (width <= 0) return;

    double playhead_x = gtk_adjustment_get_value(w->adjustment) * width;

    if (fabs(x - playhead_x) <= PLAYHEAD_HIT_RADIUS) {
        /* Near playhead — begin drag */
        w->dragging = TRUE;
        w->drag_start_value = gtk_adjustment_get_value(w->adjustment);
        w->playhead_hovered = TRUE;
    } else {
        /* Away from playhead — click-to-jump + emit seek immediately */
        double new_val = CLAMP(x / (double)width, 0.0, 1.0);
        gtk_adjustment_set_value(w->adjustment, new_val);
        g_signal_emit(w, waveform_seek_signal_id, 0, new_val);
    }
    gtk_widget_queue_draw(GTK_WIDGET(w));
}

static void on_drag_update(GtkGestureDrag *g, double offset_x, double offset_y, gpointer data) {
    (void)offset_y;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);
    if (!w->dragging) return;

    int width = gtk_widget_get_width(GTK_WIDGET(w));
    if (width <= 0) return;

    double start_x;
    gtk_gesture_drag_get_start_point(g, &start_x, NULL);
    gtk_adjustment_set_value(w->adjustment,
        CLAMP((start_x + offset_x) / (double)width, 0.0, 1.0));
    gtk_widget_queue_draw(GTK_WIDGET(w));
}

static void on_drag_end(GtkGestureDrag *g, double offset_x, double offset_y, gpointer data) {
    (void)g; (void)offset_x; (void)offset_y;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);
    if (w->dragging) {
        w->dragging = FALSE;
        g_signal_emit(w, waveform_seek_signal_id, 0,
                      gtk_adjustment_get_value(w->adjustment));
    }
}

/* Right-click during drag: cancel without seeking, snap back to playback position */
static void on_cancel_click(GtkGestureClick *g, int n_press, double x, double y, gpointer data) {
    (void)g; (void)n_press; (void)x; (void)y;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);
    if (w->dragging) {
        w->dragging = FALSE;
        gtk_adjustment_set_value(w->adjustment, w->playback_value);
        gtk_widget_queue_draw(GTK_WIDGET(w));
    }
}

/* ─── Leave fade-out tick callback ─────────────────────────────────────── */

static gboolean on_leave_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);
    gtk_widget_queue_draw(widget);

    /* Done once all hover-driven animations have settled to zero */
    if (w->spotlight_t < 0.01f && w->playhead_hover_t < 0.01f && w->bar_dim_t < 0.01f) {
        w->leave_tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ─── Input: Motion (playhead hover + seek spotlight) ─────────────────── */

static void on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data) {
    (void)ctrl; (void)y;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);

    w->cursor_x = x;
    w->waveform_hovered = TRUE;

    /* Cancel leave fade-out if cursor re-enters */
    if (w->leave_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w), w->leave_tick_id);
        w->leave_tick_id = 0;
    }

    if (!w->dragging) {
        int width = gtk_widget_get_width(GTK_WIDGET(w));
        if (width > 0) {
            double playhead_x = gtk_adjustment_get_value(w->adjustment) * width;
            w->playhead_hovered = fabs(x - playhead_x) <= PLAYHEAD_HIT_RADIUS;
        }
    }
    gtk_widget_queue_draw(GTK_WIDGET(w));
}

static void on_leave(GtkEventControllerMotion *ctrl, gpointer data) {
    (void)ctrl;
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(data);
    w->waveform_hovered = FALSE;
    if (!w->dragging)
        w->playhead_hovered = FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(w));

    /* Ensure fade-out animation completes even without external redraws
     * (e.g. when playback is paused and no tick callback drives frames). */
    if (w->leave_tick_id == 0 &&
        (w->spotlight_t > 0.01f || w->playhead_hover_t > 0.01f || w->bar_dim_t > 0.01f)) {
        w->leave_tick_id = gtk_widget_add_tick_callback(
            GTK_WIDGET(w), on_leave_tick, w, NULL);
    }
}

/* ─── Gamma-correct color lerp ─────────────────────────────────────────────
 * GSK composites in sRGB-encoded space. Lerping RGB values directly in sRGB
 * produces a perceptually duller midpoint (e.g. grey→cyan goes muddy halfway).
 * Round-tripping through linear light gives the perceptually-uniform mix and
 * removes a slow brightness ripple on the bar that straddles the playhead.
 * Alpha is treated as linear coverage and not transformed. */

static inline float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline float linear_to_srgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static inline GdkRGBA rgba_lerp_linear(GdkRGBA a, GdkRGBA b, float t) {
    float ar = srgb_to_linear(a.red),   br = srgb_to_linear(b.red);
    float ag = srgb_to_linear(a.green), bg = srgb_to_linear(b.green);
    float ab = srgb_to_linear(a.blue),  bb = srgb_to_linear(b.blue);
    return (GdkRGBA){
        linear_to_srgb(ar + t * (br - ar)),
        linear_to_srgb(ag + t * (bg - ag)),
        linear_to_srgb(ab + t * (bb - ab)),
        a.alpha + t * (b.alpha - a.alpha),
    };
}

/* ─── Rendering ────────────────────────────────────────────────────────── */

static void ui_waveform_seek_bar_snapshot(GtkWidget *widget, GtkSnapshot *snap) {
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(widget);
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0 || !w->has_data || w->num_bins <= 0) return;

    /* Grow-out animation (new track) */
    if (w->animating) {
        gint64 now = g_get_monotonic_time();
        float t = (float)((double)(now - w->anim_start_us) / 1000.0 / WAVEFORM_ANIM_MS);
        if (t >= 1.0f) { t = 1.0f; w->animating = FALSE; }
        w->anim_progress = t * (2.0f - t);  /* ease-out quadratic */
    }

    float cy = height / 2.0f;
    float max_h = fmaxf(cy - 1.0f, 1.0f);
    float step = WAVEFORM_BAR_WIDTH + WAVEFORM_BAR_GAP;
    int bar_count = MAX((int)(width / step), 1);
    gboolean sensitive = gtk_widget_is_sensitive(widget);
    float drag_x = (float)(gtk_adjustment_get_value(w->adjustment) * width);
    float play_x = (float)(w->playback_value * width);

    /* During drag: playhead follows drag, playback position shown as dim trail.
     * When not dragging: single split point, normal played color. */
    float split_x = drag_x;

    /* ── Playhead hover lerp (playhead white→cyan) ── */
    float ph_target = w->playhead_hovered ? 1.0f : 0.0f;
    w->playhead_hover_t += (ph_target - w->playhead_hover_t) * PLAYHEAD_LERP_SPEED;
    if (fabsf(w->playhead_hover_t - ph_target) < 0.01f)
        w->playhead_hover_t = ph_target;

    /* ── Bar dim lerp (dims played bars on any hover, drag, or disabled) ── */
    float dim_target = (w->waveform_hovered || w->dragging || !sensitive) ? 1.0f : 0.0f;
    w->bar_dim_t += (dim_target - w->bar_dim_t) * PLAYHEAD_LERP_SPEED;
    if (fabsf(w->bar_dim_t - dim_target) < 0.01f)
        w->bar_dim_t = dim_target;

    /* ── Seek spotlight: lerp visibility, find targeted bar index ─────── */
    gboolean spotlight_active = sensitive && w->waveform_hovered && !w->playhead_hovered && !w->dragging;
    float spot_target = spotlight_active ? 1.0f : 0.0f;
    w->spotlight_t += (spot_target - w->spotlight_t) * SPOTLIGHT_LERP_SPEED;
    if (fabsf(w->spotlight_t - spot_target) < 0.01f) w->spotlight_t = spot_target;

    int cursor_bar = -1;
    if (w->spotlight_t > 0.001f && w->cursor_x >= 0.0)
        cursor_bar = (int)(w->cursor_x / step);

    /* ── Pre-compute played color (constant across all bars this frame) ── */
    const GdkRGBA *bright = sensitive ? &WAVEFORM_PLAYED : &WAVEFORM_PLAYED_DIM;
    GdkRGBA played_color = rgba_lerp_linear(*bright, WAVEFORM_PLAYED_DIM, w->bar_dim_t);

    /* ── Waveform bars ───────────────────────────────────────────────── */
    for (int i = 0; i < bar_count; i++) {
        float x = i * step;
        int bin = MIN((i * w->num_bins) / bar_count, w->num_bins - 1);

        float val = fmaxf(w->loudness[bin], WAVEFORM_MIN_BAR_H);
        float bar_h = fmaxf(max_h * val * w->anim_progress, 0.5f);

        /* Base color: only the drag_fill/play_fill lerp varies per bar.
         * The played→dim color is pre-computed above (constant this frame). */
        float drag_fill = CLAMP((split_x - x) / WAVEFORM_BAR_WIDTH, 0.0f, 1.0f);
        GdkRGBA color;
        if (drag_fill > 0.0f) {
            color = rgba_lerp_linear(WAVEFORM_UNPLAYED, played_color, drag_fill);
        } else if (w->dragging) {
            float play_fill = CLAMP((play_x - x) / WAVEFORM_BAR_WIDTH, 0.0f, 1.0f);
            color = rgba_lerp_linear(WAVEFORM_UNPLAYED, WAVEFORM_PLAYED_DIM, play_fill);
        } else {
            color = WAVEFORM_UNPLAYED;
        }

        /* Seek spotlight: center bar doubles height + turns white */
        if (cursor_bar >= 0 && i == cursor_bar) {
            float boosted = fmaxf(bar_h * (1.0f + SPOTLIGHT_SCALE), max_h * SPOTLIGHT_MIN_H_FRAC);
            bar_h = bar_h + w->spotlight_t * (fminf(boosted, max_h) - bar_h);
            color = rgba_lerp_linear(color, PLAYHEAD_DEFAULT, w->spotlight_t);
        }

        graphene_rect_t rect = GRAPHENE_RECT_INIT(x, cy - bar_h, WAVEFORM_BAR_WIDTH, bar_h * 2.0f);
        GskRoundedRect rrect;
        gsk_rounded_rect_init_from_rect(&rrect, &rect, WAVEFORM_BAR_RADIUS);
        gtk_snapshot_push_rounded_clip(snap, &rrect);
        gtk_snapshot_append_color(snap, &color, &rect);
        gtk_snapshot_pop(snap);
    }

    /* ── Playhead (soft-edge gradient for sub-pixel-stable luminance) ── */
    if (split_x > 0.0f && split_x < (float)width) {
        GdkRGBA pc = rgba_lerp_linear(PLAYHEAD_DEFAULT, PLAYHEAD_HOVER, w->playhead_hover_t);

        if (w->playhead_hover_t != ph_target || w->bar_dim_t != dim_target)
            gtk_widget_queue_draw(widget);

        /* A hard-edged rect of fractional X position pulses in brightness as
         * sub-pixel coverage is composited in sRGB space. A 1 px alpha ramp on
         * each side spreads the partial coverage over a smooth gradient, so
         * the integrated luminance stays nearly invariant to pixel phase. */
        float ph_h = max_h * PLAYHEAD_HEIGHT_FRAC;
        float ph_full_w = PLAYHEAD_WIDTH + 2.0f * PLAYHEAD_EDGE_FADE;
        graphene_rect_t line = GRAPHENE_RECT_INIT(
            split_x - ph_full_w * 0.5f, cy - ph_h, ph_full_w, ph_h * 2.0f);

        graphene_point_t gstart = GRAPHENE_POINT_INIT(line.origin.x, 0);
        graphene_point_t gend   = GRAPHENE_POINT_INIT(line.origin.x + ph_full_w, 0);
        float ramp = PLAYHEAD_EDGE_FADE / ph_full_w;
        GdkRGBA edge = { pc.red, pc.green, pc.blue, 0.0f };
        GskColorStop stops[4] = {
            { 0.0f,        edge },
            { ramp,        pc   },
            { 1.0f - ramp, pc   },
            { 1.0f,        edge },
        };
        gtk_snapshot_append_linear_gradient(snap, &line, &gstart, &gend, stops, 4);
    }

    /* Keep redrawing while spotlight is fading */
    if (w->spotlight_t != spot_target)
        gtk_widget_queue_draw(widget);
}

static void ui_waveform_seek_bar_measure(GtkWidget *widget, GtkOrientation o,
                                          int for_size, int *min, int *nat,
                                          int *min_bl, int *nat_bl) {
    (void)widget; (void)for_size;
    if (o == GTK_ORIENTATION_HORIZONTAL) { *min = 100; *nat = 400; }
    else                                 { *min = 36;  *nat = 36;  }
    *min_bl = *nat_bl = -1;
}

static void ui_waveform_seek_bar_dispose(GObject *obj) {
    UiWaveformSeekBar *w = UI_WAVEFORM_SEEK_BAR(obj);
    if (w->leave_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(w), w->leave_tick_id);
        w->leave_tick_id = 0;
    }
    if (w->drag_gesture) {
        g_signal_handlers_disconnect_by_data(w->drag_gesture, w);
        w->drag_gesture = NULL;
    }
    g_clear_object(&w->adjustment);
    G_OBJECT_CLASS(ui_waveform_seek_bar_parent_class)->dispose(obj);
}

static void ui_waveform_seek_bar_class_init(UiWaveformSeekBarClass *klass) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

    oc->dispose = ui_waveform_seek_bar_dispose;
    wc->snapshot = ui_waveform_seek_bar_snapshot;
    wc->measure = ui_waveform_seek_bar_measure;
    gtk_widget_class_set_css_name(wc, "waveform-seek-bar");

    /* "seek" signal: emitted with double value (0.0–1.0) on committed seeks */
    waveform_seek_signal_id = g_signal_new("seek",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static void ui_waveform_seek_bar_init(UiWaveformSeekBar *w) {
    /* Loudness data */
    memset(w->loudness, 0, sizeof(w->loudness));
    w->num_bins = 0;
    w->has_data = FALSE;

    /* Animation */
    w->anim_progress = 0.0f;
    w->anim_start_us = 0;
    w->animating = FALSE;

    /* Hover / drag state */
    w->dragging = FALSE;
    w->drag_start_value = 0.0;
    w->playback_value = 0.0;
    w->playhead_hovered = FALSE;
    w->playhead_hover_t = 0.0f;
    w->bar_dim_t = 0.0f;
    w->waveform_hovered = FALSE;
    w->spotlight_t = 0.0f;
    w->cursor_x = -1.0;
    w->leave_tick_id = 0;

    /* Position adjustment (0.0–1.0, owned) */
    w->adjustment = gtk_adjustment_new(0.0, 0.0, 1.0, 0.001, 0.01, 0.0);
    g_object_ref_sink(w->adjustment);

    /* Drag gesture: click-to-seek + playhead drag */
    w->drag_gesture = GTK_GESTURE(gtk_gesture_drag_new());
    g_signal_connect(w->drag_gesture, "drag-begin",  G_CALLBACK(on_drag_begin),  w);
    g_signal_connect(w->drag_gesture, "drag-update", G_CALLBACK(on_drag_update), w);
    g_signal_connect(w->drag_gesture, "drag-end",    G_CALLBACK(on_drag_end),    w);
    gtk_widget_add_controller(GTK_WIDGET(w), GTK_EVENT_CONTROLLER(w->drag_gesture));

    /* Right-click gesture: cancel drag without seeking */
    GtkGesture *cancel = GTK_GESTURE(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cancel), GDK_BUTTON_SECONDARY);
    g_signal_connect(cancel, "pressed", G_CALLBACK(on_cancel_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w), GTK_EVENT_CONTROLLER(cancel));

    /* Motion controller: playhead hover + seek spotlight */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), w);
    g_signal_connect(motion, "leave",  G_CALLBACK(on_leave),  w);
    gtk_widget_add_controller(GTK_WIDGET(w), motion);
}

GtkWidget *ui_waveform_seek_bar_new(void) {
    return g_object_new(UI_TYPE_WAVEFORM_SEEK_BAR, NULL);
}

GtkAdjustment *ui_waveform_seek_bar_get_adjustment(UiWaveformSeekBar *w) {
    g_return_val_if_fail(UI_IS_WAVEFORM_SEEK_BAR(w), NULL);
    return w->adjustment;
}

gboolean ui_waveform_seek_bar_is_dragging(UiWaveformSeekBar *w) {
    g_return_val_if_fail(UI_IS_WAVEFORM_SEEK_BAR(w), FALSE);
    return w->dragging;
}

void ui_waveform_seek_bar_set_playback_position(UiWaveformSeekBar *w, double value) {
    g_return_if_fail(UI_IS_WAVEFORM_SEEK_BAR(w));
    w->playback_value = CLAMP(value, 0.0, 1.0);
}

void ui_waveform_seek_bar_set_loudness(UiWaveformSeekBar *w, const float *bins, int count) {
    g_return_if_fail(UI_IS_WAVEFORM_SEEK_BAR(w));
    if (!bins || count <= 0) return;

    int n = MIN(count, WAVEFORM_MAX_BINS);
    memcpy(w->loudness, bins, n * sizeof(float));
    w->num_bins = n;
    w->has_data = TRUE;

    w->anim_start_us = g_get_monotonic_time();
    w->anim_progress = 0.0f;
    w->animating = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(w));
}

void ui_waveform_seek_bar_clear(UiWaveformSeekBar *w) {
    g_return_if_fail(UI_IS_WAVEFORM_SEEK_BAR(w));
    w->has_data = FALSE;
    w->num_bins = 0;
    w->anim_progress = 0.0f;
    w->animating = FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(w));
}

gboolean ui_waveform_seek_bar_is_animating(UiWaveformSeekBar *w) {
    g_return_val_if_fail(UI_IS_WAVEFORM_SEEK_BAR(w), FALSE);
    return w->animating;
}
