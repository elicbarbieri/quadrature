/**
 * @file perf_chart.c
 * @brief Custom Cairo chart widgets implementation
 *
 * Grafana-style charts with hover detection and tooltips.
 */

#include "perf_chart.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Colors (Cyan theme matching app style)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const GdkRGBA COLOR_CYAN       = {0.00, 0.83, 1.00, 1.0};
static const GdkRGBA COLOR_CYAN_HOVER = {0.00, 1.00, 1.00, 1.0};
static const GdkRGBA COLOR_GRID       = {0.20, 0.20, 0.20, 1.0};
static const GdkRGBA COLOR_TEXT       = {0.53, 0.53, 0.53, 1.0};
static const GdkRGBA COLOR_TEXT_LIGHT = {0.80, 0.80, 0.80, 1.0};
static const GdkRGBA COLOR_BG         = {0.10, 0.10, 0.10, 1.0};

/* Series colors for line charts */
static const GdkRGBA SERIES_COLORS[4] = {
    {0.00, 0.83, 1.00, 1.0},  /* Cyan */
    {0.20, 0.80, 0.20, 1.0},  /* Green */
    {1.00, 0.80, 0.00, 1.0},  /* Yellow */
    {1.00, 0.33, 0.33, 1.0},  /* Red */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfChart Base Class
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char* title;
    GtkWidget* drawing_area;
    GtkPopover* tooltip;
    GtkLabel* tooltip_label;
    int hover_element;
    int width;
    int height;
} PerfChartPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(PerfChart, perf_chart, GTK_TYPE_WIDGET)

static void perf_chart_draw_func(GtkDrawingArea* area G_GNUC_UNUSED, cairo_t* cr,
                                  int width, int height, gpointer data) {
    PerfChart* chart = PERF_CHART(data);
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);
    PerfChartClass* klass = PERF_CHART_GET_CLASS(chart);

    priv->width = width;
    priv->height = height;

    /* Background */
    cairo_set_source_rgb(cr, COLOR_BG.red, COLOR_BG.green, COLOR_BG.blue);
    cairo_paint(cr);

    /* Draw title */
    if (priv->title) {
        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, 8, 16);
        cairo_show_text(cr, priv->title);
    }

    /* Call subclass draw */
    if (klass->draw) {
        klass->draw(chart, cr, width, height);
    }
}

static void on_motion(GtkEventControllerMotion* ctrl G_GNUC_UNUSED, double x, double y, gpointer data) {
    PerfChart* chart = PERF_CHART(data);
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);
    PerfChartClass* klass = PERF_CHART_GET_CLASS(chart);

    int element = -1;
    if (klass->hit_test) {
        element = klass->hit_test(chart, x, y);
    }

    if (element != priv->hover_element) {
        priv->hover_element = element;
        gtk_widget_queue_draw(priv->drawing_area);

        if (element >= 0 && klass->format_tooltip) {
            char* text = klass->format_tooltip(chart, element);
            if (text) {
                gtk_label_set_text(priv->tooltip_label, text);
                GdkRectangle rect = {(int)x, (int)y, 1, 1};
                gtk_popover_set_pointing_to(priv->tooltip, &rect);
                gtk_popover_popup(priv->tooltip);
                g_free(text);
            }
        } else {
            gtk_popover_popdown(priv->tooltip);
        }
    }
}

static void on_leave(GtkEventControllerMotion* ctrl G_GNUC_UNUSED, gpointer data) {
    PerfChart* chart = PERF_CHART(data);
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);

    if (priv->hover_element >= 0) {
        priv->hover_element = -1;
        gtk_widget_queue_draw(priv->drawing_area);
        gtk_popover_popdown(priv->tooltip);
    }
}

static void perf_chart_dispose(GObject* obj) {
    PerfChart* chart = PERF_CHART(obj);
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);

    g_clear_pointer(&priv->title, g_free);
    g_clear_pointer(&priv->drawing_area, gtk_widget_unparent);

    G_OBJECT_CLASS(perf_chart_parent_class)->dispose(obj);
}

