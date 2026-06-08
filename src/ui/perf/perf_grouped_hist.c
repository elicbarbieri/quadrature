/**
 * perf_grouped_hist.c — Grouped bar histogram widget (Cairo-based)
 *
 * Draws N thin bars side-by-side for each bucket. Y axis = % of samples.
 * Legend at top-right. X labels show bucket ranges (e.g., "0-10%").
 */

#include "internal.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ui_math.c */
double ui_log_pct_norm(double pct);

#define GROUPED_HIST_MAX_GROUPS  4
#define GROUPED_HIST_MAX_BUCKETS 16

/* Default channel colors */
static const GdkRGBA DEFAULT_COLORS[GROUPED_HIST_MAX_GROUPS] = {
    { 0.298, 0.608, 0.910, 1.0 },  /* Ch1 — blue    */
    { 0.290, 0.871, 0.502, 1.0 },  /* Ch2 — green   */
    { 0.961, 0.620, 0.043, 1.0 },  /* Ch3 — amber   */
    { 0.655, 0.545, 0.980, 1.0 },  /* Ch4 — purple  */
};

struct _PerfGroupedHist {
    GtkDrawingArea parent;

    char  title[64];
    char  unit[16];
    int   num_groups;
    int   num_buckets;

    char     group_labels[GROUPED_HIST_MAX_GROUPS][32];
    GdkRGBA  group_colors[GROUPED_HIST_MAX_GROUPS];
    gboolean group_visible[GROUPED_HIST_MAX_GROUPS];
    uint32_t data[GROUPED_HIST_MAX_GROUPS][GROUPED_HIST_MAX_BUCKETS];

    char     bucket_labels[GROUPED_HIST_MAX_BUCKETS][16]; /* custom X labels */
    gboolean use_custom_labels;                           /* false = percentage mode */
    gboolean use_log_scale;                               /* log10 Y axis */
};

G_DEFINE_FINAL_TYPE(PerfGroupedHist, perf_grouped_hist, GTK_TYPE_DRAWING_AREA)

/* log_pct_norm → ui_ui_log_pct_norm() in ui_math.c */

/* ── Draw function ────────────────────────────────────────────────────────── */

