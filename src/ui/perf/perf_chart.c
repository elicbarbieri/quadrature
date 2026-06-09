/**
 * @file perf_chart.c
 * @brief Custom Cairo chart widgets implementation
 *
 * Grafana-style charts with hover detection and tooltips.
 */

#include "internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Colors (Cyan theme matching app style)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const GdkRGBA COLOR_CYAN = { 0.00, 0.83, 1.00, 1.0 };
static const GdkRGBA COLOR_CYAN_HOVER = { 0.00, 1.00, 1.00, 1.0 };
static const GdkRGBA COLOR_GRID = { 0.20, 0.20, 0.20, 1.0 };
static const GdkRGBA COLOR_TEXT = { 0.53, 0.53, 0.53, 1.0 };
static const GdkRGBA COLOR_TEXT_LIGHT = { 0.80, 0.80, 0.80, 1.0 };
static const GdkRGBA COLOR_BG = { 0.10, 0.10, 0.10, 1.0 };

/* Series colors for line charts */
static const GdkRGBA SERIES_COLORS[4] = {
    { 0.00, 0.83, 1.00, 1.0 }, /* Cyan */
    { 0.20, 0.80, 0.20, 1.0 }, /* Green */
    { 1.00, 0.80, 0.00, 1.0 }, /* Yellow */
    { 1.00, 0.33, 0.33, 1.0 }, /* Red */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfChart Base Class
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char *title;
    GtkWidget *drawing_area;
    GtkPopover *tooltip;
    GtkLabel *tooltip_label;
    int hover_element;
    int width;
    int height;
} PerfChartPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(PerfChart, perf_chart, GTK_TYPE_WIDGET)

static void
perf_chart_draw_func(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)area;
    PerfChart *chart = PERF_CHART(data);
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);
    PerfChartClass *klass = PERF_CHART_GET_CLASS(chart);

    priv->width = width;
    priv->height = height;

    /* Background */
    cairo_set_source_rgb(cr, COLOR_BG.red, COLOR_BG.green, COLOR_BG.blue);
    cairo_paint(cr);

    /* Draw title */
    if (priv->title) {
        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 16);
        cairo_move_to(cr, 8, 20);
        cairo_show_text(cr, priv->title);
    }

    /* Call subclass draw */
    if (klass->draw) {
        klass->draw(chart, cr, width, height);
    }
}

static void
on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    (void)ctrl;
    PerfChart *chart = PERF_CHART(data);
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);
    PerfChartClass *klass = PERF_CHART_GET_CLASS(chart);

    int element = -1;
    if (klass->hit_test) {
        element = klass->hit_test(chart, x, y);
    }

    if (element != priv->hover_element) {
        priv->hover_element = element;
        gtk_widget_queue_draw(priv->drawing_area);

        if (element >= 0 && klass->format_tooltip) {
            char *text = klass->format_tooltip(chart, element);
            if (text) {
                gtk_label_set_text(priv->tooltip_label, text);
                GdkRectangle rect = { (int)x, (int)y, 1, 1 };
                gtk_popover_set_pointing_to(priv->tooltip, &rect);
                gtk_popover_popup(priv->tooltip);
                g_free(text);
            }
        } else {
            gtk_popover_popdown(priv->tooltip);
        }
    }
}

static void
on_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    (void)ctrl;
    PerfChart *chart = PERF_CHART(data);
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);

    if (priv->hover_element >= 0) {
        priv->hover_element = -1;
        gtk_widget_queue_draw(priv->drawing_area);
        gtk_popover_popdown(priv->tooltip);
    }
}

static void
perf_chart_dispose(GObject *obj)
{
    PerfChart *chart = PERF_CHART(obj);
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);

    g_clear_pointer(&priv->title, g_free);
    g_clear_pointer(&priv->drawing_area, gtk_widget_unparent);

    G_OBJECT_CLASS(perf_chart_parent_class)->dispose(obj);
}

static void
perf_chart_class_init(PerfChartClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

    oc->dispose = perf_chart_dispose;
    gtk_widget_class_set_layout_manager_type(wc, GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(wc, "perf-chart");
}

static void
perf_chart_init(PerfChart *chart)
{
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);

    priv->hover_element = -1;

    /* Create drawing area */
    priv->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(priv->drawing_area, TRUE);
    gtk_widget_set_vexpand(priv->drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(priv->drawing_area), perf_chart_draw_func, chart, NULL);
    gtk_widget_set_parent(priv->drawing_area, GTK_WIDGET(chart));

    /* Motion controller for hover */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), chart);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), chart);
    gtk_widget_add_controller(priv->drawing_area, motion);

    /* Tooltip popover */
    priv->tooltip = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_autohide(priv->tooltip, FALSE);
    gtk_widget_set_parent(GTK_WIDGET(priv->tooltip), priv->drawing_area);

    priv->tooltip_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(priv->tooltip_label), "perf-tooltip");
    gtk_popover_set_child(priv->tooltip, GTK_WIDGET(priv->tooltip_label));
}

void
perf_chart_set_title(PerfChart *chart, const char *title)
{
    g_return_if_fail(PERF_IS_CHART(chart));
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);
    g_free(priv->title);
    priv->title = g_strdup(title);
    gtk_widget_queue_draw(priv->drawing_area);
}

