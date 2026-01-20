/**
 * @file perf_chart.h
 * @brief Custom Cairo chart widgets for performance dashboard
 *
 * GtkDrawingArea-based charts with hover detection and tooltips.
 */

#ifndef QUADRATURE_PERF_CHART_H
#define QUADRATURE_PERF_CHART_H

#include <gtk/gtk.h>
#include "quadrature/core/perf_dashboard.h"

G_BEGIN_DECLS

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfChart Base Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_CHART (perf_chart_get_type())
G_DECLARE_DERIVABLE_TYPE(PerfChart, perf_chart, PERF, CHART, GtkWidget)

struct _PerfChartClass {
    GtkWidgetClass parent_class;

    /* Virtual functions for subclasses */
    void (*draw)(PerfChart* chart, cairo_t* cr, int width, int height);
    int  (*hit_test)(PerfChart* chart, double x, double y);
    char* (*format_tooltip)(PerfChart* chart, int element);
};

/* Common properties */
void perf_chart_set_title(PerfChart* chart, const char* title);
const char* perf_chart_get_title(PerfChart* chart);
void perf_chart_queue_redraw(PerfChart* chart);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfHistogram Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_HISTOGRAM (perf_histogram_chart_get_type())
G_DECLARE_FINAL_TYPE(PerfHistogramChart, perf_histogram_chart, PERF, HISTOGRAM_CHART, PerfChart)

/**
 * Create a new histogram chart
 * @param title Chart title
 * @param unit Unit label (e.g., "ms")
 * @return New histogram widget
 */
GtkWidget* perf_histogram_chart_new(const char* title, const char* unit);

/**
 * Update histogram data from stats
 * @param chart Histogram chart
 * @param stats Statistics to display
 */
void perf_histogram_chart_set_data(PerfHistogramChart* chart, const perf_hist_stats_t* stats);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfLineChart Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_LINE_CHART (perf_line_chart_get_type())
G_DECLARE_FINAL_TYPE(PerfLineChart, perf_line_chart, PERF, LINE_CHART, PerfChart)

#define PERF_LINE_CHART_MAX_SERIES 4

/**
 * Create a new line chart
 * @param title Chart title
 * @param num_series Number of data series (1-4)
 * @return New line chart widget
 */
GtkWidget* perf_line_chart_new(const char* title, int num_series);

/**
 * Set series name and color
 * @param chart Line chart
 * @param series Series index (0-3)
 * @param name Series name for legend
 * @param color Series color (GdkRGBA)
 */
void perf_line_chart_set_series(PerfLineChart* chart, int series,
                                 const char* name, const GdkRGBA* color);

/**
 * Update series data from time series
 * @param chart Line chart
 * @param series Series index
 * @param values Array of values
 * @param count Number of values
 */
void perf_line_chart_set_data(PerfLineChart* chart, int series,
                               const double* values, size_t count);

/**
 * Set Y-axis range
 * @param chart Line chart
 * @param min Minimum value
 * @param max Maximum value
 */
void perf_line_chart_set_range(PerfLineChart* chart, double min, double max);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfGauge Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_GAUGE (perf_gauge_get_type())
G_DECLARE_FINAL_TYPE(PerfGauge, perf_gauge, PERF, GAUGE, PerfChart)

/**
 * Create a new gauge chart
 * @param title Chart title
 * @param max Maximum value
 * @return New gauge widget
 */
GtkWidget* perf_gauge_new(const char* title, double max);

/**
 * Set gauge value
 * @param gauge Gauge widget
 * @param value Current value
 * @param label Optional label text
 */
void perf_gauge_set_value(PerfGauge* gauge, double value, const char* label);

G_END_DECLS

#endif /* QUADRATURE_PERF_CHART_H */
