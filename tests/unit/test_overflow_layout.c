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
                                  int spacing,
                                  gboolean *needs_overflow);

/* ═══════════════════════════════════════════════════════════════════════════
 * All items fit — no overflow needed
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, all_fit_exactly) {
    int widths[] = {50, 50, 50};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(150, widths, 3, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 3);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, all_fit_with_room) {
    int widths[] = {30, 30, 30};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(200, widths, 3, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 3);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, single_item_fits) {
    int widths[] = {100};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 1, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert_not(needs_overflow);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Overflow needed — show as many as fit with overflow reserved
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, overflow_shows_fitting_items) {
    /* Budget 100, overflow 20. Items 40+40+40=120>100.
     * With overflow: 40+0sp+20=60 fits, 40+40+1sp*0+20=100 fits too. */
    int widths[] = {40, 40, 40};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 3, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 2);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_shows_one_item) {
    /* Budget 80, overflow 20. First item 50 fits, second 50 doesn't */
    int widths[] = {50, 50, 50};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(80, widths, 3, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_zero_items_when_none_fit) {
    /* Budget 30, overflow 25 → 5 for items. No item (width 20) fits in 5 */
    int widths[] = {20, 20, 20};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(30, widths, 3, 25, 0, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_with_varying_widths) {
    /* Budget 120, overflow 15. 40+30=70, +15=85 fits. +40=125>120 */
    int widths[] = {40, 30, 40, 50};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(120, widths, 4, 15, 0, &needs_overflow);
    cr_assert_eq(shown, 2);
    cr_assert(needs_overflow);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge cases
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, zero_items) {
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(100, NULL, 0, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, zero_budget) {
    int widths[] = {10, 10};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(0, widths, 2, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert(needs_overflow);
}

Test(overflow_layout, overflow_wider_than_budget) {
    /* Overflow itself doesn't fit — should still report overflow, show 0 items */
    int widths[] = {10, 10};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(15, widths, 2, 30, 0, &needs_overflow);
    cr_assert_eq(shown, 0);
    cr_assert(needs_overflow);
}

Test(overflow_layout, exactly_one_over) {
    /* 3 items: 40+40+40=120 > budget 100. With overflow 20: 40+40+sp*2+20 fits at sp=0 */
    int widths[] = {40, 40, 40};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 3, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 2);
    cr_assert(needs_overflow);
}

Test(overflow_layout, first_item_equals_remaining_budget) {
    /* Budget 50, overflow 20. First item 30 + 1sp*0 + 20 = 50 → fits exactly */
    int widths[] = {30, 30};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(50, widths, 2, 20, 0, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert(needs_overflow);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Spacing-aware tests — verify inter-item gaps are included in the plan
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(overflow_layout, spacing_all_fit_with_gaps) {
    /* 3 items of 30 + 2 gaps of 6 = 102. Budget 102 → fits exactly */
    int widths[] = {30, 30, 30};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(102, widths, 3, 20, 6, &needs_overflow);
    cr_assert_eq(shown, 3);
    cr_assert_not(needs_overflow);
}

Test(overflow_layout, spacing_triggers_overflow) {
    /* 3 items of 30 = 90, but + 2 gaps of 6 = 102 > budget 100 → overflow */
    int widths[] = {30, 30, 30};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(100, widths, 3, 20, 6, &needs_overflow);
    cr_assert(needs_overflow);
    /* 1 item(30) + 1 gap(6) + overflow(20) = 56 ≤ 100. 2 items: 30+30+2*6+20=92 ≤ 100.
     * 3rd would be: 30+30+30+3*6+20=128>100. So shown=2 */
    cr_assert_eq(shown, 2);
}

Test(overflow_layout, spacing_reduces_shown_count) {
    /* Budget 80, overflow 20, spacing 10. Items: [30, 30, 30].
     * Without spacing: 30+30+20=80 → 2 shown.
     * With spacing: 30+30+2*10+20=100 > 80. Try 1: 30+1*10+20=60 ≤ 80. shown=1 */
    int widths[] = {30, 30, 30};
    gboolean needs_overflow = FALSE;
    guint shown = ui_overflow_box_plan_layout(80, widths, 3, 20, 10, &needs_overflow);
    cr_assert(needs_overflow);
    cr_assert_eq(shown, 1);
}

Test(overflow_layout, spacing_single_item_no_gap) {
    /* Single item has no inter-item gap */
    int widths[] = {80};
    gboolean needs_overflow = TRUE;
    guint shown = ui_overflow_box_plan_layout(80, widths, 1, 20, 6, &needs_overflow);
    cr_assert_eq(shown, 1);
    cr_assert_not(needs_overflow);
}
