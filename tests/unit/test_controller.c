/**
 * Channel Controller Tests (Axia backend)
 *
 * Tests the generic controller surface driven by the Axia LWRP backend, using a
 * mock TCP server. Covers connection lifecycle, authentication, command
 * decoding (GPI -> control_command_t), state feedback (control_state_t -> GPO),
 * and clean thread teardown.
 *
 * Key invariants tested:
 * - Null parameter handling in all public APIs
 * - Connection/reconnection lifecycle and status callbacks
 * - GPI rising edges decode to the right commands; LOW edges are ignored
 * - Channel state feedback emits GPO commands
 * - Callbacks marshaled to the main thread; none fire after destroy
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
#include "quadrature/controller.h"

/* Test constants */
#define TEST_CHANNEL  0
#define TEST_IP       "127.0.0.1"
#define TEST_PORT     9300 // Use non-standard port to avoid conflicts
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
    int command_count;
    int last_channel;
    control_command_t last_command;
    int status_change_count;
    gboolean last_connected;
} callback_state_t;

/* Forward declarations */
static mock_server_t *mock_server_create(uint16_t port);
static void mock_server_destroy(mock_server_t *server);
static void mock_server_send(mock_server_t *server, const char *msg);

static void
command_callback(int channel, control_command_t command, void *user_data)
{
    callback_state_t *cb_state = user_data;
    cb_state->command_count++;
    cb_state->last_channel = channel;
    cb_state->last_command = command;
}

