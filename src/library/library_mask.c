/**
 * Library bitmask helpers.
 *
 * Standalone translation unit so the unit test can link against these
 * without pulling in the full library cache / DB engine.
 */

#include "quadrature/library.h"

uint32_t library_mask_after_toggle(uint32_t current_mask, int lib_idx) {
    uint32_t toggled = current_mask ^ (1u << lib_idx);
    return toggled ? toggled : LIBRARY_MASK_ALL;
}

uint32_t library_mask_solo(int lib_idx) {
    return 1u << lib_idx;
}