void
perf_chart_queue_redraw(PerfChart *chart)
{
    g_return_if_fail(PERF_IS_CHART(chart));
    PerfChartPrivate *priv = perf_chart_get_instance_private(chart);
    gtk_widget_queue_draw(priv->drawing_area);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfHistogramChart
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _PerfHistogramChart {
    PerfChart parent;

    char *unit;
    perf_hist_stats_t stats;

    perf_adaptive_hist_data_t adaptive;
    gboolean use_adaptive;
};

G_DEFINE_FINAL_TYPE(PerfHistogramChart, perf_histogram_chart, PERF_TYPE_CHART)

/* Get bucket range string */
static void
bucket_range_str(int bucket, char *buf, size_t len)
{
    if (bucket == 0) {
        snprintf(buf, len, "0-1");
    } else if (bucket == PERF_HIST_BUCKETS - 1) {
        snprintf(buf, len, "%d+", 1 << (bucket - 1));
    } else {
        snprintf(buf, len, "%d-%d", 1 << (bucket - 1), 1 << bucket);
    }
}

static void
histogram_draw(PerfChart *base, cairo_t *cr, int w, int h)
{
    PerfHistogramChart *chart = PERF_HISTOGRAM_CHART(base);
    PerfChartPrivate *priv = perf_chart_get_instance_private(base);

    int margin_left = 55;
    int margin_right = 10;
    int margin_top = 36;
    int margin_bottom = 48;

    int chart_w = w - margin_left - margin_right;
    int chart_h = h - margin_top - margin_bottom;

    /* Draw grid lines */
    cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    cairo_set_line_width(cr, 1);
    for (int i = 0; i <= 4; i++) {
        int y = margin_top + (chart_h * i / 4);
        cairo_move_to(cr, margin_left, y + 0.5);
        cairo_line_to(cr, w - margin_right, y + 0.5);
        cairo_stroke(cr);
    }

    if (chart->use_adaptive) {
        /* ── Adaptive equal-width bins ── */
        const perf_adaptive_hist_data_t *a = &chart->adaptive;
        int num_bins = (int)a->num_bins;
        if (num_bins < 1)
            return;
        int bar_w = chart_w / num_bins;

        uint32_t max_count = 1;
        for (int i = 0; i < num_bins; i++)
            if (a->bin_counts[i] > max_count)
                max_count = a->bin_counts[i];

        for (int i = 0; i < num_bins; i++) {
            if (a->bin_counts[i] == 0)
                continue;
            double bar_h = ((double)a->bin_counts[i] / (double)max_count) * chart_h;
            int x = margin_left + i * bar_w;
            int y = margin_top + chart_h - (int)bar_h;

            if (i == priv->hover_element)
                cairo_set_source_rgb(
                    cr, COLOR_CYAN_HOVER.red, COLOR_CYAN_HOVER.green, COLOR_CYAN_HOVER.blue);
            else
                cairo_set_source_rgb(cr, COLOR_CYAN.red, COLOR_CYAN.green, COLOR_CYAN.blue);

            cairo_rectangle(cr, x + 1, y, bar_w - 2, bar_h);
            cairo_fill(cr);
        }

        /* X labels — show every few bins to avoid overlap */
        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        cairo_set_font_size(cr, 14);
        int step = (num_bins > 12) ? 4 : (num_bins > 6) ? 2 : 1;
        for (int i = 0; i < num_bins; i += step) {
            int x = margin_left + i * bar_w + bar_w / 2;
            cairo_move_to(cr, x - 10, h - margin_bottom + 20);
            cairo_show_text(cr, a->bin_labels[i]);
        }

        if (chart->unit) {
            cairo_move_to(cr, w - margin_right - 30, h - 5);
            cairo_show_text(cr, chart->unit);
        }

        /* Percentile markers */
        cairo_set_source_rgb(
            cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
        cairo_set_font_size(cr, 15);
        char stats_buf[128];
        snprintf(stats_buf,
                 sizeof(stats_buf),
                 "p50: %.1fms  p90: %.1fms  p99: %.1fms",
                 a->p50,
                 a->p90,
                 a->p99);
        cairo_move_to(cr, margin_left, h - 5);
        cairo_show_text(cr, stats_buf);
    } else {
        /* ── Original log-scale buckets ── */
        perf_hist_stats_t *s = &chart->stats;
        int bar_w = chart_w / PERF_HIST_BUCKETS;

        uint64_t max_count = 1;
        for (int i = 0; i < PERF_HIST_BUCKETS; i++)
            if (s->bucket_counts[i] > max_count)
                max_count = s->bucket_counts[i];

        for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
            if (s->bucket_counts[i] == 0)
                continue;
            double bar_h = ((double)s->bucket_counts[i] / (double)max_count) * chart_h;
            int x = margin_left + i * bar_w;
            int y = margin_top + chart_h - (int)bar_h;

            if (i == priv->hover_element)
                cairo_set_source_rgb(
                    cr, COLOR_CYAN_HOVER.red, COLOR_CYAN_HOVER.green, COLOR_CYAN_HOVER.blue);
            else
                cairo_set_source_rgb(cr, COLOR_CYAN.red, COLOR_CYAN.green, COLOR_CYAN.blue);

            cairo_rectangle(cr, x + 1, y, bar_w - 2, bar_h);
            cairo_fill(cr);
        }

        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        cairo_set_font_size(cr, 14);
        for (int i = 0; i < PERF_HIST_BUCKETS; i += 4) {
            char buf[16];
            bucket_range_str(i, buf, sizeof(buf));
            int x = margin_left + i * bar_w + bar_w / 2;
            cairo_move_to(cr, x - 10, h - margin_bottom + 20);
            cairo_show_text(cr, buf);
        }

        if (chart->unit) {
            cairo_move_to(cr, w - margin_right - 30, h - 5);
            cairo_show_text(cr, chart->unit);
        }

        cairo_set_source_rgb(
            cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
        cairo_set_font_size(cr, 15);
        char stats_buf[128];
        snprintf(stats_buf,
                 sizeof(stats_buf),
                 "p50: %lums  p90: %lums  p99: %lums",
                 (unsigned long)(s->p50 / 1000),
                 (unsigned long)(s->p90 / 1000),
                 (unsigned long)(s->p99 / 1000));
        cairo_move_to(cr, margin_left, h - 5);
        cairo_show_text(cr, stats_buf);
    }
}

static int
histogram_hit_test(PerfChart *base, double x, double y)
{
    PerfHistogramChart *chart = PERF_HISTOGRAM_CHART(base);
    PerfChartPrivate *priv = perf_chart_get_instance_private(base);

    int margin_left = 55;
    int margin_right = 10;
    int margin_top = 36;
    int margin_bottom = 48;

    int chart_w = priv->width - margin_left - margin_right;
    int num_bins = chart->use_adaptive ? (int)chart->adaptive.num_bins : PERF_HIST_BUCKETS;
    if (num_bins < 1)
        return -1;
    int bar_w = chart_w / num_bins;

    if (x < margin_left || x >= priv->width - margin_right)
        return -1;
    if (y < margin_top || y >= priv->height - margin_bottom)
        return -1;

    int bucket = (int)(x - margin_left) / bar_w;
    if (bucket < 0 || bucket >= num_bins)
        return -1;

    return bucket;
}

static char *
histogram_format_tooltip(PerfChart *base, int element)
{
    PerfHistogramChart *chart = PERF_HISTOGRAM_CHART(base);

    if (chart->use_adaptive) {
        const perf_adaptive_hist_data_t *a = &chart->adaptive;
        if (element < 0 || element >= (int)a->num_bins)
            return NULL;
        double percent
            = a->count > 0 ? (double)a->bin_counts[element] / (double)a->count * 100.0 : 0.0;
        return g_strdup_printf("Range: %s %s\nCount: %u\nPercent: %.1f%%",
                               a->bin_labels[element],
                               chart->unit ? chart->unit : "",
                               a->bin_counts[element],
                               percent);
    }

    perf_hist_stats_t *s = &chart->stats;
    if (element < 0 || element >= PERF_HIST_BUCKETS)
        return NULL;

    char range[32];
    bucket_range_str(element, range, sizeof(range));

    double percent
        = s->count > 0 ? (double)s->bucket_counts[element] / (double)s->count * 100.0 : 0.0;

    return g_strdup_printf("Range: %s %s\nCount: %lu\nPercent: %.1f%%",
                           range,
                           chart->unit ? chart->unit : "",
                           (unsigned long)s->bucket_counts[element],
                           percent);
}

static void
perf_histogram_chart_dispose(GObject *obj)
{
    PerfHistogramChart *chart = PERF_HISTOGRAM_CHART(obj);
    g_clear_pointer(&chart->unit, g_free);
    G_OBJECT_CLASS(perf_histogram_chart_parent_class)->dispose(obj);
}

static void
perf_histogram_chart_class_init(PerfHistogramChartClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    PerfChartClass *cc = PERF_CHART_CLASS(klass);

    oc->dispose = perf_histogram_chart_dispose;
    cc->draw = histogram_draw;
    cc->hit_test = histogram_hit_test;
    cc->format_tooltip = histogram_format_tooltip;
}

static void
perf_histogram_chart_init(PerfHistogramChart *chart)
{
    memset(&chart->stats, 0, sizeof(chart->stats));
}

GtkWidget *
perf_histogram_chart_new(const char *title, const char *unit)
{
    PerfHistogramChart *chart = g_object_new(PERF_TYPE_HISTOGRAM, NULL);
    perf_chart_set_title(PERF_CHART(chart), title);
    chart->unit = g_strdup(unit);
    return GTK_WIDGET(chart);
}

void
perf_histogram_chart_set_adaptive_data(PerfHistogramChart *chart,
                                       const perf_adaptive_hist_data_t *data)
{
    g_return_if_fail(PERF_IS_HISTOGRAM_CHART(chart));
    g_return_if_fail(data != NULL);

    chart->use_adaptive = TRUE;
    memcpy(&chart->adaptive, data, sizeof(perf_adaptive_hist_data_t));
    perf_chart_queue_redraw(PERF_CHART(chart));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfLineChart
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _PerfLineChart {
    PerfChart parent;

    int num_series;
    char *series_names[PERF_LINE_CHART_MAX_SERIES];
    GdkRGBA series_colors[PERF_LINE_CHART_MAX_SERIES];
    gboolean series_visible[PERF_LINE_CHART_MAX_SERIES];
    double *series_data[PERF_LINE_CHART_MAX_SERIES];
    size_t series_count[PERF_LINE_CHART_MAX_SERIES];
    size_t series_capacity[PERF_LINE_CHART_MAX_SERIES];
    double y_min;
    double y_max;

    /* Hover tooltip state */
    double sample_interval_ms; /* ms between samples (0 = hover disabled) */
    char *y_unit;              /* unit string for tooltip (e.g. "µs") */
    gboolean log_scale;        /* logarithmic Y axis */

    /* Fixed-capacity right-aligned rendering */
    size_t capacity; /* max display points (0 = stretch-to-fill) */

    /* Smooth scroll state (frame-clock interpolation) */
    gint64 last_data_time_us; /* monotonic time of last set_data() call */
    guint tick_id;            /* GdkFrameClock tick callback id, 0 = none */

    /* Time axis (X-axis labels showing elapsed time) */
    gboolean show_time_axis;
};

G_DEFINE_FINAL_TYPE(PerfLineChart, perf_line_chart, PERF_TYPE_CHART)

/* Map a data value to pixel Y coordinate.
 * Linear: (value - y_min) / y_range → [0, chart_h]
 * Log:    log10(value) / log10(y_max) → [0, chart_h]  (floor at 1.0) */
static inline double
line_chart_val_to_y(PerfLineChart *chart, int margin_top, int chart_h, double value)
{
    double t;
    if (chart->log_scale) {
        double v = value > 1.0 ? value : 1.0;
        double log_max = log10(chart->y_max > 1.0 ? chart->y_max : 10.0);
        t = log10(v) / log_max;
        if (t > 1.0)
            t = 1.0;
    } else {
        double y_range = chart->y_max - chart->y_min;
        if (y_range <= 0)
            y_range = 100;
        t = (value - chart->y_min) / y_range;
    }
    return margin_top + chart_h * (1.0 - t);
}

static gboolean
line_chart_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    (void)clock;
    PerfLineChart *chart = PERF_LINE_CHART(data);

    /* Self-remove after 2x sample interval of no new data */
    if (chart->last_data_time_us > 0 && chart->sample_interval_ms > 0) {
        gint64 now = g_get_monotonic_time();
        double elapsed_ms = (double)(now - chart->last_data_time_us) / 1000.0;
        if (elapsed_ms > chart->sample_interval_ms * 2.0) {
            chart->tick_id = 0;
            return G_SOURCE_REMOVE;
        }
    }

    PerfChartPrivate *priv = perf_chart_get_instance_private(PERF_CHART(chart));
    gtk_widget_queue_draw(priv->drawing_area);
    return G_SOURCE_CONTINUE;
}

static void
line_chart_draw(PerfChart *base, cairo_t *cr, int w, int h)
{
    PerfLineChart *chart = PERF_LINE_CHART(base);

    int margin_left = 60;
    int margin_right = 10;
    int margin_top = 36;
    int margin_bottom = 30;

    int chart_w = w - margin_left - margin_right;
    int chart_h = h - margin_top - margin_bottom;

    /* Draw grid lines + Y-axis labels */
    cairo_set_line_width(cr, 1);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);

    if (chart->log_scale) {
        /* Log grid: one line per decade (1, 10, 100, 1000, ...) */
        double log_max = log10(chart->y_max > 1.0 ? chart->y_max : 10.0);
        int decades = (int)ceil(log_max);
        for (int d = 0; d <= decades; d++) {
            double val = pow(10.0, d);
            double t = (double)d / log_max;
            if (t > 1.0)
                break;
            int y = margin_top + (int)(chart_h * (1.0 - t));

            cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
            cairo_move_to(cr, margin_left, y + 0.5);
            cairo_line_to(cr, w - margin_right, y + 0.5);
            cairo_stroke(cr);

            char buf[16];
            if (val >= 1000)
                snprintf(buf, sizeof(buf), "%.0fk", val / 1000);
            else
                snprintf(buf, sizeof(buf), "%.0f", val);
            cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
            cairo_move_to(cr, 5, y + 4);
            cairo_show_text(cr, buf);
        }
    } else {
        /* Linear grid: 5 evenly-spaced lines */
        double y_range = chart->y_max - chart->y_min;
        if (y_range <= 0)
            y_range = 100;
        for (int i = 0; i <= 4; i++) {
            int y = margin_top + (chart_h * i / 4);
            cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
            cairo_move_to(cr, margin_left, y + 0.5);
            cairo_line_to(cr, w - margin_right, y + 0.5);
            cairo_stroke(cr);

            double val = chart->y_max - (y_range * i / 4);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f", val);
            cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
            cairo_move_to(cr, 5, y + 4);
            cairo_show_text(cr, buf);
        }
    }

    /* Determine x_step: fixed from capacity, or stretch-to-fill */
    size_t max_count = 0;
    for (int s = 0; s < chart->num_series; s++) {
        if (chart->series_visible[s] && chart->series_count[s] > max_count)
            max_count = chart->series_count[s];
    }
    size_t span = (chart->capacity >= 2) ? chart->capacity : max_count;
    double x_step = (span >= 2) ? (double)chart_w / (double)(span - 1) : 0;

    /* Smooth scroll: fractional pixel offset since last data update.
     * Clamped to [0, 1 sample] so we never scroll more than one data point
     * ahead — prevents data from drifting off-screen between updates. */
    double px_offset = 0;
    if (chart->sample_interval_ms > 0 && chart->last_data_time_us > 0 && x_step > 0) {
        double elapsed_us = (double)(g_get_monotonic_time() - chart->last_data_time_us);
        double sample_offset = elapsed_us / (chart->sample_interval_ms * 1000.0);
        if (sample_offset > 2.0)
            sample_offset = 2.0;
        px_offset = sample_offset * x_step;
    }

    /* Clip to chart area so scrolled-off points don't draw in margins */
    cairo_save(cr);
    cairo_rectangle(cr, margin_left, margin_top, chart_w, chart_h);
    cairo_clip(cr);

    /* Draw each series */
    for (int s = 0; s < chart->num_series; s++) {
        if (!chart->series_visible[s])
            continue;
        if (!chart->series_data[s] || chart->series_count[s] < 2)
            continue;

        GdkRGBA *color = &chart->series_colors[s];
        cairo_set_source_rgba(cr, color->red, color->green, color->blue, 0.8);
        cairo_set_line_width(cr, 1.5);

        size_t count = chart->series_count[s];

        /* Right-align: newest point (index count-1) at right edge of chart,
         * older points extend leftward at fixed x_step intervals */
        double x_base = margin_left + chart_w - (double)(count - 1) * x_step - px_offset;

        cairo_move_to(
            cr, x_base, line_chart_val_to_y(chart, margin_top, chart_h, chart->series_data[s][0]));

        for (size_t i = 1; i < count; i++) {
            double x = x_base + (double)i * x_step;
            double y = line_chart_val_to_y(chart, margin_top, chart_h, chart->series_data[s][i]);
            cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }

    cairo_restore(cr);

    /* Draw hover crosshair */
    PerfChartPrivate *priv = perf_chart_get_instance_private(base);
    if (chart->sample_interval_ms > 0 && priv->hover_element >= 0) {
        if (max_count >= 2 && (size_t)priv->hover_element < max_count) {
            /* Right-aligned: same x_base as series drawing (without scroll offset) */
            double hover_x_base = margin_left + chart_w - (double)(max_count - 1) * x_step;
            double cx = hover_x_base + priv->hover_element * x_step;

            cairo_set_source_rgba(
                cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue, 0.5);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, cx + 0.5, margin_top);
            cairo_line_to(cr, cx + 0.5, margin_top + chart_h);
            cairo_stroke(cr);

            for (int s = 0; s < chart->num_series; s++) {
                if (!chart->series_visible[s] || !chart->series_data[s])
                    continue;
                if ((size_t)priv->hover_element >= chart->series_count[s])
                    continue;
                double val = chart->series_data[s][priv->hover_element];
                double dy = line_chart_val_to_y(chart, margin_top, chart_h, val);
                GdkRGBA *color = &chart->series_colors[s];
                cairo_set_source_rgb(cr, color->red, color->green, color->blue);
                cairo_arc(cr, cx, dy, 3, 0, 2 * G_PI);
                cairo_fill(cr);
            }
        }
    }

    /* Draw legend */
    cairo_set_font_size(cr, 14);
    int legend_x = w - margin_right - 10;
    for (int s = chart->num_series - 1; s >= 0; s--) {
        if (!chart->series_visible[s] || !chart->series_names[s])
            continue;

        cairo_text_extents_t ext;
        cairo_text_extents(cr, chart->series_names[s], &ext);
        legend_x -= (int)ext.width + 20;

        GdkRGBA *color = &chart->series_colors[s];
        cairo_set_source_rgb(cr, color->red, color->green, color->blue);
        cairo_rectangle(cr, legend_x, margin_top - 12, 8, 8);
        cairo_fill(cr);

        cairo_set_source_rgb(
            cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
        cairo_move_to(cr, legend_x + 12, margin_top - 4);
        cairo_show_text(cr, chart->series_names[s]);
    }

    /* Draw time axis (X-axis labels) */
    if (chart->show_time_axis && chart->sample_interval_ms > 0 && max_count >= 2) {
        double total_sec = (double)(max_count - 1) * chart->sample_interval_ms / 1000.0;
        /* Choose tick interval: target ~120-200px spacing */
        double tick_sec;
        if (total_sec <= 60)
            tick_sec = 10;
        else if (total_sec <= 300)
            tick_sec = 30;
        else if (total_sec <= 600)
            tick_sec = 60;
        else
            tick_sec = 120;

        int y_axis = margin_top + chart_h;
        cairo_set_font_size(cr, 12);

        /* Walk from right (newest = 0s ago) to left */
        for (double ago = 0; ago <= total_sec + 0.01; ago += tick_sec) {
            double frac = ago / total_sec; /* 0 = right edge, 1 = left edge */
            double x_pos = margin_left + chart_w * (1.0 - frac);

            /* Tick mark */
            cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
            cairo_move_to(cr, x_pos + 0.5, y_axis);
            cairo_line_to(cr, x_pos + 0.5, y_axis + 4);
            cairo_stroke(cr);

            /* Label */
            char buf[16];
            int mins = (int)(ago / 60);
            int secs = (int)ago % 60;
            if (ago < 0.5)
                snprintf(buf, sizeof(buf), "now");
            else if (secs == 0)
                snprintf(buf, sizeof(buf), "-%dm", mins);
            else
                snprintf(buf, sizeof(buf), "-%d:%02d", mins, secs);

            cairo_text_extents_t ext;
            cairo_text_extents(cr, buf, &ext);
            double lx = x_pos - ext.width / 2;
            if (lx < margin_left)
                lx = margin_left;
            if (lx + ext.width > w - margin_right)
                lx = w - margin_right - ext.width;

            cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
            cairo_move_to(cr, lx, y_axis + 16);
            cairo_show_text(cr, buf);
        }
    }
}

static int
line_chart_hit_test(PerfChart *base, double x, double y)
{
    PerfLineChart *chart = PERF_LINE_CHART(base);
    if (chart->sample_interval_ms <= 0)
        return -1; /* hover disabled */

    PerfChartPrivate *priv = perf_chart_get_instance_private(base);
    int margin_left = 60, margin_right = 10, margin_top = 36, margin_bottom = 30;
    int chart_w = priv->width - margin_left - margin_right;
    int chart_h = priv->height - margin_top - margin_bottom;
    if (chart_w <= 0 || chart_h <= 0)
        return -1;

    if (x < margin_left || x >= priv->width - margin_right)
        return -1;
    if (y < margin_top || y >= priv->height - margin_bottom)
        return -1;

    /* Find the longest visible series to determine point count */
    size_t max_count = 0;
    for (int s = 0; s < chart->num_series; s++) {
        if (chart->series_visible[s] && chart->series_count[s] > max_count)
            max_count = chart->series_count[s];
    }
    if (max_count < 2)
        return -1;

    size_t span = (chart->capacity >= 2) ? chart->capacity : max_count;
    double x_step = (double)chart_w / (double)(span - 1);

    /* Right-aligned: data starts at right edge, extends left */
    double x_base = margin_left + chart_w - (double)(max_count - 1) * x_step;
    double rel_x = x - x_base + x_step * 0.5;
    if (rel_x < 0)
        return -1;
    int idx = (int)(rel_x / x_step);
    if (idx < 0)
        idx = 0;
    if ((size_t)idx >= max_count)
        idx = (int)(max_count - 1);
    return idx;
}

static char *
line_chart_format_tooltip(PerfChart *base, int element)
{
    PerfLineChart *chart = PERF_LINE_CHART(base);
    if (element < 0 || chart->sample_interval_ms <= 0)
        return NULL;

    /* Find the longest visible series to compute time offset */
    size_t max_count = 0;
    for (int s = 0; s < chart->num_series; s++) {
        if (chart->series_visible[s] && chart->series_count[s] > max_count)
            max_count = chart->series_count[s];
    }
    if (max_count == 0)
        return NULL;

    /* Time offset: rightmost point = now, leftmost = -(count-1)*interval */
    double seconds_ago
        = (double)((int)max_count - 1 - element) * chart->sample_interval_ms / 1000.0;

    /* Build value readout for each visible series, time offset at bottom */
    GString *tip = g_string_new(NULL);
    const char *unit = chart->y_unit ? chart->y_unit : "";
    gboolean first = TRUE;
    for (int s = 0; s < chart->num_series; s++) {
        if (!chart->series_visible[s] || !chart->series_data[s])
            continue;
        if ((size_t)element >= chart->series_count[s])
            continue;
        double val = chart->series_data[s][element];
        const char *name = chart->series_names[s] ? chart->series_names[s] : "";
        if (!first)
            g_string_append_c(tip, '\n');
        g_string_append_printf(tip, "%s: %.1f %s", name, val, unit);
        first = FALSE;
    }
    g_string_append_printf(tip, "\n−%.1fs", seconds_ago);
    return g_string_free(tip, FALSE);
}

static void
perf_line_chart_dispose(GObject *obj)
{
    PerfLineChart *chart = PERF_LINE_CHART(obj);
    if (chart->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(chart), chart->tick_id);
        chart->tick_id = 0;
    }
    for (int i = 0; i < PERF_LINE_CHART_MAX_SERIES; i++) {
        g_clear_pointer(&chart->series_names[i], g_free);
        g_clear_pointer(&chart->series_data[i], g_free);
    }
    g_clear_pointer(&chart->y_unit, g_free);
    G_OBJECT_CLASS(perf_line_chart_parent_class)->dispose(obj);
}

static void
perf_line_chart_class_init(PerfLineChartClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    PerfChartClass *cc = PERF_CHART_CLASS(klass);

    oc->dispose = perf_line_chart_dispose;
    cc->draw = line_chart_draw;
    cc->hit_test = line_chart_hit_test;
    cc->format_tooltip = line_chart_format_tooltip;
}

static void
perf_line_chart_init(PerfLineChart *chart)
{
    chart->y_min = 0;
    chart->y_max = 100;
    for (int i = 0; i < PERF_LINE_CHART_MAX_SERIES; i++) {
        chart->series_colors[i] = SERIES_COLORS[i % 4];
        chart->series_visible[i] = TRUE;
    }
}

GtkWidget *
perf_line_chart_new(const char *title, int num_series)
{
    g_return_val_if_fail(num_series > 0 && num_series <= PERF_LINE_CHART_MAX_SERIES, NULL);

    PerfLineChart *chart = g_object_new(PERF_TYPE_LINE_CHART, NULL);
    perf_chart_set_title(PERF_CHART(chart), title);
    chart->num_series = num_series;
    return GTK_WIDGET(chart);
}

void
perf_line_chart_set_series(PerfLineChart *chart, int series, const char *name, const GdkRGBA *color)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);

    g_free(chart->series_names[series]);
    chart->series_names[series] = g_strdup(name);
    if (color)
        chart->series_colors[series] = *color;
}

