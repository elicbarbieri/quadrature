/**
 * folder_album.c - Folder-based album context and compilation detection
 *
 * This module implements the directory-first album grouping strategy where
 * each folder is treated as a single album. Metadata is gathered from all
 * tracks in the directory to determine:
 * - Album title (most common album tag, or folder name as fallback)
 * - Album artist (explicit tag, or "Various Artists" for compilations)
 * - Compilation status (detected via multiple heuristics)
 *
 * Based on Strawberry's dual approach: directory structure is the source
 * of truth for album grouping, while metadata tags are compared to detect
 * errors.
 */

#include "internal.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // For strcasecmp

// =============================================================================
// Constants
// =============================================================================

#define COMPILATION_ARTIST_THRESHOLD 5  // 5+ unique artists = compilation

// Keywords that suggest a compilation album
static const char* COMPILATION_KEYWORDS[] = {
    "various",
    "compilation",
    "greatest hits",
    "best of",
    "collection",
    "anthology",
    "soundtrack",
    "ost",
    "sampler",
    "mixed by",
    "presents",
    NULL
};

// =============================================================================
// Folder Album Context (internal struct definition)
// =============================================================================

struct folder_album_context {
    char* directory_path;           // Full path to the directory
    char* folder_name;              // Just the folder name (last component)
    char* detected_album_title;     // Most common album tag
    char* detected_album_artist;    // Album artist or "Various Artists"
    char* most_common_artist;       // Most frequent track artist
    GHashTable* artists;            // artist name -> count (int*)
    GHashTable* album_titles;       // album title -> count (int*)
    GHashTable* years;              // year -> count (int*)
    size_t track_count;
    bool is_compilation;
    bool has_explicit_compilation_tag;
    bool has_album_artist_tag;
    char* explicit_album_artist;    // From ALBUMARTIST tag
    uint16_t most_common_year;
};

// =============================================================================
// Helper Functions
// =============================================================================

static void increment_hash_count(GHashTable* table, const char* key) {
    if (!key || !*key) return;

    gpointer existing = g_hash_table_lookup(table, key);
    if (existing) {
        int* count = (int*)existing;
        (*count)++;
    } else {
        int* count = g_new(int, 1);
        *count = 1;
        g_hash_table_insert(table, g_strdup(key), count);
    }
}

static const char* get_most_common_key(GHashTable* table) {
    const char* most_common = NULL;
    int max_count = 0;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, table);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        int count = *(int*)value;
        if (count > max_count) {
            max_count = count;
            most_common = (const char*)key;
        }
    }

    return most_common;
}

static uint16_t get_most_common_year(GHashTable* table) {
    uint16_t most_common = 0;
    int max_count = 0;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, table);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        int count = *(int*)value;
        if (count > max_count) {
            max_count = count;
            most_common = (uint16_t)GPOINTER_TO_INT(key);
        }
    }

    return most_common;
}

static bool title_contains_compilation_keywords(const char* title) {
    if (!title) return false;

    // Convert to lowercase for case-insensitive matching
    char* lower = g_ascii_strdown(title, -1);

    bool found = false;
    for (int i = 0; COMPILATION_KEYWORDS[i] != NULL; i++) {
        if (strstr(lower, COMPILATION_KEYWORDS[i]) != NULL) {
            found = true;
            break;
        }
    }

    g_free(lower);
    return found;
}

static char* extract_folder_name(const char* path) {
    if (!path) return NULL;

    const char* last_slash = strrchr(path, '/');
    if (last_slash && *(last_slash + 1)) {
        return strdup(last_slash + 1);
    }

    return strdup(path);
}

// =============================================================================
// Public API
// =============================================================================

folder_album_context_t* folder_album_context_new(const char* dir_path) {
    if (!dir_path) return NULL;

    folder_album_context_t* ctx = calloc(1, sizeof(folder_album_context_t));
    if (!ctx) return NULL;

    ctx->directory_path = strdup(dir_path);
    ctx->folder_name = extract_folder_name(dir_path);
    ctx->artists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ctx->album_titles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ctx->years = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    return ctx;
}

void folder_album_context_add_track(folder_album_context_t* ctx,
                                     const extended_metadata_t* meta) {
    if (!ctx || !meta) return;

    ctx->track_count++;

    // Track artist frequency
    if (meta->artist && *meta->artist) {
        increment_hash_count(ctx->artists, meta->artist);
    }

    // Track album title frequency
    if (meta->album && *meta->album) {
        increment_hash_count(ctx->album_titles, meta->album);
    }

    // Track year frequency
    if (meta->year > 0) {
        gpointer key = GINT_TO_POINTER(meta->year);
        gpointer existing = g_hash_table_lookup(ctx->years, key);
        if (existing) {
            int* count = (int*)existing;
            (*count)++;
        } else {
            int* count = g_new(int, 1);
            *count = 1;
            g_hash_table_insert(ctx->years, key, count);
        }
    }

    // Check for explicit compilation tag
    if (meta->compilation) {
        ctx->has_explicit_compilation_tag = true;
    }

    // Check for album artist tag
    if (meta->album_artist && *meta->album_artist) {
        ctx->has_album_artist_tag = true;
        // Store the first album artist we see (they should all be the same)
        if (!ctx->explicit_album_artist) {
            ctx->explicit_album_artist = strdup(meta->album_artist);
        }
    }
}

