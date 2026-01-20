#include <glib.h>
/**
 * Indexer utilities: audio file detection, metadata extraction.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

// =============================================================================
// Audio File Detection
// =============================================================================

const char* AUDIO_EXTENSIONS[] = {
    ".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aac",
    ".wav", ".aiff", ".wma", ".ape", ".wv", NULL
};

bool is_audio_file(const char* path) {
    if (!path) return false;

    const char* ext = strrchr(path, '.');
    if (!ext) return false;

    for (const char** e = AUDIO_EXTENSIONS; *e; e++) {
        if (strcasecmp(ext, *e) == 0) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Disc Folder Detection
// =============================================================================

// Word representations of numbers 1-10
static const char* DISC_WORDS[] = {
    "one", "two", "three", "four", "five",
    "six", "seven", "eight", "nine", "ten"
};

// Parse a number from string (1-99), returns 0 on failure
static uint16_t parse_disc_number(const char* str) {
    if (!str || !*str) return 0;

    // Skip leading whitespace/separators
    while (*str == ' ' || *str == '-' || *str == '_') str++;
    if (!*str) return 0;

    // Try numeric
    if (*str >= '1' && *str <= '9') {
        int num = atoi(str);
        if (num >= 1 && num <= 99) {
            return (uint16_t)num;
        }
    }

    // Try word representations (case-insensitive)
    for (int i = 0; i < 10; i++) {
        if (strncasecmp(str, DISC_WORDS[i], strlen(DISC_WORDS[i])) == 0) {
            return (uint16_t)(i + 1);
        }
    }

    return 0;
}

bool is_disc_folder(const char* dir_name) {
    return get_disc_number_from_folder(dir_name) > 0;
}

uint16_t get_disc_number_from_folder(const char* dir_name) {
    if (!dir_name || !*dir_name) return 0;

    // Pattern: CD[sep]N or cd[sep]N
    if (strncasecmp(dir_name, "cd", 2) == 0) {
        return parse_disc_number(dir_name + 2);
    }

    // Pattern: Disc[sep]N or disc[sep]N
    if (strncasecmp(dir_name, "disc", 4) == 0) {
        return parse_disc_number(dir_name + 4);
    }

    // Pattern: D[sep]N or d[sep]N (single letter, must be followed by number)
    if ((dir_name[0] == 'D' || dir_name[0] == 'd') &&
        (dir_name[1] >= '1' && dir_name[1] <= '9')) {
        return parse_disc_number(dir_name + 1);
    }

    return 0;
}

// =============================================================================
// Index Item
// =============================================================================

void index_item_free(index_item_t* item) {
    if (!item) return;
    free(item->path);
    free(item->title);
    free(item->artist);
    free(item->album);
    item->path = NULL;
    item->title = NULL;
    item->artist = NULL;
    item->album = NULL;
}

// =============================================================================
// Metadata Extraction
// =============================================================================

quadrature_result_t extract_audio_metadata(const char* path, index_item_t* out) {
    if (!path || !out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    out->path = strdup(path);
    out->title = NULL;
    out->artist = NULL;
    out->album = NULL;
    out->duration_ms = 0;
    out->track_num = 0;
    out->year = 0;

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) {
        g_debug("Failed to open audio file: %s", path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (avformat_find_stream_info(fmt, NULL) < 0) {
        g_debug("Failed to read stream info: %s", path);
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_INTERNAL;
    }

    if (fmt->duration != AV_NOPTS_VALUE) {
        out->duration_ms = (uint32_t)(fmt->duration / 1000);
    }

    AVDictionaryEntry* tag = NULL;
    while ((tag = av_dict_get(fmt->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (strcasecmp(tag->key, "title") == 0) {
            out->title = strdup(tag->value);
        } else if (strcasecmp(tag->key, "artist") == 0) {
            out->artist = strdup(tag->value);
        } else if (strcasecmp(tag->key, "album") == 0) {
            out->album = strdup(tag->value);
        } else if (strcasecmp(tag->key, "track") == 0) {
            out->track_num = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "date") == 0 || strcasecmp(tag->key, "year") == 0) {
            out->year = (uint16_t)atoi(tag->value);
        }
    }

    avformat_close_input(&fmt);

    // Fallback: use filename as title if no title tag
    if (!out->title) {
        const char* fname = strrchr(path, '/');
        fname = fname ? fname + 1 : path;
        char* copy = strdup(fname);
        char* dot = strrchr(copy, '.');
        if (dot) *dot = '\0';
        out->title = copy;
    }

    return QUADRATURE_OK;
}

// =============================================================================
// Extended Metadata Extraction
// =============================================================================

void extended_metadata_free(extended_metadata_t* meta) {
    if (!meta) return;
    free(meta->path);
    free(meta->title);
    free(meta->artist);
    free(meta->album);
    free(meta->album_artist);
    free(meta->genre);
    free(meta->comment);
    free(meta->encoder);
    free(meta->codec);
    free(meta->raw_json);
    memset(meta, 0, sizeof(*meta));
}

// Parse "N/M" format to get N and M
static void parse_track_disc_format(const char* value, uint16_t* num, uint16_t* total) {
    if (!value) return;

    *num = (uint16_t)atoi(value);

    const char* slash = strchr(value, '/');
    if (slash && total) {
        *total = (uint16_t)atoi(slash + 1);
    }
}

// Build JSON string from all metadata tags
static char* build_raw_json(AVFormatContext* fmt) {
    GString* json = g_string_new("{");
    bool first = true;

    AVDictionaryEntry* tag = NULL;
    while ((tag = av_dict_get(fmt->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (!first) g_string_append(json, ",");
        first = false;

        // Escape key and value for JSON
        char* escaped_key = g_strescape(tag->key, NULL);
        char* escaped_value = g_strescape(tag->value, NULL);

        g_string_append_printf(json, "\"%s\":\"%s\"", escaped_key, escaped_value);

        g_free(escaped_key);
        g_free(escaped_value);
    }

    g_string_append(json, "}");
    return g_string_free(json, FALSE);
}

// Get codec name from stream
static char* get_codec_name(AVFormatContext* fmt) {
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            const AVCodec* codec = avcodec_find_decoder(fmt->streams[i]->codecpar->codec_id);
            if (codec && codec->name) {
                // Convert to uppercase for display
                char* name = g_ascii_strup(codec->name, -1);
                return name;
            }
        }
    }
    return NULL;
}

// Check if file has embedded artwork
static bool has_embedded_artwork(AVFormatContext* fmt) {
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            return true;
        }
    }
    return false;
}

quadrature_result_t extract_extended_metadata(const char* path, extended_metadata_t* out) {
    if (!path || !out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(out, 0, sizeof(*out));
    out->path = strdup(path);
    out->disc_num = 1;  // Default to disc 1

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) {
        g_debug("Failed to open audio file: %s", path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    if (avformat_find_stream_info(fmt, NULL) < 0) {
        g_debug("Failed to read stream info: %s", path);
        avformat_close_input(&fmt);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Duration
    if (fmt->duration != AV_NOPTS_VALUE) {
        out->duration_ms = (uint32_t)(fmt->duration / 1000);
    }

    // Get audio stream info
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters* codecpar = fmt->streams[i]->codecpar;
        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            out->sample_rate = codecpar->sample_rate;
            out->channels = codecpar->ch_layout.nb_channels;
            out->bitrate = (int32_t)(codecpar->bit_rate / 1000);  // Convert to kbps
            if (out->bitrate == 0 && fmt->bit_rate > 0) {
                out->bitrate = (int32_t)(fmt->bit_rate / 1000);
            }
            break;
        }
    }

    // Codec name
    out->codec = get_codec_name(fmt);

    // Check for embedded art
    out->has_embedded_art = has_embedded_artwork(fmt);

    // Build raw JSON before processing tags
    out->raw_json = build_raw_json(fmt);

    // Extract metadata tags
    AVDictionaryEntry* tag = NULL;
    while ((tag = av_dict_get(fmt->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (strcasecmp(tag->key, "title") == 0) {
            free(out->title);
            out->title = strdup(tag->value);
        } else if (strcasecmp(tag->key, "artist") == 0) {
            free(out->artist);
            out->artist = strdup(tag->value);
        } else if (strcasecmp(tag->key, "album") == 0) {
            free(out->album);
            out->album = strdup(tag->value);
        } else if (strcasecmp(tag->key, "albumartist") == 0 ||
                   strcasecmp(tag->key, "album_artist") == 0 ||
                   strcasecmp(tag->key, "album artist") == 0) {
            free(out->album_artist);
            out->album_artist = strdup(tag->value);
        } else if (strcasecmp(tag->key, "track") == 0) {
            parse_track_disc_format(tag->value, &out->track_num, &out->track_total);
        } else if (strcasecmp(tag->key, "disc") == 0 ||
                   strcasecmp(tag->key, "discnumber") == 0) {
            parse_track_disc_format(tag->value, &out->disc_num, &out->disc_total);
        } else if (strcasecmp(tag->key, "date") == 0 ||
                   strcasecmp(tag->key, "year") == 0) {
            out->year = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "genre") == 0) {
            free(out->genre);
            out->genre = strdup(tag->value);
        } else if (strcasecmp(tag->key, "comment") == 0) {
            free(out->comment);
            out->comment = strdup(tag->value);
        } else if (strcasecmp(tag->key, "encoder") == 0 ||
                   strcasecmp(tag->key, "encoded_by") == 0) {
            free(out->encoder);
            out->encoder = strdup(tag->value);
        } else if (strcasecmp(tag->key, "compilation") == 0 ||
                   strcasecmp(tag->key, "tcmp") == 0) {
            // iTunes compilation flag
            out->compilation = (strcmp(tag->value, "1") == 0 ||
                               strcasecmp(tag->value, "true") == 0 ||
                               strcasecmp(tag->value, "yes") == 0);
        }
    }

    avformat_close_input(&fmt);

    // Fallback: use filename as title if no title tag
    if (!out->title) {
        const char* fname = strrchr(path, '/');
        fname = fname ? fname + 1 : path;
        char* copy = strdup(fname);
        char* dot = strrchr(copy, '.');
        if (dot) *dot = '\0';
        out->title = copy;
    }

    // Ensure disc_num is at least 1
    if (out->disc_num == 0) out->disc_num = 1;

    return QUADRATURE_OK;
}

size_t scan_audio_files_in_dir(const char* dir, char*** files_out, struct stat** stats_out) {
    if (!dir) return 0;

    DIR* d = opendir(dir);
    if (!d) return 0;

    size_t cap = 64;
    size_t count = 0;
    char** files = NULL;
    struct stat* stats = NULL;

    if (files_out) {
        files = malloc(cap * sizeof(char*));
        if (!files) {
            closedir(d);
            return 0;
        }
    }

    if (stats_out) {
        stats = malloc(cap * sizeof(struct stat));
        if (!stats) {
            free(files);
            closedir(d);
            return 0;
        }
    }

    char path_buf[INDEXER_PATH_MAX];
    struct dirent* ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        // Use d_type to skip non-regular files without stat()
        if (ent->d_type != DT_REG) continue;

        if (!is_audio_file(ent->d_name)) continue;

        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir, ent->d_name);

        // Only stat for mtime/size (required for output), file type already confirmed
        struct stat st;
        if (stats_out && stat(path_buf, &st) != 0) continue;

        if (files_out || stats_out) {
            if (count >= cap) {
                cap *= 2;
                if (files) {
                    char** new_files = realloc(files, cap * sizeof(char*));
                    if (!new_files) {
                        for (size_t i = 0; i < count; i++) free(files[i]);
                        free(files);
                        free(stats);
                        closedir(d);
                        return 0;
                    }
                    files = new_files;
                }
                if (stats) {
                    struct stat* new_stats = realloc(stats, cap * sizeof(struct stat));
                    if (!new_stats) {
                        for (size_t i = 0; i < count; i++) free(files[i]);
                        free(files);
                        free(stats);
                        closedir(d);
                        return 0;
                    }
                    stats = new_stats;
                }
            }
        }

        if (files) files[count] = strdup(path_buf);
        if (stats) stats[count] = st;
        count++;
    }

    closedir(d);

    if (files_out) *files_out = files;
    if (stats_out) *stats_out = stats;

    return count;
}
