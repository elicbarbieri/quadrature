/**
 * QuadScrubber — Index rail overlay for library list views
 *
 * Sits at halign=END on a GtkOverlay wrapping the list's GtkScrolledWindow.
 * Bell-curve tick marks drawn directly over content (no dim overlay).
 * GTK_OVERFLOW_VISIBLE lets labels extend leftward of the widget bounds.
 *
 * Scrolling model:
 *   Drag   — pure absolute proportional: cursor Y maps directly to list fraction.
 *             Cursor always matches the active tick (the "thumb"). The ns-resize
 *             cursor signals scrubbing mode. Slow drag = fine control naturally.
 *   Click  — ease-out cubic animation to the bucket's start position.
 *   Wheel  — per-event dt acceleration: slow spin = 1× step, fast spin = up to
 *             WHEEL_MAX_ACCEL×. Event is consumed so GtkScrolledWindow doesn't
 *             double-scroll when hovering the scrubber strip.
 *   Passive— vadj "value-changed" keeps scroll_fraction + active_idx in sync
 *             with kinetic scrolling outside the scrubber strip.
 */

#define G_LOG_DOMAIN "quadrature"

#include <gtk/gtk.h>
#include <pango/pango.h>
#include <math.h>

#include "../internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SCRUB_PADDING       12.0    /* px — top/bottom inset so edge ticks float */
#define MIN_LABEL_SPACING   11      /* px — skip labels crowded closer than this */
#define RAIL_MARGIN_END     6       /* px — rail inset from widget right edge */
#define TICK_LENGTH_MIN     2.0     /* px — shortest tick for distant buckets */
#define TICK_LENGTH_MAX     22.0    /* px — full tick at scroll centre */
#define LABEL_OVERFLOW      32      /* px — leftward Cairo overflow for year labels */
#define LABEL_MARGIN        4       /* px — gap between label right edge and tick start */
#define BADGE_MARGIN_END    44      /* px — badge right margin (sits left of widget) */
#define BADGE_HEIGHT_HALF   24      /* px — badge vertical centering offset */
#define ANIM_DURATION_US    200000  /* µs — 200 ms ease-out for click jumps */

/* Bell-curve: which ticks grow near the current scroll position */
#define BELL_SIGMA          0.18    /* list-fraction units (~4–5 buckets at A-Z scale) */


/* ═══════════════════════════════════════════════════════════════════════════
 * Private State
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    double  start_value;
    double  target_value;
    gint64  start_time_us;
    guint   tick_id;
} ScrubAnimation;

struct _QuadScrubber {
    GtkWidget      parent_instance;

    GPtrArray     *buckets;           /* ScrubberBucket*, owned */
    guint          total_items;
    int            active_idx;        /* -1 = none */
    gboolean       dragging;
    double         scroll_fraction;   /* continuous 0–1; bell-curve centre + scroll target */

    GtkListView   *list_view;         /* weak ref */
    GtkAdjustment *vadj;              /* weak ref */
    gulong         vadj_signal;

    GtkWidget     *badge;             /* sibling GtkLabel in parent overlay, weak ref */

    ScrubAnimation anim;

    /* Cached font descriptors — avoid parsing "Bold 9" / "9" every frame */
    PangoFontDescription *fd_normal;
    PangoFontDescription *fd_bold;

    /* Bell-curve LUT — avoid exp() per bucket per frame */
    float bell_lut[256];
};

struct _QuadScrubberClass { GtkWidgetClass parent_class; };
G_DEFINE_FINAL_TYPE(QuadScrubber, quad_scrubber, GTK_TYPE_WIDGET)

/* ═══════════════════════════════════════════════════════════════════════════
 * Animation — ease-out cubic for click-to-jump
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cancel_animation(QuadScrubber *self) {
    if (self->anim.tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->anim.tick_id);
        self->anim.tick_id = 0;
    }
}

static gboolean scroll_tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)data;
    QuadScrubber *self = QUAD_SCRUBBER(widget);

    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double t   = (double)(now - self->anim.start_time_us) / (double)ANIM_DURATION_US;

    if (t >= 1.0) {
        gtk_adjustment_set_value(self->vadj, self->anim.target_value);
        self->anim.tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    double ease  = 1.0 - pow(1.0 - CLAMP(t, 0.0, 1.0), 3.0);
    double value = self->anim.start_value +
                   ease * (self->anim.target_value - self->anim.start_value);
    gtk_adjustment_set_value(self->vadj, value);
    return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

void scrubber_bucket_free(ScrubberBucket *bucket) {
    if (!bucket) return;
    g_free(bucket->label);
    g_free(bucket);
}

static int find_active_bucket(QuadScrubber *self, guint approx_pos) {
    if (!self->buckets || self->buckets->len == 0) return -1;
    for (guint i = 0; i < self->buckets->len; i++) {
        ScrubberBucket *b = g_ptr_array_index(self->buckets, i);
        guint end = (i + 1 < self->buckets->len)
                    ? ((ScrubberBucket *)g_ptr_array_index(self->buckets, i + 1))->position
                    : self->total_items;
        if (approx_pos >= b->position && approx_pos < end)
            return (int)i;
    }
    return (int)(self->buckets->len - 1);
}

static int bucket_from_fraction(QuadScrubber *self, double frac) {
    guint pos = (guint)(CLAMP(frac, 0.0, 1.0) * (double)self->total_items);
    return find_active_bucket(self, pos);
}

/* Map widget Y coordinate to list fraction, respecting top/bottom padding. */
static double y_to_fraction(QuadScrubber *self, double y) {
    int height = gtk_widget_get_height(GTK_WIDGET(self));
    double usable = MAX(1.0, (double)height - 2.0 * SCRUB_PADDING);
    return CLAMP((y - SCRUB_PADDING) / usable, 0.0, 1.0);
}