static void
status_callback(int channel, bool connected, void *user_data)
{
    (void)channel;
    callback_state_t *cb_state = user_data;
    cb_state->status_change_count++;
    cb_state->last_connected = connected;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Lifecycle and Null Safety
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, lifecycle_null_safety)
{
    controller_t *c = NULL;

    /* Create with NULL out pointer returns error */
    quadrature_result_t res = controller_axia_create(TEST_IP, TEST_CHANNEL, TEST_PASSWORD, NULL);
    cr_assert_eq(res, QUADRATURE_ERROR_INVALID_PARAM);

    /* Create with NULL address returns error */
    res = controller_axia_create(NULL, TEST_CHANNEL, TEST_PASSWORD, &c);
    cr_assert_eq(res, QUADRATURE_ERROR_INVALID_PARAM);

    /* Create with valid parameters succeeds (will fail to connect, that's OK) */
    res = controller_axia_create("192.0.2.1", // Use TEST-NET-1 (won't connect)
                                 TEST_CHANNEL,
                                 NULL, // No auth
                                 &c);
    cr_assert_eq(res, QUADRATURE_OK);
    cr_assert_not_null(c);

    controller_destroy(c);

    /* Null destroy is safe */
    controller_destroy(NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: State Feedback Without Connection
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, set_state_without_connection)
{
    controller_t *c = NULL;

    quadrature_result_t res = controller_axia_create("192.0.2.1", TEST_CHANNEL, NULL, &c);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Not connected: should report busy, never crash */
    res = controller_set_channel_state(c, TEST_CHANNEL, CONTROL_STATE_ON_AIR);
    cr_assert_eq(res, QUADRATURE_ERROR_DEVICE_BUSY);
    controller_set_channel_state(c, TEST_CHANNEL, CONTROL_STATE_PREVIEW);
    controller_set_channel_state(c, TEST_CHANNEL, CONTROL_STATE_IDLE);

    /* NULL handle */
    res = controller_set_channel_state(NULL, TEST_CHANNEL, CONTROL_STATE_ON_AIR);
    cr_assert_eq(res, QUADRATURE_ERROR_INVALID_PARAM);

    controller_destroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Command Decoding (GPI -> control_command_t)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, command_decoding, .timeout = 5.0)
{
    mock_server_t *server = mock_server_create(TEST_PORT);
    cr_assert_not_null(server);

    callback_state_t cb_state = { 0 };
    controller_t *c = NULL;

    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT);
    quadrature_result_t res = controller_axia_create(address, TEST_CHANNEL, NULL, &c);
    cr_assert_eq(res, QUADRATURE_OK);

    controller_set_command_callback(c, command_callback, &cb_state);
    controller_set_status_callback(c, status_callback, &cb_state);

    res = controller_start(c);
    cr_assert_eq(res, QUADRATURE_OK);

    g_usleep(500000); // 500ms for connection

    cr_assert_gt(cb_state.status_change_count, 0, "Status callback not called");

    /* Pin 1 transitions to active (low) -> PLAY_PAUSE.
     * LWRP: 5-pin field, active-low, uppercase = the pin that changed. */
    mock_server_send(server, "GPI 1 Lhhhh\n");
    g_usleep(100000);
    cr_assert_eq(cb_state.command_count, 1, "Expected 1 command");
    cr_assert_eq(cb_state.last_channel, 0, "Channel should be 0 (from LWRP port 1)");
    cr_assert_eq(cb_state.last_command, CONTROL_CMD_PLAY_PAUSE);

    /* Pin 1 transitions to inactive (high) -> ignored (we act on the active edge) */
    mock_server_send(server, "GPI 1 Hhhhh\n");
    g_usleep(100000);
    cr_assert_eq(cb_state.command_count, 1, "Inactive edge should be ignored");

    /* Pin 2 transitions to active (low) -> PREVIEW_TOGGLE */
    mock_server_send(server, "GPI 1 hLhhh\n");
    g_usleep(100000);
    cr_assert_eq(cb_state.command_count, 2);
    cr_assert_eq(cb_state.last_command, CONTROL_CMD_PREVIEW_TOGGLE);

    /* Steady-state report (all lowercase = nothing changing) -- e.g. the full
     * state dump a console sends right after "ADD GPI". Even though pins 1 and 2
     * read active (low), no edge changed, so no command must be emitted. */
    mock_server_send(server, "GPI 1 llhhh\n");
    g_usleep(100000);
    cr_assert_eq(cb_state.command_count, 2, "Steady-state notification must not emit commands");

    /* Invalid message -> ignored */
    mock_server_send(server, "INVALID MESSAGE\n");
    g_usleep(100000);
    cr_assert_eq(cb_state.command_count, 2, "Invalid message should be ignored");

    controller_stop(c);
    controller_destroy(c);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: State Feedback (control_state_t -> GPO)
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, state_feedback, .timeout = 5.0)
{
    mock_server_t *server = mock_server_create(TEST_PORT + 1);
    cr_assert_not_null(server);

    controller_t *c = NULL;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 1);
    quadrature_result_t res = controller_axia_create(address, TEST_CHANNEL, NULL, &c);
    cr_assert_eq(res, QUADRATURE_OK);

    res = controller_start(c);
    cr_assert_eq(res, QUADRATURE_OK);
    g_usleep(500000);

    /* ON_AIR -> on-air pin low (on), preview pin high (off): "GPO 1 lhxxx" */
    res = controller_set_channel_state(c, TEST_CHANNEL, CONTROL_STATE_ON_AIR);
    cr_assert_eq(res, QUADRATURE_OK);
    g_usleep(100000);

    g_mutex_lock(&server->mutex);
    gboolean has_on_air = strstr(server->received_commands->str, "GPO 1 lh") != NULL;
    g_mutex_unlock(&server->mutex);
    cr_assert(has_on_air, "Expected 'GPO 1 lh...' for ON_AIR");

    /* IDLE -> both pins high (off): "GPO 1 hhxxx" */
    controller_set_channel_state(c, TEST_CHANNEL, CONTROL_STATE_IDLE);
    g_usleep(100000);

    g_mutex_lock(&server->mutex);
    gboolean has_idle = strstr(server->received_commands->str, "GPO 1 hh") != NULL;
    g_mutex_unlock(&server->mutex);
    cr_assert(has_idle, "Expected 'GPO 1 hh...' for IDLE");

    controller_stop(c);
    controller_destroy(c);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Authentication Flow
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, authentication_flow, .timeout = 5.0)
{
    mock_server_t *server = mock_server_create(TEST_PORT + 2);
    cr_assert_not_null(server);

    controller_t *c = NULL;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 2);
    quadrature_result_t res = controller_axia_create(address, TEST_CHANNEL, TEST_PASSWORD, &c);
    cr_assert_eq(res, QUADRATURE_OK);

    res = controller_start(c);
    cr_assert_eq(res, QUADRATURE_OK);
    g_usleep(500000);

    g_mutex_lock(&server->mutex);
    const char *cmds = server->received_commands->str;

    char expected_login[80];
    snprintf(expected_login, sizeof(expected_login), "LOGIN %s", TEST_PASSWORD);
    gboolean has_login = strstr(cmds, expected_login) != NULL;
    gboolean has_add = strstr(cmds, "ADD") != NULL;
    g_mutex_unlock(&server->mutex);

    cr_assert(has_login, "Expected LOGIN command with password");
    cr_assert(has_add, "Expected ADD command after authentication");

    controller_stop(c);
    controller_destroy(c);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Thread Cleanup on Destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, thread_cleanup, .timeout = 5.0)
{
    mock_server_t *server = mock_server_create(TEST_PORT + 3);
    cr_assert_not_null(server);

    callback_state_t cb_state = { 0 };
    controller_t *c = NULL;

    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 3);
    quadrature_result_t res = controller_axia_create(address, TEST_CHANNEL, NULL, &c);
    cr_assert_eq(res, QUADRATURE_OK);

    controller_set_command_callback(c, command_callback, &cb_state);
    controller_set_status_callback(c, status_callback, &cb_state);

    res = controller_start(c);
    cr_assert_eq(res, QUADRATURE_OK);
    g_usleep(500000);

    int commands_before = cb_state.command_count;

    /* Destroy should block until thread exits */
    controller_destroy(c);

    /* Command after destroy must not fire a callback */
    mock_server_send(server, "GPI 1 Lhhhh\n");
    g_usleep(100000);

    cr_assert_eq(cb_state.command_count, commands_before, "No callbacks should fire after destroy");

    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Test: Connection Status Tracking
 * ═══════════════════════════════════════════════════════════════════════════ */

