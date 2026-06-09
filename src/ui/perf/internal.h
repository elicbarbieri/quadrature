/**
 * Performance Dashboard Module - Internal Header
 *
 * Consolidates all perf UI widget declarations:
 * - PerfChart base widget + subclasses (histogram, line, timeline, gauge)
 * - PerfGroupedHist widget
 * - PerfView dashboard widget
 */

#ifndef QUADRATURE_UI_PERF_INTERNAL_H
#define QUADRATURE_UI_PERF_INTERNAL_H

#include <gtk/gtk.h>
#include <stdint.h>
#include "../../core/internal.h"

G_BEGIN_DECLS

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfChart Base Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_CHART (perf_chart_get_type())
G_DECLARE_DERIVABLE_TYPE(PerfChart, perf_chart, PERF, CHART, GtkWidget)

struct _PerfChartClass {
    GtkWidgetClass parent_class;

    /* Virtual functions for subclasses */
    void (*draw)(PerfChart *chart, cairo_t *cr, int width, int height);
    int (*hit_test)(PerfChart *chart, double x, double y);
    char *(*format_tooltip)(PerfChart *chart, int element);
};

/* Common properties */
void perf_chart_set_title(PerfChart *chart, const char *title);
void perf_chart_queue_redraw(PerfChart *chart);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfHistogram Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_HISTOGRAM (perf_histogram_chart_get_type())
G_DECLARE_FINAL_TYPE(PerfHistogramChart, perf_histogram_chart, PERF, HISTOGRAM_CHART, PerfChart)

GtkWidget *perf_histogram_chart_new(const char *title, const char *unit);

#define PERF_ADAPTIVE_MAX_BINS 24

typedef struct {
    uint32_t num_bins;
    uint32_t bin_counts[PERF_ADAPTIVE_MAX_BINS];
    char bin_labels[PERF_ADAPTIVE_MAX_BINS][16];
    double p50, p90, p99, min, max;
    uint64_t count;
} perf_adaptive_hist_data_t;

void perf_histogram_chart_set_adaptive_data(PerfHistogramChart *chart,
                                            const perf_adaptive_hist_data_t *data);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfLineChart Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_LINE_CHART (perf_line_chart_get_type())
G_DECLARE_FINAL_TYPE(PerfLineChart, perf_line_chart, PERF, LINE_CHART, PerfChart)

#define PERF_LINE_CHART_MAX_SERIES 8

GtkWidget *perf_line_chart_new(const char *title, int num_series);
void perf_line_chart_set_series(PerfLineChart *chart,
                                int series,
                                const char *name,
                                const GdkRGBA *color);
void perf_line_chart_set_data(PerfLineChart *chart, int series, const double *values, size_t count);
void perf_line_chart_set_range(PerfLineChart *chart, double min, double max);
void perf_line_chart_set_series_visible(PerfLineChart *chart, int series, gboolean visible);

/** Enable hover tooltip with time-based X axis and value readout.
 *  sample_interval_ms: time between consecutive data points (e.g. 10.0 for ~10ms)
 *  y_unit: unit string for Y values (e.g. "µs", "MB") */
void perf_line_chart_set_hover(PerfLineChart *chart, double sample_interval_ms, const char *y_unit);
void perf_line_chart_set_log_scale(PerfLineChart *chart, gboolean log_scale);

/** Reset smooth-scroll origin. Call once per tick when a new display point appeared. */
void perf_line_chart_scroll_reset(PerfLineChart *chart);

/** Set fixed display capacity for right-aligned rendering.
 *  When set, x_step is derived from capacity (not current count), and data
 *  is right-aligned so the newest point sits at the right edge. */
void perf_line_chart_set_capacity(PerfLineChart *chart, size_t capacity);

/** Enable time-axis labels along the bottom edge.
 *  Uses sample_interval_ms to compute elapsed-time labels (e.g. "-10m ... now"). */
void perf_line_chart_set_time_axis(PerfLineChart *chart, gboolean show);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfStackedAreaChart Widget (Memory breakdown)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_STACKED_AREA_CHART (perf_stacked_area_chart_get_type())
G_DECLARE_FINAL_TYPE(
    PerfStackedAreaChart, perf_stacked_area_chart, PERF, STACKED_AREA_CHART, PerfChart)

GtkWidget *perf_stacked_area_chart_new(const char *title, int num_series);
void perf_stacked_area_chart_set_series(PerfStackedAreaChart *chart,
                                        int series,
                                        const char *name,
                                        const GdkRGBA *color);
void perf_stacked_area_chart_set_data(PerfStackedAreaChart *chart,
                                      int series,
                                      const double *values,
                                      size_t count);
void perf_stacked_area_chart_set_y_max(PerfStackedAreaChart *chart, double max_mb);
void perf_stacked_area_chart_set_hover(PerfStackedAreaChart *chart,
                                       double interval_ms,
                                       const char *unit);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfTimelineChart Widget (Fault Event Timeline)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_TIMELINE_CHART (perf_timeline_chart_get_type())
G_DECLARE_FINAL_TYPE(PerfTimelineChart, perf_timeline_chart, PERF, TIMELINE_CHART, PerfChart)

#define PERF_TIMELINE_MAX_EVENTS 2000
#define PERF_TIMELINE_MAX_LANES  5 /* 4 channels + 1 system lane */

GtkWidget *perf_timeline_chart_new(const char *title, int num_lanes);
void perf_timeline_chart_add_event(PerfTimelineChart *chart,
                                   uint64_t timestamp_us,
                                   int lane,
                                   int severity,
                                   const char *type,
                                   const char *tooltip);
void perf_timeline_chart_clear(PerfTimelineChart *chart);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfGroupedHist Widget — Grouped bar histogram
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PERF_TYPE_GROUPED_HIST (perf_grouped_hist_get_type())
G_DECLARE_FINAL_TYPE(PerfGroupedHist, perf_grouped_hist, PERF, GROUPED_HIST, GtkDrawingArea)

GtkWidget *
perf_grouped_hist_new(const char *title, const char *unit, int num_groups, int num_buckets);
void perf_grouped_hist_set_group(PerfGroupedHist *hist,
                                 int group,
                                 const char *label,
                                 const GdkRGBA *color);
void perf_grouped_hist_set_data(PerfGroupedHist *hist,
                                int group,
                                const uint32_t *bucket_counts,
                                int count);
void perf_grouped_hist_set_num_buckets(PerfGroupedHist *hist, int num_buckets);
void perf_grouped_hist_set_bucket_label(PerfGroupedHist *hist, int bucket, const char *label);
void perf_grouped_hist_set_group_visible(PerfGroupedHist *hist, int group, gboolean visible);
void perf_grouped_hist_set_log_scale(PerfGroupedHist *hist, gboolean log_scale);

/* ═══════════════════════════════════════════════════════════════════════════
 * PerfView Dashboard Widget
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declarations (guarded — these may already be typedef'd by other headers) */
typedef struct audio_cache audio_cache_t;
typedef struct audio_pipeline audio_pipeline_t;
#ifndef QUADRATURE_LIBRARY_H
typedef struct library_cache library_cache_t;
#endif
#ifndef QUADRATURE_UI_LIBRARY_INTERNAL_H
typedef struct _ArtworkManager ArtworkManager;
#endif

GtkWidget *perf_view_new(audio_pipeline_t *pipeline,
                         library_cache_t *library_cache,
                         ArtworkManager *artwork_mgr);

G_END_DECLS

#endif /* QUADRATURE_UI_PERF_INTERNAL_H */
