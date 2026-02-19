/**
 * Axia LWRP Protocol Helpers
 *
 * Parsing and formatting functions for Livewire Routing Protocol (LWRP) messages.
 */

#include "internal.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Parse incoming GPI message: "GPI <ch> <pin_data>"
 * pin_data format: pin 1 = "H" or "L", pin 2 = "xH" or "xL", etc.
 * Examples:
 *   "GPI 1 H"   → channel=1, pin=1, state=1
 *   "GPI 1 xH"  → channel=1, pin=2, state=1
 *   "GPI 2 xxL" → channel=2, pin=3, state=0
 */
bool axia_protocol_parse_gpi(const char *line, int *out_channel, int *out_pin, int *out_state) {
    if (!line || !out_channel || !out_pin || !out_state) return false;
    
    /* Skip leading whitespace */
    while (isspace(*line)) line++;
    
    /* Check for "GPI " prefix */
    if (strncmp(line, "GPI ", 4) != 0) return false;
    line += 4;
    
    /* Parse channel number */
    char *endptr;
    long ch = strtol(line, &endptr, 10);
    if (endptr == line || ch < 1 || ch > 4) return false;
    *out_channel = (int)ch;
    line = endptr;
    
    /* Skip whitespace */
    while (isspace(*line)) line++;
    
    /* Parse pin data: count 'x' padding, then read state character */
    int pin = 1;
    while (*line == 'x') {
        pin++;
        line++;
    }
    
    if (pin < 1 || pin > 5) return false;
    *out_pin = pin;
    
    /* Read state character */
    if (*line == 'H' || *line == 'h') {
        *out_state = 1;
    } else if (*line == 'L' || *line == 'l') {
        *out_state = 0;
    } else {
        return false;
    }
    
    return true;
}

/* Format GPO command: "GPO <ch> <pin_data>\n"
 * pin_data format: same as GPI (pin 1 = "H", pin 2 = "xH", etc.)
 */
int axia_protocol_format_gpo(char *buffer, int channel, int pin, int state) {
    if (!buffer || channel < 1 || channel > 4 || pin < 1 || pin > 5) return 0;
    
    /* Build pin data string: 'x' padding + state character */
    char pin_data[6] = {0};
    for (int i = 0; i < (pin - 1); i++) {
        pin_data[i] = 'x';
    }
    pin_data[pin - 1] = (state == 1) ? 'H' : 'L';
    
    return snprintf(buffer, 32, "GPO %d %s\n", channel, pin_data);
}

/* Format LOGIN command: "LOGIN <password>\n" */
int axia_protocol_format_login(char *buffer, const char *password) {
    if (!buffer || !password) return 0;
    return snprintf(buffer, 80, "LOGIN %s\n", password);
}
