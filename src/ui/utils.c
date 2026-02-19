/**
 * Quadrature UI Utilities
 *
 * Reusable UI widgets and helpers shared across the UI layer.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * ProportionalBox Widget
 *
 * Horizontal container with four named child slots:
 *   art (natural) | left (ratio * flexible) | right ((1-ratio) * flexible) | meta (natural)
 *
 * Sizing is done entirely inside GTK's own size_allocate vfunc so children
 * receive their final widths before the first pixel is drawn — no tick
 * callback, no post-allocation set_size_request, no one-frame pop.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _ProportionalBox {
    GtkWidget  parent_instance;
    GtkWidget *art;
    GtkWidget *col_left;
    GtkWidget *col_right;
    GtkWidget *col_meta;
    double     left_ratio;
    int        spacing;
};

enum {
    PROP_0,
    PROP_SPACING,
    PROP_LEFT_RATIO,
    N_PROPS
};

static GParamSpec *prop_specs[N_PROPS];

static void proportional_box_buildable_init(GtkBuildableIface *iface);

G_DEFINE_TYPE_WITH_CODE(ProportionalBox, proportional_box, GTK_TYPE_WIDGET,
    G_IMPLEMENT_INTERFACE(GTK_TYPE_BUILDABLE, proportional_box_buildable_init))

/* ── child slot management ────────────────────────────────────────────── */

static void pbox_set_slot(ProportionalBox *self, GtkWidget **slot, GtkWidget *child) {
    if (*slot) gtk_widget_unparent(*slot);
    *slot = child;
    if (child) gtk_widget_set_parent(child, GTK_WIDGET(self));
}

/* ── GtkBuildable: map child type= to slot ────────────────────────────── */

static void proportional_box_buildable_add_child(GtkBuildable *buildable,
    GtkBuilder *builder, GObject *child, const char *type)
{
    (void)builder;
    if (!GTK_IS_WIDGET(child)) return;
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(buildable);
    GtkWidget *w = GTK_WIDGET(child);

    if      (g_strcmp0(type, "art")   == 0) pbox_set_slot(self, &self->art,       w);
    else if (g_strcmp0(type, "left")  == 0) pbox_set_slot(self, &self->col_left,  w);
    else if (g_strcmp0(type, "right") == 0) pbox_set_slot(self, &self->col_right, w);
    else if (g_strcmp0(type, "meta")  == 0) pbox_set_slot(self, &self->col_meta,  w);
    else GTK_BUILDER_WARN_INVALID_CHILD_TYPE(buildable, type);
}

static void proportional_box_buildable_init(GtkBuildableIface *iface) {
    iface->add_child = proportional_box_buildable_add_child;
}

/* ── GObject property boilerplate ─────────────────────────────────────── */

