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

    /* Pre-allocate callbacks — run with the computed pixel budget
     * BEFORE gtk_widget_size_allocate on each flexible child.
     * Lets consumers (genre pills, art strip) reflow children while
     * the budget is known and before the child's vfunc lays them out. */
    PBoxPreAllocate pre_alloc_left;
    gpointer        pre_alloc_left_data;
    PBoxPreAllocate pre_alloc_right;
    gpointer        pre_alloc_right_data;
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
    /* Fixed slots (art, meta) contribute their full minimum width.
     * Flexible slots (left, right) only contribute to natural width —
     * they accept whatever proportional space remains after fixed slots.
     * This prevents content-heavy rows (e.g. many credit pills) from
     * inflating the minimum width and pushing sibling panels off-screen. */
    GtkWidget *fixed_slots[]    = { self->art, self->col_meta };
    GtkWidget *flexible_slots[] = { self->col_left, self->col_right };
    int min_acc = 0, nat_acc = 0, n_visible = 0;

    for (int i = 0; i < 2; i++) {
        if (!fixed_slots[i] || !gtk_widget_get_visible(fixed_slots[i])) continue;
        int cmin, cnat;
        gtk_widget_measure(fixed_slots[i], orientation, -1, &cmin, &cnat, NULL, NULL);
        if (orientation == GTK_ORIENTATION_HORIZONTAL) {
            min_acc += cmin;
            nat_acc += cnat;
            n_visible++;
        } else {
            min_acc = MAX(min_acc, cmin);
            nat_acc = MAX(nat_acc, cnat);
        }
    }

    for (int i = 0; i < 2; i++) {
        if (!flexible_slots[i] || !gtk_widget_get_visible(flexible_slots[i])) continue;
        int cmin, cnat;
        gtk_widget_measure(flexible_slots[i], orientation, -1, &cmin, &cnat, NULL, NULL);
        if (orientation == GTK_ORIENTATION_HORIZONTAL) {
            /* Flexible columns: minimum is 0, natural is full request */
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

    /* Fire pre-allocate callbacks with the exact pixel budget.
     * Consumers (genre pills, art strip) reflow children here —
     * BEFORE the child's GtkBox vfunc lays them out. */
    if (has_left && self->pre_alloc_left)
        self->pre_alloc_left(self->col_left, left_w, self->pre_alloc_left_data);
    if (has_right && self->pre_alloc_right)
        self->pre_alloc_right(self->col_right, right_w, self->pre_alloc_right_data);

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

void proportional_box_set_pre_allocate(ProportionalBox *self,
                                        const char *slot,
                                        PBoxPreAllocate callback,
                                        gpointer user_data) {
    g_return_if_fail(QUADRATURE_IS_PROPORTIONAL_BOX(self));
    if (g_strcmp0(slot, "left") == 0) {
        self->pre_alloc_left = callback;
        self->pre_alloc_left_data = user_data;
    } else if (g_strcmp0(slot, "right") == 0) {
        self->pre_alloc_right = callback;
        self->pre_alloc_right_data = user_data;
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

/* ═══════════════════════════════════════════════════════════════════════════
 * QuadOverflowBox Widget
 *
 * Horizontal container that shows as many children as fit within the
 * allocated width.  The LAST child is treated as the overflow indicator
 * ("…") and is shown only when items are hidden.
 *
 * Overflow planning happens inside size_allocate — not reactively via
 * signals.  Children that don't fit get child_visible=FALSE and zero
 * allocation.  No queue_resize, no deferred correction, no clipping.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _QuadOverflowBox {
    GtkWidget parent_instance;
    int       spacing;
    int       row_spacing;
    guint     item_count;   /* Number of populated items (excludes overflow indicator) */
    guint     max_rows;     /* Max visible rows (1 = single-row, default) */
};

enum {
    OFB_PROP_0,
    OFB_PROP_SPACING,
    OFB_PROP_ROW_SPACING,
    OFB_PROP_MAX_ROWS,
    OFB_N_PROPS
};

static GParamSpec *ofb_props[OFB_N_PROPS];

G_DEFINE_TYPE(QuadOverflowBox, quad_overflow_box, GTK_TYPE_WIDGET)

/* ── Helpers ─────────────────────────────────────────────────────────── */

/** Count visible children (all children are always "visible" — we control
 *  display via child_visible and allocation, not the visible property). */
static guint ofb_child_count(GtkWidget *widget) {
    guint n = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c))
        n++;
    return n;
}

static GtkWidget *ofb_nth_child(GtkWidget *widget, guint n) {
    GtkWidget *c = gtk_widget_get_first_child(widget);
    for (guint i = 0; i < n && c; i++)
        c = gtk_widget_get_next_sibling(c);
    return c;
}

/* ── GObject ─────────────────────────────────────────────────────────── */

static void quad_overflow_box_get_property(GObject *obj, guint id,
    GValue *val, GParamSpec *pspec)
{
    QuadOverflowBox *self = QUADRATURE_OVERFLOW_BOX(obj);
    switch (id) {
    case OFB_PROP_SPACING:     g_value_set_int(val, self->spacing);     break;
    case OFB_PROP_ROW_SPACING: g_value_set_int(val, self->row_spacing); break;
    case OFB_PROP_MAX_ROWS:    g_value_set_uint(val, self->max_rows);   break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void quad_overflow_box_set_property(GObject *obj, guint id,
    const GValue *val, GParamSpec *pspec)
{
    QuadOverflowBox *self = QUADRATURE_OVERFLOW_BOX(obj);
    switch (id) {
    case OFB_PROP_SPACING:     self->spacing = g_value_get_int(val);       break;
    case OFB_PROP_ROW_SPACING: self->row_spacing = g_value_get_int(val);   break;
    case OFB_PROP_MAX_ROWS:    self->max_rows = g_value_get_uint(val);     break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec); return;
    }
    gtk_widget_queue_resize(GTK_WIDGET(obj));
}

static void quad_overflow_box_dispose(GObject *obj) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(obj));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_widget_unparent(child);
        child = next;
    }
    G_OBJECT_CLASS(quad_overflow_box_parent_class)->dispose(obj);
}

