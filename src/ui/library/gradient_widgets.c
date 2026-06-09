/**
 * Quadrature Gradient Widgets
 *
 * GPU-accelerated gradient overlays for detail view backgrounds.
 * EdgeColors: samples average color from top/bottom rows of a texture.
 * QuadGradientFade: GtkWidget that renders a linear gradient via snapshot.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge-Color Sampling
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Sample average color from top and bottom N rows of a texture.
 * Downloads texture to CPU once, averages pixel rows.
 * CAIRO_FORMAT_ARGB32 on little-endian: bytes are [B, G, R, A]. */
EdgeColors
sample_edge_colors(GdkTexture *texture, int num_rows)
{
    int w = gdk_texture_get_width(texture);
    int h = gdk_texture_get_height(texture);
    int rows = MIN(num_rows, h / 2);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    guchar *data = cairo_image_surface_get_data(surface);
    gsize stride = (gsize)cairo_image_surface_get_stride(surface);
    gdk_texture_download(texture, data, stride);

    /* Average top rows */
    double tr = 0, tg = 0, tb = 0;
    int count = rows * w;
    for (int y = 0; y < rows; y++) {
        guchar *row = data + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *px = row + x * 4;
            tr += px[2];
            tg += px[1];
            tb += px[0];
        }
    }

    /* Average bottom rows */
    double br = 0, bg_ = 0, bb = 0;
    for (int y = h - rows; y < h; y++) {
        guchar *row = data + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *px = row + x * 4;
            br += px[2];
            bg_ += px[1];
            bb += px[0];
        }
    }

    cairo_surface_destroy(surface);

    EdgeColors colors = {
        .top = { tr / count / 255.0, tg / count / 255.0, tb / count / 255.0, 1.0 },
        .bottom = { br / count / 255.0, bg_ / count / 255.0, bb / count / 255.0, 1.0 },
    };
    return colors;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * QuadGradientFade — GPU-accelerated gradient overlay widget
 *
 * Replaces GtkDrawingArea + cairo with gtk_snapshot_append_linear_gradient().
 * Cairo paints to a CPU surface then uploads a texture every frame;
 * linear_gradient goes straight to the GPU compositor as a shader op.
 * ═══════════════════════════════════════════════════════════════════════════ */

G_DEFINE_FINAL_TYPE(QuadGradientFade, quad_gradient_fade, GTK_TYPE_WIDGET)

static void
quad_gradient_fade_snapshot(GtkWidget *widget, GtkSnapshot *snap)
{
    QuadGradientFade *self = (QuadGradientFade *)widget;
    float w = (float)gtk_widget_get_width(widget);
    float h = (float)gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0)
        return;

    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, w, h);
    graphene_point_t start = GRAPHENE_POINT_INIT(0, 0);
    graphene_point_t end = GRAPHENE_POINT_INIT(0, h);

    const GdkRGBA *ec = &self->grad.edge_color;
    const float bg = 0x12 / 255.0f;

    GskColorStop stops[3];
    if (self->grad.from_top) {
        stops[0] = (GskColorStop){ 0.0f, { bg, bg, bg, 1.0f } };
        stops[1] = (GskColorStop){ 0.3f, { ec->red, ec->green, ec->blue, 0.35f } };
        stops[2] = (GskColorStop){ 1.0f, { ec->red, ec->green, ec->blue, 0.0f } };
    } else {
        stops[0] = (GskColorStop){ 0.0f, { ec->red, ec->green, ec->blue, 0.0f } };
        stops[1] = (GskColorStop){ 0.7f, { ec->red, ec->green, ec->blue, 0.35f } };
        stops[2] = (GskColorStop){ 1.0f, { bg, bg, bg, 1.0f } };
    }

    gtk_snapshot_append_linear_gradient(snap, &bounds, &start, &end, stops, 3);
}

static void
quad_gradient_fade_init(QuadGradientFade *self)
{
    (void)self;
}

static void
quad_gradient_fade_class_init(QuadGradientFadeClass *klass)
{
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    wc->snapshot = quad_gradient_fade_snapshot;
}

void
quad_gradient_fade_set_color(QuadGradientFade *self, const GdkRGBA *color, gboolean from_top)
{
    self->grad.edge_color = *color;
    self->grad.from_top = from_top;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
