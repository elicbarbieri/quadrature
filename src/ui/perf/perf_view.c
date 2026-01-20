/**
 * @file perf_view.c
 * @brief Performance dashboard view
 *
 * Grafana-style monitoring dashboard with charts and log viewer.
 */

#include "perf_chart.h"
#include "../internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Private State
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    perf_dashboard_t* dashboard;

    /* Charts */
    PerfHistogramChart* decode_hist;
    PerfHistogramChart* artwork_hist;
    PerfLineChart* hit_rate_chart;
    PerfLineChart* memory_chart;

    /* Health panel */
    GtkWidget* health_labels[PERF_MAX_PLAYERS];

    /* Log viewer */
    GtkTextView* log_view;
    GtkTextBuffer* log_buffer;
    GtkWidget* pause_btn;
    GtkDropDown* level_drop;
    perf_log_level_t min_level;
    gboolean auto_scroll;

    /* Update timer */
    guint timer_id;
} PerfViewPrivate;

/* ═══════════════════════════════════════════════════════════════════════════
 * Log Viewer Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
static const char* level_css[] = {"perf-log-debug", "perf-log-info", "perf-log-warn", "perf-log-error"};

static void append_log_entry(PerfViewPrivate* priv, const perf_log_entry_t* entry) {
    if (entry->level < priv->min_level) return;

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(priv->log_buffer, &end);

    /* Format timestamp */
    uint64_t sec = entry->timestamp_us / 1000000;
    uint64_t ms = (entry->timestamp_us / 1000) % 1000;
    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%02lu:%02lu:%02lu.%03lu ",
             (sec / 3600) % 24, (sec / 60) % 60, sec % 60, ms);

    /* Insert timestamp */
    gtk_text_buffer_insert(priv->log_buffer, &end, time_buf, -1);

    /* Insert level with tag */
    GtkTextIter level_start = end;
    char level_buf[16];
    snprintf(level_buf, sizeof(level_buf), "%-5s ", level_names[entry->level]);
    gtk_text_buffer_insert(priv->log_buffer, &end, level_buf, -1);
    gtk_text_buffer_apply_tag_by_name(priv->log_buffer, level_css[entry->level],
                                       &level_start, &end);

    /* Insert source and message */
    char msg_buf[256];
    snprintf(msg_buf, sizeof(msg_buf), "[%s] %s\n", entry->source, entry->message);
    gtk_text_buffer_insert(priv->log_buffer, &end, msg_buf, -1);

    /* Auto-scroll to end */
    if (priv->auto_scroll) {
        GtkTextMark* mark = gtk_text_buffer_get_insert(priv->log_buffer);
        gtk_text_buffer_move_mark(priv->log_buffer, mark, &end);
        gtk_text_view_scroll_mark_onscreen(priv->log_view, mark);
    }

    /* Trim to 1000 lines */
    int line_count = gtk_text_buffer_get_line_count(priv->log_buffer);
    if (line_count > PERF_LOG_SIZE) {
        GtkTextIter start, trim_end;
        gtk_text_buffer_get_start_iter(priv->log_buffer, &start);
        gtk_text_buffer_get_iter_at_line(priv->log_buffer, &trim_end, line_count - PERF_LOG_SIZE);
        gtk_text_buffer_delete(priv->log_buffer, &start, &trim_end);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Update Timer
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean update_dashboard(gpointer data) {
    PerfViewPrivate* priv = data;
    if (!priv->dashboard) return G_SOURCE_CONTINUE;

    /* Update histograms */
    perf_hist_stats_t stats;

    perf_get_histogram_stats(&priv->dashboard->audio_decode, &stats);
    perf_histogram_chart_set_data(priv->decode_hist, &stats);

    perf_get_histogram_stats(&priv->dashboard->artwork_load_time, &stats);
    perf_histogram_chart_set_data(priv->artwork_hist, &stats);

    /* Update time series */
    double values[PERF_TIMESERIES_SIZE];
    size_t count;

    perf_get_timeseries(&priv->dashboard->artwork_hit_rate, values, &count);
    perf_line_chart_set_data(priv->hit_rate_chart, 0, values, count);

    perf_get_timeseries(&priv->dashboard->cache_hit_rate, values, &count);
    perf_line_chart_set_data(priv->hit_rate_chart, 1, values, count);

    /* Update audio health */
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        uint64_t underruns, callbacks;
        double jitter_ms;
        perf_get_audio_health(priv->dashboard, i, &underruns, &callbacks, &jitter_ms);

        char buf[128];
        const char* status = underruns > 0 ? "perf-health-warn" : "perf-health-ok";
        if (underruns > 10) status = "perf-health-error";

        snprintf(buf, sizeof(buf), "Ch%d: %lu callbacks, %lu underruns, %.2fms jitter",
                 i + 1, (unsigned long)callbacks, (unsigned long)underruns, jitter_ms);
        gtk_label_set_text(GTK_LABEL(priv->health_labels[i]), buf);

        /* Update CSS class */
        gtk_widget_remove_css_class(priv->health_labels[i], "perf-health-ok");
        gtk_widget_remove_css_class(priv->health_labels[i], "perf-health-warn");
        gtk_widget_remove_css_class(priv->health_labels[i], "perf-health-error");
        gtk_widget_add_css_class(priv->health_labels[i], status);
    }

    /* Read and display new log entries */
    perf_log_entry_t logs[50];
    int n = perf_read_logs(priv->dashboard, logs, 50);
    for (int i = 0; i < n; i++) {
        append_log_entry(priv, &logs[i]);
    }

    return G_SOURCE_CONTINUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_clear_clicked(GtkButton* btn G_GNUC_UNUSED, gpointer data) {
    PerfViewPrivate* priv = data;
    gtk_text_buffer_set_text(priv->log_buffer, "", 0);
    if (priv->dashboard) {
        perf_dashboard_reset(priv->dashboard);
    }
}

static void on_pause_toggled(GtkToggleButton* btn, gpointer data) {
    PerfViewPrivate* priv = data;
    gboolean paused = gtk_toggle_button_get_active(btn);
    priv->auto_scroll = !paused;
    if (priv->dashboard) {
        perf_dashboard_pause(priv->dashboard, paused);
    }
}

static void on_level_changed(GtkDropDown* drop, GParamSpec* pspec G_GNUC_UNUSED, gpointer data) {
    PerfViewPrivate* priv = data;
    guint idx = gtk_drop_down_get_selected(drop);
    priv->min_level = (perf_log_level_t)idx;
}

static void on_view_destroy(GtkWidget* widget G_GNUC_UNUSED, gpointer data) {
    PerfViewPrivate* priv = data;
    if (priv->timer_id) {
        g_source_remove(priv->timer_id);
        priv->timer_id = 0;
    }
    g_free(priv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Widget Construction
 * ═══════════════════════════════════════════════════════════════════════════ */

static GtkWidget* make_audio_health_panel(PerfViewPrivate* priv) {
    GtkWidget* frame = gtk_frame_new("Audio Health");
    gtk_widget_add_css_class(frame, "perf-chart");

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);

    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        priv->health_labels[i] = gtk_label_new("Ch? -- callbacks, -- underruns");
        gtk_widget_add_css_class(priv->health_labels[i], "perf-health-ok");
        gtk_label_set_xalign(GTK_LABEL(priv->health_labels[i]), 0);
        gtk_box_append(GTK_BOX(vbox), priv->health_labels[i]);
    }

    gtk_frame_set_child(GTK_FRAME(frame), vbox);
    return frame;
}

static GtkWidget* make_log_viewer(PerfViewPrivate* priv) {
    GtkWidget* frame = gtk_frame_new("Log");
    gtk_widget_add_css_class(frame, "perf-chart");

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    /* Toolbar */
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 4);

    /* Level filter */
    const char* const levels[] = {"All", "Info+", "Warn+", "Error", NULL};
    priv->level_drop = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(levels));
    g_signal_connect(priv->level_drop, "notify::selected", G_CALLBACK(on_level_changed), priv);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(priv->level_drop));

    /* Spacer */
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar), spacer);

    /* Pause button */
    priv->pause_btn = gtk_toggle_button_new_with_label("Pause");
    g_signal_connect(priv->pause_btn, "toggled", G_CALLBACK(on_pause_toggled), priv);
    gtk_box_append(GTK_BOX(toolbar), priv->pause_btn);

    /* Clear button */
    GtkWidget* clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), priv);
    gtk_box_append(GTK_BOX(toolbar), clear_btn);

    gtk_box_append(GTK_BOX(vbox), toolbar);

    /* Scrolled text view */
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 150);
    gtk_widget_set_vexpand(scroll, TRUE);

    priv->log_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(priv->log_view, FALSE);
    gtk_text_view_set_monospace(priv->log_view, TRUE);
    gtk_text_view_set_cursor_visible(priv->log_view, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(priv->log_view), "perf-log");

    priv->log_buffer = gtk_text_view_get_buffer(priv->log_view);

    /* Create tags for log levels */
    gtk_text_buffer_create_tag(priv->log_buffer, "perf-log-debug", "foreground", "#888888", NULL);
    gtk_text_buffer_create_tag(priv->log_buffer, "perf-log-info", "foreground", "#cccccc", NULL);
    gtk_text_buffer_create_tag(priv->log_buffer, "perf-log-warn", "foreground", "#ffcc00", NULL);
    gtk_text_buffer_create_tag(priv->log_buffer, "perf-log-error", "foreground", "#ff3333", NULL);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(priv->log_view));
    gtk_box_append(GTK_BOX(vbox), scroll);

    gtk_frame_set_child(GTK_FRAME(frame), vbox);
    return frame;
}

