/**
 * Indexer utilities: audio file detection, metadata extraction.
 */

#define G_LOG_DOMAIN "quadrature"

#include <glib.h>

#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include <libavformat/avformat.h>

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
    free(item->album_artist);
    free(item->album);
    free(item->genre);
    free(item->mb_release_id);
    item->path = NULL;
    item->title = NULL;
    item->artist = NULL;
    item->album_artist = NULL;
    item->album = NULL;
    item->genre = NULL;
    item->mb_release_id = NULL;
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
    out->album_artist = NULL;
    out->album = NULL;
    out->genre = NULL;
    out->mb_release_id = NULL;
    out->duration_ms = 0;
    out->track_num = 0;
    out->disc_num = 0;
    out->year = 0;

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) {
        g_debug("Failed to open audio file: %s", path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    // Reduce probe limits — sufficient for tags + basic stream info
    fmt->max_analyze_duration = 500000;  // 0.5s (default: 5s)
    fmt->probesize = 512 * 1024;         // 512KB (default: 5MB)

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
        } else if (strcasecmp(tag->key, "disc") == 0 ||
                   strcasecmp(tag->key, "discnumber") == 0) {
            out->disc_num = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "date") == 0 || strcasecmp(tag->key, "year") == 0) {
            out->year = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "album_artist") == 0 ||
                   strcasecmp(tag->key, "albumartist") == 0) {
            free(out->album_artist);
            out->album_artist = strdup(tag->value);
        } else if (strcasecmp(tag->key, "genre") == 0) {
            free(out->genre);
            out->genre = strdup(tag->value);
            for (char* p = out->genre; *p; p++) *p = tolower((unsigned char)*p);
        } else if (strcasecmp(tag->key, "musicbrainz_albumid") == 0) {
            free(out->mb_release_id);
            out->mb_release_id = strdup(tag->value);
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
// Artist Tag Splitting
// =============================================================================

typedef struct {
    const char* pattern;   /* delimiter to search for (lowercase) */
    const char* canonical; /* join_phrase to store in the DB */
    size_t len;            /* strlen(pattern) */
} delimiter_t;

static const delimiter_t DELIMITERS[] = {
    { " featuring ", " feat. ", 11 },
    { " feat. ",     " feat. ",  7 },
    { " feat ",      " feat. ",  6 },
    { " ft. ",       " ft. ",    5 },
    { " ft ",        " ft. ",    4 },
    { " & ",         " & ",      3 },
};
#define DELIMITER_COUNT (sizeof(DELIMITERS) / sizeof(DELIMITERS[0]))

void artist_credits_free(artist_credit_t* credits, size_t count) {
    if (!credits) return;
    for (size_t i = 0; i < count; i++) {
        free(credits[i].name);
        free(credits[i].join_phrase);
    }
    free(credits);
}

size_t parse_artist_tag(const char* tag, artist_credit_t** out) {
    g_assert(tag && out);

    /* Allocate a growable result array. */
    size_t cap = 4, count = 0;
    artist_credit_t* credits = malloc(cap * sizeof(artist_credit_t));
    g_assert(credits);

    const char* cursor = tag;

    while (*cursor) {
        /* Find the earliest delimiter in the remaining string. */
        const char* best_pos = NULL;
        const delimiter_t* best_delim = NULL;

        for (size_t d = 0; d < DELIMITER_COUNT; d++) {
            const char* p = cursor;
            size_t dlen = DELIMITERS[d].len;
            while (*p) {
                if (g_ascii_strncasecmp(p, DELIMITERS[d].pattern, dlen) == 0) {
                    if (!best_pos || p < best_pos) {
                        best_pos   = p;
                        best_delim = &DELIMITERS[d];
                    }
                    break;
                }
                p++;
            }
            /* Short-circuit: can't find an earlier match than cursor itself */
            if (best_pos == cursor) break;
        }

        if (!best_pos) {
            /* No delimiter found — emit the rest as the final credit. */
            if (count == cap) credits = realloc(credits, (cap *= 2) * sizeof(artist_credit_t));
            credits[count++] = (artist_credit_t){
                .name        = strdup(cursor),
                .join_phrase = strdup(""),
            };
            break;
        }

        /* Emit the segment before the delimiter. */
        if (count == cap) credits = realloc(credits, (cap *= 2) * sizeof(artist_credit_t));
        credits[count++] = (artist_credit_t){
            .name        = strndup(cursor, (size_t)(best_pos - cursor)),
            .join_phrase = strdup(best_delim->canonical),
        };
        cursor = best_pos + best_delim->len;
    }

    *out = credits;
    return count;
}

