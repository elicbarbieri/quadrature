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
 * QuadratureOverflowBox Widget
 *
 * Horizontal container that shows as many children as fit within the
 * allocated width.  The LAST child is treated as the overflow indicator
 * ("…") and is shown only when items are hidden.
 *
 * Overflow planning happens inside size_allocate — not reactively via
 * signals.  Children that don't fit get child_visible=FALSE and zero
 * allocation.  No queue_resize, no deferred correction, no clipping.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct _QuadratureOverflowBox {
    GtkWidget parent_instance;
    int       spacing;
    int       row_spacing;
    guint     pinned_count; /* First N children always visible, never overflowed */
    guint     item_count;   /* Next M children are items; may overflow to indicator */
    guint     max_rows;     /* Max visible rows (1 = single-row, default) */
};

enum {
    OFB_PROP_0,
    OFB_PROP_SPACING,
    OFB_PROP_ROW_SPACING,
    OFB_PROP_PINNED_COUNT,
    OFB_PROP_ITEM_COUNT,
    OFB_PROP_MAX_ROWS,
    OFB_N_PROPS
};

static GParamSpec *ofb_props[OFB_N_PROPS];

G_DEFINE_TYPE(QuadratureOverflowBox, quadrature_overflow_box, GTK_TYPE_WIDGET)

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

static void quadrature_overflow_box_get_property(GObject *obj, guint id,
    GValue *val, GParamSpec *pspec)
{
    QuadratureOverflowBox *self = QUADRATURE_OVERFLOW_BOX(obj);
    switch (id) {
    case OFB_PROP_SPACING:      g_value_set_int(val, self->spacing);      break;
    case OFB_PROP_ROW_SPACING:  g_value_set_int(val, self->row_spacing);  break;
    case OFB_PROP_PINNED_COUNT: g_value_set_uint(val, self->pinned_count); break;
    case OFB_PROP_ITEM_COUNT:   g_value_set_uint(val, self->item_count);  break;
    case OFB_PROP_MAX_ROWS:     g_value_set_uint(val, self->max_rows);    break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void quadrature_overflow_box_set_property(GObject *obj, guint id,
    const GValue *val, GParamSpec *pspec)
{
    QuadratureOverflowBox *self = QUADRATURE_OVERFLOW_BOX(obj);
    switch (id) {
    case OFB_PROP_SPACING:      self->spacing      = g_value_get_int(val);  break;
    case OFB_PROP_ROW_SPACING:  self->row_spacing  = g_value_get_int(val);  break;
    case OFB_PROP_PINNED_COUNT: self->pinned_count = g_value_get_uint(val); break;
    case OFB_PROP_ITEM_COUNT:   self->item_count   = g_value_get_uint(val); break;
    case OFB_PROP_MAX_ROWS:     self->max_rows     = g_value_get_uint(val); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec); return;
    }
    gtk_widget_queue_resize(GTK_WIDGET(obj));
}

static void quadrature_overflow_box_dispose(GObject *obj) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(obj));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_widget_unparent(child);
        child = next;
    }
    G_OBJECT_CLASS(quadrature_overflow_box_parent_class)->dispose(obj);
}

/* ── Measure ─────────────────────────────────────────────────────────── */

static GtkSizeRequestMode quadrature_overflow_box_get_request_mode(GtkWidget *widget) {
    (void)widget;
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void quadrature_overflow_box_measure(GtkWidget *widget, GtkOrientation orientation,
    int for_size, int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
    QuadratureOverflowBox *self = QUADRATURE_OVERFLOW_BOX(widget);
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
            guint flow = self->pinned_count + self->item_count;
            GtkWidget *c = gtk_widget_get_first_child(widget);
            for (guint i = 0; i < flow && c; i++) {
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
            /* No width hint — report 1-row height (achievable at natural width).
             * GTK requires measure(V,-1).min <= measure(V,W).min for all W;
             * returning max_rows here violates that when a wide W needs fewer rows. */
            rows = 1;
        }
    }

    int h = (int)rows * row_h + ((int)rows - 1) * self->row_spacing;
    *minimum = h;  /* Multi-row: min = natural to prevent under-allocation */
    *natural = h;
}