void
perf_line_chart_set_data(PerfLineChart *chart, int series, const double *values, size_t count)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);

    /* Skip if data unchanged */
    if (chart->series_count[series] == count && chart->series_data[series]
        && memcmp(chart->series_data[series], values, count * sizeof(double)) == 0)
        return;

    /* Grow buffer if needed (never shrink — avoids steady-state allocs) */
    if (count > chart->series_capacity[series]) {
        g_free(chart->series_data[series]);
        chart->series_data[series] = g_malloc(count * sizeof(double));
        chart->series_capacity[series] = count;
    }
    memcpy(chart->series_data[series], values, count * sizeof(double));
    chart->series_count[series] = count;
    perf_chart_queue_redraw(PERF_CHART(chart));
}

void
perf_line_chart_scroll_reset(PerfLineChart *chart)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->last_data_time_us = g_get_monotonic_time();

    /* Restart tick callback for smooth scroll animation (self-removes when idle) */
    if (chart->tick_id == 0 && chart->sample_interval_ms > 0)
        chart->tick_id
            = gtk_widget_add_tick_callback(GTK_WIDGET(chart), line_chart_tick, chart, NULL);
}

void
perf_line_chart_set_range(PerfLineChart *chart, double min, double max)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->y_min = min;
    chart->y_max = max;
}

