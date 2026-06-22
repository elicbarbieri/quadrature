/**
 * Axia Livewire+ Controller Backend
 *
 * A control source backed by an Axia console over the Livewire Routing Protocol
 * (LWRP) on a TCP socket. One axia_ctx_t == one channel on one console.
 *
 * Mapping (this is the Axia hardware convention, kept out of the app):
 *   Inbound  GPI pin 1 rising edge -> CONTROL_CMD_PLAY_PAUSE
 *            GPI pin 2 rising edge -> CONTROL_CMD_PREVIEW_TOGGLE
 *   Outbound channel state -> GPO pin 1 (ON-AIR LED) + pin 2 (PREVIEW LED):
 *            IDLE/QUEUED  -> both LOW
 *            PREVIEW      -> preview HIGH, on-air LOW
 *            ON_AIR       -> on-air HIGH, preview LOW
 *
 * The pure LWRP wire parsing/formatting helpers live at the bottom of this file
 * (no I/O; unit-tested directly). The generic listener thread, reconnect/backoff
 * and callback marshaling live in controller.c and are reused.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* Pin indices (0-based) for this console convention: pin 1 = on-air, pin 2 = preview. */
#define AXIA_IDX_ON_AIR  0
#define AXIA_IDX_PREVIEW 1

/* Driver vtable entries */
static bool axia_connect(void *ctx);
static bool axia_pump(void *ctx, controller_emit_fn emit, controller_t *core);
static bool axia_set_state(void *ctx, int channel, control_state_t state);
static void axia_wake(void *ctx);
static void axia_disconnect(void *ctx);
static void axia_destroy(void *ctx);

static const controller_driver_t AXIA_DRIVER = {
    .connect = axia_connect,
    .pump = axia_pump,
    .set_state = axia_set_state,
    .wake = axia_wake,
    .disconnect = axia_disconnect,
    .destroy = axia_destroy,
};

/* Internal helpers */
static bool tcp_connect(axia_ctx_t *ax);
static bool authenticate(axia_ctx_t *ax);
static bool subscribe(axia_ctx_t *ax);

/* ═══════════════════════════════════════════════════════════════════════════
 * Constructor
 * ═══════════════════════════════════════════════════════════════════════════ */

quadrature_result_t
controller_axia_create(const char *address, int channel, const char *password, controller_t **out)
{
    if (!address || channel < 0 || channel >= 4 || !out)
        return QUADRATURE_ERROR_INVALID_PARAM;

    /* Parse address: "ip:port" or "ip" (default LWRP port). */
    char *ip = NULL;
    uint16_t port = LWRP_PORT;

    const char *colon = strchr(address, ':');
    if (colon) {
        ip = g_strndup(address, colon - address);
        long parsed_port = strtol(colon + 1, NULL, 10);
        if (parsed_port <= 0 || parsed_port > 65535) {
            g_free(ip);
            return QUADRATURE_ERROR_INVALID_PARAM;
        }
        port = (uint16_t)parsed_port;
    } else {
        ip = g_strdup(address);
    }

    if (!ip || ip[0] == '\0') {
        g_free(ip);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    axia_ctx_t *ax = g_new0(axia_ctx_t, 1);
    ax->console_ip = ip;
    ax->port = port;
    ax->channel = channel;
    ax->password = password ? g_strdup(password) : NULL;
    ax->sockfd = -1;

    controller_t *c = controller_core_new(&AXIA_DRIVER, ax, channel);
    if (!c) {
        axia_destroy(ax);
        return QUADRATURE_ERROR_INTERNAL;
    }

    *out = c;
    g_debug(
        "Axia: created controller for channel %d (%s:%u)", channel + 1, ax->console_ip, ax->port);
    return QUADRATURE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Driver: connect (TCP + auth + subscribe)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool
axia_connect(void *ctx)
{
    axia_ctx_t *ax = (axia_ctx_t *)ctx;

    /* Reset partial-line state for the fresh session. */
    ax->line_pos = 0;

    if (!tcp_connect(ax))
        return false;

    if (ax->password && !authenticate(ax)) {
        axia_disconnect(ax);
        return false;
    }

    if (!subscribe(ax)) {
        axia_disconnect(ax);
        return false;
    }

    return true;
}

static bool
tcp_connect(axia_ctx_t *ax)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        g_warning("Axia: channel %d failed to create socket: %s", ax->channel + 1, strerror(errno));
        return false;
    }

    /* Non-blocking connect for a bounded timeout. */
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ax->port);

    if (inet_pton(AF_INET, ax->console_ip, &addr.sin_addr) <= 0) {
        g_warning("Axia: channel %d invalid IP address: %s", ax->channel + 1, ax->console_ip);
        close(sockfd);
        return false;
    }

    int result = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        g_debug("Axia: channel %d connect failed: %s", ax->channel + 1, strerror(errno));
        close(sockfd);
        return false;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);

    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
    if (result <= 0) {
        g_debug("Axia: channel %d connect timeout", ax->channel + 1);
        close(sockfd);
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        g_debug("Axia: channel %d connect error: %s", ax->channel + 1, strerror(error));
        close(sockfd);
        return false;
    }

    /* Back to blocking mode for the read loop. */
    fcntl(sockfd, F_SETFL, flags);

    ax->sockfd = sockfd;
    g_debug("Axia: channel %d TCP connected to %s:%u", ax->channel + 1, ax->console_ip, ax->port);
    return true;
}