/* Push scroll_fraction to the GtkAdjustment */
static void apply_scroll(QuadScrubber *self) {
    if (!self->vadj) return;
    double upper     = gtk_adjustment_get_upper(self->vadj);
    double page_size = gtk_adjustment_get_page_size(self->vadj);
    double value     = CLAMP(self->scroll_fraction * MAX(0.0, upper - page_size),
                             0.0, upper - page_size);
    gtk_adjustment_set_value(self->vadj, value);
}

/* Animate to the start of bucket at idx — used for click jumps */
static void scroll_to_bucket_animated(QuadScrubber *self, int idx) {
    if (idx < 0 || !self->vadj || !self->buckets || idx >= (int)self->buckets->len)
        return;
    cancel_animation(self);

    ScrubberBucket *b = g_ptr_array_index(self->buckets, idx);
    double fraction   = (double)b->position / MAX(1.0, (double)self->total_items);
    double upper      = gtk_adjustment_get_upper(self->vadj);
    double page_size  = gtk_adjustment_get_page_size(self->vadj);

    self->anim.start_value   = gtk_adjustment_get_value(self->vadj);
    self->anim.target_value  = CLAMP(fraction * MAX(0.0, upper - page_size),
                                     0.0, upper - page_size);
    self->anim.start_time_us = g_get_monotonic_time();
    self->anim.tick_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(self), scroll_tick_cb, NULL, NULL);
}