void
perf_line_chart_set_series_visible(PerfLineChart *chart, int series, gboolean visible)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);
    if (chart->series_visible[series] != visible) {
        chart->series_visible[series] = visible;
        perf_chart_queue_redraw(PERF_CHART(chart));
    }
}

void
perf_line_chart_set_hover(PerfLineChart *chart, double sample_interval_ms, const char *y_unit)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->sample_interval_ms = sample_interval_ms;
    g_free(chart->y_unit);
    chart->y_unit = g_strdup(y_unit);
}

void
perf_line_chart_set_log_scale(PerfLineChart *chart, gboolean log_scale)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->log_scale = log_scale;
    perf_chart_queue_redraw(PERF_CHART(chart));
}

void
perf_line_chart_set_capacity(PerfLineChart *chart, size_t capacity)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->capacity = capacity;
}

void
perf_line_chart_set_time_axis(PerfLineChart *chart, gboolean show)
{
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->show_time_axis = show;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfStackedAreaChart — Stacked area chart for memory breakdown
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_STACKED_MAX_SERIES 16

/* Extended color palette for stacked area series */
static const GdkRGBA STACKED_COLORS[PERF_STACKED_MAX_SERIES] = {
    { 0.00, 0.83, 1.00, 0.7 }, /* 0: Cyan — audio cache */
    { 0.30, 0.69, 0.31, 0.7 }, /* 1: Green — lib cache 0 */
    { 0.13, 0.59, 0.95, 0.7 }, /* 2: Blue — lib cache 1 */
    { 0.61, 0.15, 0.69, 0.7 }, /* 3: Purple — lib cache 2 */
    { 0.97, 0.62, 0.04, 0.7 }, /* 4: Orange — artwork textures */
    { 0.96, 0.76, 0.19, 0.5 }, /* 5: Yellow — atlas mmap 0 */
    { 0.80, 0.86, 0.22, 0.5 }, /* 6: Lime — atlas mmap 1 */
    { 0.55, 0.76, 0.29, 0.5 }, /* 7: Light green — atlas mmap 2 */
    { 0.40, 0.60, 0.80, 0.7 }, /* 8-15: neutral fill */
    { 0.50, 0.50, 0.70, 0.7 }, { 0.60, 0.40, 0.60, 0.7 }, { 0.70, 0.50, 0.40, 0.7 },
    { 0.50, 0.70, 0.60, 0.7 }, { 0.60, 0.60, 0.50, 0.7 }, { 0.40, 0.50, 0.70, 0.7 },
    { 0.70, 0.40, 0.50, 0.7 },
};

struct _PerfStackedAreaChart {
    PerfChart parent;

    int num_series;
    char *series_names[PERF_STACKED_MAX_SERIES];
    GdkRGBA series_colors[PERF_STACKED_MAX_SERIES];
    gboolean series_visible[PERF_STACKED_MAX_SERIES];
    double *series_data[PERF_STACKED_MAX_SERIES];
    size_t series_count[PERF_STACKED_MAX_SERIES];
    size_t series_capacity[PERF_STACKED_MAX_SERIES];

    double y_max;              /* manual ceiling in MB (0 = auto-scale) */
    double sample_interval_ms; /* for hover tooltip */
    char *y_unit;
};

G_DEFINE_FINAL_TYPE(PerfStackedAreaChart, perf_stacked_area_chart, PERF_TYPE_CHART)

static void
stacked_area_draw(PerfChart *base, cairo_t *cr, int w, int h)
{
    PerfStackedAreaChart *chart = PERF_STACKED_AREA_CHART(base);

    int margin_left = 60, margin_right = 10, margin_top = 36, margin_bottom = 30;
    int chart_w = w - margin_left - margin_right;
    int chart_h = h - margin_top - margin_bottom;
    if (chart_w <= 0 || chart_h <= 0)
        return;

    /* Find max data count across visible series */
    size_t max_count = 0;
    for (int s = 0; s < chart->num_series; s++) {
        if (chart->series_visible[s] && chart->series_count[s] > max_count)
            max_count = chart->series_count[s];
    }
    if (max_count < 2)
        return;

    /* Compute stacked totals at each sample to find Y ceiling */
    double auto_max = 0;
    for (size_t i = 0; i < max_count; i++) {
        double stack = 0;
        for (int s = 0; s < chart->num_series; s++) {
            if (!chart->series_visible[s] || !chart->series_data[s])
                continue;
            if (i < chart->series_count[s])
                stack += chart->series_data[s][i];
        }
        if (stack > auto_max)
            auto_max = stack;
    }
    double y_ceil = (chart->y_max > 0) ? chart->y_max : (auto_max > 0 ? auto_max * 1.15 : 100);

    /* Y-axis grid: 5 lines, linear */
    cairo_set_line_width(cr, 1);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    for (int i = 0; i <= 4; i++) {
        int y = margin_top + chart_h * i / 4;
        cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
        cairo_move_to(cr, margin_left, y + 0.5);
        cairo_line_to(cr, w - margin_right, y + 0.5);
        cairo_stroke(cr);

        double val = y_ceil - (y_ceil * i / 4);
        char buf[16];
        if (val >= 1024)
            snprintf(buf, sizeof(buf), "%.1f GB", val / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%.0f MB", val);
        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        cairo_move_to(cr, 5, y + 4);
        cairo_show_text(cr, buf);
    }

    double x_step = (double)chart_w / (double)(max_count - 1);

    /* Clip to chart area */
    cairo_save(cr);
    cairo_rectangle(cr, margin_left, margin_top, chart_w, chart_h);
    cairo_clip(cr);

    /* ─── Stacked area band fill ───
     *
     * Each series s is drawn as a filled band between its floor (cumulative
     * sum of series 0..s-1) and its ceiling (floor + series[s] value).
     *
     * Drawing order: series 0 first (bottom), working up. Each band is a
     * closed polygon: trace ceiling left→right, then floor right→left.
     *
     * We pre-compute cumulative floors into cum_floor[max_count], then for
     * each series build cum_ceil = cum_floor + series_data. After drawing
     * the band, cum_floor is updated to cum_ceil for the next series. */
    double *cum_floor = g_alloca(max_count * sizeof(double));
    memset(cum_floor, 0, max_count * sizeof(double));

    for (int s = 0; s < chart->num_series; s++) {
        if (!chart->series_visible[s] || !chart->series_data[s])
            continue;

        const GdkRGBA *c = &chart->series_colors[s];

        /* Trace ceiling: left → right */
        cairo_new_path(cr);
        for (size_t i = 0; i < max_count; i++) {
            double val = (i < chart->series_count[s]) ? chart->series_data[s][i] : 0.0;
            double ceil_val = cum_floor[i] + val;
            double x = margin_left + (double)i * x_step;
            double y = margin_top + chart_h * (1.0 - ceil_val / y_ceil);
            if (i == 0)
                cairo_move_to(cr, x, y);
            else
                cairo_line_to(cr, x, y);
        }
        /* Trace floor: right → left */
        for (size_t i = max_count; i > 0; i--) {
            double x = margin_left + (double)(i - 1) * x_step;
            double y = margin_top + chart_h * (1.0 - cum_floor[i - 1] / y_ceil);
            cairo_line_to(cr, x, y);
        }
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha);
        cairo_fill(cr);

        /* Update floor for next series */
        for (size_t i = 0; i < max_count; i++) {
            double val = (i < chart->series_count[s]) ? chart->series_data[s][i] : 0.0;
            cum_floor[i] += val;
        }
    }

    cairo_restore(cr);

    /* Draw legend (right-aligned, compact) */
    cairo_set_font_size(cr, 12);
    int legend_x = w - margin_right - 10;
    for (int s = chart->num_series - 1; s >= 0; s--) {
        if (!chart->series_visible[s] || !chart->series_names[s])
            continue;

        cairo_text_extents_t ext;
        cairo_text_extents(cr, chart->series_names[s], &ext);
        legend_x -= (int)ext.width + 18;

        const GdkRGBA *c = &chart->series_colors[s];
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha);
        cairo_rectangle(cr, legend_x, margin_top - 12, 8, 8);
        cairo_fill(cr);

        cairo_set_source_rgb(
            cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
        cairo_move_to(cr, legend_x + 12, margin_top - 4);
        cairo_show_text(cr, chart->series_names[s]);
    }

    /* Draw hover crosshair */
    PerfChartPrivate *priv = perf_chart_get_instance_private(base);
    if (chart->sample_interval_ms > 0 && priv->hover_element >= 0
        && (size_t)priv->hover_element < max_count) {
        double cx = margin_left + priv->hover_element * x_step;

        cairo_set_source_rgba(
            cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue, 0.5);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, cx + 0.5, margin_top);
        cairo_line_to(cr, cx + 0.5, margin_top + chart_h);
        cairo_stroke(cr);
    }

    /* Draw time axis (X-axis labels) — always shown for stacked area */
    if (chart->sample_interval_ms > 0 && max_count >= 2) {
        double total_sec = (double)(max_count - 1) * chart->sample_interval_ms / 1000.0;
        double tick_sec;
        if (total_sec <= 60)
            tick_sec = 10;
        else if (total_sec <= 300)
            tick_sec = 30;
        else if (total_sec <= 600)
            tick_sec = 60;
        else
            tick_sec = 120;

        int y_axis = margin_top + chart_h;
        cairo_set_font_size(cr, 12);

        for (double ago = 0; ago <= total_sec + 0.01; ago += tick_sec) {
            double frac = ago / total_sec;
            double x_pos = margin_left + chart_w * (1.0 - frac);

            cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
            cairo_move_to(cr, x_pos + 0.5, y_axis);
            cairo_line_to(cr, x_pos + 0.5, y_axis + 4);
            cairo_stroke(cr);

            char buf[16];
            int mins = (int)(ago / 60);
            int secs = (int)ago % 60;
            if (ago < 0.5)
                snprintf(buf, sizeof(buf), "now");
            else if (secs == 0)
                snprintf(buf, sizeof(buf), "-%dm", mins);
            else
                snprintf(buf, sizeof(buf), "-%d:%02d", mins, secs);

            cairo_text_extents_t ext;
            cairo_text_extents(cr, buf, &ext);
            double lx = x_pos - ext.width / 2;
            if (lx < margin_left)
                lx = margin_left;
            if (lx + ext.width > w - margin_right)
                lx = w - margin_right - ext.width;

            cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
            cairo_move_to(cr, lx, y_axis + 16);
            cairo_show_text(cr, buf);
        }
    }
}

