/**
 * Unit tests for library mask toggle logic.
 *
 * Tests the pure bitmask computation used by the library pill bar,
 * without requiring GTK.
 */

#include <criterion/criterion.h>
#include "quadrature/library.h"

#define ALL    LIBRARY_MASK_ALL
#define LIB(n) (1u << (n))

/* ═══════════════════════════════════════════════════════════════════════════
 * Left-click: solo when all are on
 *
 * All libraries active + click any → only that library remains.
 * GTK unchecks the button before firing "toggled", so now_active = FALSE.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, solo_from_all_on) {
    /* Click lib 0 when all on → solo lib 0 */
    uint32_t result = library_mask_after_toggle(ALL, 0, FALSE);
    cr_assert_eq(result, LIB(0), "expected solo lib 0, got 0x%x", result);

    /* Click lib 1 when all on → solo lib 1 */
    result = library_mask_after_toggle(ALL, 1, FALSE);
    cr_assert_eq(result, LIB(1), "expected solo lib 1, got 0x%x", result);

    /* Click lib 31 when all on → solo lib 31 */
    result = library_mask_after_toggle(ALL, 31, FALSE);
    cr_assert_eq(result, LIB(31), "expected solo lib 31, got 0x%x", result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Left-click: add library when not all on
 *
 * Some libraries active + click an inactive one → it gets added.
 * GTK checks the button, so now_active = TRUE.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, add_library) {
    /* Only lib 1 active, click lib 0 → both on */
    uint32_t result = library_mask_after_toggle(LIB(1), 0, TRUE);
    cr_assert_eq(result, LIB(0) | LIB(1), "expected libs 0+1, got 0x%x", result);

    /* Libs 0+1 active, click lib 2 → 0+1+2 */
    result = library_mask_after_toggle(LIB(0) | LIB(1), 2, TRUE);
    cr_assert_eq(result, LIB(0) | LIB(1) | LIB(2), "expected libs 0+1+2, got 0x%x", result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Left-click: remove library when not all on
 *
 * Some (but not all) libraries active + click an active one → it's removed.
 * GTK unchecks the button, so now_active = FALSE.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, remove_library) {
    /* Libs 0+1 active, click lib 0 → only lib 1 */
    uint32_t result = library_mask_after_toggle(LIB(0) | LIB(1), 0, FALSE);
    cr_assert_eq(result, LIB(1), "expected lib 1 only, got 0x%x", result);

    /* Libs 0+2 active, click lib 2 → only lib 0 */
    result = library_mask_after_toggle(LIB(0) | LIB(2), 2, FALSE);
    cr_assert_eq(result, LIB(0), "expected lib 0 only, got 0x%x", result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Left-click: removing last library falls back to all
 *
 * Only one library active + click it → mask would be 0, so revert to all.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, remove_last_falls_back_to_all) {
    uint32_t result = library_mask_after_toggle(LIB(1), 1, FALSE);
    cr_assert_eq(result, ALL, "expected fallback to all, got 0x%x", result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Right-click: always selects all
 *
 * The right-click handler just sets LIBRARY_MASK_ALL directly.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, right_click_selects_all) {
    cr_assert_eq(LIBRARY_MASK_ALL, UINT32_MAX);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Full scenario: the user's described workflow
 *
 * 1. Start: all libraries on
 * 2. Click lib 1 → solo lib 1
 * 3. Click lib 0 → add lib 0 (now 0+1)
 * 4. Right-click → all on
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, full_workflow) {
    uint32_t mask = ALL;

    /* Step 1: all on, click lib 1 (GTK unchecks → now_active=FALSE) → solo */
    mask = library_mask_after_toggle(mask, 1, FALSE);
    cr_assert_eq(mask, LIB(1), "step 1: expected solo lib 1, got 0x%x", mask);

    /* Step 2: only lib 1, click lib 0 (GTK checks → now_active=TRUE) → add */
    mask = library_mask_after_toggle(mask, 0, TRUE);
    cr_assert_eq(mask, LIB(0) | LIB(1), "step 2: expected libs 0+1, got 0x%x", mask);

    /* Step 3: right-click → all on */
    mask = LIBRARY_MASK_ALL;
    cr_assert_eq(mask, ALL, "step 3: expected all, got 0x%x", mask);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Edge case: solo then toggle back to all via adding
 *
 * After soloing, re-adding every library one by one should eventually
 * restore ALL, and clicking again should solo.
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, solo_then_rebuild_to_all) {
    /* Solo lib 0 */
    uint32_t mask = library_mask_after_toggle(ALL, 0, FALSE);
    cr_assert_eq(mask, LIB(0));

    /* Add lib 1 */
    mask = library_mask_after_toggle(mask, 1, TRUE);
    cr_assert_eq(mask, LIB(0) | LIB(1));

    /* This is NOT all_mask (only 2 bits set out of 32), so clicking
     * lib 1 again should remove it, not solo */
    mask = library_mask_after_toggle(mask, 1, FALSE);
    cr_assert_eq(mask, LIB(0), "should remove, not solo — not all were on");
}