static void perf_chart_class_init(PerfChartClass* klass) {
    GObjectClass* oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass* wc = GTK_WIDGET_CLASS(klass);

    oc->dispose = perf_chart_dispose;
    gtk_widget_class_set_layout_manager_type(wc, GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(wc, "perf-chart");
}

static void perf_chart_init(PerfChart* chart) {
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);

    priv->hover_element = -1;

    /* Create drawing area */
    priv->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(priv->drawing_area, TRUE);
    gtk_widget_set_vexpand(priv->drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(priv->drawing_area),
                                    perf_chart_draw_func, chart, NULL);
    gtk_widget_set_parent(priv->drawing_area, GTK_WIDGET(chart));

    /* Motion controller for hover */
    GtkEventController* motion = gtk_event_controller_motion_new();
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

void perf_chart_set_title(PerfChart* chart, const char* title) {
    g_return_if_fail(PERF_IS_CHART(chart));
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);
    g_free(priv->title);
    priv->title = g_strdup(title);
    gtk_widget_queue_draw(priv->drawing_area);
}

const char* perf_chart_get_title(PerfChart* chart) {
    g_return_val_if_fail(PERF_IS_CHART(chart), NULL);
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);
    return priv->title;
}

void perf_chart_queue_redraw(PerfChart* chart) {
    g_return_if_fail(PERF_IS_CHART(chart));
    PerfChartPrivate* priv = perf_chart_get_instance_private(chart);
    gtk_widget_queue_draw(priv->drawing_area);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfHistogramChart
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _PerfHistogramChart {
    PerfChart parent;

    char* unit;
    perf_hist_stats_t stats;
};

G_DEFINE_FINAL_TYPE(PerfHistogramChart, perf_histogram_chart, PERF_TYPE_CHART)

/* Get bucket range string */
static void bucket_range_str(int bucket, char* buf, size_t len) {
    if (bucket == 0) {
        snprintf(buf, len, "0-1");
    } else if (bucket == PERF_HIST_BUCKETS - 1) {
        snprintf(buf, len, "%d+", 1 << (bucket - 1));
    } else {
        snprintf(buf, len, "%d-%d", 1 << (bucket - 1), 1 << bucket);
    }
}

static void histogram_draw(PerfChart* base, cairo_t* cr, int w, int h) {
    PerfHistogramChart* chart = PERF_HISTOGRAM_CHART(base);
    PerfChartPrivate* priv = perf_chart_get_instance_private(base);
    perf_hist_stats_t* s = &chart->stats;

    int margin_left = 40;
    int margin_right = 10;
    int margin_top = 28;
    int margin_bottom = 35;

    int chart_w = w - margin_left - margin_right;
    int chart_h = h - margin_top - margin_bottom;
    int bar_w = chart_w / PERF_HIST_BUCKETS;

    /* Find max bucket count */
    uint64_t max_count = 1;
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        if (s->bucket_counts[i] > max_count) max_count = s->bucket_counts[i];
    }

    /* Draw grid lines */
    cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    cairo_set_line_width(cr, 1);
    for (int i = 0; i <= 4; i++) {
        int y = margin_top + (chart_h * i / 4);
        cairo_move_to(cr, margin_left, y + 0.5);
        cairo_line_to(cr, w - margin_right, y + 0.5);
        cairo_stroke(cr);
    }

    /* Draw bars */
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        if (s->bucket_counts[i] == 0) continue;

        double bar_h = ((double)s->bucket_counts[i] / (double)max_count) * chart_h;
        int x = margin_left + i * bar_w;
        int y = margin_top + chart_h - (int)bar_h;

        /* Highlight hovered bar */
        if (i == priv->hover_element) {
            cairo_set_source_rgb(cr, COLOR_CYAN_HOVER.red, COLOR_CYAN_HOVER.green, COLOR_CYAN_HOVER.blue);
        } else {
            cairo_set_source_rgb(cr, COLOR_CYAN.red, COLOR_CYAN.green, COLOR_CYAN.blue);
        }

        cairo_rectangle(cr, x + 1, y, bar_w - 2, bar_h);
        cairo_fill(cr);
    }

    /* Draw X-axis labels (every 4th bucket) */
    cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);

    for (int i = 0; i < PERF_HIST_BUCKETS; i += 4) {
        char buf[16];
        bucket_range_str(i, buf, sizeof(buf));
        int x = margin_left + i * bar_w + bar_w / 2;
        cairo_move_to(cr, x - 10, h - margin_bottom + 15);
        cairo_show_text(cr, buf);
    }

    /* Draw unit label */
    if (chart->unit) {
        cairo_move_to(cr, w - margin_right - 20, h - 5);
        cairo_show_text(cr, chart->unit);
    }

    /* Draw percentile markers */
    cairo_set_source_rgb(cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
    cairo_set_font_size(cr, 10);

    char stats_buf[128];
    snprintf(stats_buf, sizeof(stats_buf), "p50: %lums  p90: %lums  p99: %lums",
             (unsigned long)(s->p50 / 1000),
             (unsigned long)(s->p90 / 1000),
             (unsigned long)(s->p99 / 1000));
    cairo_move_to(cr, margin_left, h - 5);
    cairo_show_text(cr, stats_buf);
}

