/**
 * MusicBrainz tag detection and reading.
 *
 * Reads existing MUSICBRAINZ_* tags from audio files to determine
 * if they've already been tagged (e.g., from private trackers).
 *
 * Uses FFmpeg for reading metadata - more reliable across formats
 * than TagLib for read-only access.
 */

#include "internal.h"
#include <libavformat/avformat.h>
#include <string.h>

// =============================================================================
// FFmpeg Metadata Helpers
// =============================================================================

static char* get_metadata_tag(AVFormatContext* fmt_ctx, const char* key) {
    AVDictionaryEntry* tag = av_dict_get(fmt_ctx->metadata, key, NULL, 0);
    if (tag && tag->value) {
        return g_strdup(tag->value);
    }
    return NULL;
}

// Try multiple variations of a tag name (different containers use different conventions)
static char* get_mb_tag(AVFormatContext* fmt_ctx, const char* base_name) {
    char* value = NULL;

    // Try uppercase with underscore (FLAC, Vorbis)
    value = get_metadata_tag(fmt_ctx, base_name);
    if (value) return value;

    // Try with spaces instead of underscores
    char* spaced = g_strdup(base_name);
    for (char* p = spaced; *p; p++) {
        if (*p == '_') *p = ' ';
    }
    value = get_metadata_tag(fmt_ctx, spaced);
    g_free(spaced);
    if (value) return value;

    // Try mixed case (MusicBrainz Picard style)
    // e.g., "MUSICBRAINZ_TRACKID" -> "MusicBrainz Track Id"
    if (g_str_has_prefix(base_name, "MUSICBRAINZ_")) {
        const char* suffix = base_name + 12; // Skip "MUSICBRAINZ_"

        // Build mixed case version
        GString* mixed = g_string_new("MusicBrainz ");

        // Convert suffix like "TRACKID" to "Track Id"
        bool next_upper = true;
        for (const char* p = suffix; *p; p++) {
            if (next_upper) {
                g_string_append_c(mixed, g_ascii_toupper(*p));
                next_upper = false;
            } else if (*p == '_' || *p == ' ') {
                g_string_append_c(mixed, ' ');
                next_upper = true;
            } else {
                g_string_append_c(mixed, g_ascii_tolower(*p));
            }
        }

        value = get_metadata_tag(fmt_ctx, mixed->str);
        g_string_free(mixed, TRUE);
        if (value) return value;
    }

    return NULL;
}

// =============================================================================
// Public API
// =============================================================================

quadrature_result_t mb_tags_read(const char* audio_path, mb_tags_t* tags) {
    if (!audio_path || !tags) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(tags, 0, sizeof(mb_tags_t));

    AVFormatContext* fmt_ctx = NULL;

    if (avformat_open_input(&fmt_ctx, audio_path, NULL, NULL) < 0) {
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Read MusicBrainz tags
    tags->track_id = get_mb_tag(fmt_ctx, MB_TAG_TRACKID);
    tags->release_id = get_mb_tag(fmt_ctx, MB_TAG_ALBUMID);
    tags->artist_id = get_mb_tag(fmt_ctx, MB_TAG_ARTISTID);
    tags->album_artist_id = get_mb_tag(fmt_ctx, MB_TAG_ALBUMARTISTID);
    tags->release_group_id = get_mb_tag(fmt_ctx, MB_TAG_RELEASEGROUPID);
    tags->recording_id = get_mb_tag(fmt_ctx, MB_TAG_RELEASETRACKID);

    avformat_close_input(&fmt_ctx);

    return QUADRATURE_OK;
}

bool mb_tags_exist(const char* audio_path) {
    if (!audio_path) {
        return false;
    }

    mb_tags_t tags;
    if (mb_tags_read(audio_path, &tags) != QUADRATURE_OK) {
        return false;
    }

    // Check if we have either TRACKID or RELEASETRACKID
    bool has_tags = (tags.track_id != NULL) || (tags.recording_id != NULL);

    mb_tags_free(&tags);
    return has_tags;
}

void mb_tags_free(mb_tags_t* tags) {
    if (!tags) return;

    g_free(tags->track_id);
    g_free(tags->release_id);
    g_free(tags->artist_id);
    g_free(tags->album_artist_id);
    g_free(tags->release_group_id);
    g_free(tags->recording_id);

    memset(tags, 0, sizeof(mb_tags_t));
}