static int
stacked_area_hit_test(PerfChart *base, double x, double y)
{
    (void)y;
    PerfStackedAreaChart *chart = PERF_STACKED_AREA_CHART(base);
    if (chart->sample_interval_ms <= 0)
        return -1;

    PerfChartPrivate *priv = perf_chart_get_instance_private(base);
    int margin_left = 60, margin_right = 10, margin_top = 36, margin_bottom = 30;
    int chart_w = priv->width - margin_left - margin_right;
    int chart_h = priv->height - margin_top - margin_bottom;
    if (chart_w <= 0 || chart_h <= 0)
        return -1;
    if (x < margin_left || x > priv->width - margin_right)
        return -1;
    if (y < margin_top || y > margin_top + chart_h)
        return -1;

    size_t max_count = 0;
    for (int s = 0; s < chart->num_series; s++) {
        if (chart->series_visible[s] && chart->series_count[s] > max_count)
            max_count = chart->series_count[s];
    }
    if (max_count < 2)
        return -1;
    double x_step = (double)chart_w / (double)(max_count - 1);
    int idx = (int)((x - margin_left) / x_step + 0.5);
    if (idx < 0)
        idx = 0;
    if ((size_t)idx >= max_count)
        idx = (int)(max_count - 1);
    return idx;
}

