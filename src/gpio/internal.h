/**
 * Quadrature GPIO Internal Declarations
 *
 * Private types and functions for Axia GPIO implementation.
 */

#ifndef QUADRATURE_GPIO_INTERNAL_H
#define QUADRATURE_GPIO_INTERNAL_H

#include "quadrature/gpio.h"
#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>

/* LWRP protocol constants */
#define LWRP_PORT                  93
#define LWRP_MAX_LINE              512
#define RECONNECT_INITIAL_DELAY_MS 100
#define RECONNECT_MAX_DELAY_MS     5000
#define RECONNECT_BACKOFF_FACTOR   2

/* Main GPIO handle structure */
struct axia_gpio_t {
    char *console_ip; // Heap-allocated console IP
    uint16_t port;    // TCP port (default: 93)
    int channel_id;   // 0-3 (internal), sent as 1-4 to console
    char *password;   // LWRP password for LOGIN command (NULL if not required)
    int sockfd;       // TCP socket file descriptor (-1 if not connected)

    /* Thread management */
    pthread_t listener_thread;
    _Atomic bool running;   // Thread should keep running
    _Atomic bool connected; // Currently connected to console

    /* Callbacks */
    GMutex callback_mutex;                       // Protects callback pointers
    axia_gpio_callback_t callback;               // User callback for GPIO events
    void *callback_user_data;                    // User data for GPIO callback
    axia_gpio_status_callback_t status_callback; // User callback for status changes
    void *status_callback_user_data;             // User data for status callback

    /* Reconnection state */
    int reconnect_delay_ms; // Current reconnect delay (exponential backoff)
};

/* Protocol helpers (axia_protocol.c) */

/**
 * Parse incoming GPIO message
 * Handles: "GPI <ch> <pin_data> <state>"
 * Example: "GPI 1 H" = Channel 1, Pin 1, HIGH
 *          "GPI 1 xH" = Channel 1, Pin 2, HIGH
 * @return true if parsed successfully, false otherwise
 */
bool axia_protocol_parse_gpi(const char *line, int *out_channel, int *out_pin, int *out_state);

/**
 * Format GPIO output command
 * Builds: "GPO <ch> <pin_data> <state>\n"
 * @param buffer Output buffer (must be at least 32 bytes)
 * @param channel Channel ID (1-4, LWRP protocol)
 * @param pin Pin number (1-5)
 * @param state State (0=LOW, 1=HIGH)
 * @return Number of bytes written to buffer
 */
int axia_protocol_format_gpo(char *buffer, int channel, int pin, int state);

/**
 * Format LOGIN command: "LOGIN <password>\n"
 * @param buffer Output buffer (must be at least 80 bytes)
 * @param password Password string
 * @return Number of bytes written to buffer
 */
int axia_protocol_format_login(char *buffer, const char *password);

#endif /* QUADRATURE_GPIO_INTERNAL_H */