static void update_badge(QuadScrubber *self, int idx, double y) {
    if (!self->badge) return;
    if (idx < 0 || !self->buckets || idx >= (int)self->buckets->len) {
        gtk_widget_set_visible(self->badge, FALSE);
        return;
    }
    ScrubberBucket *b = g_ptr_array_index(self->buckets, idx);
    gtk_label_set_text(GTK_LABEL(self->badge), b->label);
    gtk_widget_set_margin_top(self->badge, MAX(0, (int)(y - BADGE_HEIGHT_HALF)));
    gtk_widget_set_visible(self->badge, TRUE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Gesture Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_enter(GtkEventControllerMotion *ctrl, double x, double y, gpointer data) {
    (void)ctrl; (void)x; (void)y;
    gtk_widget_add_css_class(GTK_WIDGET(data), "scrubber-expanded");
}

static void on_leave(GtkEventControllerMotion *ctrl, gpointer data) {
    (void)ctrl;
    QuadScrubber *self = QUAD_SCRUBBER(data);
    if (!self->dragging)
        gtk_widget_remove_css_class(GTK_WIDGET(self), "scrubber-expanded");
}

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    (void)gesture; (void)x;
    QuadScrubber *self = QUAD_SCRUBBER(data);

    cancel_animation(self);
    self->dragging = TRUE;

    /* Pure absolute: snap to proportional Y position immediately.
     * From here the cursor IS the thumb — no position drift possible. */
    self->scroll_fraction = y_to_fraction(self, y);
    apply_scroll(self);

    /* ns-resize cursor signals "you are dragging a vertical position control" */
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "ns-resize");
    gtk_widget_add_css_class(GTK_WIDGET(self), "scrubber-expanded");

    int idx = bucket_from_fraction(self, self->scroll_fraction);
    if (idx >= 0) {
        self->active_idx = idx;
        update_badge(self, idx, y);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

static void on_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    (void)dx;
    QuadScrubber *self = QUAD_SCRUBBER(data);

    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    (void)start_x;
    double y = start_y + dy;

    /* Absolute: fraction is always derived from current Y.
     * Cursor Y = tick Y at all times — no drift, no confusion. */
    self->scroll_fraction = y_to_fraction(self, y);
    apply_scroll(self);

    int idx = bucket_from_fraction(self, self->scroll_fraction);
    if (idx >= 0 && idx != self->active_idx) {
        self->active_idx = idx;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
    update_badge(self, self->active_idx, y);
}

static void on_drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data) {
    (void)gesture; (void)dx; (void)dy;
    QuadScrubber *self = QUAD_SCRUBBER(data);
    self->dragging = FALSE;

    gtk_widget_set_cursor(GTK_WIDGET(self), NULL); /* restore default cursor */
    if (self->badge) gtk_widget_set_visible(self->badge, FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(self), "scrubber-expanded");
}

static void on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                              gpointer data) {
    (void)gesture; (void)n_press; (void)x;
    QuadScrubber *self = QUAD_SCRUBBER(data);
    if (!self->buckets || self->total_items == 0) return;

    double frac = y_to_fraction(self, y);
    int idx = bucket_from_fraction(self, frac);
    if (idx >= 0) {
        self->active_idx = idx;
        scroll_to_bucket_animated(self, idx);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scroll Tracking — keep state in sync with external scroll
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_vadj_changed(GtkAdjustment *adj, gpointer data) {
    QuadScrubber *self = QUAD_SCRUBBER(data);
    if (self->dragging || !self->buckets || self->total_items == 0) return;

    double value     = gtk_adjustment_get_value(adj);
    double upper     = gtk_adjustment_get_upper(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    double range     = MAX(1.0, upper - page_size);

    self->scroll_fraction = CLAMP(value / range, 0.0, 1.0);

    int new_idx = bucket_from_fraction(self, self->scroll_fraction);
    if (new_idx != self->active_idx) {
        self->active_idx = new_idx;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Snapshot — bell-curve tick marks drawn directly over list content
 *
 * SCRUB_PADDING keeps first/last ticks 12px from widget edges.
 * +0.5 pixel-snapping on all line coordinates for crisp 1px strokes.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void quad_scrubber_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    GTK_WIDGET_CLASS(quad_scrubber_parent_class)->snapshot(widget, snapshot);

    QuadScrubber *self = QUAD_SCRUBBER(widget);
    if (!self->buckets || self->buckets->len == 0 || self->total_items == 0) return;

    int width  = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    if (height <= 0 || width <= 0) return;

    /* All rendering uses native GtkSnapshot ops (GPU render nodes).
     * No cairo fallback — eliminates per-frame CPU surface allocation. */

    float usable_h = (float)MAX(1.0, (double)height - 2.0 * SCRUB_PADDING);
    float rail_x   = floorf((float)(width - RAIL_MARGIN_END)) + 0.5f;

    /* Subtle vertical rail, inset by padding at both ends */
    GdkRGBA rail_color = { 0.55f, 0.55f, 0.55f, 0.22f };
    graphene_rect_t rail_rect = GRAPHENE_RECT_INIT(
        rail_x - 0.5f, (float)SCRUB_PADDING, 1.0f, usable_h);
    gtk_snapshot_append_color(snapshot, &rail_color, &rail_rect);

    PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);
    float prev_y = -999.0f;

    for (guint i = 0; i < self->buckets->len; i++) {
        ScrubberBucket *b = g_ptr_array_index(self->buckets, i);

        float bucket_frac = (float)b->position / fmaxf(1.0f, (float)self->total_items);
        float y           = (float)SCRUB_PADDING + bucket_frac * usable_h;

        if (y < prev_y + MIN_LABEL_SPACING && (int)i != self->active_idx) continue;
        prev_y = y;

        gboolean active = ((int)i == self->active_idx);

        /* Bell-curve weight via LUT — no exp() call */
        float dist = fabsf(bucket_frac - (float)self->scroll_fraction);
        int lut_idx = (int)(dist * 255.0f);
        if (lut_idx > 255) lut_idx = 255;
        float bell = self->bell_lut[lut_idx];

        float tick_len    = (float)TICK_LENGTH_MIN + ((float)TICK_LENGTH_MAX - (float)TICK_LENGTH_MIN) * bell;
        float label_alpha = active ? 1.0f : (0.18f + 0.62f * bell);
        float tick_alpha  = active ? 0.90f : (0.15f + 0.50f * bell);

        /* Use cached font descriptors — no string parsing per frame */
        pango_layout_set_font_description(layout, active ? self->fd_bold : self->fd_normal);
        pango_layout_set_text(layout, b->label, -1);

        int lw, lh;
        pango_layout_get_pixel_size(layout, &lw, &lh);

        float tick_start_x = rail_x - tick_len;
        float label_x      = rail_x - (float)TICK_LENGTH_MAX - (float)LABEL_MARGIN - (float)lw;
        float tick_y       = floorf(y) + 0.5f;

        /* Label — native GtkSnapshot render node (no Cairo surface) */
        float r = active ? 0.93f : 0.70f;
        GdkRGBA label_color = { r, r, r, label_alpha };
        gtk_snapshot_save(snapshot);
        graphene_point_t label_pt = GRAPHENE_POINT_INIT(
            label_x, floorf(y - (float)lh / 2.0f));
        gtk_snapshot_translate(snapshot, &label_pt);
        gtk_snapshot_append_layout(snapshot, layout, &label_color);
        gtk_snapshot_restore(snapshot);

        /* Tick — grows leftward from rail based on bell weight */
        float tick_h = active ? 1.5f : 0.9f;
        GdkRGBA tick_color = { 0.70f, 0.70f, 0.70f, tick_alpha };
        graphene_rect_t tick_rect = GRAPHENE_RECT_INIT(
            floorf(tick_start_x) + 0.5f, tick_y - tick_h * 0.5f,
            rail_x - floorf(tick_start_x) - 0.5f, tick_h);
        gtk_snapshot_append_color(snapshot, &tick_color, &tick_rect);
    }

    g_object_unref(layout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

static void quad_scrubber_dispose(GObject *object) {
    QuadScrubber *self = QUAD_SCRUBBER(object);

    cancel_animation(self);

    if (self->vadj && self->vadj_signal) {
        g_signal_handler_disconnect(self->vadj, self->vadj_signal);
        self->vadj_signal = 0;
    }
    if (self->buckets) {
        g_ptr_array_unref(self->buckets);
        self->buckets = NULL;
    }
    g_clear_pointer(&self->fd_normal, pango_font_description_free);
    g_clear_pointer(&self->fd_bold, pango_font_description_free);

    G_OBJECT_CLASS(quad_scrubber_parent_class)->dispose(object);
}

static void quad_scrubber_class_init(QuadScrubberClass *klass) {
    GObjectClass   *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose  = quad_scrubber_dispose;
    widget_class->snapshot = quad_scrubber_snapshot;

    gtk_widget_class_set_css_name(widget_class, "scrubber");
}

static void quad_scrubber_init(QuadScrubber *self) {
    self->active_idx = -1;
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_VISIBLE);

    /* Cache font descriptors — avoid parsing string per bucket per frame */
    self->fd_normal = pango_font_description_from_string("9");
    self->fd_bold   = pango_font_description_from_string("Bold 9");

    ui_bell_curve_lut(self->bell_lut, 256, BELL_SIGMA);

    GtkEventControllerMotion *motion = GTK_EVENT_CONTROLLER_MOTION(
        gtk_event_controller_motion_new());
    g_signal_connect(motion, "enter", G_CALLBACK(on_enter), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(motion));

    GtkGestureDrag *drag = GTK_GESTURE_DRAG(gtk_gesture_drag_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_drag_begin),  self);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(on_drag_end),    self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));

    GtkGestureClick *click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

    gtk_gesture_group(GTK_GESTURE(drag), GTK_GESTURE(click));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *quad_scrubber_new(void) {
    return g_object_new(QUAD_TYPE_SCRUBBER, NULL);
}

void quad_scrubber_set_list_view(QuadScrubber *self, GtkListView *list_view) {
    g_assert(QUAD_IS_SCRUBBER(self));
    self->list_view = list_view;
}

void quad_scrubber_set_vadj(QuadScrubber *self, GtkAdjustment *vadj) {
    g_assert(QUAD_IS_SCRUBBER(self));
    if (self->vadj && self->vadj_signal) {
        g_signal_handler_disconnect(self->vadj, self->vadj_signal);
        self->vadj_signal = 0;
    }
    self->vadj = vadj;
    if (vadj)
        self->vadj_signal = g_signal_connect(vadj, "value-changed",
                                              G_CALLBACK(on_vadj_changed), self);
}

void quad_scrubber_set_badge(QuadScrubber *self, GtkWidget *badge) {
    g_assert(QUAD_IS_SCRUBBER(self));
    self->badge = badge;
}

void quad_scrubber_set_buckets(QuadScrubber *self, GPtrArray *buckets) {
    g_assert(QUAD_IS_SCRUBBER(self));
    if (self->buckets) g_ptr_array_unref(self->buckets);
    self->buckets    = buckets;
    self->active_idx = -1;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void quad_scrubber_set_total(QuadScrubber *self, guint total_items) {
    g_assert(QUAD_IS_SCRUBBER(self));
    self->total_items = total_items;
}

void quad_scrubber_clear(QuadScrubber *self) {
    g_assert(QUAD_IS_SCRUBBER(self));
    cancel_animation(self);
    if (self->buckets) {
        g_ptr_array_unref(self->buckets);
        self->buckets = NULL;
    }
    self->total_items     = 0;
    self->active_idx      = -1;
    self->scroll_fraction = 0.0;
    if (self->badge) gtk_widget_set_visible(self->badge, FALSE);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