static int histogram_hit_test(PerfChart* base, double x, double y) {
    PerfChartPrivate* priv = perf_chart_get_instance_private(base);

    int margin_left = 40;
    int margin_right = 10;
    int margin_top = 28;
    int margin_bottom = 35;

    int chart_w = priv->width - margin_left - margin_right;
    int bar_w = chart_w / PERF_HIST_BUCKETS;

    if (x < margin_left || x >= priv->width - margin_right) return -1;
    if (y < margin_top || y >= priv->height - margin_bottom) return -1;

    int bucket = (int)(x - margin_left) / bar_w;
    if (bucket < 0 || bucket >= PERF_HIST_BUCKETS) return -1;

    return bucket;
}

static char* histogram_format_tooltip(PerfChart* base, int element) {
    PerfHistogramChart* chart = PERF_HISTOGRAM_CHART(base);
    perf_hist_stats_t* s = &chart->stats;

    if (element < 0 || element >= PERF_HIST_BUCKETS) return NULL;

    char range[32];
    bucket_range_str(element, range, sizeof(range));

    double percent = s->count > 0 ?
        (double)s->bucket_counts[element] / (double)s->count * 100.0 : 0.0;

    return g_strdup_printf("Range: %s %s\nCount: %lu\nPercent: %.1f%%",
                           range, chart->unit ? chart->unit : "",
                           (unsigned long)s->bucket_counts[element],
                           percent);
}

static void perf_histogram_chart_dispose(GObject* obj) {
    PerfHistogramChart* chart = PERF_HISTOGRAM_CHART(obj);
    g_clear_pointer(&chart->unit, g_free);
    G_OBJECT_CLASS(perf_histogram_chart_parent_class)->dispose(obj);
}

static void perf_histogram_chart_class_init(PerfHistogramChartClass* klass) {
    GObjectClass* oc = G_OBJECT_CLASS(klass);
    PerfChartClass* cc = PERF_CHART_CLASS(klass);

    oc->dispose = perf_histogram_chart_dispose;
    cc->draw = histogram_draw;
    cc->hit_test = histogram_hit_test;
    cc->format_tooltip = histogram_format_tooltip;
}

static void perf_histogram_chart_init(PerfHistogramChart* chart) {
    memset(&chart->stats, 0, sizeof(chart->stats));
}

GtkWidget* perf_histogram_chart_new(const char* title, const char* unit) {
    PerfHistogramChart* chart = g_object_new(PERF_TYPE_HISTOGRAM, NULL);
    perf_chart_set_title(PERF_CHART(chart), title);
    chart->unit = g_strdup(unit);
    return GTK_WIDGET(chart);
}