/* ── Size Allocate — the overflow planning happens HERE ──────────── */

static void quadrature_overflow_box_size_allocate(GtkWidget *widget, int width,
    int height, int baseline)
{
    QuadratureOverflowBox *self = QUADRATURE_OVERFLOW_BOX(widget);
    guint total = ofb_child_count(widget);
    if (total == 0) return;

    guint pinned_count = self->pinned_count;
    guint item_count   = self->item_count;
    guint max_rows     = self->max_rows;
    guint non_overflow = pinned_count + item_count;

    /* The overflow indicator is the LAST child when the caller has reserved
     * more children than pinned+items (e.g. pre-allocated pill slots +
     * overflow indicator). Any children between non_overflow and total-1
     * are reserved-but-unused slots that should be hidden. */
    gboolean has_overflow_child = (total > non_overflow);
    GtkWidget *overflow_widget =
        has_overflow_child ? ofb_nth_child(widget, total - 1) : NULL;

    guint flow_n = non_overflow;

    /* ── Measure all flowed children (pinned + items) ──────────────── */
    int nat_w[flow_n > 0 ? flow_n : 1];
    int row_h = 0;
    GtkWidget *child = gtk_widget_get_first_child(widget);
    for (guint i = 0; i < flow_n && child; i++) {
        int w = 0, h = 0;
        gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
        gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL,   -1, NULL, &h, NULL, NULL);
        nat_w[i] = w;
        row_h = MAX(row_h, h);
        child = gtk_widget_get_next_sibling(child);
    }

    int overflow_nat_w = 0;
    if (overflow_widget) {
        int h = 0;
        gtk_widget_measure(overflow_widget, GTK_ORIENTATION_HORIZONTAL, -1,
                           NULL, &overflow_nat_w, NULL, NULL);
        gtk_widget_measure(overflow_widget, GTK_ORIENTATION_VERTICAL, -1,
                           NULL, &h, NULL, NULL);
        row_h = MAX(row_h, h);
    }
    if (row_h == 0) row_h = height;

    /* ── Phase 1: flow everything onto rows (unlimited) ────────────── */
    guint row_of[flow_n > 0 ? flow_n : 1];
    int   x_of  [flow_n > 0 ? flow_n : 1];
    int x = 0;
    guint row = 0;

    for (guint i = 0; i < flow_n; i++) {
        int w = nat_w[i];
        int needed = (x > 0) ? self->spacing + w : w;
        if (x > 0 && x + needed > width) {
            row++;
            x = 0;
            needed = w;
        }
        row_of[i] = row;
        x_of[i]   = x;
        x += needed;
    }
    guint total_rows = row + 1;

    /* ── Phase 2: apply max_rows constraint (pinned always shown) ──── */
    gboolean needs_overflow = FALSE;
    guint show = flow_n;  /* how many flowed children to render */

    if (max_rows > 0 && total_rows > max_rows) {
        needs_overflow = TRUE;
        guint last_row = max_rows - 1;

        /* Drop everything past the last allowed row */
        show = flow_n;
        for (guint i = 0; i < flow_n; i++) {
            if (row_of[i] > last_row) { show = i; break; }
        }

        /* Never hide pinned children */
        if (show < pinned_count) show = pinned_count;

        /* On the last row, drop ITEMS from right until the indicator fits.
         * Pinned children on the last row are preserved even if this
         * causes visual overflow — they're pinned for a reason. */
        while (show > pinned_count && row_of[show - 1] == last_row) {
            int end_x = x_of[show - 1] + nat_w[show - 1];
            if (end_x + self->spacing + overflow_nat_w <= width) break;
            show--;
        }
    }

    /* ── Phase 2.5: per-row horizontal alignment ───────────────────────
     * Honor halign=END / GTK_ALIGN_CENTER by shifting each row to the
     * right edge / center of the available width. The default GTK_ALIGN_FILL
     * (and START) leaves rows left-aligned. Multi-row mode aligns each row
     * independently, since each row's used width may differ. */
    GtkAlign halign = gtk_widget_get_halign(widget);
    guint align_rows = (max_rows > 0 && total_rows > max_rows)
        ? max_rows : total_rows;
    int row_shift[align_rows > 0 ? align_rows : 1];
    for (guint r = 0; r < align_rows; r++) row_shift[r] = 0;

    if (halign == GTK_ALIGN_END || halign == GTK_ALIGN_CENTER) {
        /* Find the rightmost edge of visible content on each row */
        int row_end[align_rows > 0 ? align_rows : 1];
        for (guint r = 0; r < align_rows; r++) row_end[r] = 0;
        for (guint i = 0; i < show; i++) {
            guint r = row_of[i];
            if (r >= align_rows) continue;
            int end_x = x_of[i] + nat_w[i];
            if (end_x > row_end[r]) row_end[r] = end_x;
        }
        if (needs_overflow && overflow_widget) {
            guint ov_row = (show > 0) ? row_of[show - 1]
                                       : (max_rows > 0 ? max_rows - 1 : 0);
            if (ov_row < align_rows) {
                int ov_end_x = (show > 0)
                    ? x_of[show - 1] + nat_w[show - 1] + self->spacing + overflow_nat_w
                    : overflow_nat_w;
                if (ov_end_x > row_end[ov_row]) row_end[ov_row] = ov_end_x;
            }
        }
        for (guint r = 0; r < align_rows; r++) {
            int slack = width - row_end[r];
            if (slack < 0) slack = 0;
            row_shift[r] = (halign == GTK_ALIGN_CENTER) ? slack / 2 : slack;
        }
    }

    /* ── Phase 3: allocate flowed children ─────────────────────────── */
    child = gtk_widget_get_first_child(widget);
    for (guint i = 0; i < flow_n && child; i++) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (i < show) {
            int y = (int)row_of[i] * (row_h + self->row_spacing);
            int shift = (row_of[i] < align_rows) ? row_shift[row_of[i]] : 0;
            gtk_widget_set_child_visible(child, TRUE);
            gtk_widget_size_allocate(child,
                &(GtkAllocation){x_of[i] + shift, y, nat_w[i], row_h}, baseline);
        } else {
            gtk_widget_set_child_visible(child, FALSE);
            gtk_widget_size_allocate(child,
                &(GtkAllocation){0, 0, nat_w[i], row_h}, baseline);
        }
        child = next;
    }

    /* Hide reserved-but-unused slots between flow_n and overflow_widget.
     * Occurs only when callers pre-allocate more children than they use
     * (e.g. genre pill pool). Allocate at natural width to avoid GTK
     * "allocation width too small" warnings. */
    for (; child && child != overflow_widget;) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        int cnat = 0;
        gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1,
                           NULL, &cnat, NULL, NULL);
        gtk_widget_set_child_visible(child, FALSE);
        gtk_widget_size_allocate(child,
            &(GtkAllocation){0, 0, MAX(cnat, 1), row_h > 0 ? row_h : 1}, baseline);
        child = next;
    }

    /* ── Overflow indicator ────────────────────────────────────────── */
    if (overflow_widget) {
        if (needs_overflow) {
            int ov_x = 0;
            guint ov_row = 0;
            if (show > 0) {
                ov_row = row_of[show - 1];
                ov_x = x_of[show - 1] + nat_w[show - 1] + self->spacing;
            } else if (max_rows > 0) {
                ov_row = max_rows - 1;
            }
            int y = (int)ov_row * (row_h + self->row_spacing);
            int shift = (ov_row < align_rows) ? row_shift[ov_row] : 0;
            gtk_widget_set_child_visible(overflow_widget, TRUE);
            gtk_widget_size_allocate(overflow_widget,
                &(GtkAllocation){ov_x + shift, y, overflow_nat_w, row_h}, baseline);
        } else {
            gtk_widget_set_child_visible(overflow_widget, FALSE);
            gtk_widget_size_allocate(overflow_widget,
                &(GtkAllocation){0, 0, overflow_nat_w,
                                 row_h > 0 ? row_h : 1}, baseline);
        }
    }
}

