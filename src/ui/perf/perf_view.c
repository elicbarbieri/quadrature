/**
 * @file perf_view.c
 * @brief Grafana-style performance dashboard with comprehensive audio metrics
 *
 * Displays audio engine metrics (callback latency, budget utilization, underruns)
 * and audio cache metrics (decode time distribution, memory usage, hit rates).
 */

#include "perf_chart.h"
#include "perf_view.h"
#include "../internal.h"
#include "../../audio/internal.h"
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Private State
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    perf_dashboard_t* dashboard;
    audio_cache_t* audio_cache;

    /* Audio Engine - Per-Player Metrics */
    PerfHistogramChart* callback_latency_hist[PERF_MAX_PLAYERS];
    PerfLineChart* budget_util_chart[PERF_MAX_PLAYERS];
    GtkWidget* player_stats_grid;

    /* Audio Cache Metrics */
    PerfHistogramChart* decode_time_hist;
    PerfLineChart* memory_chart;
    PerfLineChart* hit_rate_chart;
    PerfHistogramChart* artwork_hist;

    /* Audio Health Summary */
    GtkWidget* health_summary;

    /* Log viewer */
    GtkTextView* log_view;
    GtkTextBuffer* log_buffer;
    GtkWidget* pause_btn;
    GtkDropDown* level_drop;
    perf_log_level_t min_level;
    gboolean auto_scroll;

    /* Update timer */
    guint timer_id;
    
    /* Current selected player for detail view */
    int selected_player;
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
             (unsigned long)((sec / 3600) % 24), (unsigned long)((sec / 60) % 60), 
             (unsigned long)(sec % 60), (unsigned long)ms);

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
 * Update Timer - Refresh All Metrics
 * ═══════════════════════════════════════════════════════════════════════════ */

static gboolean update_dashboard(gpointer data) {
    PerfViewPrivate* priv = data;
    if (!priv->dashboard) return G_SOURCE_CONTINUE;

    /* Update audio cache decode histogram */
    if (priv->audio_cache && priv->decode_time_hist) {
        audio_cache_decode_event_t events[100];
        uint32_t event_count = audio_cache_get_decode_events(priv->audio_cache, events, 100);
        
        /* Build histogram from events */
        perf_histogram_t hist;
        memset(&hist, 0, sizeof(hist));
        atomic_store(&hist.min, UINT64_MAX);
        atomic_store(&hist.max, 0);
        
        for (uint32_t i = 0; i < event_count; i++) {
            uint64_t time_us = (uint64_t)events[i].decode_duration_ms * 1000;
            
            /* Compute bucket (log scale) */
            int bucket = 0;
            if (time_us >= 1000) {
                uint64_t ms = time_us / 1000;
                bucket = (int)(log2((double)ms)) + 1;
                if (bucket >= PERF_HIST_BUCKETS) bucket = PERF_HIST_BUCKETS - 1;
            }
            
            atomic_fetch_add(&hist.buckets[bucket], 1);
            atomic_fetch_add(&hist.count, 1);
            atomic_fetch_add(&hist.sum, time_us);
            
            uint64_t cur_min = atomic_load(&hist.min);
            if (time_us < cur_min) atomic_store(&hist.min, time_us);
            
            uint64_t cur_max = atomic_load(&hist.max);
            if (time_us > cur_max) atomic_store(&hist.max, time_us);
        }
        
        perf_hist_stats_t stats;
        perf_get_histogram_stats(&hist, &stats);
        perf_histogram_chart_set_data(priv->decode_time_hist, &stats);
    }

    /* Update artwork load time histogram */
    perf_hist_stats_t stats;
    perf_get_histogram_stats(&priv->dashboard->artwork_load_time, &stats);
    perf_histogram_chart_set_data(priv->artwork_hist, &stats);

    /* Update cache hit rates */
    double values[PERF_TIMESERIES_SIZE];
    size_t count;

    perf_get_timeseries(&priv->dashboard->artwork_hit_rate, values, &count);
    perf_line_chart_set_data(priv->hit_rate_chart, 0, values, count);

    perf_get_timeseries(&priv->dashboard->cache_hit_rate, values, &count);
    perf_line_chart_set_data(priv->hit_rate_chart, 1, values, count);

    /* Update per-player metrics */
    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        /* Callback latency histogram */
        perf_get_histogram_stats(&priv->dashboard->callback_time[i], &stats);
        perf_histogram_chart_set_data(priv->callback_latency_hist[i], &stats);
        
        /* Budget utilization line chart */
        perf_get_timeseries(&priv->dashboard->budget_pct[i], values, &count);
        perf_line_chart_set_data(priv->budget_util_chart[i], 0, values, count);
    }

    /* Update health summary */
    if (priv->health_summary) {
        GString* text = g_string_new("");
        
        for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
            uint64_t underruns, callbacks;
            double jitter;
            perf_get_audio_health(priv->dashboard, i, &underruns, &callbacks, &jitter);
            
            const char* status_icon = "✓";
            const char* status_color = "#4ade80";  /* green */
            
            /* Simple health status based on underruns */
            float underrun_pct = callbacks > 0 ? (float)underruns / (float)callbacks * 100.0f : 0.0f;
            
            if (underrun_pct > 0.1f || underruns > 0) {
                status_icon = "⚠";
                status_color = "#fbbf24";  /* yellow */
            }
            if (underrun_pct > 1.0f) {
                status_icon = "✗";
                status_color = "#f87171";  /* red */
            }
            
            g_string_append_printf(text, 
                "<span foreground='%s' weight='bold'>%s Ch%d</span>  "
                "Callbacks: %lu  Underruns: %lu  Jitter: %.2fms\n",
                status_color, status_icon, i + 1,
                (unsigned long)callbacks,
                (unsigned long)underruns,
                jitter);
        }
        
        gtk_label_set_markup(GTK_LABEL(priv->health_summary), text->str);
        g_string_free(text, TRUE);
    }

    /* Memory usage */
    if (priv->audio_cache && priv->memory_chart) {
        audio_cache_stats_t cache_stats;
        audio_cache_get_stats(priv->audio_cache, &cache_stats);
        
        /* Convert to MB */
        double memory_mb = cache_stats.memory_usage_pct * 512.0 / 100.0;  /* Assume 512MB limit */
        perf_timeseries_add(&priv->dashboard->memory_usage, memory_mb);
        
        perf_get_timeseries(&priv->dashboard->memory_usage, values, &count);
        perf_line_chart_set_data(priv->memory_chart, 0, values, count);
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

static GtkWidget* make_health_summary(PerfViewPrivate* priv) {
    GtkWidget* frame = gtk_frame_new("Audio Health Summary");
    gtk_widget_add_css_class(frame, "perf-chart");

    priv->health_summary = gtk_label_new("Initializing...");
    gtk_label_set_use_markup(GTK_LABEL(priv->health_summary), TRUE);
    gtk_label_set_xalign(GTK_LABEL(priv->health_summary), 0);
    gtk_widget_set_margin_start(priv->health_summary, 8);
    gtk_widget_set_margin_end(priv->health_summary, 8);
    gtk_widget_set_margin_top(priv->health_summary, 8);
    gtk_widget_set_margin_bottom(priv->health_summary, 8);

    gtk_frame_set_child(GTK_FRAME(frame), priv->health_summary);
    return frame;
}

static GtkWidget* make_log_viewer(PerfViewPrivate* priv) {
    GtkWidget* frame = gtk_frame_new("Event Log");
    gtk_widget_add_css_class(frame, "perf-chart");

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(vbox, "perf-view-container");

    /* Toolbar */
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 4);
    gtk_widget_add_css_class(toolbar, "perf-view-toolbar");

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
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 200);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 200);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);

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