/* ── Measure ─────────────────────────────────────────────────────────── */

static GtkSizeRequestMode quad_overflow_box_get_request_mode(GtkWidget *widget) {
    (void)widget;
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void quad_overflow_box_measure(GtkWidget *widget, GtkOrientation orientation,
    int for_size, int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
    QuadOverflowBox *self = QUADRATURE_OVERFLOW_BOX(widget);
    if (min_baseline) *min_baseline = -1;
    if (nat_baseline) *nat_baseline = -1;

    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        /* Natural = sum of all children; minimum = 0 (can hide everything). */
        int nat_acc = 0;
        guint n = 0;
        for (GtkWidget *c = gtk_widget_get_first_child(widget); c;
             c = gtk_widget_get_next_sibling(c)) {
            int cnat;
            gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1,
                               NULL, &cnat, NULL, NULL);
            nat_acc += cnat + (n > 0 ? self->spacing : 0);
            n++;
        }
        *minimum = 0;
        *natural = nat_acc;
        return;
    }

    /* Vertical: compute row height, then number of rows given available width. */
    int row_h = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c)) {
        int cnat;
        gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, -1,
                           NULL, &cnat, NULL, NULL);
        row_h = MAX(row_h, cnat);
    }
    if (row_h == 0) { *minimum = *natural = 0; return; }

    guint rows = 1;
    if (self->max_rows > 1) {
        if (for_size > 0) {
            /* Simulate flow to count rows given available width */
            int x = 0;
            GtkWidget *c = gtk_widget_get_first_child(widget);
            for (guint i = 0; i < self->item_count && c; i++) {
                int cnat;
                gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1,
                                   NULL, &cnat, NULL, NULL);
                int needed = (x > 0) ? self->spacing + cnat : cnat;
                if (x > 0 && x + needed > for_size) {
                    rows++;
                    x = cnat;
                } else {
                    x += needed;
                }
                c = gtk_widget_get_next_sibling(c);
            }
            if (rows > self->max_rows) rows = self->max_rows;
        } else {
            /* No width hint — report max possible height to prevent overlap */
            rows = self->max_rows;
        }
    }

    int h = (int)rows * row_h + ((int)rows - 1) * self->row_spacing;
    *minimum = h;  /* Multi-row: min = natural to prevent under-allocation */
    *natural = h;
}