static char *
stacked_area_format_tooltip(PerfChart *base, int element)
{
    PerfStackedAreaChart *chart = PERF_STACKED_AREA_CHART(base);
    GString *s = g_string_new(NULL);
    double total = 0;

    for (int i = 0; i < chart->num_series; i++) {
        if (!chart->series_visible[i] || !chart->series_data[i])
            continue;
        if ((size_t)element >= chart->series_count[i])
            continue;
        double val = chart->series_data[i][element];
        total += val;
        if (chart->series_names[i])
            g_string_append_printf(s,
                                   "%s: %.1f %s\n",
                                   chart->series_names[i],
                                   val,
                                   chart->y_unit ? chart->y_unit : "");
    }
    g_string_append_printf(s, "Total: %.1f %s", total, chart->y_unit ? chart->y_unit : "");
    return g_string_free(s, FALSE);
}

static void
perf_stacked_area_chart_dispose(GObject *obj)
{
    PerfStackedAreaChart *chart = PERF_STACKED_AREA_CHART(obj);
    for (int i = 0; i < PERF_STACKED_MAX_SERIES; i++) {
        g_clear_pointer(&chart->series_names[i], g_free);
        g_clear_pointer(&chart->series_data[i], g_free);
    }
    g_clear_pointer(&chart->y_unit, g_free);
    G_OBJECT_CLASS(perf_stacked_area_chart_parent_class)->dispose(obj);
}