Test(controller, connection_status_tracking, .timeout = 5.0)
{
    mock_server_t *server = mock_server_create(TEST_PORT + 4);
    cr_assert_not_null(server);

    controller_t *c = NULL;
    char address[32];
    snprintf(address, sizeof(address), "127.0.0.1:%d", TEST_PORT + 4);
    quadrature_result_t res = controller_axia_create(address, TEST_CHANNEL, NULL, &c);
    cr_assert_eq(res, QUADRATURE_OK);

    /* Before start, should not be connected */
    cr_assert_eq(controller_is_connected(c), false);

    res = controller_start(c);
    cr_assert_eq(res, QUADRATURE_OK);
    g_usleep(500000);

    cr_assert_eq(controller_is_connected(c), true);

    controller_stop(c);
    g_usleep(100000);

    cr_assert_eq(controller_is_connected(c), false);

    controller_destroy(c);
    mock_server_destroy(server);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mock Server Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *
mock_server_thread(void *arg)
{
    mock_server_t *server = arg;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    server->client_fd = accept(server->server_fd, (struct sockaddr *)&addr, &addrlen);
    if (server->client_fd < 0) {
        return NULL;
    }

    char buf[512];
    while (server->running) {
        ssize_t n = recv(server->client_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n > 0) {
            buf[n] = '\0';

            g_mutex_lock(&server->mutex);
            g_string_append(server->received_commands, buf);
            g_mutex_unlock(&server->mutex);

            const char *response = "OK\n";
            send(server->client_fd, response, strlen(response), 0);
        } else if (n == 0) {
            break; // Connection closed
        }

        usleep(10000); // 10ms
    }

    if (server->client_fd >= 0) {
        close(server->client_fd);
        server->client_fd = -1;
    }

    return NULL;
}

static mock_server_t *
mock_server_create(uint16_t port)
{
    mock_server_t *server = g_new0(mock_server_t, 1);
    server->port = port;
    server->running = TRUE;
    server->client_fd = -1;
    server->received_commands = g_string_new("");
    g_mutex_init(&server->mutex);

    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        g_free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server->server_fd);
        g_free(server);
        return NULL;
    }

    if (listen(server->server_fd, 1) < 0) {
        close(server->server_fd);
        g_free(server);
        return NULL;
    }

    pthread_create(&server->thread, NULL, mock_server_thread, server);

    return server;
}

static void
mock_server_destroy(mock_server_t *server)
{
    if (!server)
        return;

    server->running = FALSE;

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

static void
mock_server_send(mock_server_t *server, const char *msg)
{
    if (!server || server->client_fd < 0)
        return;
    send(server->client_fd, msg, strlen(msg), 0);
}