static void draw_fn(GtkDrawingArea* da, cairo_t* cr,
                    int width, int height, gpointer user_data) {
    PerfGroupedHist* self = PERF_GROUPED_HIST(user_data);
    (void)da;

    const int PAD_L = 50, PAD_R = 8, PAD_T = 32, PAD_B = 48;
    int chart_w = width  - PAD_L - PAD_R;
    int chart_h = height - PAD_T - PAD_B;
    if (chart_w < 10 || chart_h < 10) return;

    /* Background */
    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
    cairo_paint(cr);

    /* Title */
    cairo_set_source_rgba(cr, 0.533, 0.533, 0.533, 1.0);
    cairo_set_font_size(cr, 16.0);
    cairo_move_to(cr, PAD_L, 20);
    cairo_show_text(cr, self->title);

    /* Count visible groups for bar layout */
    int visible_groups[GROUPED_HIST_MAX_GROUPS];
    int num_visible = 0;
    for (int g = 0; g < self->num_groups; g++) {
        if (self->group_visible[g])
            visible_groups[num_visible++] = g;
    }
    if (num_visible == 0) return;

    /* Find max count across visible groups/buckets for Y scaling */
    uint32_t max_count = 1;
    for (int vi = 0; vi < num_visible; vi++) {
        int g = visible_groups[vi];
        for (int b = 0; b < self->num_buckets; b++)
            if (self->data[g][b] > max_count) max_count = self->data[g][b];
    }

    /* Compute total samples per group for normalization */
    uint32_t group_total[GROUPED_HIST_MAX_GROUPS] = {0};
    for (int g = 0; g < self->num_groups; g++)
        for (int b = 0; b < self->num_buckets; b++)
            group_total[g] += self->data[g][b];

    /* Draw grid lines + Y labels */
    cairo_set_font_size(cr, 14.0);
    if (self->use_log_scale) {
        /* Piecewise grid: 0%, 0.1%, 1%, 10%, 100% at even ¼ intervals */
        static const double grid_pcts[] = { 0.0, 0.1, 1.0, 10.0, 100.0 };
        static const char*  grid_labels[] = { "0%", "0.1%", "1%", "10%", "100%" };
        for (int d = 0; d < 5; d++) {
            double norm = ui_log_pct_norm(grid_pcts[d]);
            double y = PAD_T + chart_h - (chart_h * norm);
            cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, 1.0);
            cairo_set_line_width(cr, 0.5);
            cairo_move_to(cr, PAD_L, y);
            cairo_line_to(cr, PAD_L + chart_w, y);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
            cairo_move_to(cr, 2, y + 3);
            cairo_show_text(cr, grid_labels[d]);
        }
    } else {
        for (int pct = 0; pct <= 100; pct += 25) {
            double y = PAD_T + chart_h - (chart_h * pct / 100.0);
            cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, 1.0);
            cairo_set_line_width(cr, 0.5);
            cairo_move_to(cr, PAD_L, y);
            cairo_line_to(cr, PAD_L + chart_w, y);
            cairo_stroke(cr);

            char label[8];
            snprintf(label, sizeof(label), "%d%%", pct);
            cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
            cairo_move_to(cr, 2, y + 3);
            cairo_show_text(cr, label);
        }
    }

    /* Bar layout: use num_visible for spacing, not num_groups */
    double bucket_w = (double)chart_w / self->num_buckets;
    double group_bar_w = bucket_w * 0.82 / num_visible;
    double bucket_gap = bucket_w * 0.09;

    for (int b = 0; b < self->num_buckets; b++) {
        double bx = PAD_L + b * bucket_w + bucket_gap;

        for (int vi = 0; vi < num_visible; vi++) {
            int g = visible_groups[vi];
            uint32_t count = self->data[g][b];
            double pct_of_samples = (group_total[g] > 0)
                ? (double)count / (double)group_total[g] * 100.0
                : 0.0;
            double bar_h;
            if (self->use_log_scale) {
                double norm = ui_log_pct_norm(pct_of_samples);
                if (norm > 1.0) norm = 1.0;
                bar_h = chart_h * norm;
            } else {
                bar_h = chart_h * pct_of_samples / 100.0;
            }

            double bx_g = bx + vi * group_bar_w;
            double by = PAD_T + chart_h - bar_h;

            const GdkRGBA* c = &self->group_colors[g];
            cairo_set_source_rgba(cr, c->red, c->green, c->blue, 0.85);
            cairo_rectangle(cr, bx_g, by, group_bar_w - 0.5, bar_h);
            cairo_fill(cr);
        }

        /* X label: custom labels or percentage mode */
        cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
        cairo_set_font_size(cr, 12.0);
        if (self->use_custom_labels && self->bucket_labels[b][0]) {
            cairo_move_to(cr, PAD_L + b * bucket_w + bucket_w * 0.1, height - 4);
            cairo_show_text(cr, self->bucket_labels[b]);
        } else {
            char xlabel[8];
            int lo = b * (100 / self->num_buckets);
            snprintf(xlabel, sizeof(xlabel), "%d", lo);
            cairo_move_to(cr, PAD_L + b * bucket_w + bucket_w * 0.3, height - 4);
            cairo_show_text(cr, xlabel);
        }
    }

    /* Legend (top-right, horizontal, only visible groups) */
    double lx = PAD_L + chart_w - num_visible * 60;
    double ly = 6;
    for (int vi = 0; vi < num_visible; vi++) {
        int g = visible_groups[vi];
        const GdkRGBA* c = &self->group_colors[g];
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, 0.9);
        cairo_rectangle(cr, lx + vi * 60, ly, 12, 10);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 1.0);
        cairo_set_font_size(cr, 14.0);
        cairo_move_to(cr, lx + vi * 60 + 16, ly + 11);
        cairo_show_text(cr, self->group_labels[g]);
    }

    /* X axis unit label */
    cairo_set_source_rgba(cr, 0.35, 0.35, 0.35, 1.0);
    cairo_set_font_size(cr, 14.0);
    cairo_move_to(cr, PAD_L + chart_w / 2 - 12, height - 2);
    cairo_show_text(cr, self->unit);
}

/* ── GObject boilerplate ──────────────────────────────────────────────────── */

static void perf_grouped_hist_init(PerfGroupedHist* self) {
    /* Set default colors, labels, and visibility */
    for (int g = 0; g < GROUPED_HIST_MAX_GROUPS; g++) {
        self->group_colors[g] = DEFAULT_COLORS[g];
        self->group_visible[g] = TRUE;
        snprintf(self->group_labels[g], sizeof(self->group_labels[g]), "Ch%d", g + 1);
    }
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self), draw_fn, self, NULL);
}