/* ── Size Allocate — the overflow planning happens HERE ──────────── */

static void quad_overflow_box_size_allocate(GtkWidget *widget, int width,
    int height, int baseline)
{
    QuadOverflowBox *self = QUADRATURE_OVERFLOW_BOX(widget);
    guint total = ofb_child_count(widget);
    if (total == 0) return;

    guint item_count = self->item_count;
    guint max_rows   = self->max_rows;
    GtkWidget *overflow_widget = ofb_nth_child(widget, total - 1);

    /* ── Measure all items ─────────────────────────────────────────── */
    int item_nat_w[item_count > 0 ? item_count : 1];
    int row_h = 0;
    GtkWidget *child = gtk_widget_get_first_child(widget);
    for (guint i = 0; i < item_count && child; i++) {
        int nat_w = 0, nat_h = 0;
        gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1,
                           NULL, &nat_w, NULL, NULL);
        gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, -1,
                           NULL, &nat_h, NULL, NULL);
        item_nat_w[i] = nat_w;
        row_h = MAX(row_h, nat_h);
        child = gtk_widget_get_next_sibling(child);
    }

    int overflow_nat_w = 0;
    if (overflow_widget) {
        int nat_h = 0;
        gtk_widget_measure(overflow_widget, GTK_ORIENTATION_HORIZONTAL, -1,
                           NULL, &overflow_nat_w, NULL, NULL);
        gtk_widget_measure(overflow_widget, GTK_ORIENTATION_VERTICAL, -1,
                           NULL, &nat_h, NULL, NULL);
        row_h = MAX(row_h, nat_h);
    }
    if (row_h == 0) row_h = height;

    /* ── Phase 1: flow items into rows (unlimited) ─────────────────── */
    guint item_row[item_count > 0 ? item_count : 1];
    int   item_x[item_count > 0 ? item_count : 1];
    int x = 0;
    guint row = 0;

    for (guint i = 0; i < item_count; i++) {
        int w = item_nat_w[i];
        int needed = (x > 0) ? self->spacing + w : w;
        if (x > 0 && x + needed > width) {
            /* wrap to next row */
            row++;
            x = 0;
            needed = w;
        }
        item_row[i] = row;
        item_x[i]   = x;
        x += needed;
    }
    guint total_rows = row + 1;

    /* ── Phase 2: apply max_rows constraint ────────────────────────── */
    gboolean needs_overflow = FALSE;
    guint show = item_count;

    if (max_rows > 0 && total_rows > max_rows) {
        needs_overflow = TRUE;
        guint last_row = max_rows - 1;

        /* Find first item beyond the last allowed row */
        show = item_count;
        for (guint i = 0; i < item_count; i++) {
            if (item_row[i] > last_row) { show = i; break; }
        }

        /* On the last row, remove items from right until "…" fits */
        while (show > 0 && item_row[show - 1] == last_row) {
            int end_x = item_x[show - 1] + item_nat_w[show - 1];
            if (end_x + self->spacing + overflow_nat_w <= width) break;
            show--;
        }
    }

    /* ── Phase 3: allocate visible items ───────────────────────────── */
    child = gtk_widget_get_first_child(widget);
    for (guint i = 0; i < item_count && child; i++) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (i < show) {
            int y = (int)item_row[i] * (row_h + self->row_spacing);
            gtk_widget_set_child_visible(child, TRUE);
            gtk_widget_size_allocate(child,
                &(GtkAllocation){item_x[i], y, item_nat_w[i], row_h}, baseline);
        } else {
            gtk_widget_set_child_visible(child, FALSE);
            /* Use natural size to avoid GTK allocation warnings */
            gtk_widget_size_allocate(child,
                &(GtkAllocation){0, 0, item_nat_w[i], row_h}, baseline);
        }
        child = next;
    }

    /* Hide unpopulated pre-allocated slots */
    for (; child && child != overflow_widget;) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_widget_set_child_visible(child, FALSE);
        gtk_widget_size_allocate(child,
            &(GtkAllocation){0, 0, 1, row_h > 0 ? row_h : 1}, baseline);
        child = next;
    }

    /* ── Overflow indicator ────────────────────────────────────────── */
    if (overflow_widget) {
        if (needs_overflow) {
            int ov_x = 0;
            guint ov_row = 0;
            if (show > 0) {
                ov_row = item_row[show - 1];
                ov_x = item_x[show - 1] + item_nat_w[show - 1] + self->spacing;
            } else if (max_rows > 0) {
                ov_row = max_rows - 1;
            }
            int y = (int)ov_row * (row_h + self->row_spacing);
            gtk_widget_set_child_visible(overflow_widget, TRUE);
            gtk_widget_size_allocate(overflow_widget,
                &(GtkAllocation){ov_x, y, overflow_nat_w, row_h}, baseline);
        } else {
            gtk_widget_set_child_visible(overflow_widget, FALSE);
            gtk_widget_size_allocate(overflow_widget,
                &(GtkAllocation){0, 0, overflow_nat_w,
                                 row_h > 0 ? row_h : 1}, baseline);
        }
    }
}