/* ── Snapshot ────────────────────────────────────────────────────────── */

static void quadrature_overflow_box_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c;
         c = gtk_widget_get_next_sibling(c)) {
        if (gtk_widget_get_child_visible(c))
            gtk_widget_snapshot_child(widget, c, snapshot);
    }
}

/* ── Class / instance init ───────────────────────────────────────────── */

static void quadrature_overflow_box_class_init(QuadratureOverflowBoxClass *klass) {
    GObjectClass   *obj_class    = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    obj_class->dispose      = quadrature_overflow_box_dispose;
    obj_class->get_property = quadrature_overflow_box_get_property;
    obj_class->set_property = quadrature_overflow_box_set_property;

    widget_class->get_request_mode = quadrature_overflow_box_get_request_mode;
    widget_class->measure          = quadrature_overflow_box_measure;
    widget_class->size_allocate    = quadrature_overflow_box_size_allocate;
    widget_class->snapshot         = quadrature_overflow_box_snapshot;

    gtk_widget_class_set_css_name(widget_class, "overflow-box");

    ofb_props[OFB_PROP_SPACING] = g_param_spec_int(
        "spacing", "Spacing", "Horizontal gap between children in pixels",
        0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    ofb_props[OFB_PROP_ROW_SPACING] = g_param_spec_int(
        "row-spacing", "Row Spacing", "Vertical gap between rows in pixels",
        0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    ofb_props[OFB_PROP_PINNED_COUNT] = g_param_spec_uint(
        "pinned-count", "Pinned Count",
        "Number of leading children that are always visible (never overflowed)",
        0, G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    ofb_props[OFB_PROP_ITEM_COUNT] = g_param_spec_uint(
        "item-count", "Item Count",
        "Number of overflowable item children (after pinned, before indicator)",
        0, G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    ofb_props[OFB_PROP_MAX_ROWS] = g_param_spec_uint(
        "max-rows", "Max Rows", "Maximum visible rows (1 = single-row)",
        1, G_MAXUINT, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(obj_class, OFB_N_PROPS, ofb_props);
}

static void quadrature_overflow_box_init(QuadratureOverflowBox *self) {
    self->spacing     = 0;
    self->row_spacing = 0;
    self->max_rows    = 1;
}

void quadrature_overflow_box_append(QuadratureOverflowBox *self, GtkWidget *child) {
    gtk_widget_set_parent(child, GTK_WIDGET(self));
}

void quadrature_overflow_box_set_item_count(QuadratureOverflowBox *self, guint count) {
    self->item_count = count;
    gtk_widget_queue_resize(GTK_WIDGET(self));
}

void quadrature_overflow_box_set_pinned_count(QuadratureOverflowBox *self, guint count) {
    self->pinned_count = count;
    gtk_widget_queue_resize(GTK_WIDGET(self));
}

/** Remove all children beyond the first `pinned_count`.
 *  Lets a caller clear items + overflow indicator while preserving any
 *  template-declared pinned children. Resets item_count to 0. */
void quadrature_overflow_box_clear_items(QuadratureOverflowBox *self) {
    guint skip = self->pinned_count;
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    for (guint i = 0; i < skip && child; i++)
        child = gtk_widget_get_next_sibling(child);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_widget_unparent(child);
        child = next;
    }
    self->item_count = 0;
    gtk_widget_queue_resize(GTK_WIDGET(self));
}

/** Remove all children regardless of pinned_count. Resets both counts. */
void quadrature_overflow_box_clear_all(QuadratureOverflowBox *self) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_widget_unparent(child);
        child = next;
    }
    self->pinned_count = 0;
    self->item_count = 0;
    gtk_widget_queue_resize(GTK_WIDGET(self));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Smooth Scroll — Critically-Damped Spring (Firefox MSD Model)
 *
 * Converts discrete mouse-wheel events into physically-simulated scroll.
 * Each wheel notch moves the spring's REST POSITION further.  The spring
 * carries the viewport smoothly to the new target — rapid notches push
 * the target ahead and the spring naturally accelerates.
 *
 * Physics:  x'' = -k*(x - dest) - 2*ζ*√k * x'
 *   k     = spring constant (stiffness)
 *   ζ     = 1.0 (critically damped — no oscillation, fastest convergence)
 *   dest  = target scroll position (updated by each wheel event)
 *
 * Integrated via semi-implicit Euler at 120 Hz with wall-clock dt.
 * Frame-rate independent: same feel at 30, 60, 144 Hz.
 *
 * Constants derived from Firefox's MSD physics prefs and Chrome's
 * InverseDelta timing model.  See Chromium scroll_offset_animation_curve.cc
 * and Firefox AxisPhysicsMSDModel.cpp for reference.
 *
 * Attach with: ui_smooth_scroll_attach(scrolled_window)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Pixels per discrete wheel notch.  This is the distance the spring target
 * advances per notch.  ~1.5 rows at 64px row height. */
#define SS_PIXELS_PER_NOTCH     96.0

/* Spring constant (px/s², since ζ=1 and mass=1).
 * Firefox defaults: 1000 (regular), 1250 (begin), 2000 (settle).
 * We use the regular constant — the spring model with target accumulation
 * gives natural acceleration on rapid input without needing the adaptive
 * stiffness that Firefox uses for its velocity-based model. */
#define SS_SPRING_K            1000.0    /* regular scrolling */

/* Damping ratio — 1.0 = critically damped (no bounce, fastest settle).
 * damping coefficient = 2 * ζ * √k */
#define SS_DAMPING_RATIO         1.0

/* Simulation timestep: 120 Hz for smooth sub-frame integration.
 * Higher than display rate avoids temporal aliasing. */
#define SS_SIMULATION_DT        (1.0 / 120.0)

/* Stop threshold: when |distance to target| < this AND |velocity| < this,
 * snap to target and stop the tick callback. */
#define SS_POSITION_EPSILON      0.5    /* px */
#define SS_VELOCITY_EPSILON      0.5    /* px/s */

/* Max accumulated distance ahead of current position.
 * Prevents runaway target from very fast wheel spinning.
 * ~8 screen-heights at 1080p would be extreme; this caps it. */
#define SS_MAX_LEAD           4000.0    /* px */

typedef struct {
    GtkAdjustment *vadj;

    /* Spring simulation state (in scroll-position space, px) */
    double position;      /* current animated position */
    double velocity;      /* current velocity (px/s) */
    double target;        /* where the spring is pulling toward */

    /* Precomputed damping coefficient: 2 * ζ * √k */
    double damping_coeff;
    double spring_k;

    /* Frame clock tracking */
    gint64 last_time_us;  /* monotonic µs of last tick */
    guint  tick_id;
} SmoothScrollState;

static void ss_step(SmoothScrollState *ss, double dt) {
    /* Semi-implicit Euler integration (symplectic — stable, no energy gain).
     * Update velocity first, then position. */
    double displacement = ss->position - ss->target;
    double spring_force = -ss->spring_k * displacement;
    double damping_force = -ss->damping_coeff * ss->velocity;
    double accel = spring_force + damping_force;

    ss->velocity += accel * dt;
    ss->position += ss->velocity * dt;
}

static gboolean smooth_scroll_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget;
    SmoothScrollState *ss = data;

    gint64 now_us = gdk_frame_clock_get_frame_time(clock);
    double dt_s = (double)(now_us - ss->last_time_us) / 1e6;
    ss->last_time_us = now_us;

    /* Clamp dt to avoid spiral of death after a stall (e.g. window drag) */
    if (dt_s > 0.05) dt_s = 0.05;  /* max 50ms = 20fps floor */

    /* Integrate at fixed 120 Hz timestep for stability */
    double remaining = dt_s;
    while (remaining > 0.0001) {
        double step = (remaining > SS_SIMULATION_DT) ? SS_SIMULATION_DT : remaining;
        ss_step(ss, step);
        remaining -= step;
    }

    /* Apply to adjustment */
    double upper    = gtk_adjustment_get_upper(ss->vadj);
    double page     = gtk_adjustment_get_page_size(ss->vadj);
    double max_val  = upper - page;
    double clamped  = CLAMP(ss->position, 0.0, max_val);

    gtk_adjustment_set_value(ss->vadj, clamped);

    /* Boundary: if we hit the edge, kill velocity and snap target */
    if ((clamped <= 0.0 && ss->velocity < 0.0) ||
        (clamped >= max_val && ss->velocity > 0.0)) {
        ss->velocity = 0.0;
        ss->position = clamped;
        ss->target = clamped;
    }

    /* Converged? */
    if (fabs(ss->position - ss->target) < SS_POSITION_EPSILON &&
        fabs(ss->velocity) < SS_VELOCITY_EPSILON) {
        ss->position = ss->target;
        gtk_adjustment_set_value(ss->vadj, CLAMP(ss->position, 0.0, max_val));
        ss->tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean smooth_scroll_event(GtkEventControllerScroll *ctrl,
                                     double dx, double dy,
                                     gpointer data) {
    (void)ctrl; (void)dx;
    SmoothScrollState *ss = data;

    /* Each wheel notch advances the spring target.
     * Rapid notches push the target further ahead — the spring naturally
     * accelerates to keep up, giving momentum without explicit velocity
     * accumulation. */
    double delta = dy * SS_PIXELS_PER_NOTCH;

    /* Cap lead distance to prevent runaway */
    double current_lead = ss->target - ss->position;
    if (fabs(current_lead + delta) < SS_MAX_LEAD)
        ss->target += delta;
    else
        ss->target = ss->position + copysign(SS_MAX_LEAD, current_lead + delta);

    /* Start animation if not running */
    if (!ss->tick_id) {
        /* Sync position with current adjustment value (in case something
         * else scrolled while we were idle — e.g. keyboard, programmatic) */
        ss->position = gtk_adjustment_get_value(ss->vadj);
        ss->target = ss->position + delta;
        ss->velocity = 0.0;
        ss->last_time_us = g_get_monotonic_time();

        GtkWidget *sw = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
        ss->tick_id = gtk_widget_add_tick_callback(sw, smooth_scroll_tick, ss, NULL);
    }

    return TRUE;  /* consume — prevent GtkScrolledWindow's default discrete jump */
}

static void smooth_scroll_state_free(gpointer data) {
    SmoothScrollState *ss = data;
    /* tick_id is auto-removed when widget is destroyed */
    g_free(ss);
}

void ui_smooth_scroll_attach(GtkScrolledWindow *sw) {
    g_assert(GTK_IS_SCROLLED_WINDOW(sw));

    SmoothScrollState *ss = g_new0(SmoothScrollState, 1);
    ss->vadj = gtk_scrolled_window_get_vadjustment(sw);
    ss->spring_k = SS_SPRING_K;
    ss->damping_coeff = 2.0 * SS_DAMPING_RATIO * sqrt(SS_SPRING_K);
    ss->position = gtk_adjustment_get_value(ss->vadj);
    ss->target = ss->position;

    GtkEventControllerScroll *ctrl = GTK_EVENT_CONTROLLER_SCROLL(
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                         GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
    g_signal_connect(ctrl, "scroll", G_CALLBACK(smooth_scroll_event), ss);

    /* Store state on the widget so it's freed with the widget */
    g_object_set_data_full(G_OBJECT(sw), "smooth-scroll-state", ss,
                           smooth_scroll_state_free);

    gtk_widget_add_controller(GTK_WIDGET(sw), GTK_EVENT_CONTROLLER(ctrl));
}