static void perf_grouped_hist_class_init(PerfGroupedHistClass* klass) {
    (void)klass;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

GtkWidget* perf_grouped_hist_new(const char* title, const char* unit,
                                  int num_groups, int num_buckets) {
    PerfGroupedHist* self = g_object_new(PERF_TYPE_GROUPED_HIST, NULL);

    if (title) {
        g_strlcpy(self->title, title, sizeof(self->title));
        self->title[sizeof(self->title) - 1] = '\0';
    }
    if (unit) {
        g_strlcpy(self->unit, unit, sizeof(self->unit));
        self->unit[sizeof(self->unit) - 1] = '\0';
    }
    self->num_groups  = (num_groups  > GROUPED_HIST_MAX_GROUPS)  ? GROUPED_HIST_MAX_GROUPS  : num_groups;
    self->num_buckets = (num_buckets > GROUPED_HIST_MAX_BUCKETS) ? GROUPED_HIST_MAX_BUCKETS : num_buckets;

    return GTK_WIDGET(self);
}

void perf_grouped_hist_set_group(PerfGroupedHist* hist, int group,
                                  const char* label, const GdkRGBA* color) {
    g_return_if_fail(PERF_IS_GROUPED_HIST(hist));
    if (group < 0 || group >= GROUPED_HIST_MAX_GROUPS) return;

    if (label) {
        g_strlcpy(hist->group_labels[group], label, sizeof(hist->group_labels[group]));
        hist->group_labels[group][sizeof(hist->group_labels[group]) - 1] = '\0';
    }
    if (color) hist->group_colors[group] = *color;
}

void perf_grouped_hist_set_data(PerfGroupedHist* hist, int group,
                                 const uint32_t* bucket_counts, int count) {
    g_return_if_fail(PERF_IS_GROUPED_HIST(hist));
    if (group < 0 || group >= GROUPED_HIST_MAX_GROUPS) return;
    if (!bucket_counts || count <= 0) return;

    int n = (count > GROUPED_HIST_MAX_BUCKETS) ? GROUPED_HIST_MAX_BUCKETS : count;
    memcpy(hist->data[group], bucket_counts, n * sizeof(uint32_t));
    gtk_widget_queue_draw(GTK_WIDGET(hist));
}

void perf_grouped_hist_set_num_buckets(PerfGroupedHist* hist, int num_buckets) {
    g_return_if_fail(PERF_IS_GROUPED_HIST(hist));
    if (num_buckets < 1) num_buckets = 1;
    if (num_buckets > GROUPED_HIST_MAX_BUCKETS) num_buckets = GROUPED_HIST_MAX_BUCKETS;
    hist->num_buckets = num_buckets;
    memset(hist->data, 0, sizeof(hist->data));
    gtk_widget_queue_draw(GTK_WIDGET(hist));
}

void perf_grouped_hist_set_bucket_label(PerfGroupedHist* hist, int bucket, const char* label) {
    g_return_if_fail(PERF_IS_GROUPED_HIST(hist));
    if (bucket < 0 || bucket >= GROUPED_HIST_MAX_BUCKETS) return;
    if (label) {
        g_strlcpy(hist->bucket_labels[bucket], label, sizeof(hist->bucket_labels[bucket]));
        hist->bucket_labels[bucket][sizeof(hist->bucket_labels[bucket]) - 1] = '\0';
    } else {
        hist->bucket_labels[bucket][0] = '\0';
    }
    hist->use_custom_labels = TRUE;
}

void perf_grouped_hist_set_group_visible(PerfGroupedHist* hist, int group, gboolean visible) {
    g_return_if_fail(PERF_IS_GROUPED_HIST(hist));
    if (group < 0 || group >= GROUPED_HIST_MAX_GROUPS) return;
    if (hist->group_visible[group] == visible) return;
    hist->group_visible[group] = visible;
    gtk_widget_queue_draw(GTK_WIDGET(hist));
}

void perf_grouped_hist_set_log_scale(PerfGroupedHist* hist, gboolean log_scale) {
    g_return_if_fail(PERF_IS_GROUPED_HIST(hist));
    hist->use_log_scale = log_scale;
    gtk_widget_queue_draw(GTK_WIDGET(hist));
}
