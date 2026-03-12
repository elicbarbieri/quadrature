/**
 * Unit tests for ui_overflow_box_plan_layout() (overflow_plan.c)
 *
 * Tests the pure integer layout planner that decides how many items
 * to show before inserting an overflow indicator.
 */

#include <criterion/criterion.h>
#include <glib.h>

/* Forward-declare the function under test (lives in overflow_plan.c) */
guint ui_overflow_box_plan_layout(int budget,
                                  const int *item_widths,
                                  guint item_count,
                                  int overflow_width,
                                  gboolean *needs_overflow);

/* ═══════════════════════════════════════════════════════════════════════════
 * All items fit — no overflow needed
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, all_fit_exactly) {
    int widths[] = {50, 50, 50};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(150, widths, 3, 20, &needs_overflow);
    cr_assert_eq(shown, 3);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, all_fit_with_room) {
    int widths[] = {30, 30, 30};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(200, widths, 3, 20, &needs_overflow);
    cr_assert_eq(shown, 3);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, single_item_fits) {
    int widths[] = {100};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 1, 20, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert_not(needs_overflow);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Overflow needed — show as many as fit with overflow reserved
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, overflow_shows_fitting_items) {
    /* Budget 100, overflow 20 → 80 for items. Items: 40+40=80 fits, +40=120 doesn't */
    int widths[] = {40, 40, 40};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 3, 20, &needs_overflow);
    cr_assert_eq(shown, 2);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_shows_one_item) {
    /* Budget 80, overflow 20 → 60 for items. First item 50 fits, second 50 doesn't */
    int widths[] = {50, 50, 50};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(80, widths, 3, 20, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_zero_items_when_none_fit) {
    /* Budget 30, overflow 25 → 5 for items. No item (width 20) fits in 5 */
    int widths[] = {20, 20, 20};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(30, widths, 3, 25, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_with_varying_widths) {
    /* Budget 120, overflow 15 → 105 for items. 40+30=70 fits, +40=110 > 105 */
    int widths[] = {40, 30, 40, 50};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(120, widths, 4, 15, &needs_overflow);
    cr_assert_eq(shown, 2);
    cr_assert(needs_overflow);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge cases
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, zero_items) {
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(100, NULL, 0, 20, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, zero_budget) {
    int widths[] = {10, 10};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(0, widths, 2, 20, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_wider_than_budget) {
    /* Overflow itself doesn't fit — should still report overflow, show 0 items */
    int widths[] = {10, 10};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(15, widths, 2, 30, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert(needs_overflow);
}

Test(overflow_layout, exactly_one_over) {
    /* 3 artists: 40+40+40=120 > budget 100. With overflow 20 → 80 avail: 40+40=80 fits */
    int widths[] = {40, 40, 40};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 3, 20, &needs_overflow);
    cr_assert_eq(shown, 2);
    cr_assert(needs_overflow);
}

Test(overflow_layout, first_item_equals_remaining_budget) {
    /* Budget 50, overflow 20 → 30 for items. First item exactly 30 → fits */
    int widths[] = {30, 30};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(50, widths, 2, 20, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert(needs_overflow);
}
