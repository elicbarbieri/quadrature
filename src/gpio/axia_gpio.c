/**
 * Axia Livewire+ GPIO Implementation
 *
 * Manages TCP connection to Axia console via LWRP protocol.
 * Each axia_gpio_t instance handles one channel on one console.
 */

#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* Logging macros */
#define LOG_ERROR(fmt, ...) g_warning("Axia GPIO: " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  g_warning("Axia GPIO: " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  g_info("Axia GPIO: " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) g_debug("Axia GPIO: " fmt, ##__VA_ARGS__)

/* Forward declarations */
static void *listener_thread_func(void *arg);
static bool connect_to_console(axia_gpio_t *gpio);
static void disconnect(axia_gpio_t *gpio);
static bool send_subscription(axia_gpio_t *gpio);
static bool authenticate(axia_gpio_t *gpio);
static bool read_and_dispatch_events(axia_gpio_t *gpio);
static void dispatch_gpio_event(axia_gpio_t *gpio, axia_pin_t pin, axia_state_t state);
static void dispatch_status_event(axia_gpio_t *gpio, bool connected);
static void exponential_backoff_sleep(axia_gpio_t *gpio);

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
axia_gpio_create(const char *address, int channel_id, const char *password, axia_gpio_t **out)
{
    if (!address || channel_id < 0 || channel_id >= 4 || !out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    /* Parse address into IP and port (format: "ip:port" or "ip") */
    char *ip = NULL;
    uint16_t port = LWRP_PORT; // Default to 93

    char *colon = strchr(address, ':');
    if (colon) {
        /* Format: "ip:port" */
        ip = g_strndup(address, colon - address);
        long parsed_port = strtol(colon + 1, NULL, 10);
        if (parsed_port <= 0 || parsed_port > 65535) {
            g_free(ip);
            return QUADRATURE_ERROR_INVALID_PARAM;
        }
        port = (uint16_t)parsed_port;
    } else {
        /* Format: "ip" only, use default port */
        ip = g_strdup(address);
    }

    /* Validate IP address is not empty */
    if (!ip || ip[0] == '\0') {
        g_free(ip);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    axia_gpio_t *gpio = g_new0(axia_gpio_t, 1);
    gpio->console_ip = ip;
    gpio->port = port;
    gpio->channel_id = channel_id;
    gpio->password = password ? g_strdup(password) : NULL;
    gpio->sockfd = -1;

    atomic_init(&gpio->running, false);
    atomic_init(&gpio->connected, false);

    g_mutex_init(&gpio->callback_mutex);
    gpio->callback = NULL;
    gpio->callback_user_data = NULL;
    gpio->status_callback = NULL;
    gpio->status_callback_user_data = NULL;

    gpio->reconnect_delay_ms = RECONNECT_INITIAL_DELAY_MS;

    *out = gpio;
    LOG_DEBUG("Created GPIO handler for channel %d (%s:%u)",
              channel_id + 1,
              gpio->console_ip,
              gpio->port);
    return QUADRATURE_OK;
}

void
axia_gpio_destroy(axia_gpio_t *gpio)
{
    if (!gpio)
        return;

    LOG_DEBUG("Destroying GPIO handler for channel %d", gpio->channel_id + 1);

    /* Stop thread if running */
    if (atomic_load(&gpio->running)) {
        axia_gpio_stop(gpio);
    }

    g_mutex_clear(&gpio->callback_mutex);
    g_free(gpio->console_ip);
    g_free(gpio->password);
    g_free(gpio);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback & Thread Control
 * ═══════════════════════════════════════════════════════════════════════════ */

void
axia_gpio_set_callback(axia_gpio_t *gpio, axia_gpio_callback_t callback, void *user_data)
{
    if (!gpio)
        return;

    g_mutex_lock(&gpio->callback_mutex);
    gpio->callback = callback;
    gpio->callback_user_data = user_data;
    g_mutex_unlock(&gpio->callback_mutex);
}

void
axia_gpio_set_status_callback(axia_gpio_t *gpio,
                              axia_gpio_status_callback_t callback,
                              void *user_data)
{
    if (!gpio)
        return;

    g_mutex_lock(&gpio->callback_mutex);
    gpio->status_callback = callback;
    gpio->status_callback_user_data = user_data;
    g_mutex_unlock(&gpio->callback_mutex);
}

quadrature_result_t
axia_gpio_start(axia_gpio_t *gpio)
{
    if (!gpio)
        return QUADRATURE_ERROR_INVALID_PARAM;

    if (atomic_load(&gpio->running)) {
        LOG_WARN("Channel %d: GPIO already running", gpio->channel_id + 1);
        return QUADRATURE_OK; /* Already running */
    }

    atomic_store(&gpio->running, true);

    int result = pthread_create(&gpio->listener_thread, NULL, listener_thread_func, gpio);
    if (result != 0) {
        LOG_ERROR("Channel %d: Failed to create listener thread: %s",
                  gpio->channel_id + 1,
                  strerror(result));
        atomic_store(&gpio->running, false);
        return QUADRATURE_ERROR_INTERNAL;
    }

    LOG_INFO("Channel %d: GPIO listener thread started", gpio->channel_id + 1);
    return QUADRATURE_OK;
}

quadrature_result_t
axia_gpio_stop(axia_gpio_t *gpio)
{
    if (!gpio)
        return QUADRATURE_ERROR_INVALID_PARAM;

    if (!atomic_load(&gpio->running)) {
        return QUADRATURE_OK; /* Not running */
    }

    LOG_INFO("Channel %d: Stopping GPIO listener thread", gpio->channel_id + 1);

    atomic_store(&gpio->running, false);

    /* Close socket to wake up blocking recv() */
    if (gpio->sockfd >= 0) {
        shutdown(gpio->sockfd, SHUT_RDWR);
    }

    /* Wait for thread to exit */
    pthread_join(gpio->listener_thread, NULL);

    disconnect(gpio);

    LOG_INFO("Channel %d: GPIO listener thread stopped", gpio->channel_id + 1);
    return QUADRATURE_OK;
}

bool
axia_gpio_is_connected(const axia_gpio_t *gpio)
{
    if (!gpio)
        return false;
    return atomic_load(&gpio->connected);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Send GPIO State (app → console LED feedback)
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
axia_gpio_set(axia_gpio_t *gpio, axia_pin_t pin, axia_state_t state)
{
    if (!gpio)
        return QUADRATURE_ERROR_INVALID_PARAM;

    if (!atomic_load(&gpio->connected)) {
        LOG_DEBUG("Channel %d: Cannot send GPIO (not connected)", gpio->channel_id + 1);
        return QUADRATURE_ERROR_DEVICE_BUSY;
    }

    /* Convert internal channel ID (0-3) to LWRP channel ID (1-4) */
    int lwrp_channel = gpio->channel_id + 1;

    /* Format command */
    char cmd[32];
    int len = axia_protocol_format_gpo(cmd, lwrp_channel, pin, state);
    if (len <= 0) {
        LOG_ERROR("Channel %d: Failed to format GPO command", gpio->channel_id + 1);
        return QUADRATURE_ERROR_INTERNAL;
    }

    /* Send command */
    ssize_t sent = send(gpio->sockfd, cmd, len, MSG_NOSIGNAL);
    if (sent < 0) {
        LOG_ERROR(
            "Channel %d: Failed to send GPO command: %s", gpio->channel_id + 1, strerror(errno));
        atomic_store(&gpio->connected, false);
        return QUADRATURE_ERROR_INTERNAL;
    }

    LOG_INFO("Channel %d: Sent GPO pin=%d state=%s",
             gpio->channel_id + 1,
             pin,
             state == AXIA_STATE_HIGH ? "HIGH" : "LOW");

    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Listener Thread
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *
listener_thread_func(void *arg)
{
    axia_gpio_t *gpio = (axia_gpio_t *)arg;

    LOG_DEBUG("Channel %d: Listener thread started", gpio->channel_id + 1);

    while (atomic_load(&gpio->running)) {
        /* Attempt connection */
        if (!connect_to_console(gpio)) {
            exponential_backoff_sleep(gpio);
            continue;
        }

        /* Authenticate if password provided */
        if (gpio->password) {
            if (!authenticate(gpio)) {
                disconnect(gpio);
                exponential_backoff_sleep(gpio);
                continue;
            }
        }

        /* Subscribe to GPIO */
        if (!send_subscription(gpio)) {
            disconnect(gpio);
            exponential_backoff_sleep(gpio);
            continue;
        }

        /* Connection successful - reset backoff */
        gpio->reconnect_delay_ms = RECONNECT_INITIAL_DELAY_MS;
        atomic_store(&gpio->connected, true);
        dispatch_status_event(gpio, true);

        LOG_INFO("Channel %d: Connected to console %s", gpio->channel_id + 1, gpio->console_ip);

        /* Read loop */
        while (atomic_load(&gpio->running) && atomic_load(&gpio->connected)) {
            if (!read_and_dispatch_events(gpio)) {
                break; /* Disconnected or error */
            }
        }

        disconnect(gpio);
        dispatch_status_event(gpio, false);

        LOG_INFO("Channel %d: Disconnected from console", gpio->channel_id + 1);
    }

    LOG_DEBUG("Channel %d: Listener thread exiting", gpio->channel_id + 1);
    return NULL;
}

static bool
connect_to_console(axia_gpio_t *gpio)
{
    /* Create socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Channel %d: Failed to create socket: %s", gpio->channel_id + 1, strerror(errno));
        return false;
    }

    /* Set non-blocking for connect timeout */
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    /* Setup address */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gpio->port);

    if (inet_pton(AF_INET, gpio->console_ip, &addr.sin_addr) <= 0) {
        LOG_ERROR("Channel %d: Invalid IP address: %s", gpio->channel_id + 1, gpio->console_ip);
        close(sockfd);
        return false;
    }

    /* Attempt connection */
    int result = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        LOG_DEBUG("Channel %d: Connect failed: %s", gpio->channel_id + 1, strerror(errno));
        close(sockfd);
        return false;
    }

    /* Wait for connection with timeout */
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);

    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
    if (result <= 0) {
        LOG_DEBUG("Channel %d: Connect timeout", gpio->channel_id + 1);
        close(sockfd);
        return false;
    }

    /* Check for connection error */
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        LOG_DEBUG("Channel %d: Connect error: %s", gpio->channel_id + 1, strerror(error));
        close(sockfd);
        return false;
    }

    /* Set back to blocking mode */
    fcntl(sockfd, F_SETFL, flags);

    gpio->sockfd = sockfd;
    LOG_DEBUG(
        "Channel %d: TCP connected to %s:%u", gpio->channel_id + 1, gpio->console_ip, gpio->port);

    return true;
}

static void
disconnect(axia_gpio_t *gpio)
{
    atomic_store(&gpio->connected, false);

    if (gpio->sockfd >= 0) {
        close(gpio->sockfd);
        gpio->sockfd = -1;
    }
}

static bool
authenticate(axia_gpio_t *gpio)
{
    /* LWRP auth: "LOGIN <password>\n" — no response on success */
    char cmd[80];
    int len = axia_protocol_format_login(cmd, gpio->password);
    if (send(gpio->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        LOG_ERROR("Channel %d: Failed to send LOGIN: %s", gpio->channel_id + 1, strerror(errno));
        return false;
    }
    LOG_INFO("Channel %d: Sent LOGIN", gpio->channel_id + 1);
    return true;
}

static bool
send_subscription(axia_gpio_t *gpio)
{
    char cmd[32];

    /* Subscribe to GPI (console → app button presses) */
    int len = snprintf(cmd, sizeof(cmd), "ADD GPI\n");
    if (send(gpio->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        LOG_ERROR("Channel %d: Failed to send ADD GPI: %s", gpio->channel_id + 1, strerror(errno));
        return false;
    }

    /* Subscribe to GPO (app → console LED feedback) */
    len = snprintf(cmd, sizeof(cmd), "ADD GPO\n");
    if (send(gpio->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        LOG_ERROR("Channel %d: Failed to send ADD GPO: %s", gpio->channel_id + 1, strerror(errno));
        return false;
    }

    LOG_DEBUG("Channel %d: Subscribed to GPIO events", gpio->channel_id + 1);
    return true;
}

static bool
read_and_dispatch_events(axia_gpio_t *gpio)
{
    char buffer[LWRP_MAX_LINE];
    static char line_buffer[LWRP_MAX_LINE] = { 0 };
    static int line_pos = 0;

    /* Read data */
    ssize_t n = recv(gpio->sockfd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        if (n == 0) {
            LOG_DEBUG("Channel %d: Connection closed by peer", gpio->channel_id + 1);
        } else {
            LOG_ERROR("Channel %d: recv error: %s", gpio->channel_id + 1, strerror(errno));
        }
        return false;
    }

    buffer[n] = '\0';

    /* Process line by line */
    for (ssize_t i = 0; i < n; i++) {
        char c = buffer[i];

        if (c == '\n' || c == '\r') {
            if (line_pos > 0) {
                line_buffer[line_pos] = '\0';

                /* Parse GPI message */
                int channel, pin, state;
                if (axia_protocol_parse_gpi(line_buffer, &channel, &pin, &state)) {
                    /* Convert LWRP channel (1-4) to internal (0-3) */
                    if (channel == gpio->channel_id + 1) {
                        dispatch_gpio_event(gpio, (axia_pin_t)pin, (axia_state_t)state);
                    }
                }

                line_pos = 0;
            }
        } else if (line_pos < LWRP_MAX_LINE - 1) {
            line_buffer[line_pos++] = c;
        }
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback Dispatch (marshaled to GTK main thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    axia_gpio_t *gpio;
    int channel_id;
    axia_pin_t pin;
    axia_state_t state;
} gpio_callback_data_t;

typedef struct {
    axia_gpio_t *gpio;
    int channel_id;
    bool connected;
} status_callback_data_t;

static gboolean
invoke_gpio_callback_on_main_thread(gpointer data)
{
    gpio_callback_data_t *cb_data = (gpio_callback_data_t *)data;

    g_mutex_lock(&cb_data->gpio->callback_mutex);
    if (cb_data->gpio->callback) {
        cb_data->gpio->callback(
            cb_data->channel_id, cb_data->pin, cb_data->state, cb_data->gpio->callback_user_data);
    }
    g_mutex_unlock(&cb_data->gpio->callback_mutex);

    g_free(cb_data);
    return G_SOURCE_REMOVE;
}

static gboolean
invoke_status_callback_on_main_thread(gpointer data)
{
    status_callback_data_t *cb_data = (status_callback_data_t *)data;

    g_mutex_lock(&cb_data->gpio->callback_mutex);
    if (cb_data->gpio->status_callback) {
        cb_data->gpio->status_callback(
            cb_data->channel_id, cb_data->connected, cb_data->gpio->status_callback_user_data);
    }
    g_mutex_unlock(&cb_data->gpio->callback_mutex);

    g_free(cb_data);
    return G_SOURCE_REMOVE;
}

static void
dispatch_gpio_event(axia_gpio_t *gpio, axia_pin_t pin, axia_state_t state)
{
    LOG_INFO("Channel %d: Received GPI pin=%d state=%s",
             gpio->channel_id + 1,
             pin,
             state == AXIA_STATE_HIGH ? "HIGH" : "LOW");

    gpio_callback_data_t *data = g_new0(gpio_callback_data_t, 1);
    data->gpio = gpio;
    data->channel_id = gpio->channel_id;
    data->pin = pin;
    data->state = state;

    g_main_context_invoke(NULL, invoke_gpio_callback_on_main_thread, data);
}

static void
dispatch_status_event(axia_gpio_t *gpio, bool connected)
{
    status_callback_data_t *data = g_new0(status_callback_data_t, 1);
    data->gpio = gpio;
    data->channel_id = gpio->channel_id;
    data->connected = connected;

    g_main_context_invoke(NULL, invoke_status_callback_on_main_thread, data);
}

static void
exponential_backoff_sleep(axia_gpio_t *gpio)
{
    if (!atomic_load(&gpio->running))
        return;

    LOG_DEBUG("Channel %d: Reconnecting in %dms", gpio->channel_id + 1, gpio->reconnect_delay_ms);

    /* Sleep in small chunks to allow quick exit */
    int remaining_ms = gpio->reconnect_delay_ms;
    while (remaining_ms > 0 && atomic_load(&gpio->running)) {
        int sleep_ms = (remaining_ms > 100) ? 100 : remaining_ms;
        usleep(sleep_ms * 1000);
        remaining_ms -= sleep_ms;
    }

    /* Increase delay for next time (exponential backoff) */
    gpio->reconnect_delay_ms *= RECONNECT_BACKOFF_FACTOR;
    if (gpio->reconnect_delay_ms > RECONNECT_MAX_DELAY_MS) {
        gpio->reconnect_delay_ms = RECONNECT_MAX_DELAY_MS;
    }
}
