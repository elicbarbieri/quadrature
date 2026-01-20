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
    float bars[MAX_BARS];
    float smoothed[MAX_BARS];
    int num_bars;
};

G_DEFINE_FINAL_TYPE(UiSpectrum, ui_spectrum, GTK_TYPE_WIDGET)

static void ui_spectrum_snapshot(GtkWidget *widget, GtkSnapshot *snap) {
    UiSpectrum *s = UI_SPECTRUM(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0 || s->num_bars <= 0) return;

    /* Mirrored cava-style: bass in center, bars extend up and down */
    float cy = h / 2.0f;
    float max_h = fmaxf(cy - 1.0f, 1.0f);  /* ensure at least 1px bar height */

    /* Calculate how many bars fit with minimum bar width */
    /* total_bars * (bar_width + gap) - gap <= width */
    /* total_bars <= (width + gap) / (bar_width + gap) */
    int max_total = (int)((w + BAR_GAP) / (MIN_BAR_WIDTH + BAR_GAP));
    int max_half = max_total / 2;  /* mirrored, so half on each side */
    if (max_half < 1) max_half = 1;  /* always show at least 1 bar */
    int num_bars = (max_half < s->num_bars) ? max_half : s->num_bars;

    int total = num_bars * 2;
    float gap_total = BAR_GAP * (total - 1);
    float bw = fmaxf(MIN_BAR_WIDTH, (w - gap_total) / total);

    for (int i = 0; i < num_bars; i++) {
        float val = fmaxf(s->smoothed[i], 0.03f);
        float bar_h = max_h * val;
        float alpha = 0.7f + val * 0.3f;
        GdkRGBA c = {UI_COLOR_CYAN.red, UI_COLOR_CYAN.green, UI_COLOR_CYAN.blue, alpha};

        /* Left side: reversed (bass at center) */
        int li = num_bars - 1 - i;
        float lx = li * (bw + BAR_GAP);

        /* Right side: normal (bass at center) */
        float rx = (num_bars + i) * (bw + BAR_GAP);

        float y = cy - bar_h;
        float th = bar_h * 2.0f;

        graphene_rect_t lr = GRAPHENE_RECT_INIT(lx, y, bw, th);
        GskRoundedRect lrr;
        gsk_rounded_rect_init_from_rect(&lrr, &lr, BAR_RADIUS);
        gtk_snapshot_push_rounded_clip(snap, &lrr);
        gtk_snapshot_append_color(snap, &c, &lr);
        gtk_snapshot_pop(snap);

        graphene_rect_t rr = GRAPHENE_RECT_INIT(rx, y, bw, th);
        GskRoundedRect rrr;
        gsk_rounded_rect_init_from_rect(&rrr, &rr, BAR_RADIUS);
        gtk_snapshot_push_rounded_clip(snap, &rrr);
        gtk_snapshot_append_color(snap, &c, &rr);
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
    memset(s->bars, 0, sizeof(s->bars));
    memset(s->smoothed, 0, sizeof(s->smoothed));
}

GtkWidget *ui_spectrum_new(int num_bars) {
    UiSpectrum *s = g_object_new(UI_TYPE_SPECTRUM, NULL);
    if (num_bars > 0 && num_bars <= MAX_BARS) {
        s->num_bars = num_bars;
    }
    return GTK_WIDGET(s);
}

void ui_spectrum_set_bars(UiSpectrum *s, const float *bars, int count) {
    g_return_if_fail(UI_IS_SPECTRUM(s));
    if (!bars || count <= 0) return;

    int n = MIN(count, s->num_bars);
    for (int i = 0; i < n; i++) {
        float target = CLAMP(bars[i], 0.0f, 1.0f);
        float cur = s->smoothed[i];
        /* Fast attack, slow release */
        s->smoothed[i] = (target > cur)
            ? cur + (target - cur) * 0.5f
            : cur + (target - cur) * 0.15f;
        s->bars[i] = target;
    }
    gtk_widget_queue_draw(GTK_WIDGET(s));
}