void perf_histogram_chart_set_data(PerfHistogramChart* chart, const perf_hist_stats_t* stats) {
    g_return_if_fail(PERF_IS_HISTOGRAM_CHART(chart));
    g_return_if_fail(stats != NULL);

    memcpy(&chart->stats, stats, sizeof(perf_hist_stats_t));
    perf_chart_queue_redraw(PERF_CHART(chart));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfLineChart
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _PerfLineChart {
    PerfChart parent;

    int num_series;
    char* series_names[PERF_LINE_CHART_MAX_SERIES];
    GdkRGBA series_colors[PERF_LINE_CHART_MAX_SERIES];
    double* series_data[PERF_LINE_CHART_MAX_SERIES];
    size_t series_count[PERF_LINE_CHART_MAX_SERIES];
    double y_min;
    double y_max;
};

G_DEFINE_FINAL_TYPE(PerfLineChart, perf_line_chart, PERF_TYPE_CHART)

static void line_chart_draw(PerfChart* base, cairo_t* cr, int w, int h) {
    PerfLineChart* chart = PERF_LINE_CHART(base);

    int margin_left = 45;
    int margin_right = 10;
    int margin_top = 28;
    int margin_bottom = 25;

    int chart_w = w - margin_left - margin_right;
    int chart_h = h - margin_top - margin_bottom;

    double y_range = chart->y_max - chart->y_min;
    if (y_range <= 0) y_range = 100;

    /* Draw grid lines */
    cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    cairo_set_line_width(cr, 1);
    for (int i = 0; i <= 4; i++) {
        int y = margin_top + (chart_h * i / 4);
        cairo_move_to(cr, margin_left, y + 0.5);
        cairo_line_to(cr, w - margin_right, y + 0.5);
        cairo_stroke(cr);

        /* Y-axis label */
        double val = chart->y_max - (y_range * i / 4);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", val);
        cairo_set_source_rgb(cr, COLOR_TEXT.red, COLOR_TEXT.green, COLOR_TEXT.blue);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9);
        cairo_move_to(cr, 5, y + 3);
        cairo_show_text(cr, buf);
        cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    }

    /* Draw each series */
    for (int s = 0; s < chart->num_series; s++) {
        if (!chart->series_data[s] || chart->series_count[s] < 2) continue;

        GdkRGBA* color = &chart->series_colors[s];
        cairo_set_source_rgba(cr, color->red, color->green, color->blue, 0.8);
        cairo_set_line_width(cr, 1.5);

        size_t count = chart->series_count[s];
        double x_step = (double)chart_w / (double)(count - 1);

        /* Draw line */
        cairo_move_to(cr, margin_left,
            margin_top + chart_h - ((chart->series_data[s][0] - chart->y_min) / y_range * chart_h));

        for (size_t i = 1; i < count; i++) {
            double x = margin_left + i * x_step;
            double y = margin_top + chart_h -
                ((chart->series_data[s][i] - chart->y_min) / y_range * chart_h);
            cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }

    /* Draw legend */
    cairo_set_font_size(cr, 9);
    int legend_x = w - margin_right - 10;
    for (int s = chart->num_series - 1; s >= 0; s--) {
        if (!chart->series_names[s]) continue;

        cairo_text_extents_t ext;
        cairo_text_extents(cr, chart->series_names[s], &ext);
        legend_x -= (int)ext.width + 20;

        GdkRGBA* color = &chart->series_colors[s];
        cairo_set_source_rgb(cr, color->red, color->green, color->blue);
        cairo_rectangle(cr, legend_x, margin_top - 12, 8, 8);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
        cairo_move_to(cr, legend_x + 12, margin_top - 4);
        cairo_show_text(cr, chart->series_names[s]);
    }
}

static int line_chart_hit_test(PerfChart* base, double x, double y) {
    (void)base; (void)x; (void)y;
    return -1;  /* No hover for line charts */
}

static void perf_line_chart_dispose(GObject* obj) {
    PerfLineChart* chart = PERF_LINE_CHART(obj);
    for (int i = 0; i < PERF_LINE_CHART_MAX_SERIES; i++) {
        g_clear_pointer(&chart->series_names[i], g_free);
        g_clear_pointer(&chart->series_data[i], g_free);
    }
    G_OBJECT_CLASS(perf_line_chart_parent_class)->dispose(obj);
}

static void perf_line_chart_class_init(PerfLineChartClass* klass) {
    GObjectClass* oc = G_OBJECT_CLASS(klass);
    PerfChartClass* cc = PERF_CHART_CLASS(klass);

    oc->dispose = perf_line_chart_dispose;
    cc->draw = line_chart_draw;
    cc->hit_test = line_chart_hit_test;
    cc->format_tooltip = NULL;
}

static void perf_line_chart_init(PerfLineChart* chart) {
    chart->y_min = 0;
    chart->y_max = 100;
    for (int i = 0; i < PERF_LINE_CHART_MAX_SERIES; i++) {
        chart->series_colors[i] = SERIES_COLORS[i];
    }
}

GtkWidget* perf_line_chart_new(const char* title, int num_series) {
    g_return_val_if_fail(num_series > 0 && num_series <= PERF_LINE_CHART_MAX_SERIES, NULL);

    PerfLineChart* chart = g_object_new(PERF_TYPE_LINE_CHART, NULL);
    perf_chart_set_title(PERF_CHART(chart), title);
    chart->num_series = num_series;
    return GTK_WIDGET(chart);
}

void perf_line_chart_set_series(PerfLineChart* chart, int series,
                                 const char* name, const GdkRGBA* color) {
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);

    g_free(chart->series_names[series]);
    chart->series_names[series] = g_strdup(name);
    if (color) chart->series_colors[series] = *color;
}

