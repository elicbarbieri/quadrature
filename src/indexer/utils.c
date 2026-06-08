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

    // Try numeric (allow leading zeros: "01" → 1)
    if (*str >= '0' && *str <= '9') {
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

    // Pattern: Digital Media[sep]N (MusicBrainz/Picard convention for digital releases)
    if (strncasecmp(dir_name, "digital media", 13) == 0) {
        return parse_disc_number(dir_name + 13);
    }

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
    g_free(item->path);
    g_free(item->title);
    g_free(item->artist);
    g_free(item->album_artist);
    g_free(item->album);
    g_free(item->genre);
    g_free(item->mb_release_id);
    g_free(item->mb_release_group_id);
    g_free(item->mb_artist_id);
    g_free(item->mb_album_artist_id);
    item->path = NULL;
    item->title = NULL;
    item->artist = NULL;
    item->album_artist = NULL;
    item->album = NULL;
    item->genre = NULL;
    item->mb_release_id = NULL;
    item->mb_release_group_id = NULL;
    item->mb_artist_id = NULL;
    item->mb_album_artist_id = NULL;
}

// =============================================================================
// Metadata Extraction
// =============================================================================

quadrature_result_t extract_audio_metadata(const char* path, index_item_t* out) {
    g_assert(path != NULL);
    g_assert(out != NULL);

    out->path = g_strdup(path);
    out->title = NULL;
    out->artist = NULL;
    out->album_artist = NULL;
    out->album = NULL;
    out->genre = NULL;
    out->mb_release_id = NULL;
    out->mb_release_group_id = NULL;
    out->mb_artist_id = NULL;
    out->mb_album_artist_id = NULL;
    out->duration_ms = 0;
    out->track_num = 0;
    out->disc_num = 0;
    out->year = 0;

    // Minimal probing: tags + stream info for duration.
    // Must set probesize/analyzeduration via options BEFORE open.
    AVDictionary* open_opts = NULL;
    av_dict_set(&open_opts, "probesize", "65536", 0);       // 64 KB
    av_dict_set(&open_opts, "analyzeduration", "500000", 0); // 0.5s

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, &open_opts) != 0) {
        g_debug("Failed to open audio file: %s", path);
        av_dict_free(&open_opts);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }
    av_dict_free(&open_opts);

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
            out->title = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "artist") == 0) {
            out->artist = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "album") == 0) {
            out->album = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "track") == 0) {
            out->track_num = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "disc") == 0 ||
                   strcasecmp(tag->key, "discnumber") == 0) {
            out->disc_num = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "date") == 0 || strcasecmp(tag->key, "year") == 0) {
            out->year = (uint16_t)atoi(tag->value);
        } else if (strcasecmp(tag->key, "album_artist") == 0 ||
                   strcasecmp(tag->key, "albumartist") == 0) {
            g_free(out->album_artist);
            out->album_artist = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "genre") == 0) {
            g_free(out->genre);
            out->genre = g_strdup(tag->value);
            for (char* p = out->genre; *p; p++) *p = tolower((unsigned char)*p);
        } else if (strcasecmp(tag->key, "musicbrainz_albumid") == 0) {
            g_free(out->mb_release_id);
            out->mb_release_id = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "musicbrainz_releasegroupid") == 0) {
            g_free(out->mb_release_group_id);
            out->mb_release_group_id = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "musicbrainz_artistid") == 0) {
            g_free(out->mb_artist_id);
            out->mb_artist_id = g_strdup(tag->value);
        } else if (strcasecmp(tag->key, "musicbrainz_albumartistid") == 0) {
            g_free(out->mb_album_artist_id);
            out->mb_album_artist_id = g_strdup(tag->value);
        }
    }

    avformat_close_input(&fmt);

    // Fallback: use filename as title if no title tag
    if (!out->title) {
        const char* fname = strrchr(path, '/');
        fname = fname ? fname + 1 : path;
        char* copy = g_strdup(fname);
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

// =============================================================================
// Title Featuring Extraction
// =============================================================================

/**
 * Featuring prefixes to match inside parentheses/brackets (case-insensitive).
 * Ordered longest-first to avoid partial matches.
 */
static const struct { const char* prefix; size_t len; } FEAT_PREFIXES[] = {
    { "featuring ", 10 },
    { "feat. ",     6 },
    { "feat ",      5 },
    { "ft. ",       4 },
    { "ft ",        3 },
};
#define FEAT_PREFIX_COUNT (sizeof(FEAT_PREFIXES) / sizeof(FEAT_PREFIXES[0]))

bool title_extract_featuring(const char* title, char** clean_out, char** feat_out) {
    if (!title || !clean_out || !feat_out) return false;

    for (const char* p = title; *p; p++) {
        if (*p != '(' && *p != '[') continue;

        char close_char = (*p == '(') ? ')' : ']';
        const char* inside = p + 1;

        for (size_t i = 0; i < FEAT_PREFIX_COUNT; i++) {
            if (g_ascii_strncasecmp(inside, FEAT_PREFIXES[i].prefix,
                                     FEAT_PREFIXES[i].len) != 0)
                continue;

            const char* artist_start = inside + FEAT_PREFIXES[i].len;
            const char* end = strchr(artist_start, close_char);
            if (!end) continue;

            // Found: extract artist names
            *feat_out = g_strndup(artist_start, (gsize)(end - artist_start));

            // Build clean title: everything before bracket + everything after
            GString* clean = g_string_sized_new(strlen(title));

            // Before bracket, trimming trailing whitespace
            const char* before_end = p;
            while (before_end > title && *(before_end - 1) == ' ')
                before_end--;
            g_string_append_len(clean, title, (gssize)(before_end - title));

            // After closing bracket
            const char* after = end + 1;
            if (*after && clean->len > 0) {
                if (*after != ' ')
                    g_string_append_c(clean, ' ');
                g_string_append(clean, after);
            }

            // Trim trailing whitespace
            while (clean->len > 0 && clean->str[clean->len - 1] == ' ')
                g_string_truncate(clean, clean->len - 1);

            *clean_out = g_string_free(clean, FALSE);
            return true;
        }
    }

    return false;
}

// =============================================================================
// Artist Delimiter Detection
// =============================================================================

/**
 * Check if a single delimiter character varies across artist tags.
 * Returns true if 2+ tags have different suffixes after the delimiter.
 */
static bool is_delimiter_varying(const char* const* artist_tags, size_t count, char delim) {
    const char* first_suffix = NULL;

    for (size_t i = 0; i < count; i++) {
        if (!artist_tags[i]) continue;
        const char* pos = strchr(artist_tags[i], delim);
        if (!pos) continue;

        const char* suffix = pos + 1;
        if (!first_suffix) {
            first_suffix = suffix;
        } else if (strcasecmp(suffix, first_suffix) != 0) {
            return true;
        }
    }

    return false;
}

char detect_artist_delimiter(const char* const* artist_tags, size_t count) {
    if (is_delimiter_varying(artist_tags, count, ';')) return ';';
    if (is_delimiter_varying(artist_tags, count, '/')) return '/';
    return '\0';
}

// =============================================================================
// Artist Tag Splitting
// =============================================================================

void artist_credits_free(artist_credit_t* credits, size_t count) {
    if (!credits) return;
    for (size_t i = 0; i < count; i++) {
        g_free(credits[i].name);
        g_free(credits[i].join_phrase);
    }
    g_free(credits);
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
            if (count == cap) credits = g_realloc(credits, (cap *= 2) * sizeof(artist_credit_t));
            credits[count++] = (artist_credit_t){
                .name        = g_strdup(cursor),
                .join_phrase = g_strdup(""),
            };
            break;
        }

        /* Emit the segment before the delimiter. */
        if (count == cap) credits = g_realloc(credits, (cap *= 2) * sizeof(artist_credit_t));
        credits[count++] = (artist_credit_t){
            .name        = g_strndup(cursor, (size_t)(best_pos - cursor)),
            .join_phrase = g_strdup(best_delim->canonical),
        };
        cursor = best_pos + best_delim->len;
    }

    *out = credits;
    return count;
}

