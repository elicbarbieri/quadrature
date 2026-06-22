/**
 * Axia LWRP GPIO Protocol Tests
 *
 * Pure unit tests for the wire parsing/formatting in axia_protocol.c, asserting
 * an EXACT match to the Livewire Routing Protocol GPIO format:
 *
 *   - GPI/GPO ports have exactly 5 pins; state is a 5-character field.
 *   - Active-LOW: 'H'/'h' = high (off/inactive), 'L'/'l' = low (on/active).
 *   - UPPERCASE = the pin just changed; lowercase = steady state.
 *   - When setting a GPO, 'x' = leave that pin unchanged.
 *
 * References:
 *   - Media Realm, "Debugging Livewire GPIO with Telnet"
 *   - anthonyeden/Livewire-Routing-Protocol-Client (GPO set = "GPO n xxlxx")
 */

#include <criterion/criterion.h>
#include <string.h>
#include "internal.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * parse_gpi — valid 5-character lines
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_protocol, parse_gpi_all_high_steady)
{
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 1 hhhhh", &line));
    cr_assert_eq(line.port, 1);
    for (int i = 0; i < AXIA_GPIO_PINS; i++) {
        cr_assert_eq(line.active[i], false, "pin %d should be inactive (high)", i + 1);
        cr_assert_eq(line.changed[i], false, "pin %d should be steady", i + 1);
    }
}

Test(axia_protocol, parse_gpi_all_low_steady)
{
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 2 lllll", &line));
    cr_assert_eq(line.port, 2);
    for (int i = 0; i < AXIA_GPIO_PINS; i++) {
        cr_assert_eq(line.active[i], true, "pin %d should be active (low)", i + 1);
        cr_assert_eq(line.changed[i], false);
    }
}

Test(axia_protocol, parse_gpi_pin1_active_changed)
{
    /* Pin 1 just went low (on): uppercase L in first position. */
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 1 Lhhhh", &line));
    cr_assert_eq(line.port, 1);
    cr_assert_eq(line.active[0], true);
    cr_assert_eq(line.changed[0], true);
    for (int i = 1; i < AXIA_GPIO_PINS; i++) {
        cr_assert_eq(line.active[i], false);
        cr_assert_eq(line.changed[i], false);
    }
}

Test(axia_protocol, parse_gpi_pin1_inactive_changed)
{
    /* Pin 1 just went high (off): uppercase H in first position. */
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 1 Hhhhh", &line));
    cr_assert_eq(line.active[0], false);
    cr_assert_eq(line.changed[0], true);
}

Test(axia_protocol, parse_gpi_pin3_active_changed)
{
    /* Pin 3 (index 2) changed to low; others steady high. */
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 4 hhLhh", &line));
    cr_assert_eq(line.port, 4);
    cr_assert_eq(line.active[2], true);
    cr_assert_eq(line.changed[2], true);
    cr_assert_eq(line.active[0], false);
    cr_assert_eq(line.active[1], false);
    cr_assert_eq(line.active[3], false);
    cr_assert_eq(line.active[4], false);
}

Test(axia_protocol, parse_gpi_mixed)
{
    /* pin1 low steady, pin2 low changed, pin3 high changed, pin4/5 high steady */
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 7 lLHhh", &line));
    cr_assert_eq(line.port, 7);
    cr_assert(line.active[0] && !line.changed[0]);
    cr_assert(line.active[1] && line.changed[1]);
    cr_assert(!line.active[2] && line.changed[2]);
    cr_assert(!line.active[3] && !line.changed[3]);
    cr_assert(!line.active[4] && !line.changed[4]);
}

Test(axia_protocol, parse_gpi_high_port_number)
{
    /* LWRP port numbers are not limited to 1-4. */
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 42 hhhhh", &line));
    cr_assert_eq(line.port, 42);
}

Test(axia_protocol, parse_gpi_leading_whitespace)
{
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("   GPI 1 hhhhh", &line));
    cr_assert_eq(line.port, 1);
}

