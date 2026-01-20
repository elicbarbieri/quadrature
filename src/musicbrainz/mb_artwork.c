/**
 * Cover Art Archive client.
 *
 * Downloads album artwork from the Cover Art Archive using MusicBrainz release IDs.
 */

#include "internal.h"
#include <glib/gstdio.h>
#include <string.h>

// =============================================================================
// SHA-256 Hashing
// =============================================================================

static char* compute_sha256(const uint8_t* data, size_t size) {
    GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, data, size);

    const char* hex = g_checksum_get_string(checksum);
    char* result = g_strdup_printf("sha256:%s", hex);

    g_checksum_free(checksum);
    return result;
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_artwork_download(mb_http_client_t* client,
                                         const char* release_id,
                                         mb_artwork_t* artwork) {
    if (!client || !release_id || !artwork) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(artwork, 0, sizeof(mb_artwork_t));

    // Try to get front cover from Cover Art Archive
    // First try the 500px version, then fall back to original
    char* url = g_strdup_printf("%s/release/%s/front-500", COVERART_API_URL, release_id);

    uint8_t* data = NULL;
    size_t data_len = 0;

    quadrature_result_t result = mb_http_download(client, url, MB_RATE_COVERART,
                                                   &data, &data_len);
    g_free(url);

    // If 500px version not available, try original
    if (result == QUADRATURE_ERROR_FILE_NOT_FOUND) {
        url = g_strdup_printf("%s/release/%s/front", COVERART_API_URL, release_id);
        result = mb_http_download(client, url, MB_RATE_COVERART, &data, &data_len);
        g_free(url);
    }

    if (result != QUADRATURE_OK) {
        return result;
    }

    if (!data || data_len == 0) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    artwork->data = data;
    artwork->size = data_len;
    artwork->hash = compute_sha256(data, data_len);

    return QUADRATURE_OK;
}

quadrature_result_t mb_artwork_write(const char* album_path,
                                      const mb_artwork_t* artwork,
                                      bool dry_run) {
    if (!album_path || !artwork || !artwork->data) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    char* cover_path = g_build_filename(album_path, "cover.jpg", NULL);

    if (dry_run) {
        g_debug("DRY RUN: Would write %zu bytes to %s", artwork->size, cover_path);
        g_free(cover_path);
        return QUADRATURE_OK;
    }

    // Write to temp file first, then rename (atomic)
    char* temp_path = g_strdup_printf("%s.tmp", cover_path);

    GError* error = NULL;
    if (!g_file_set_contents(temp_path, (const char*)artwork->data, artwork->size, &error)) {
        g_warning("Failed to write artwork to %s: %s", temp_path, error->message);
        g_error_free(error);
        g_free(temp_path);
        g_free(cover_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Rename temp to final
    if (g_rename(temp_path, cover_path) != 0) {
        g_warning("Failed to rename artwork file: %s -> %s", temp_path, cover_path);
        g_unlink(temp_path);
        g_free(temp_path);
        g_free(cover_path);
        return QUADRATURE_ERROR_INTERNAL;
    }

    g_debug("Wrote artwork to: %s", cover_path);

    g_free(temp_path);
    g_free(cover_path);

    return QUADRATURE_OK;
}

void mb_artwork_free(mb_artwork_t* artwork) {
    if (!artwork) return;

    g_free(artwork->data);
    g_free(artwork->hash);

    memset(artwork, 0, sizeof(mb_artwork_t));
}