void perf_line_chart_set_data(PerfLineChart* chart, int series,
                               const double* values, size_t count) {
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    g_return_if_fail(series >= 0 && series < chart->num_series);

    g_free(chart->series_data[series]);
    chart->series_data[series] = g_memdup2(values, count * sizeof(double));
    chart->series_count[series] = count;
    perf_chart_queue_redraw(PERF_CHART(chart));
}

void perf_line_chart_set_range(PerfLineChart* chart, double min, double max) {
    g_return_if_fail(PERF_IS_LINE_CHART(chart));
    chart->y_min = min;
    chart->y_max = max;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfGauge
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _PerfGauge {
    PerfChart parent;

    double value;
    double max;
    char* label;
};

G_DEFINE_FINAL_TYPE(PerfGauge, perf_gauge, PERF_TYPE_CHART)

static void gauge_draw(PerfChart* base, cairo_t* cr, int w, int h) {
    PerfGauge* gauge = PERF_GAUGE(base);

    int margin = 20;
    int gauge_h = 20;
    int center_y = h / 2;

    /* Background bar */
    cairo_set_source_rgb(cr, COLOR_GRID.red, COLOR_GRID.green, COLOR_GRID.blue);
    cairo_rectangle(cr, margin, center_y - gauge_h / 2, w - 2 * margin, gauge_h);
    cairo_fill(cr);

    /* Value bar */
    double ratio = gauge->max > 0 ? gauge->value / gauge->max : 0;
    if (ratio > 1) ratio = 1;

    /* Color based on value (green -> yellow -> red) */
    GdkRGBA color;
    if (ratio < 0.5) {
        color = (GdkRGBA){0.2 + ratio * 1.6, 0.8, 0.2, 1.0};
    } else {
        color = (GdkRGBA){1.0, 0.8 - (ratio - 0.5) * 1.0, 0.2, 1.0};
    }

    cairo_set_source_rgb(cr, color.red, color.green, color.blue);
    cairo_rectangle(cr, margin, center_y - gauge_h / 2,
                    (w - 2 * margin) * ratio, gauge_h);
    cairo_fill(cr);

    /* Label */
    if (gauge->label) {
        cairo_set_source_rgb(cr, COLOR_TEXT_LIGHT.red, COLOR_TEXT_LIGHT.green, COLOR_TEXT_LIGHT.blue);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, gauge->label, &ext);
        cairo_move_to(cr, (w - ext.width) / 2, center_y + 5);
        cairo_show_text(cr, gauge->label);
    }
}

static void perf_gauge_dispose(GObject* obj) {
    PerfGauge* gauge = PERF_GAUGE(obj);
    g_clear_pointer(&gauge->label, g_free);
    G_OBJECT_CLASS(perf_gauge_parent_class)->dispose(obj);
}

static void perf_gauge_class_init(PerfGaugeClass* klass) {
    GObjectClass* oc = G_OBJECT_CLASS(klass);
    PerfChartClass* cc = PERF_CHART_CLASS(klass);

    oc->dispose = perf_gauge_dispose;
    cc->draw = gauge_draw;
    cc->hit_test = NULL;
    cc->format_tooltip = NULL;
}

static void perf_gauge_init(PerfGauge* gauge) {
    gauge->max = 100;
}

GtkWidget* perf_gauge_new(const char* title, double max) {
    PerfGauge* gauge = g_object_new(PERF_TYPE_GAUGE, NULL);
    perf_chart_set_title(PERF_CHART(gauge), title);
    gauge->max = max;
    return GTK_WIDGET(gauge);
}

void perf_gauge_set_value(PerfGauge* gauge, double value, const char* label) {
    g_return_if_fail(PERF_IS_GAUGE(gauge));
    gauge->value = value;
    g_free(gauge->label);
    gauge->label = g_strdup(label);
    perf_chart_queue_redraw(PERF_CHART(gauge));
}