static void
perf_stacked_area_chart_class_init(PerfStackedAreaChartClass *klass)
{
    PerfChartClass *chart_class = PERF_CHART_CLASS(klass);
    chart_class->draw = stacked_area_draw;
    chart_class->hit_test = stacked_area_hit_test;
    chart_class->format_tooltip = stacked_area_format_tooltip;

    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->dispose = perf_stacked_area_chart_dispose;
}

static void
perf_stacked_area_chart_init(PerfStackedAreaChart *chart)
{
    for (int i = 0; i < PERF_STACKED_MAX_SERIES; i++) {
        chart->series_colors[i] = STACKED_COLORS[i];
        chart->series_visible[i] = FALSE;
    }
}

GtkWidget *
perf_stacked_area_chart_new(const char *title, int num_series)
{
    PerfStackedAreaChart *chart = g_object_new(PERF_TYPE_STACKED_AREA_CHART, NULL);
    perf_chart_set_title(PERF_CHART(chart), title);
    chart->num_series = CLAMP(num_series, 1, PERF_STACKED_MAX_SERIES);
    for (int i = 0; i < chart->num_series; i++)
        chart->series_visible[i] = TRUE;
    return GTK_WIDGET(chart);
}

void
perf_stacked_area_chart_set_series(PerfStackedAreaChart *chart,
                                   int series,
                                   const char *name,
                                   const GdkRGBA *color)
{
    g_return_if_fail(PERF_IS_STACKED_AREA_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);
    g_free(chart->series_names[series]);
    chart->series_names[series] = g_strdup(name);
    if (color)
        chart->series_colors[series] = *color;
}

void
perf_stacked_area_chart_set_data(PerfStackedAreaChart *chart,
                                 int series,
                                 const double *values,
                                 size_t count)
{
    g_return_if_fail(PERF_IS_STACKED_AREA_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);

    /* Skip if data unchanged */
    if (chart->series_count[series] == count && chart->series_data[series]
        && memcmp(chart->series_data[series], values, count * sizeof(double)) == 0)
        return;

    /* Grow buffer if needed */
    if (count > chart->series_capacity[series]) {
        g_free(chart->series_data[series]);
        chart->series_data[series] = g_malloc(count * sizeof(double));
        chart->series_capacity[series] = count;
    }
    memcpy(chart->series_data[series], values, count * sizeof(double));
    chart->series_count[series] = count;
    perf_chart_queue_redraw(PERF_CHART(chart));
}

void
perf_stacked_area_chart_set_y_max(PerfStackedAreaChart *chart, double max_mb)
{
    g_return_if_fail(PERF_IS_STACKED_AREA_CHART(chart));
    chart->y_max = max_mb;
}

void
perf_stacked_area_chart_set_hover(PerfStackedAreaChart *chart, double interval_ms, const char *unit)
{
    g_return_if_fail(PERF_IS_STACKED_AREA_CHART(chart));
    chart->sample_interval_ms = interval_ms;
    g_free(chart->y_unit);
    chart->y_unit = g_strdup(unit);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfTimelineChart (Fault Event Timeline)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t timestamp_us;
    int lane;
    int severity; /* 0=info, 1=warn, 2=error */
    char type[32];
    char tooltip[256];
} timeline_event_t;

struct _PerfTimelineChart {
    PerfChart parent;

    int num_lanes;
    timeline_event_t *events; /* heap-allocated; PERF_TIMELINE_MAX_EVENTS entries */
    int event_count;
    int event_write;    /* Ring buffer write index */
    int selected_event; /* Persistent selection from click, -1 = none */
};

G_DEFINE_FINAL_TYPE(PerfTimelineChart, perf_timeline_chart, PERF_TYPE_CHART)

/* Severity colors */
static const GdkRGBA SEVERITY_COLORS[] = {
    { 0.29, 0.87, 0.50, 0.8 }, /* Info: green */
    { 0.98, 0.75, 0.14, 0.8 }, /* Warn: yellow */
    { 0.97, 0.44, 0.44, 0.9 }, /* Error: red */
};

static void
timeline_draw(PerfChart *base, cairo_t *cr, int w, int h)
{
    PerfTimelineChart *chart = PERF_TIMELINE_CHART(base);

    int margin_left = 60;
    int margin_right = 10;
    int margin_top = 36;
    int margin_bottom = 28;

    int chart_w = w - margin_left - margin_right;
    int chart_h = h - margin_top - margin_bottom;

    if (chart_w <= 0 || chart_h <= 0)
        return;

    int lane_h = chart_h / (chart->num_lanes > 0 ? chart->num_lanes : 1);

    /* Draw lane separators and labels */
    cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    cairo_set_line_width(cr, 1);
    cairo_set_font_size(cr, 14);

    static const char *lane_labels[] = { "Ch1", "Ch2", "Ch3", "Ch4", "Sys" };
    for (int i = 0; i < chart->num_lanes; i++) {
        int y = margin_top + i * lane_h;

        /* Lane separator */
        cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
        cairo_move_to(cr, margin_left, y + 0.5);
        cairo_line_to(cr, w - margin_right, y + 0.5);
        cairo_stroke(cr);

        /* Lane label */
        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        const char *label = (i < 5) ? lane_labels[i] : "?";
        cairo_move_to(cr, 8, y + lane_h / 2 + 3);
        cairo_show_text(cr, label);
    }

    /* Bottom separator */
    cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    cairo_move_to(cr, margin_left, margin_top + chart_h + 0.5);
    cairo_line_to(cr, w - margin_right, margin_top + chart_h + 0.5);
    cairo_stroke(cr);

    /* Determine time window (last 5 minutes) */
    uint64_t now = (uint64_t)g_get_monotonic_time();
    uint64_t window_us = 5 * 60 * 1000000ULL; /* 5 minutes */
    uint64_t start_us = (now > window_us) ? now - window_us : 0;

    /* Draw events as markers */
    int count = chart->event_count;
    int start_idx = (count > PERF_TIMELINE_MAX_EVENTS) ? chart->event_write : 0;
    int total = (count > PERF_TIMELINE_MAX_EVENTS) ? PERF_TIMELINE_MAX_EVENTS : count;

    for (int i = 0; i < total; i++) {
        int idx = (start_idx + i) % PERF_TIMELINE_MAX_EVENTS;
        const timeline_event_t *ev = &chart->events[idx];

        if (ev->timestamp_us < start_us)
            continue;
        if (ev->lane < 0 || ev->lane >= chart->num_lanes)
            continue;

        /* Map timestamp to x position */
        double t = (double)(ev->timestamp_us - start_us) / (double)window_us;
        double x = margin_left + t * chart_w;
        double y = margin_top + ev->lane * lane_h + lane_h / 2.0;

        /* Draw circle marker */
        int sev = ev->severity;
        if (sev < 0)
            sev = 0;
        if (sev > 2)
            sev = 2;
        const GdkRGBA *color = &SEVERITY_COLORS[sev];
        cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);

        double radius = (sev == 2) ? 4.0 : 3.0;
        cairo_arc(cr, x, y, radius, 0, 2.0 * G_PI);
        cairo_fill(cr);
    }

    /* Draw selection ring for clicked event */
    if (chart->selected_event >= 0 && chart->selected_event < PERF_TIMELINE_MAX_EVENTS) {
        const timeline_event_t *sel = &chart->events[chart->selected_event];
        if (sel->timestamp_us >= start_us && sel->lane >= 0 && sel->lane < chart->num_lanes) {
            double t = (double)(sel->timestamp_us - start_us) / (double)window_us;
            double sx = margin_left + t * chart_w;
            double sy = margin_top + sel->lane * lane_h + lane_h / 2.0;
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
            cairo_set_line_width(cr, 2.0);
            cairo_arc(cr, sx, sy, 7.0, 0, 2.0 * G_PI);
            cairo_stroke(cr);
        }
    }

    /* Time axis labels */
    cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
    cairo_set_font_size(cr, 14);
    for (int i = 0; i <= 5; i++) {
        double x = margin_left + (chart_w * i / 5.0);
        int mins_ago = 5 - i;
        char buf[16];
        snprintf(buf, sizeof(buf), "-%dm", mins_ago);
        if (mins_ago == 0)
            snprintf(buf, sizeof(buf), "now");
        cairo_move_to(cr, x - 8, h - 5);
        cairo_show_text(cr, buf);
    }
}

