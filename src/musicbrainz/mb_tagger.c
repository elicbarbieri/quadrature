/**
 * Tag writer using TagLib.
 *
 * Writes MusicBrainz metadata tags to audio files. Uses TagLib for writing
 * since it handles format-specific tag writing better than FFmpeg.
 */

#include "internal.h"
#include <taglib/tag_c.h>
#include <string.h>

// =============================================================================
// Tag Writing
// =============================================================================

quadrature_result_t mb_write_tags(const char* audio_path,
                                   const mb_release_t* release,
                                   const mb_recording_t* recording,
                                   bool dry_run) {
    if (!audio_path || !release || !recording) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (dry_run) {
        g_debug("DRY RUN: Would write tags to %s", audio_path);
        g_debug("  MUSICBRAINZ_TRACKID: %s", recording->id ? recording->id : "(none)");
        g_debug("  MUSICBRAINZ_ALBUMID: %s", release->id ? release->id : "(none)");
        return QUADRATURE_OK;
    }

    // Open file with TagLib
    TagLib_File* file = taglib_file_new(audio_path);
    if (!file) {
        g_warning("Failed to open file for tagging: %s", audio_path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (!taglib_file_is_valid(file)) {
        g_warning("Invalid audio file: %s", audio_path);
        taglib_file_free(file);
        return QUADRATURE_ERROR_UNSUPPORTED_FORMAT;
    }

    // Write MusicBrainz IDs using property interface
    // Note: TagLib C API uses taglib_property_set for custom tags

    if (recording->id) {
        taglib_property_set(file, MB_TAG_TRACKID, recording->id);
    }

    if (release->id) {
        taglib_property_set(file, MB_TAG_ALBUMID, release->id);
    }

    if (release->release_group_id) {
        taglib_property_set(file, MB_TAG_RELEASEGROUPID, release->release_group_id);
    }

    // Write artist IDs
    if (release->artists && release->artist_count > 0 && release->artists[0].id) {
        taglib_property_set(file, MB_TAG_ALBUMARTISTID, release->artists[0].id);
    }

    if (recording->artists && recording->artist_count > 0 && recording->artists[0].id) {
        taglib_property_set(file, MB_TAG_ARTISTID, recording->artists[0].id);
    }

    // Also update standard tags if we have the data
    TagLib_Tag* tag = taglib_file_tag(file);
    if (tag) {
        // Only update if we have valid data
        if (recording->title) {
            taglib_tag_set_title(tag, recording->title);
        }

        if (release->title) {
            taglib_tag_set_album(tag, release->title);
        }

        if (recording->artists && recording->artist_count > 0 && recording->artists[0].name) {
            taglib_tag_set_artist(tag, recording->artists[0].name);
        }

        if (recording->position > 0) {
            taglib_tag_set_track(tag, recording->position);
        }

        // Parse year from date
        if (release->date && strlen(release->date) >= 4) {
            int year = atoi(release->date);
            if (year > 0) {
                taglib_tag_set_year(tag, year);
            }
        }

        // Set genre if available
        if (release->genres && release->genre_count > 0 && release->genres[0]) {
            taglib_tag_set_genre(tag, release->genres[0]);
        }
    }

    // Save changes
    if (!taglib_file_save(file)) {
        g_warning("Failed to save tags to: %s", audio_path);
        taglib_file_free(file);
        return QUADRATURE_ERROR_INTERNAL;
    }

    taglib_file_free(file);

    g_debug("Wrote MusicBrainz tags to: %s", audio_path);
    return QUADRATURE_OK;
}

quadrature_result_t mb_embed_artwork(const char* audio_path,
                                      const uint8_t* image_data,
                                      size_t image_size,
                                      bool dry_run) {
    if (!audio_path || !image_data || image_size == 0) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    if (dry_run) {
        g_debug("DRY RUN: Would embed %zu bytes of artwork in %s", image_size, audio_path);
        return QUADRATURE_OK;
    }

    // Open file with TagLib
    TagLib_File* file = taglib_file_new(audio_path);
    if (!file) {
        g_warning("Failed to open file for artwork embedding: %s", audio_path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (!taglib_file_is_valid(file)) {
        g_warning("Invalid audio file for artwork: %s", audio_path);
        taglib_file_free(file);
        return QUADRATURE_ERROR_UNSUPPORTED_FORMAT;
    }

    // Set cover art using complex properties
    // Create picture data structure
    TagLib_Complex_Property_Picture_Data picture = {
        .data = (char*)image_data,
        .size = (unsigned int)image_size,
        .mimeType = "image/jpeg",
        .pictureType = "Front Cover",
        .description = "Album cover"
    };

    if (!taglib_complex_property_set_picture(file, "PICTURE", &picture)) {
        // Try alternative property name for different formats
        if (!taglib_complex_property_set_picture(file, "METADATA_BLOCK_PICTURE", &picture)) {
            g_warning("Failed to embed artwork in: %s", audio_path);
            taglib_file_free(file);
            return QUADRATURE_ERROR_INTERNAL;
        }
    }

    // Save changes
    if (!taglib_file_save(file)) {
        g_warning("Failed to save artwork to: %s", audio_path);
        taglib_file_free(file);
        return QUADRATURE_ERROR_INTERNAL;
    }

    taglib_file_free(file);

    g_debug("Embedded artwork in: %s", audio_path);
    return QUADRATURE_OK;
}