Test(axia_protocol, parse_gpi_trailing_whitespace_ok)
{
    axia_gpio_line_t line;
    cr_assert(axia_protocol_parse_gpi("GPI 1 hhhhh ", &line));
    cr_assert_eq(line.port, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * parse_gpi — rejections
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_protocol, parse_gpi_rejects_null)
{
    axia_gpio_line_t line;
    cr_assert_not(axia_protocol_parse_gpi(NULL, &line));
    cr_assert_not(axia_protocol_parse_gpi("GPI 1 hhhhh", NULL));
}

Test(axia_protocol, parse_gpi_rejects_wrong_prefix)
{
    axia_gpio_line_t line;
    cr_assert_not(axia_protocol_parse_gpi("GPO 1 hhhhh", &line));
    cr_assert_not(axia_protocol_parse_gpi("XYZ 1 hhhhh", &line));
    cr_assert_not(axia_protocol_parse_gpi("GP 1 hhhhh", &line));
}

Test(axia_protocol, parse_gpi_rejects_too_few_pins)
{
    axia_gpio_line_t line;
    cr_assert_not(axia_protocol_parse_gpi("GPI 1 hhh", &line));
    cr_assert_not(axia_protocol_parse_gpi("GPI 1 hhhh", &line));
}

Test(axia_protocol, parse_gpi_rejects_too_many_pins)
{
    axia_gpio_line_t line;
    cr_assert_not(axia_protocol_parse_gpi("GPI 1 hhhhhh", &line));
}

Test(axia_protocol, parse_gpi_rejects_invalid_pin_char)
{
    axia_gpio_line_t line;
    /* 'x' is only valid when SETTING a GPO, never in a notification. */
    cr_assert_not(axia_protocol_parse_gpi("GPI 1 hhxhh", &line));
    cr_assert_not(axia_protocol_parse_gpi("GPI 1 hh?hh", &line));
}

Test(axia_protocol, parse_gpi_rejects_bad_port)
{
    axia_gpio_line_t line;
    cr_assert_not(axia_protocol_parse_gpi("GPI 0 hhhhh", &line));
    cr_assert_not(axia_protocol_parse_gpi("GPI x hhhhh", &line));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * format_gpo — exact byte output
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_protocol, format_gpo_single_pin_low)
{
    /* Set pin 3 low, leave the rest unchanged: "GPO 1 xxlxx\n" */
    char buf[32];
    axia_gpo_pin_t pins[AXIA_GPIO_PINS] = {
        AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED, AXIA_GPO_LOW, AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED
    };
    int n = axia_protocol_format_gpo(buf, sizeof(buf), 1, pins);
    cr_assert_eq(n, (int)strlen("GPO 1 xxlxx\n"));
    cr_assert_str_eq(buf, "GPO 1 xxlxx\n");
}

Test(axia_protocol, format_gpo_on_air_pattern)
{
    /* On-air: pin1 low (on), pin2 high (off), pins 3-5 unchanged. */
    char buf[32];
    axia_gpo_pin_t pins[AXIA_GPIO_PINS] = {
        AXIA_GPO_LOW, AXIA_GPO_HIGH, AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED
    };
    int n = axia_protocol_format_gpo(buf, sizeof(buf), 2, pins);
    cr_assert_eq(n, (int)strlen("GPO 2 lhxxx\n"));
    cr_assert_str_eq(buf, "GPO 2 lhxxx\n");
}

Test(axia_protocol, format_gpo_all_unchanged)
{
    char buf[32];
    axia_gpo_pin_t pins[AXIA_GPIO_PINS] = {
        AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED, AXIA_GPO_UNCHANGED
    };
    cr_assert_gt(axia_protocol_format_gpo(buf, sizeof(buf), 1, pins), 0);
    cr_assert_str_eq(buf, "GPO 1 xxxxx\n");
}

Test(axia_protocol, format_gpo_rejects_bad_args)
{
    char buf[32];
    axia_gpo_pin_t pins[AXIA_GPIO_PINS] = { 0 };
    cr_assert_eq(axia_protocol_format_gpo(NULL, sizeof(buf), 1, pins), 0);
    cr_assert_eq(axia_protocol_format_gpo(buf, sizeof(buf), 1, NULL), 0);
    cr_assert_eq(axia_protocol_format_gpo(buf, sizeof(buf), 0, pins), 0);
}

Test(axia_protocol, format_gpo_rejects_truncation)
{
    char buf[8]; /* too small for "GPO 1 xxxxx\n" (12 bytes) */
    axia_gpo_pin_t pins[AXIA_GPIO_PINS] = { 0 };
    cr_assert_eq(axia_protocol_format_gpo(buf, sizeof(buf), 1, pins), 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * format_login
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_protocol, format_login_basic)
{
    char buf[80];
    int n = axia_protocol_format_login(buf, sizeof(buf), "secret");
    cr_assert_eq(n, (int)strlen("LOGIN secret\n"));
    cr_assert_str_eq(buf, "LOGIN secret\n");
}

Test(axia_protocol, format_login_rejects_bad_args)
{
    char buf[80];
    cr_assert_eq(axia_protocol_format_login(NULL, sizeof(buf), "x"), 0);
    cr_assert_eq(axia_protocol_format_login(buf, sizeof(buf), NULL), 0);
}

Test(axia_protocol, format_login_rejects_truncation)
{
    char buf[8]; /* too small for "LOGIN secret\n" */
    cr_assert_eq(axia_protocol_format_login(buf, sizeof(buf), "secret"), 0);
}
