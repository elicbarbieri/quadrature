/**
 * Unit tests for ui_math.c — pure math functions extracted from UI widgets.
 *
 * Tests shuttle speed mapping, log normalization, bell curve LUT,
 * duration formatting, and track count formatting.
 */

#include <criterion/criterion.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* shuttle_mode_t is in quadrature.h */
#include "quadrature/quadrature.h"

/* Forward-declare functions under test (ui_math.c) */
float  ui_shuttle_value_to_speed(double slider_value, shuttle_mode_t mode);
double ui_log_pct_norm(double pct);
void   ui_bell_curve_lut(float *lut, int n, double sigma);
void   ui_format_duration(uint32_t ms, char *buf, size_t len);
void   ui_format_track_count(char *buf, size_t len, uint32_t count);

/* ═══════════════════════════════════════════════════════════════════════════
 * Shuttle Speed Mapping
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(shuttle, off_always_1x) {
    cr_assert_float_eq(ui_shuttle_value_to_speed(0.0, SHUTTLE_MODE_OFF), 1.0f, 1e-6);
    cr_assert_float_eq(ui_shuttle_value_to_speed(3.0, SHUTTLE_MODE_OFF), 1.0f, 1e-6);
    cr_assert_float_eq(ui_shuttle_value_to_speed(-2.0, SHUTTLE_MODE_OFF), 1.0f, 1e-6);
}

Test(shuttle, center_is_1x) {
    cr_assert_float_eq(ui_shuttle_value_to_speed(0.0, SHUTTLE_MODE_KEYLOCK), 1.0f, 1e-6);
    cr_assert_float_eq(ui_shuttle_value_to_speed(0.0, SHUTTLE_MODE_PITCHED), 1.0f, 1e-6);
}

Test(shuttle, keylock_max) {
    /* Slider +3 → 4.0x */
    cr_assert_float_eq(ui_shuttle_value_to_speed(3.0, SHUTTLE_MODE_KEYLOCK), 4.0f, 1e-4);
}

Test(shuttle, keylock_min) {
    /* Slider -2 → 0.5x */
    cr_assert_float_eq(ui_shuttle_value_to_speed(-2.0, SHUTTLE_MODE_KEYLOCK), 0.5f, 1e-4);
}

Test(shuttle, pitched_max) {
    /* Slider +3 → 1.5x */
    cr_assert_float_eq(ui_shuttle_value_to_speed(3.0, SHUTTLE_MODE_PITCHED), 1.5f, 1e-4);
}

Test(shuttle, pitched_min) {
    /* Slider -2 → 0.5x */
    cr_assert_float_eq(ui_shuttle_value_to_speed(-2.0, SHUTTLE_MODE_PITCHED), 0.5f, 1e-4);
}

Test(shuttle, quadratic_midpoint_keylock) {
    /* At slider 1.5 (half of max 3), normalized = 0.5, quadratic = 0.25
     * speed = 1.0 + 3.0 * 0.25 = 1.75 */
    cr_assert_float_eq(ui_shuttle_value_to_speed(1.5, SHUTTLE_MODE_KEYLOCK), 1.75f, 1e-4);
}

Test(shuttle, quadratic_midpoint_pitched) {
    /* At slider 1.5, normalized = 0.5, quadratic = 0.25
     * speed = 1.0 + 0.5 * 0.25 = 1.125 */
    cr_assert_float_eq(ui_shuttle_value_to_speed(1.5, SHUTTLE_MODE_PITCHED), 1.125f, 1e-4);
}