/* ── Snapshot ────────────────────────────────────────────────────────── */

static void quad_overflow_box_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (gtk_widget_get_child_visible(c))
            gtk_widget_snapshot_child(widget, c, snapshot);
    }
}

/* ── Class / instance init ───────────────────────────────────────────── */

static void quad_overflow_box_class_init(QuadOverflowBoxClass *klass) {
    GObjectClass   *obj_class    = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    obj_class->dispose      = quad_overflow_box_dispose;
    obj_class->get_property = quad_overflow_box_get_property;
    obj_class->set_property = quad_overflow_box_set_property;

    widget_class->get_request_mode = quad_overflow_box_get_request_mode;
    widget_class->measure          = quad_overflow_box_measure;
    widget_class->size_allocate    = quad_overflow_box_size_allocate;
    widget_class->snapshot         = quad_overflow_box_snapshot;

    gtk_widget_class_set_css_name(widget_class, "overflow-box");

    ofb_props[OFB_PROP_SPACING] = g_param_spec_int(
        "spacing", "Spacing", "Horizontal gap between children in pixels",
        0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    ofb_props[OFB_PROP_ROW_SPACING] = g_param_spec_int(
        "row-spacing", "Row Spacing", "Vertical gap between rows in pixels",
        0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    ofb_props[OFB_PROP_MAX_ROWS] = g_param_spec_uint(
        "max-rows", "Max Rows", "Maximum visible rows (1 = single-row)",
        1, G_MAXUINT, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(obj_class, OFB_N_PROPS, ofb_props);
}

static void quad_overflow_box_init(QuadOverflowBox *self) {
    self->spacing     = 0;
    self->row_spacing = 0;
    self->max_rows    = 1;
}

void quad_overflow_box_append(QuadOverflowBox *self, GtkWidget *child) {
    gtk_widget_set_parent(child, GTK_WIDGET(self));
}

void quad_overflow_box_set_item_count(QuadOverflowBox *self, guint count) {
    self->item_count = count;
    gtk_widget_queue_allocate(GTK_WIDGET(self));
}
