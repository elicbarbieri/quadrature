/**
 * HTTP client with rate limiting for MusicBrainz/AcoustID APIs.
 *
 * Implements token bucket rate limiting per service:
 * - AcoustID: 3 requests/second
 * - MusicBrainz: 1 request/second
 * - Cover Art Archive: 1 request/second
 */

#include "internal.h"
#include <libsoup/soup.h>
#include <string.h>

// =============================================================================
// Rate Limiter
// =============================================================================

typedef struct {
    double tokens;           // Current token count
    double max_tokens;       // Maximum tokens (burst capacity)
    double refill_rate;      // Tokens per second
    int64_t last_refill;     // Last refill time (monotonic microseconds)
    GMutex mutex;
} rate_limiter_t;

static void rate_limiter_init(rate_limiter_t* rl, double rate) {
    rl->tokens = rate;       // Start with full bucket
    rl->max_tokens = rate;   // Burst capacity = 1 second worth
    rl->refill_rate = rate;
    rl->last_refill = g_get_monotonic_time();
    g_mutex_init(&rl->mutex);
}

static void rate_limiter_destroy(rate_limiter_t* rl) {
    g_mutex_clear(&rl->mutex);
}

static void rate_limiter_wait(rate_limiter_t* rl) {
    g_mutex_lock(&rl->mutex);

    // Refill tokens based on elapsed time
    int64_t now = g_get_monotonic_time();
    double elapsed_sec = (now - rl->last_refill) / 1000000.0;
    rl->tokens += elapsed_sec * rl->refill_rate;
    if (rl->tokens > rl->max_tokens) {
        rl->tokens = rl->max_tokens;
    }
    rl->last_refill = now;

    // Wait if we don't have a token
    if (rl->tokens < 1.0) {
        double wait_sec = (1.0 - rl->tokens) / rl->refill_rate;
        g_mutex_unlock(&rl->mutex);
        g_usleep((gulong)(wait_sec * 1000000));
        g_mutex_lock(&rl->mutex);

        // Refill after wait
        now = g_get_monotonic_time();
        elapsed_sec = (now - rl->last_refill) / 1000000.0;
        rl->tokens += elapsed_sec * rl->refill_rate;
        if (rl->tokens > rl->max_tokens) {
            rl->tokens = rl->max_tokens;
        }
        rl->last_refill = now;
    }

    // Consume one token
    rl->tokens -= 1.0;

    g_mutex_unlock(&rl->mutex);
}

// =============================================================================
// HTTP Client
// =============================================================================

struct mb_http_client {
    SoupSession* session;
    rate_limiter_t rate_acoustid;
    rate_limiter_t rate_musicbrainz;
    rate_limiter_t rate_coverart;
};

quadrature_result_t mb_http_client_create(mb_http_client_t** out) {
    if (!out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    mb_http_client_t* client = g_new0(mb_http_client_t, 1);

    // Create libsoup session with proper user agent
    client->session = soup_session_new_with_options(
        "user-agent", MB_USER_AGENT,
        "timeout", 30,
        NULL
    );

    // Initialize rate limiters
    rate_limiter_init(&client->rate_acoustid, ACOUSTID_RATE_LIMIT);
    rate_limiter_init(&client->rate_musicbrainz, MUSICBRAINZ_RATE_LIMIT);
    rate_limiter_init(&client->rate_coverart, COVERART_RATE_LIMIT);

    *out = client;
    return QUADRATURE_OK;
}

void mb_http_client_destroy(mb_http_client_t* client) {
    if (!client) return;

    if (client->session) {
        g_object_unref(client->session);
    }

    rate_limiter_destroy(&client->rate_acoustid);
    rate_limiter_destroy(&client->rate_musicbrainz);
    rate_limiter_destroy(&client->rate_coverart);

    g_free(client);
}

static rate_limiter_t* get_rate_limiter(mb_http_client_t* client, mb_rate_type_t type) {
    switch (type) {
        case MB_RATE_ACOUSTID:
            return &client->rate_acoustid;
        case MB_RATE_MUSICBRAINZ:
            return &client->rate_musicbrainz;
        case MB_RATE_COVERART:
            return &client->rate_coverart;
        default:
            return &client->rate_musicbrainz;
    }
}

quadrature_result_t mb_http_get(mb_http_client_t* client,
                                 const char* url,
                                 mb_rate_type_t rate_type,
                                 char** response_out,
                                 size_t* response_len_out) {
    if (!client || !url || !response_out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Apply rate limiting
    rate_limiter_t* rl = get_rate_limiter(client, rate_type);
    rate_limiter_wait(rl);

    // Create request
    SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url);
    if (!msg) {
        g_warning("Failed to create request for URL: %s", url);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Add Accept header for JSON
    SoupMessageHeaders* headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "Accept", "application/json");

    // Send request synchronously
    GError* error = NULL;
    GBytes* body = soup_session_send_and_read(client->session, msg, NULL, &error);

    if (error) {
        g_warning("HTTP request failed for %s: %s", url, error->message);
        g_error_free(error);
        g_object_unref(msg);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Check status code
    guint status = soup_message_get_status(msg);
    if (status != SOUP_STATUS_OK) {
        g_warning("HTTP %u for %s", status, url);
        g_bytes_unref(body);
        g_object_unref(msg);

        if (status == SOUP_STATUS_NOT_FOUND) {
            return QUADRATURE_ERROR_FILE_NOT_FOUND;
        }
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Extract response body
    gsize size = 0;
    const char* data = g_bytes_get_data(body, &size);

    *response_out = g_strndup(data, size);
    if (response_len_out) {
        *response_len_out = size;
    }

    g_bytes_unref(body);
    g_object_unref(msg);

    return QUADRATURE_OK;
}

quadrature_result_t mb_http_download(mb_http_client_t* client,
                                      const char* url,
                                      mb_rate_type_t rate_type,
                                      uint8_t** data_out,
                                      size_t* data_len_out) {
    if (!client || !url || !data_out || !data_len_out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Apply rate limiting
    rate_limiter_t* rl = get_rate_limiter(client, rate_type);
    rate_limiter_wait(rl);

    // Create request
    SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url);
    if (!msg) {
        g_warning("Failed to create download request for URL: %s", url);
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Send request synchronously
    GError* error = NULL;
    GBytes* body = soup_session_send_and_read(client->session, msg, NULL, &error);

    if (error) {
        g_warning("HTTP download failed for %s: %s", url, error->message);
        g_error_free(error);
        g_object_unref(msg);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Check status code
    guint status = soup_message_get_status(msg);
    if (status != SOUP_STATUS_OK) {
        g_warning("HTTP %u for download %s", status, url);
        g_bytes_unref(body);
        g_object_unref(msg);

        if (status == SOUP_STATUS_NOT_FOUND) {
            return QUADRATURE_ERROR_FILE_NOT_FOUND;
        }
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Extract response body
    gsize size = 0;
    const void* data = g_bytes_get_data(body, &size);

    *data_out = g_memdup2(data, size);
    *data_len_out = size;

    g_bytes_unref(body);
    g_object_unref(msg);

    return QUADRATURE_OK;
}
