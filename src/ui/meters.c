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
