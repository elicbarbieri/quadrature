/**
 * Overflow Box Layout Planner — Pure integer arithmetic, no GTK dependency.
 *
 * Given a width budget and item measurements, determines how many items
 * to show before inserting an overflow indicator.
 *
 * If all items fit within the budget, shows all — no overflow.
 * Otherwise, reserves space for the overflow indicator and packs
 * as many items as possible within the remaining budget.
 */

#include "internal.h"

guint ui_overflow_box_plan_layout(int budget,
                                  const int *item_widths,
                                  guint item_count,
                                  int overflow_width,
                                  int spacing,
                                  gboolean *needs_overflow) {
    if (item_count == 0) {
        *needs_overflow = FALSE;
        return 0;
    }

    /* Phase 1: do all items fit without overflow?
     * N items have (N-1) gaps between them. */
    int total = 0;
    for (guint i = 0; i < item_count; i++)
        total += item_widths[i];
    if (item_count > 1)
        total += (int)(item_count - 1) * spacing;

    if (total <= budget) {
        *needs_overflow = FALSE;
        return item_count;
    }

    /* Phase 2: need overflow — pack items + overflow indicator.
     * K shown items + 1 overflow = (K+1) widgets with K gaps between them. */
    *needs_overflow = TRUE;
    int accumulated = 0;
    guint shown = 0;

    for (guint i = 0; i < item_count; i++) {
        int candidate = accumulated + item_widths[i]
                      + (int)(shown + 1) * spacing + overflow_width;
        if (candidate > budget)
            break;
        accumulated += item_widths[i];
        shown++;
    }

    return shown;
}
