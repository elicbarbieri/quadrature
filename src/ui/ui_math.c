/**
 * UI Pure Math Functions
 *
 * Extracted from GTK widget code for testability and vectorization.
 * No GTK, no GLib dependency — just C math.
 */

#include <math.h>
#include <stdio.h>
#include "internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Shuttle Speed Mapping
 *
 * Quadratic curves with zero derivative at center for fine control near 1.0x.
 *   KEYLOCK:  slider -2..+3 → speed 0.5x..4.0x  (wide, pitch-preserved)
 *   PITCHED:  slider -2..+3 → speed 0.5x..1.5x  (conservative, vinyl-style)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KEYLOCK_SPEED_MAX  4.0f
#define KEYLOCK_SPEED_MIN  0.5f
#define PITCHED_SPEED_MAX  1.5f
#define PITCHED_SPEED_MIN  0.5f
#define SHUTTLE_SLIDER_MAX 3.0f
#define SHUTTLE_SLIDER_MIN -2.0f

float ui_shuttle_value_to_speed(double slider_value, shuttle_mode_t mode) {
    if (mode == SHUTTLE_MODE_OFF) return 1.0f;

    int pitched = (mode == SHUTTLE_MODE_PITCHED);

    if (slider_value >= 0.0) {
        float speed_max = pitched ? PITCHED_SPEED_MAX : KEYLOCK_SPEED_MAX;
        float range = speed_max - 1.0f;
        float normalized = (float)slider_value / SHUTTLE_SLIDER_MAX;
        return 1.0f + range * normalized * normalized;
    } else {
        float speed_min = pitched ? PITCHED_SPEED_MIN : KEYLOCK_SPEED_MIN;
        float range = 1.0f - speed_min;
        float normalized = (float)slider_value / SHUTTLE_SLIDER_MIN;
        return 1.0f - range * normalized * normalized;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Log-Scale Percentage Normalization
 *
 * Piecewise linear mapping for log-scale Y axis.
 * 5 anchor points at exact 1/4 intervals:
 *   0%→0, 0.1%→1/4, 1%→1/2, 10%→3/4, 100%→1
 * ═══════════════════════════════════════════════════════════════════════════ */

double ui_log_pct_norm(double pct) {
    if (pct <= 0.0)   return 0.0;
    if (pct <= 0.1)   return (pct / 0.1) / 4.0;
    if (pct <= 1.0)   return (1.0 + (pct - 0.1) / 0.9) / 4.0;
    if (pct <= 10.0)  return (2.0 + (pct - 1.0) / 9.0) / 4.0;
    if (pct <= 100.0) return (3.0 + (pct - 10.0) / 90.0) / 4.0;
    return 1.0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Bell Curve LUT Generation
 *
 * Fills a float array with bell-curve weights:
 *   lut[i] = exp(-(d²) / (2σ²))  where d = i / (n-1)
 *
 * Used by the scrubber to scale tick marks near the scroll position.
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_bell_curve_lut(float *lut, int n, double sigma) {
    double inv_2sig2 = 1.0 / (2.0 * sigma * sigma);
    for (int i = 0; i < n; i++) {
        double dist = (double)i / (double)(n - 1);
        lut[i] = (float)exp(-(dist * dist) * inv_2sig2);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Duration Formatting
 *
 * Converts milliseconds to human-readable duration string.
 *   < 1h:  "M:SS"
 *   >= 1h: "Nh MMm"
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_format_duration(uint32_t ms, char *buf, size_t len) {
    uint32_t sec = ms / 1000, min = sec / 60, hr = min / 60;
    if (hr > 0)
        snprintf(buf, len, "%uh %02um", hr, min % 60);
    else
        snprintf(buf, len, "%u:%02u", min, sec % 60);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Track Count Formatting
 *
 * 1 → "   Single", N → "NN Tracks" (capped at 99 for column alignment).
 * ═══════════════════════════════════════════════════════════════════════════ */

void ui_format_track_count(char *buf, size_t len, uint32_t count) {
    if (count == 1)
        snprintf(buf, len, "   Single");
    else {
        uint32_t display = count >= 100 ? 99 : count;
        snprintf(buf, len, "%2u Tracks", display);
    }
}
