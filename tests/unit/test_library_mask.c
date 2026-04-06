/**
 * Unit tests for library mask toggle logic.
 *
 * Tests the pure bitmask computation used by the library pill bar,
 * without requiring GTK.
 *
 * Interactions:
 *   Left-click   → library_mask_after_toggle (XOR flip)
 *   Shift+click  → library_mask_solo (only this library)
 *   Right-click   → LIBRARY_MASK_ALL (reset)
 */

#include <criterion/criterion.h>
#include "quadrature/library.h"

#define ALL    LIBRARY_MASK_ALL
#define LIB(n) (1u << (n))

/* ═══════════════════════════════════════════════════════════════════════════
 * Left-click: simple toggle (XOR)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, toggle_on) {
    /* Lib 0 off, toggle → on */
    uint32_t result = library_mask_after_toggle(LIB(1), 0);
    cr_assert_eq(result, LIB(0) | LIB(1), "expected libs 0+1, got 0x%x", result);
}

Test(library_mask, toggle_off) {
    /* Libs 0+1 on, toggle 0 → off */
    uint32_t result = library_mask_after_toggle(LIB(0) | LIB(1), 0);
    cr_assert_eq(result, LIB(1), "expected lib 1 only, got 0x%x", result);
}

Test(library_mask, toggle_last_produces_zero) {
    /* Only lib 1 on, toggle 1 → all disabled */
    uint32_t result = library_mask_after_toggle(LIB(1), 1);
    cr_assert_eq(result, 0u, "expected 0 (all disabled), got 0x%x", result);
}

Test(library_mask, toggle_from_all) {
    /* All on, toggle lib 5 → all except 5 */
    uint32_t result = library_mask_after_toggle(ALL, 5);
    cr_assert_eq(result, ALL ^ LIB(5), "expected all except lib 5, got 0x%x", result);
}

Test(library_mask, toggle_from_zero) {
    /* None on, toggle lib 3 → only 3 */
    uint32_t result = library_mask_after_toggle(0, 3);
    cr_assert_eq(result, LIB(3), "expected lib 3 only, got 0x%x", result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Shift+click: solo
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, solo) {
    cr_assert_eq(library_mask_solo(0), LIB(0));
    cr_assert_eq(library_mask_solo(1), LIB(1));
    cr_assert_eq(library_mask_solo(31), LIB(31));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Right-click: always LIBRARY_MASK_ALL
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, right_click_selects_all) {
    cr_assert_eq(LIBRARY_MASK_ALL, UINT32_MAX);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Full workflow scenario
 *
 * 1. Start: all on
 * 2. Click lib 1 → toggle off (all except 1)
 * 3. Shift+click lib 2 → solo lib 2
 * 4. Click lib 0 → toggle on (0 + 2)
 * 5. Click lib 0 → toggle off (only 2)
 * 6. Click lib 2 → toggle off (0 = all disabled)
 * 7. Right-click → all on
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, full_workflow) {
    uint32_t mask = ALL;

    /* Step 1: toggle lib 1 off */
    mask = library_mask_after_toggle(mask, 1);
    cr_assert_eq(mask, ALL ^ LIB(1), "step 1");

    /* Step 2: shift+click lib 2 → solo */
    mask = library_mask_solo(2);
    cr_assert_eq(mask, LIB(2), "step 2");

    /* Step 3: toggle lib 0 on */
    mask = library_mask_after_toggle(mask, 0);
    cr_assert_eq(mask, LIB(0) | LIB(2), "step 3");

    /* Step 4: toggle lib 0 off */
    mask = library_mask_after_toggle(mask, 0);
    cr_assert_eq(mask, LIB(2), "step 4");

    /* Step 5: toggle lib 2 off → all disabled */
    mask = library_mask_after_toggle(mask, 2);
    cr_assert_eq(mask, 0u, "step 5");

    /* Step 6: right-click → all on */
    mask = LIBRARY_MASK_ALL;
    cr_assert_eq(mask, ALL, "step 6");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Idempotency: toggling the same lib twice returns to original state
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(library_mask, toggle_idempotent) {
    uint32_t original = LIB(0) | LIB(3) | LIB(7);
    uint32_t toggled = library_mask_after_toggle(original, 3);
    uint32_t restored = library_mask_after_toggle(toggled, 3);
    cr_assert_eq(restored, original, "double toggle should restore original");
}