static void proportional_box_get_property(GObject *obj, guint id,
    GValue *val, GParamSpec *pspec)
{
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(obj);
    switch (id) {
        case PROP_SPACING:    g_value_set_int   (val, self->spacing);    break;
        case PROP_LEFT_RATIO: g_value_set_double(val, self->left_ratio); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void proportional_box_set_property(GObject *obj, guint id,
    const GValue *val, GParamSpec *pspec)
{
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(obj);
    switch (id) {
        case PROP_SPACING:
            self->spacing = g_value_get_int(val);
            gtk_widget_queue_resize(GTK_WIDGET(self));
            break;
        case PROP_LEFT_RATIO:
            self->left_ratio = g_value_get_double(val);
            gtk_widget_queue_resize(GTK_WIDGET(self));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void proportional_box_dispose(GObject *obj) {
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(obj);
    g_clear_pointer(&self->art,       gtk_widget_unparent);
    g_clear_pointer(&self->col_left,  gtk_widget_unparent);
    g_clear_pointer(&self->col_right, gtk_widget_unparent);
    g_clear_pointer(&self->col_meta,  gtk_widget_unparent);
    G_OBJECT_CLASS(proportional_box_parent_class)->dispose(obj);
}

/* ── GTK layout vfuncs ────────────────────────────────────────────────── */

static void proportional_box_measure(GtkWidget *widget, GtkOrientation orientation,
    int for_size, int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
    (void)for_size;
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(widget);
    GtkWidget *slots[] = { self->art, self->col_left, self->col_right, self->col_meta };
    int min_acc = 0, nat_acc = 0, n_visible = 0;

    for (int i = 0; i < 4; i++) {
        if (!slots[i] || !gtk_widget_get_visible(slots[i])) continue;
        int cmin, cnat;
        gtk_widget_measure(slots[i], orientation, -1, &cmin, &cnat, NULL, NULL);
        if (orientation == GTK_ORIENTATION_HORIZONTAL) {
            min_acc += cmin;
            nat_acc += cnat;
            n_visible++;
        } else {
            min_acc = MAX(min_acc, cmin);
            nat_acc = MAX(nat_acc, cnat);
        }
    }

    if (orientation == GTK_ORIENTATION_HORIZONTAL && n_visible > 1) {
        int gap = self->spacing * (n_visible - 1);
        min_acc += gap;
        nat_acc += gap;
    }

    *minimum = min_acc;
    *natural = nat_acc;
    if (min_baseline) *min_baseline = -1;
    if (nat_baseline) *nat_baseline = -1;
}

static void proportional_box_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(widget);
    int sp = self->spacing;

    gboolean has_art   = self->art       && gtk_widget_get_visible(self->art);
    gboolean has_left  = self->col_left  && gtk_widget_get_visible(self->col_left);
    gboolean has_right = self->col_right && gtk_widget_get_visible(self->col_right);
    gboolean has_meta  = self->col_meta  && gtk_widget_get_visible(self->col_meta);

    /* Measure natural widths of the fixed-size outer slots */
    int art_w = 0, meta_w = 0;
    if (has_art) {
        int mn, nat;
        gtk_widget_measure(self->art, GTK_ORIENTATION_HORIZONTAL, height, &mn, &nat, NULL, NULL);
        art_w = nat;
    }
    if (has_meta) {
        int mn, nat;
        gtk_widget_measure(self->col_meta, GTK_ORIENTATION_HORIZONTAL, height, &mn, &nat, NULL, NULL);
        meta_w = nat;
    }

    int n_visible = (has_art ? 1 : 0) + (has_left ? 1 : 0)
                  + (has_right ? 1 : 0) + (has_meta ? 1 : 0);
    int gap_total = sp * MAX(0, n_visible - 1);
    int flexible  = MAX(0, width - art_w - meta_w - gap_total);

    /* Split flexible space between the two centre columns */
    int left_w = 0, right_w = 0;
    if (has_left && has_right) {
        left_w  = (int)(flexible * self->left_ratio);
        right_w = flexible - left_w;
    } else if (has_left) {
        left_w  = flexible;
    } else if (has_right) {
        right_w = flexible;
    }

    /* Allocate children left → right */
    int x = 0;
    if (has_art) {
        gtk_widget_size_allocate(self->art,
            &(GtkAllocation){x, 0, art_w, height}, baseline);
        x += art_w + sp;
    }
    if (has_left) {
        gtk_widget_size_allocate(self->col_left,
            &(GtkAllocation){x, 0, left_w, height}, baseline);
        x += left_w + sp;
    }
    if (has_right) {
        gtk_widget_size_allocate(self->col_right,
            &(GtkAllocation){x, 0, right_w, height}, baseline);
        x += right_w + sp;
    }
    if (has_meta) {
        gtk_widget_size_allocate(self->col_meta,
            &(GtkAllocation){x, 0, meta_w, height}, baseline);
    }
}

/* Children must be snapshotted explicitly — GtkWidget's default does nothing. */
static void proportional_box_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ProportionalBox *self = QUADRATURE_PROPORTIONAL_BOX(widget);
    GtkWidget *slots[] = { self->art, self->col_left, self->col_right, self->col_meta };
    for (int i = 0; i < 4; i++) {
        if (slots[i]) gtk_widget_snapshot_child(widget, slots[i], snapshot);
    }
}

/* ── Class / instance init ────────────────────────────────────────────── */

static void proportional_box_class_init(ProportionalBoxClass *klass) {
    GObjectClass   *obj_class    = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    obj_class->dispose      = proportional_box_dispose;
    obj_class->get_property = proportional_box_get_property;
    obj_class->set_property = proportional_box_set_property;

    widget_class->measure       = proportional_box_measure;
    widget_class->size_allocate = proportional_box_size_allocate;
    widget_class->snapshot      = proportional_box_snapshot;

    gtk_widget_class_set_css_name(widget_class, "proportional-box");

    prop_specs[PROP_SPACING] = g_param_spec_int(
        "spacing", "Spacing", "Gap between child slots in pixels",
        0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    prop_specs[PROP_LEFT_RATIO] = g_param_spec_double(
        "left-ratio", "Left Ratio",
        "Fraction of flexible space given to the left column (0.0–1.0)",
        0.0, 1.0, 0.5, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(obj_class, N_PROPS, prop_specs);
}

static void proportional_box_init(ProportionalBox *self) {
    self->left_ratio = 0.5;
    self->spacing    = 0;
}
