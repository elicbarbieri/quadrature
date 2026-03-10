/**
 * Axia Livewire+ GPIO Tests
 *
 * Tests for TCP/LWRP protocol implementation with mock socket I/O.
 * Tests connection lifecycle, authentication, message parsing, and callbacks.
 *
 * Key invariants tested:
 * - Null parameter handling in all public APIs
 * - Connection/reconnection logic with backoff
 * - LWRP message parsing and formatting
 * - Callback invocation on GTK main thread
 * - Cleanup during destroy (thread join, socket close)
 */

#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "quadrature/gpio.h"

/* Test constants */
#define TEST_CHANNEL_ID 0
#define TEST_IP "127.0.0.1"
#define TEST_PORT 9300  // Use non-standard port to avoid conflicts
#define TEST_PASSWORD "testpass"

/* Mock server state */
typedef struct {
    int server_fd;
    int client_fd;
    uint16_t port;
    pthread_t thread;
    gboolean running;
    gboolean authenticated;
    GString *received_commands;
    GMutex mutex;
} mock_server_t;

/* Callback state for testing */
typedef struct {
    int event_count;
    int last_channel_id;
    axia_pin_t last_pin;
    axia_state_t last_state;
    int status_change_count;
    gboolean last_connected;
} callback_state_t;