Test(shuttle, monotonic_increase_keylock) {
    float prev = ui_shuttle_value_to_speed(-2.0, SHUTTLE_MODE_KEYLOCK);
    for (double v = -1.5; v <= 3.0; v += 0.5) {
        float cur = ui_shuttle_value_to_speed(v, SHUTTLE_MODE_KEYLOCK);
        cr_assert_gt(cur, prev, "speed should increase monotonically");
        prev = cur;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Log-Scale Normalization
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(log_norm, anchor_points) {
    cr_assert_float_eq(ui_log_pct_norm(0.0),   0.0,  1e-9);
    cr_assert_float_eq(ui_log_pct_norm(0.1),   0.25, 1e-9);
    cr_assert_float_eq(ui_log_pct_norm(1.0),   0.5,  1e-9);
    cr_assert_float_eq(ui_log_pct_norm(10.0),  0.75, 1e-9);
    cr_assert_float_eq(ui_log_pct_norm(100.0), 1.0,  1e-9);
}

Test(log_norm, negative_clamps_to_zero) {
    cr_assert_float_eq(ui_log_pct_norm(-5.0), 0.0, 1e-9);
}

Test(log_norm, above_100_clamps_to_one) {
    cr_assert_float_eq(ui_log_pct_norm(200.0), 1.0, 1e-9);
}

Test(log_norm, monotonic) {
    double prev = 0.0;
    double vals[] = {0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0};
    for (int i = 0; i < 9; i++) {
        double cur = ui_log_pct_norm(vals[i]);
        cr_assert_gt(cur, prev, "log_pct_norm must be monotonically increasing");
        prev = cur;
    }
}

Test(log_norm, midpoints_interpolate) {
    /* 0.05% is halfway between 0% and 0.1% anchors → should be ~0.125 */
    cr_assert_float_eq(ui_log_pct_norm(0.05), 0.125, 1e-9);
    /* 5.5% is halfway between 1% and 10% → should be ~0.625 */
    cr_assert_float_eq(ui_log_pct_norm(5.5), 0.625, 1e-9);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Bell Curve LUT
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(bell_lut, first_entry_is_one) {
    float lut[256];
    ui_bell_curve_lut(lut, 256, 0.18);
    cr_assert_float_eq(lut[0], 1.0f, 1e-6, "dist=0 should give weight 1.0");
}

Test(bell_lut, last_entry_near_zero) {
    float lut[256];
    ui_bell_curve_lut(lut, 256, 0.18);
    /* dist=1.0, sigma=0.18 → exp(-1/(2*0.0324)) ≈ exp(-15.43) ≈ very small */
    cr_assert_lt(lut[255], 0.001f, "dist=1.0 with sigma=0.18 should be near zero");
}

Test(bell_lut, monotonically_decreasing) {
    float lut[256];
    ui_bell_curve_lut(lut, 256, 0.18);
    for (int i = 1; i < 256; i++)
        cr_assert_leq(lut[i], lut[i - 1], "bell curve should be monotonically decreasing");
}

Test(bell_lut, wider_sigma_slower_decay) {
    float narrow[64], wide[64];
    ui_bell_curve_lut(narrow, 64, 0.1);
    ui_bell_curve_lut(wide, 64, 0.5);
    /* At midpoint (index 32), wider sigma should have higher weight */
    cr_assert_gt(wide[32], narrow[32], "wider sigma should decay more slowly");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Duration Formatting
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(format_duration, zero) {
    char buf[32];
    ui_format_duration(0, buf, sizeof(buf));
    cr_assert_str_eq(buf, "0:00");
}

Test(format_duration, seconds_only) {
    char buf[32];
    ui_format_duration(45000, buf, sizeof(buf));
    cr_assert_str_eq(buf, "0:45");
}

Test(format_duration, minutes_and_seconds) {
    char buf[32];
    ui_format_duration(195000, buf, sizeof(buf));  /* 3:15 */
    cr_assert_str_eq(buf, "3:15");
}

Test(format_duration, exact_minute) {
    char buf[32];
    ui_format_duration(120000, buf, sizeof(buf));
    cr_assert_str_eq(buf, "2:00");
}

Test(format_duration, hours) {
    char buf[32];
    ui_format_duration(3720000, buf, sizeof(buf));  /* 1h 02m */
    cr_assert_str_eq(buf, "1h 02m");
}

Test(format_duration, multi_hour) {
    char buf[32];
    ui_format_duration(7200000, buf, sizeof(buf));  /* 2h 00m */
    cr_assert_str_eq(buf, "2h 00m");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Track Count Formatting
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(format_tracks, single) {
    char buf[32];
    ui_format_track_count(buf, sizeof(buf), 1);
    cr_assert_str_eq(buf, "   Single");
}

Test(format_tracks, normal_count) {
    char buf[32];
    ui_format_track_count(buf, sizeof(buf), 12);
    cr_assert_str_eq(buf, "12 Tracks");
}

Test(format_tracks, capped_at_99) {
    char buf[32];
    ui_format_track_count(buf, sizeof(buf), 150);
    cr_assert_str_eq(buf, "99 Tracks");
}

Test(format_tracks, two_digit_padding) {
    char buf[32];
    ui_format_track_count(buf, sizeof(buf), 5);
    cr_assert_str_eq(buf, " 5 Tracks");
}