static int
timeline_hit_test(PerfChart *base, double x, double y)
{
    PerfTimelineChart *chart = PERF_TIMELINE_CHART(base);
    PerfChartPrivate *priv = perf_chart_get_instance_private(base);

    int margin_left = 60;
    int margin_right = 10;
    int margin_top = 36;
    int margin_bottom = 28;

    int chart_w = priv->width - margin_left - margin_right;
    int chart_h = priv->height - margin_top - margin_bottom;
    int lane_h = chart_h / (chart->num_lanes > 0 ? chart->num_lanes : 1);

    if (x < margin_left || x >= priv->width - margin_right)
        return -1;
    if (y < margin_top || y >= margin_top + chart_h)
        return -1;

    uint64_t now = (uint64_t)g_get_monotonic_time();
    uint64_t window_us = 5 * 60 * 1000000ULL;
    uint64_t start_us = (now > window_us) ? now - window_us : 0;

    /* Find closest event within 16px */
    int best = -1;
    double best_dist = 16.0;

    int count = chart->event_count;
    int start_idx = (count > PERF_TIMELINE_MAX_EVENTS) ? chart->event_write : 0;
    int total = (count > PERF_TIMELINE_MAX_EVENTS) ? PERF_TIMELINE_MAX_EVENTS : count;

    for (int i = 0; i < total; i++) {
        int idx = (start_idx + i) % PERF_TIMELINE_MAX_EVENTS;
        const timeline_event_t *ev = &chart->events[idx];

        if (ev->timestamp_us < start_us)
            continue;
        if (ev->lane < 0 || ev->lane >= chart->num_lanes)
            continue;

        double t = (double)(ev->timestamp_us - start_us) / (double)window_us;
        double ex = margin_left + t * chart_w;
        double ey = margin_top + ev->lane * lane_h + lane_h / 2.0;

        double dx = x - ex;
        double dy = y - ey;
        double dist = sqrt(dx * dx + dy * dy);
        if (dist < best_dist) {
            best_dist = dist;
            best = idx;
        }
    }

    return best;
}

static char *
timeline_format_tooltip(PerfChart *base, int element)
{
    PerfTimelineChart *chart = PERF_TIMELINE_CHART(base);
    if (element < 0 || element >= PERF_TIMELINE_MAX_EVENTS)
        return NULL;

    const timeline_event_t *ev = &chart->events[element];
    return g_strdup(ev->tooltip);
}

static void
on_timeline_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
    (void)gesture;
    (void)n_press;
    PerfTimelineChart *chart = PERF_TIMELINE_CHART(data);
    PerfChartPrivate *priv = perf_chart_get_instance_private(PERF_CHART(chart));
    int hit = timeline_hit_test(PERF_CHART(chart), x, y);
    chart->selected_event = hit; /* -1 clears selection */
    gtk_widget_queue_draw(priv->drawing_area);
}

static void
perf_timeline_chart_finalize(GObject *obj)
{
    PerfTimelineChart *chart = PERF_TIMELINE_CHART(obj);
    g_free(chart->events);
    G_OBJECT_CLASS(perf_timeline_chart_parent_class)->finalize(obj);
}

static void
perf_timeline_chart_class_init(PerfTimelineChartClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->finalize = perf_timeline_chart_finalize;
    PerfChartClass *cc = PERF_CHART_CLASS(klass);
    cc->draw = timeline_draw;
    cc->hit_test = timeline_hit_test;
    cc->format_tooltip = timeline_format_tooltip;
}

static void
perf_timeline_chart_init(PerfTimelineChart *chart)
{
    chart->num_lanes = PERF_TIMELINE_MAX_LANES;
    chart->event_count = 0;
    chart->event_write = 0;
    chart->selected_event = -1;
    chart->events = g_new0(timeline_event_t, PERF_TIMELINE_MAX_EVENTS);

    /* Click controller for persistent selection */
    PerfChartPrivate *priv = perf_chart_get_instance_private(PERF_CHART(chart));
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_timeline_click), chart);
    gtk_widget_add_controller(priv->drawing_area, GTK_EVENT_CONTROLLER(click));
}

GtkWidget *
perf_timeline_chart_new(const char *title, int num_lanes)
{
    PerfTimelineChart *chart = g_object_new(PERF_TYPE_TIMELINE_CHART, NULL);
    perf_chart_set_title(PERF_CHART(chart), title);
    chart->num_lanes = num_lanes;
    return GTK_WIDGET(chart);
}

void
perf_timeline_chart_add_event(PerfTimelineChart *chart,
                              uint64_t timestamp_us,
                              int lane,
                              int severity,
                              const char *type,
                              const char *tooltip)
{
    g_return_if_fail(PERF_IS_TIMELINE_CHART(chart));

    int idx = chart->event_write % PERF_TIMELINE_MAX_EVENTS;
    timeline_event_t *ev = &chart->events[idx];

    ev->timestamp_us = timestamp_us;
    ev->lane = lane;
    ev->severity = severity;
    g_strlcpy(ev->type, type ? type : "", sizeof(ev->type));
    g_strlcpy(ev->tooltip, tooltip ? tooltip : "", sizeof(ev->tooltip));

    chart->event_write = (chart->event_write + 1) % PERF_TIMELINE_MAX_EVENTS;
    chart->event_count++;

    perf_chart_queue_redraw(PERF_CHART(chart));
}

void
perf_timeline_chart_clear(PerfTimelineChart *chart)
{
    g_return_if_fail(PERF_IS_TIMELINE_CHART(chart));
    chart->event_count = 0;
    chart->event_write = 0;
    perf_chart_queue_redraw(PERF_CHART(chart));
}