void folder_album_finalize(folder_album_context_t* ctx) {
    if (!ctx) return;

    // Determine most common album title
    const char* common_title = get_most_common_key(ctx->album_titles);
    if (common_title && *common_title) {
        ctx->detected_album_title = strdup(common_title);
    } else {
        // Fall back to folder name
        ctx->detected_album_title = strdup(ctx->folder_name ? ctx->folder_name : "Unknown Album");
    }

    // Determine most common artist
    const char* common_artist = get_most_common_key(ctx->artists);
    if (common_artist) {
        ctx->most_common_artist = strdup(common_artist);
    }

    // Determine most common year
    ctx->most_common_year = get_most_common_year(ctx->years);

    // Determine if this is a compilation
    size_t unique_artists = g_hash_table_size(ctx->artists);

    // Compilation detection logic (in priority order):
    // 1. Explicit compilation tag on any track
    if (ctx->has_explicit_compilation_tag) {
        ctx->is_compilation = true;
    }
    // 2. Album artist tag = "Various Artists"
    else if (ctx->explicit_album_artist &&
             strcasecmp(ctx->explicit_album_artist, "Various Artists") == 0) {
        ctx->is_compilation = true;
    }
    // 3. 5+ unique artists in folder
    else if (unique_artists >= COMPILATION_ARTIST_THRESHOLD) {
        ctx->is_compilation = true;
    }
    // 4. Album title keywords
    else if (title_contains_compilation_keywords(ctx->detected_album_title)) {
        ctx->is_compilation = true;
    }

    // Determine album artist
    if (ctx->is_compilation) {
        // For compilations, use explicit album artist or "Various Artists"
        if (ctx->explicit_album_artist) {
            ctx->detected_album_artist = strdup(ctx->explicit_album_artist);
        } else {
            ctx->detected_album_artist = strdup("Various Artists");
        }
    } else {
        // For non-compilations, use album artist tag or most common artist
        if (ctx->explicit_album_artist) {
            ctx->detected_album_artist = strdup(ctx->explicit_album_artist);
        } else if (ctx->most_common_artist) {
            ctx->detected_album_artist = strdup(ctx->most_common_artist);
        } else {
            ctx->detected_album_artist = strdup("Unknown Artist");
        }
    }

    g_debug("Folder album finalized: \"%s\" by \"%s\" (%zu tracks, %zu artists, compilation=%d)",
            ctx->detected_album_title,
            ctx->detected_album_artist,
            ctx->track_count,
            unique_artists,
            ctx->is_compilation);
}

void folder_album_context_free(folder_album_context_t* ctx) {
    if (!ctx) return;

    free(ctx->directory_path);
    free(ctx->folder_name);
    free(ctx->detected_album_title);
    free(ctx->detected_album_artist);
    free(ctx->most_common_artist);
    free(ctx->explicit_album_artist);

    if (ctx->artists) g_hash_table_destroy(ctx->artists);
    if (ctx->album_titles) g_hash_table_destroy(ctx->album_titles);
    if (ctx->years) g_hash_table_destroy(ctx->years);

    free(ctx);
}

// =============================================================================
// Accessors
// =============================================================================

const char* folder_album_get_directory_path(const folder_album_context_t* ctx) {
    return ctx ? ctx->directory_path : NULL;
}

const char* folder_album_get_folder_name(const folder_album_context_t* ctx) {
    return ctx ? ctx->folder_name : NULL;
}

const char* folder_album_get_title(const folder_album_context_t* ctx) {
    return ctx ? ctx->detected_album_title : NULL;
}

const char* folder_album_get_artist(const folder_album_context_t* ctx) {
    return ctx ? ctx->detected_album_artist : NULL;
}

bool folder_album_is_compilation(const folder_album_context_t* ctx) {
    return ctx ? ctx->is_compilation : false;
}

uint16_t folder_album_get_year(const folder_album_context_t* ctx) {
    return ctx ? ctx->most_common_year : 0;
}

size_t folder_album_get_track_count(const folder_album_context_t* ctx) {
    return ctx ? ctx->track_count : 0;
}

size_t folder_album_get_unique_artist_count(const folder_album_context_t* ctx) {
    return ctx ? g_hash_table_size(ctx->artists) : 0;
}

// =============================================================================
// Validation Helpers
// =============================================================================

bool folder_album_track_has_album_mismatch(const folder_album_context_t* ctx,
                                            const extended_metadata_t* meta) {
    if (!ctx || !meta) return false;

    // No mismatch if track has no album tag
    if (!meta->album || !*meta->album) return false;

    // No mismatch if folder album not determined yet
    if (!ctx->detected_album_title) return false;

    // Compare (case-insensitive)
    return strcasecmp(meta->album, ctx->detected_album_title) != 0;
}

bool folder_album_track_has_artist_inconsistency(const folder_album_context_t* ctx,
                                                  const extended_metadata_t* meta) {
    if (!ctx || !meta) return false;

    // Only flag inconsistency for non-compilations with multiple artists
    // that don't have an album artist tag
    if (ctx->is_compilation) return false;
    if (ctx->has_album_artist_tag) return false;
    if (g_hash_table_size(ctx->artists) <= 1) return false;

    // Track has different artist than the most common one
    if (!meta->artist || !ctx->most_common_artist) return false;

    return strcasecmp(meta->artist, ctx->most_common_artist) != 0;
}