GtkWidget* perf_view_new(perf_dashboard_t* dashboard, audio_cache_t* cache) {
    PerfViewPrivate* priv = g_new0(PerfViewPrivate, 1);
    priv->dashboard = dashboard;
    priv->audio_cache = cache;
    priv->min_level = PERF_LOG_DEBUG;
    priv->auto_scroll = TRUE;
    priv->selected_player = 0;

    /* Scrolled window wrapper for all content */
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);

    /* Main container inside scrolled window */
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "view-container");
    gtk_widget_add_css_class(box, "perf-detail-container");
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    g_signal_connect(box, "destroy", G_CALLBACK(on_view_destroy), priv);

    /* Title */
    GtkWidget* title = gtk_label_new("Performance Dashboard");
    gtk_widget_add_css_class(title, "library-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(box), title);

    /* Health summary bar */
    GtkWidget* health = make_health_summary(priv);
    gtk_box_append(GTK_BOX(box), health);

    /* ═══════════════════════════════════════════════════════════════════════
     * SECTION 1: Audio Engine Metrics (Per-Player)
     * ═══════════════════════════════════════════════════════════════════════ */
    
    GtkWidget* section1 = gtk_label_new("Audio Engine (Per-Player Metrics)");
    gtk_widget_add_css_class(section1, "perf-section-title");
    gtk_label_set_xalign(GTK_LABEL(section1), 0);
    gtk_box_append(GTK_BOX(box), section1);

    /* Grid: 2 rows × 4 columns (callback latency + budget util for each player) */
    GtkWidget* engine_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(engine_grid), 0);
    gtk_grid_set_column_spacing(GTK_GRID(engine_grid), 0);
    gtk_widget_set_hexpand(engine_grid, TRUE);
    gtk_widget_add_css_class(engine_grid, "perf-stats-grid");

    for (int i = 0; i < PERF_MAX_PLAYERS; i++) {
        /* Row 0: Callback Latency Histograms */
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "Ch%d Callback Latency", i + 1);
        priv->callback_latency_hist[i] = PERF_HISTOGRAM_CHART(
            perf_histogram_chart_new(title_buf, "µs"));
        gtk_widget_set_size_request(GTK_WIDGET(priv->callback_latency_hist[i]), 200, 120);
        gtk_widget_add_css_class(GTK_WIDGET(priv->callback_latency_hist[i]), "perf-chart");
        gtk_grid_attach(GTK_GRID(engine_grid), GTK_WIDGET(priv->callback_latency_hist[i]), 
                       i, 0, 1, 1);

        /* Row 1: Budget Utilization Line Charts */
        snprintf(title_buf, sizeof(title_buf), "Ch%d Budget (%%)", i + 1);
        priv->budget_util_chart[i] = PERF_LINE_CHART(
            perf_line_chart_new(title_buf, 1));
        perf_line_chart_set_series(priv->budget_util_chart[i], 0, "Utilization", NULL);
        perf_line_chart_set_range(priv->budget_util_chart[i], 0, 100);
        gtk_widget_set_size_request(GTK_WIDGET(priv->budget_util_chart[i]), 200, 120);
        gtk_widget_add_css_class(GTK_WIDGET(priv->budget_util_chart[i]), "perf-chart");
        gtk_grid_attach(GTK_GRID(engine_grid), GTK_WIDGET(priv->budget_util_chart[i]), 
                       i, 1, 1, 1);
    }

    gtk_box_append(GTK_BOX(box), engine_grid);

    /* ═══════════════════════════════════════════════════════════════════════
     * SECTION 2: Audio Cache Metrics
     * ═══════════════════════════════════════════════════════════════════════ */
    
    GtkWidget* section2 = gtk_label_new("Audio Cache");
    gtk_widget_add_css_class(section2, "perf-section-title");
    gtk_label_set_xalign(GTK_LABEL(section2), 0);
    gtk_box_append(GTK_BOX(box), section2);

    /* Grid: 2 columns × 2 rows */
    GtkWidget* cache_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cache_grid), 0);
    gtk_grid_set_column_spacing(GTK_GRID(cache_grid), 0);
    gtk_widget_set_hexpand(cache_grid, TRUE);
    gtk_widget_add_css_class(cache_grid, "perf-stats-grid");
    gtk_grid_set_column_homogeneous(GTK_GRID(cache_grid), TRUE);

    /* Decode Time Distribution */
    priv->decode_time_hist = PERF_HISTOGRAM_CHART(
        perf_histogram_chart_new("Decode Time Distribution", "ms"));
    gtk_widget_set_size_request(GTK_WIDGET(priv->decode_time_hist), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->decode_time_hist), "perf-chart");
    gtk_grid_attach(GTK_GRID(cache_grid), GTK_WIDGET(priv->decode_time_hist), 0, 0, 1, 1);

    /* Memory Usage */
    priv->memory_chart = PERF_LINE_CHART(perf_line_chart_new("Memory Usage", 1));
    perf_line_chart_set_series(priv->memory_chart, 0, "Cached Audio (MB)", NULL);
    perf_line_chart_set_range(priv->memory_chart, 0, 512);
    gtk_widget_set_size_request(GTK_WIDGET(priv->memory_chart), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->memory_chart), "perf-chart");
    gtk_grid_attach(GTK_GRID(cache_grid), GTK_WIDGET(priv->memory_chart), 1, 0, 1, 1);

    /* Cache Hit Rates */
    priv->hit_rate_chart = PERF_LINE_CHART(perf_line_chart_new("Cache Hit Rates", 2));
    perf_line_chart_set_series(priv->hit_rate_chart, 0, "Artwork", NULL);
    perf_line_chart_set_series(priv->hit_rate_chart, 1, "Library", NULL);
    perf_line_chart_set_range(priv->hit_rate_chart, 0, 100);
    gtk_widget_set_size_request(GTK_WIDGET(priv->hit_rate_chart), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->hit_rate_chart), "perf-chart");
    gtk_grid_attach(GTK_GRID(cache_grid), GTK_WIDGET(priv->hit_rate_chart), 0, 1, 1, 1);

    /* Artwork Load Time */
    priv->artwork_hist = PERF_HISTOGRAM_CHART(
        perf_histogram_chart_new("Artwork Load Time", "ms"));
    gtk_widget_set_size_request(GTK_WIDGET(priv->artwork_hist), 200, 120);
    gtk_widget_add_css_class(GTK_WIDGET(priv->artwork_hist), "perf-chart");
    gtk_grid_attach(GTK_GRID(cache_grid), GTK_WIDGET(priv->artwork_hist), 1, 1, 1, 1);

    gtk_box_append(GTK_BOX(box), cache_grid);

    /* ═══════════════════════════════════════════════════════════════════════
     * SECTION 3: Event Log
     * ═══════════════════════════════════════════════════════════════════════ */
    
    GtkWidget* log_section = make_log_viewer(priv);
    gtk_box_append(GTK_BOX(box), log_section);

    /* Start 100ms update timer */
    priv->timer_id = g_timeout_add(100, update_dashboard, priv);

    /* Add box to scrolled window */
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);

    return scrolled;
}