GtkWidget* perf_view_new(perf_dashboard_t* dashboard) {
    PerfViewPrivate* priv = g_new0(PerfViewPrivate, 1);
    priv->dashboard = dashboard;
    priv->min_level = PERF_LOG_DEBUG;
    priv->auto_scroll = TRUE;

    /* Main container */
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "view-container");
    g_signal_connect(box, "destroy", G_CALLBACK(on_view_destroy), priv);

    /* Title */
    GtkWidget* title = gtk_label_new("Performance Dashboard");
    gtk_widget_add_css_class(title, "library-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(box), title);

    /* Charts grid (2 cols × 3 rows) - fits in narrower content area */
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    /* Row 0: Histograms */
    priv->decode_hist = PERF_HISTOGRAM_CHART(perf_histogram_chart_new("Audio Decode Time", "ms"));
    gtk_widget_set_size_request(GTK_WIDGET(priv->decode_hist), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->decode_hist), "perf-chart");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(priv->decode_hist), 0, 0, 1, 1);

    priv->artwork_hist = PERF_HISTOGRAM_CHART(perf_histogram_chart_new("Artwork Load Time", "ms"));
    gtk_widget_set_size_request(GTK_WIDGET(priv->artwork_hist), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->artwork_hist), "perf-chart");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(priv->artwork_hist), 1, 0, 1, 1);

    /* Row 1: Line charts */
    priv->hit_rate_chart = PERF_LINE_CHART(perf_line_chart_new("Cache Hit Rates (%)", 2));
    perf_line_chart_set_series(priv->hit_rate_chart, 0, "Artwork", NULL);
    perf_line_chart_set_series(priv->hit_rate_chart, 1, "Library", NULL);
    perf_line_chart_set_range(priv->hit_rate_chart, 0, 100);
    gtk_widget_set_size_request(GTK_WIDGET(priv->hit_rate_chart), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->hit_rate_chart), "perf-chart");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(priv->hit_rate_chart), 0, 1, 1, 1);

    priv->memory_chart = PERF_LINE_CHART(perf_line_chart_new("Memory Usage (MB)", 1));
    perf_line_chart_set_series(priv->memory_chart, 0, "Total", NULL);
    perf_line_chart_set_range(priv->memory_chart, 0, 500);
    gtk_widget_set_size_request(GTK_WIDGET(priv->memory_chart), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->memory_chart), "perf-chart");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(priv->memory_chart), 1, 1, 1, 1);

    /* Row 2: Health + Placeholder */
    GtkWidget* health_panel = make_audio_health_panel(priv);
    gtk_grid_attach(GTK_GRID(grid), health_panel, 0, 2, 1, 1);

    GtkWidget* placeholder1 = gtk_frame_new("File Size Distribution");
    gtk_widget_add_css_class(placeholder1, "perf-chart");
    gtk_widget_set_size_request(placeholder1, 200, 120);
    gtk_grid_attach(GTK_GRID(grid), placeholder1, 1, 2, 1, 1);

    gtk_box_append(GTK_BOX(box), grid);

    /* Log viewer */
    GtkWidget* log_section = make_log_viewer(priv);
    gtk_widget_set_vexpand(log_section, TRUE);
    gtk_box_append(GTK_BOX(box), log_section);

    /* Start 100ms update timer */
    priv->timer_id = g_timeout_add(100, update_dashboard, priv);

    return box;
}