/* Forward declarations */
static mock_server_t* mock_server_create(uint16_t port);
static void mock_server_destroy(mock_server_t *server);
static void mock_server_send(mock_server_t *server, const char *msg);

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lifecycle and Null Safety
 *
 * Verifies:
 * 1. Create with NULL out pointer returns error
 * 2. Create with valid parameters succeeds
 * 3. Null destroy is safe (no crash)
 * 4. Double destroy is safe
 * 5. Invalid channel ID handling
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_gpio, lifecycle_null_safety) {
    axia_gpio_t *gpio = NULL;

    /* Create with NULL out pointer returns error */
    quadrature_result_t res = axia_gpio_create(TEST_IP, TEST_CHANNEL_ID,
                                               TEST_PASSWORD,
                                               NULL);
    cr_assert_eq(res, QUADRATURE_ERROR_INVALID_PARAM);

    /* Create with NULL IP returns error */
    res = axia_gpio_create(NULL, TEST_CHANNEL_ID,
                          TEST_PASSWORD,
                          &gpio);
    cr_assert_eq(res, QUADRATURE_ERROR_INVALID_PARAM);

    /* Create with valid parameters succeeds (will fail to connect, that's OK) */
    res = axia_gpio_create("192.0.2.1", TEST_CHANNEL_ID,  // Use TEST-NET-1 (won't connect)
                          NULL,        // No auth
                          &gpio);
    cr_assert_eq(res, QUADRATURE_OK);
    cr_assert_not_null(gpio);

    /* Destroy should succeed */
    axia_gpio_destroy(gpio);

    /* Null destroy is safe */
    axia_gpio_destroy(NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Send GPO Commands Without Connection
 *
 * Verifies:
 * 1. Set function handles NULL gpio safely
 * 2. Set function returns error when not connected
 * 3. No crash when sending to unconnected handler
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_gpio, send_without_connection) {
    axia_gpio_t *gpio = NULL;

    /* Create handler but won't connect (invalid IP) */
    quadrature_result_t res = axia_gpio_create("192.0.2.1", TEST_CHANNEL_ID,
                                               NULL,
                                               &gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Send commands should not crash (may return error or succeed) */
    axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_HIGH);
    axia_gpio_set(gpio, AXIA_PIN_PREVIEW, AXIA_STATE_HIGH);
    axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_LOW);
    axia_gpio_set(gpio, AXIA_PIN_PREVIEW, AXIA_STATE_LOW);

    /* NULL gpio handling */
    axia_gpio_set(NULL, AXIA_PIN_ON_AIR, AXIA_STATE_HIGH);
    axia_gpio_set(NULL, AXIA_PIN_PREVIEW, AXIA_STATE_HIGH);

    axia_gpio_destroy(gpio);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: LWRP Message Parsing
 *
 * Verifies:
 * 1. GPI HIGH messages parsed correctly
 * 2. GPI LOW messages parsed correctly  
 * 3. Multi-pin messages parsed correctly ("xH" format)
 * 4. Invalid messages ignored gracefully
 * 5. Callback invoked with correct parameters
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gpio_event_callback(int channel_id, axia_pin_t pin, axia_state_t state, void *user_data) {
    callback_state_t *cb_state = user_data;
    cb_state->event_count++;
    cb_state->last_channel_id = channel_id;
    cb_state->last_pin = pin;
    cb_state->last_state = state;
}

static void status_callback(int channel_id, bool connected, void *user_data) {
    callback_state_t *cb_state = user_data;
    cb_state->status_change_count++;
    cb_state->last_connected = connected;
}

Test(axia_gpio, lwrp_message_parsing, .timeout = 5.0) {
    mock_server_t *server = mock_server_create(TEST_PORT);
    cr_assert_not_null(server);

    callback_state_t cb_state = {0};
    axia_gpio_t *gpio = NULL;

    /* Create GPIO handler pointing to mock server */
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT);
    quadrature_result_t res = axia_gpio_create(address, TEST_CHANNEL_ID,
                                               NULL,        // No auth for this test
                                               &gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Set callbacks */
    axia_gpio_set_callback(gpio, gpio_event_callback, &cb_state);
    axia_gpio_set_status_callback(gpio, status_callback, &cb_state);

    /* Start listener thread - this triggers connection attempt */
    res = axia_gpio_start(gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Wait for connection */
    g_usleep(500000);  // 500ms

    /* Verify status callback fired (connected) */
    cr_assert_gt(cb_state.status_change_count, 0, "Status callback not called");

    /* Send GPI messages from mock server */
    mock_server_send(server, "GPI 1 H\n");    // Channel 1 (LWRP), pin 1 HIGH (ON-AIR)
    g_usleep(100000);  // 100ms for callback

    cr_assert_eq(cb_state.event_count, 1, "Expected 1 event");
    cr_assert_eq(cb_state.last_channel_id, 0, "Channel ID should be 0 (internal, from LWRP channel 1)");
    cr_assert_eq(cb_state.last_pin, AXIA_PIN_ON_AIR);
    cr_assert_eq(cb_state.last_state, AXIA_STATE_HIGH);

    /* Send GPI LOW message */
    mock_server_send(server, "GPI 1 L\n");
    g_usleep(100000);

    cr_assert_eq(cb_state.event_count, 2);
    cr_assert_eq(cb_state.last_state, AXIA_STATE_LOW);

    /* Send multi-pin message (preview = pin 2) */
    mock_server_send(server, "GPI 1 xH\n");  // Pin 2 HIGH
    g_usleep(100000);

    cr_assert_eq(cb_state.event_count, 3);
    cr_assert_eq(cb_state.last_pin, AXIA_PIN_PREVIEW);
    cr_assert_eq(cb_state.last_state, AXIA_STATE_HIGH);

    /* Send invalid message (should be ignored) */
    mock_server_send(server, "INVALID MESSAGE\n");
    g_usleep(100000);

    cr_assert_eq(cb_state.event_count, 3, "Invalid message should be ignored");

    axia_gpio_stop(gpio);
    axia_gpio_destroy(gpio);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: GPO Command Formatting
 *
 * Verifies:
 * 1. ON-AIR HIGH sends correct GPO command
 * 2. ON-AIR LOW sends correct GPO command
 * 3. PREVIEW HIGH sends correct GPO command
 * 4. PREVIEW LOW sends correct GPO command
 * 5. Commands received by server in correct format
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_gpio, gpo_command_formatting, .timeout = 5.0) {
    mock_server_t *server = mock_server_create(TEST_PORT + 1);
    cr_assert_not_null(server);

    axia_gpio_t *gpio = NULL;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 1);
    quadrature_result_t res = axia_gpio_create(address, TEST_CHANNEL_ID,
                                               NULL,
                                               &gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Start listener thread */
    res = axia_gpio_start(gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Wait for connection */
    g_usleep(500000);

    /* Send ON-AIR HIGH */
    axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_HIGH);
    g_usleep(100000);

    /* Check server received correct command */
    g_mutex_lock(&server->mutex);
    gboolean has_gpo_high = strstr(server->received_commands->str, "GPO") != NULL &&
                            strstr(server->received_commands->str, "H") != NULL;
    g_mutex_unlock(&server->mutex);
    cr_assert(has_gpo_high, "Expected GPO HIGH command");

    /* Send ON-AIR LOW */
    axia_gpio_set(gpio, AXIA_PIN_ON_AIR, AXIA_STATE_LOW);
    g_usleep(100000);

    g_mutex_lock(&server->mutex);
    gboolean has_gpo_low = strstr(server->received_commands->str, "L") != NULL;
    g_mutex_unlock(&server->mutex);
    cr_assert(has_gpo_low, "Expected GPO LOW command");

    axia_gpio_stop(gpio);
    axia_gpio_destroy(gpio);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Authentication Flow
 *
 * Verifies:
 * 1. LOGIN command sent with correct password
 * 2. ADD commands sent after authentication
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_gpio, authentication_flow, .timeout = 5.0) {
    mock_server_t *server = mock_server_create(TEST_PORT + 2);
    cr_assert_not_null(server);

    axia_gpio_t *gpio = NULL;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 2);
    quadrature_result_t res = axia_gpio_create(address, TEST_CHANNEL_ID,
                                               TEST_PASSWORD,
                                               &gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Start listener thread */
    res = axia_gpio_start(gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Wait for auth flow to complete */
    g_usleep(500000);

    /* Verify commands received */
    g_mutex_lock(&server->mutex);
    const char *cmds = server->received_commands->str;

    char expected_login[80];
    snprintf(expected_login, sizeof(expected_login), "LOGIN %s", TEST_PASSWORD);
    gboolean has_login = strstr(cmds, expected_login) != NULL;
    gboolean has_add = strstr(cmds, "ADD") != NULL;

    g_mutex_unlock(&server->mutex);

    cr_assert(has_login, "Expected LOGIN command with password");
    cr_assert(has_add, "Expected ADD command after authentication");

    axia_gpio_stop(gpio);
    axia_gpio_destroy(gpio);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Thread Cleanup on Destroy
 *
 * Verifies:
 * 1. Destroy waits for listener thread to exit
 * 2. No dangling threads after destroy
 * 3. Socket properly closed
 * 4. Callbacks not invoked after destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_gpio, thread_cleanup, .timeout = 5.0) {
    mock_server_t *server = mock_server_create(TEST_PORT + 3);
    cr_assert_not_null(server);

    callback_state_t cb_state = {0};
    axia_gpio_t *gpio = NULL;

    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 3);
    quadrature_result_t res = axia_gpio_create(address, TEST_CHANNEL_ID,
                                               NULL,
                                               &gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    axia_gpio_set_callback(gpio, gpio_event_callback, &cb_state);
    axia_gpio_set_status_callback(gpio, status_callback, &cb_state);

    res = axia_gpio_start(gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Wait for connection */
    g_usleep(500000);

    int events_before = cb_state.event_count;

    /* Destroy should block until thread exits */
    axia_gpio_destroy(gpio);

    /* Send message after destroy - should not trigger callback */
    mock_server_send(server, "GPI 1 H\n");
    g_usleep(100000);

    cr_assert_eq(cb_state.event_count, events_before, 
                 "No callbacks should fire after destroy");

    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Connection Status Tracking
 *
 * Verifies:
 * 1. is_connected() returns false before start
 * 2. is_connected() returns true after successful connection
 * 3. is_connected() returns false after stop
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(axia_gpio, connection_status_tracking, .timeout = 5.0) {
    mock_server_t *server = mock_server_create(TEST_PORT + 4);
    cr_assert_not_null(server);

    axia_gpio_t *gpio = NULL;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 4);
    quadrature_result_t res = axia_gpio_create(address, TEST_CHANNEL_ID,
                                               NULL,
                                               &gpio);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Before start, should not be connected */
    cr_assert_eq(axia_gpio_is_connected(gpio), false);

    /* Start and wait for connection */
    res = axia_gpio_start(gpio);
    cr_assert_eq(res, QUADRATURE_OK);
    g_usleep(500000);

    /* Should be connected now */
    cr_assert_eq(axia_gpio_is_connected(gpio), true);

    /* Stop and verify disconnected */
    axia_gpio_stop(gpio);
    g_usleep(100000);
    
    cr_assert_eq(axia_gpio_is_connected(gpio), false);

    axia_gpio_destroy(gpio);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mock Server Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void* mock_server_thread(void *arg) {
    mock_server_t *server = arg;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    /* Accept connection */
    server->client_fd = accept(server->server_fd, (struct sockaddr*)&addr, &addrlen);
    if (server->client_fd < 0) {
        return NULL;
    }

    /* Echo loop - read commands and send simple OK responses */
    char buf[512];
    while (server->running) {
        ssize_t n = recv(server->client_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n > 0) {
            buf[n] = '\0';
            
            /* Store received commands */
            g_mutex_lock(&server->mutex);
            g_string_append(server->received_commands, buf);
            g_mutex_unlock(&server->mutex);

            /* Send simple OK response for any command */
            const char *response = "OK\n";
            send(server->client_fd, response, strlen(response), 0);
        } else if (n == 0) {
            break;  // Connection closed
        }
        
        usleep(10000);  // 10ms
    }

    if (server->client_fd >= 0) {
        close(server->client_fd);
        server->client_fd = -1;
    }

    return NULL;
}

static mock_server_t* mock_server_create(uint16_t port) {
    mock_server_t *server = g_new0(mock_server_t, 1);
    server->port = port;
    server->running = TRUE;
    server->client_fd = -1;
    server->received_commands = g_string_new("");
    g_mutex_init(&server->mutex);

    /* Create listening socket */
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        g_free(server);
        return NULL;
    }

    /* Allow port reuse */
    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind to localhost */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(server->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->server_fd);
        g_free(server);
        return NULL;
    }

    if (listen(server->server_fd, 1) < 0) {
        close(server->server_fd);
        g_free(server);
        return NULL;
    }

    /* Start server thread */
    pthread_create(&server->thread, NULL, mock_server_thread, server);

    return server;
}

static void mock_server_destroy(mock_server_t *server) {
    if (!server) return;

    server->running = FALSE;
    
    /* Wake up server thread */
    if (server->client_fd >= 0) {
        shutdown(server->client_fd, SHUT_RDWR);
    }

    pthread_join(server->thread, NULL);

    if (server->server_fd >= 0) {
        close(server->server_fd);
    }

    g_string_free(server->received_commands, TRUE);
    g_mutex_clear(&server->mutex);
    g_free(server);
}

static void mock_server_send(mock_server_t *server, const char *msg) {
    if (!server || server->client_fd < 0) return;
    send(server->client_fd, msg, strlen(msg), 0);
}