static bool
authenticate(axia_ctx_t *ax)
{
    /* LWRP auth: "LOGIN <password>\n" — no response on success. */
    char cmd[80];
    int len = axia_protocol_format_login(cmd, sizeof(cmd), ax->password);
    if (send(ax->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        g_warning("Axia: channel %d failed to send LOGIN: %s", ax->channel + 1, strerror(errno));
        return false;
    }
    g_info("Axia: channel %d sent LOGIN", ax->channel + 1);
    return true;
}

static bool
subscribe(axia_ctx_t *ax)
{
    char cmd[32];

    /* Subscribe to GPI (console -> app button presses). */
    int len = snprintf(cmd, sizeof(cmd), "ADD GPI\n");
    if (send(ax->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        g_warning("Axia: channel %d failed to send ADD GPI: %s", ax->channel + 1, strerror(errno));
        return false;
    }

    /* Subscribe to GPO (app -> console LED feedback). */
    len = snprintf(cmd, sizeof(cmd), "ADD GPO\n");
    if (send(ax->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        g_warning("Axia: channel %d failed to send ADD GPO: %s", ax->channel + 1, strerror(errno));
        return false;
    }

    g_debug("Axia: channel %d subscribed to GPIO events", ax->channel + 1);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Driver: pump (read GPI -> decode -> emit command)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool
axia_pump(void *ctx, controller_emit_fn emit, controller_t *core)
{
    axia_ctx_t *ax = (axia_ctx_t *)ctx;
    char buffer[LWRP_MAX_LINE];

    ssize_t n = recv(ax->sockfd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        if (n == 0) {
            g_debug("Axia: channel %d connection closed by peer", ax->channel + 1);
        } else {
            g_warning("Axia: channel %d recv error: %s", ax->channel + 1, strerror(errno));
        }
        return false;
    }

    /* Reassemble lines across recv() boundaries, decode each. */
    for (ssize_t i = 0; i < n; i++) {
        char ch = buffer[i];

        if (ch == '\n' || ch == '\r') {
            if (ax->line_pos > 0) {
                ax->line_buffer[ax->line_pos] = '\0';

                axia_gpio_line_t gpi;
                if (axia_protocol_parse_gpi(ax->line_buffer, &gpi)
                    && gpi.port == ax->channel + 1) {
                    /* Emit on the active-going edge (pin pulled low), not on
                     * release — active-low, uppercase = the pin that changed. */
                    if (gpi.changed[AXIA_IDX_ON_AIR] && gpi.active[AXIA_IDX_ON_AIR])
                        emit(core, ax->channel, CONTROL_CMD_PLAY_PAUSE);
                    if (gpi.changed[AXIA_IDX_PREVIEW] && gpi.active[AXIA_IDX_PREVIEW])
                        emit(core, ax->channel, CONTROL_CMD_PREVIEW_TOGGLE);
                }

                ax->line_pos = 0;
            }
        } else if (ax->line_pos < LWRP_MAX_LINE - 1) {
            ax->line_buffer[ax->line_pos++] = ch;
        }
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Driver: set_state (channel state -> GPO LED feedback)
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool
axia_set_state(void *ctx, int channel, control_state_t state)
{
    axia_ctx_t *ax = (axia_ctx_t *)ctx;
    (void)channel; /* Each Axia controller is bound to its own channel. */

    bool on_air = (state == CONTROL_STATE_ON_AIR);
    bool preview = (state == CONTROL_STATE_PREVIEW);

    /* Drive only the pins we own; leave the rest of the port untouched.
     * Active-low: a lit indicator is driven LOW. */
    axia_gpo_pin_t pins[AXIA_GPIO_PINS];
    for (int i = 0; i < AXIA_GPIO_PINS; i++)
        pins[i] = AXIA_GPO_UNCHANGED;
    pins[AXIA_IDX_ON_AIR] = on_air ? AXIA_GPO_LOW : AXIA_GPO_HIGH;
    pins[AXIA_IDX_PREVIEW] = preview ? AXIA_GPO_LOW : AXIA_GPO_HIGH;

    /* Internal channel id (0-based) -> LWRP port (1-based). */
    char cmd[32];
    int len = axia_protocol_format_gpo(cmd, sizeof(cmd), ax->channel + 1, pins);
    if (len <= 0) {
        g_warning("Axia: channel %d failed to format GPO command", ax->channel + 1);
        return false;
    }

    if (send(ax->sockfd, cmd, len, MSG_NOSIGNAL) < 0) {
        g_warning(
            "Axia: channel %d failed to send GPO command: %s", ax->channel + 1, strerror(errno));
        return false;
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Driver: wake / disconnect / destroy
 * ═══════════════════════════════════════════════════════════════════════════ */

static void
axia_wake(void *ctx)
{
    axia_ctx_t *ax = (axia_ctx_t *)ctx;
    /* Unblock a recv() in axia_pump(); the listener then sees running == false. */
    if (ax->sockfd >= 0)
        shutdown(ax->sockfd, SHUT_RDWR);
}

static void
axia_disconnect(void *ctx)
{
    axia_ctx_t *ax = (axia_ctx_t *)ctx;
    if (ax->sockfd >= 0) {
        close(ax->sockfd);
        ax->sockfd = -1;
    }
}

static void
axia_destroy(void *ctx)
{
    axia_ctx_t *ax = (axia_ctx_t *)ctx;
    if (!ax)
        return;
    g_free(ax->console_ip);
    g_free(ax->password);
    g_free(ax);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LWRP wire format (pure helpers — no I/O, unit-tested in test_axia_protocol.c)
 *
 * A GPI/GPO port has exactly AXIA_GPIO_PINS pins; a state line is
 * "GPI <port> <5 chars>" / "GPO <port> <5 chars>", one char per pin (pin 1
 * first). GPIO is active-LOW. In notifications each char is one of:
 *   'H' high (off), changing   'h' high (off), steady
 *   'L' low  (on),  changing   'l' low  (on),  steady
 * When SETTING a GPO, 'x' additionally means "leave this pin unchanged".
 * ═══════════════════════════════════════════════════════════════════════════ */

bool
axia_protocol_parse_gpi(const char *line, axia_gpio_line_t *out)
{
    if (!line || !out)
        return false;

    /* Leading whitespace */
    while (isspace((unsigned char)*line))
        line++;

    /* "GPI " prefix */
    if (strncmp(line, "GPI ", 4) != 0)
        return false;
    line += 4;
    while (*line == ' ')
        line++;

    /* Port number (1-based; not limited to the app's channel range) */
    char *endptr;
    long port = strtol(line, &endptr, 10);
    if (endptr == line || port < 1)
        return false;
    line = endptr;

    while (*line == ' ')
        line++;

    /* Exactly AXIA_GPIO_PINS pin characters. */
    axia_gpio_line_t result;
    memset(&result, 0, sizeof(result));
    result.port = (int)port;

    for (int i = 0; i < AXIA_GPIO_PINS; i++) {
        switch (line[i]) {
        case 'H':
            result.active[i] = false;
            result.changed[i] = true;
            break;
        case 'h':
            result.active[i] = false;
            result.changed[i] = false;
            break;
        case 'L':
            result.active[i] = true;
            result.changed[i] = true;
            break;
        case 'l':
            result.active[i] = true;
            result.changed[i] = false;
            break;
        default:
            /* Invalid char, or '\0' (too few pins) */
            return false;
        }
    }

    /* Nothing but trailing whitespace may follow the 5 pin chars. */
    const char *tail = line + AXIA_GPIO_PINS;
    while (*tail) {
        if (!isspace((unsigned char)*tail))
            return false;
        tail++;
    }

    *out = result;
    return true;
}

int
axia_protocol_format_gpo(char *buffer,
                         size_t buflen,
                         int port,
                         const axia_gpo_pin_t pins[AXIA_GPIO_PINS])
{
    if (!buffer || !pins || port < 1)
        return 0;

    char pinstr[AXIA_GPIO_PINS + 1];
    for (int i = 0; i < AXIA_GPIO_PINS; i++) {
        switch (pins[i]) {
        case AXIA_GPO_UNCHANGED:
            pinstr[i] = 'x';
            break;
        case AXIA_GPO_LOW:
            pinstr[i] = 'l';
            break;
        case AXIA_GPO_HIGH:
            pinstr[i] = 'h';
            break;
        default:
            return 0;
        }
    }
    pinstr[AXIA_GPIO_PINS] = '\0';

    int n = snprintf(buffer, buflen, "GPO %d %s\n", port, pinstr);
    if (n < 0 || (size_t)n >= buflen)
        return 0; /* truncated */
    return n;
}

int
axia_protocol_format_login(char *buffer, size_t buflen, const char *password)
{
    if (!buffer || !password)
        return 0;

    int n = snprintf(buffer, buflen, "LOGIN %s\n", password);
    if (n < 0 || (size_t)n >= buflen)
        return 0; /* truncated */
    return n;
}
