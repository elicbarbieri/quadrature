#include <glib.h>
/**
 * Library source implementation.
 *
 * Provides a uniform interface for different music library sources.
 */

#include "quadrature/library/library_source.h"
#include "quadrature/database/database.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// =============================================================================
// Library Source Structure
// =============================================================================

struct library_source {
    library_source_type_t type;
    char* path;           // Root path to music library
    char* name;           // Human-readable name
    char* db_path;        // Path to SQLite database
    quadrature_db_t* db;  // Cached database handle
    bool db_opened;       // Whether we've attempted to open the db
};

// =============================================================================
// Helpers
// =============================================================================

// Ensure directory exists
static int ensure_dir(const char* path) {
    char tmp[4096];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

// Get default data directory
static char* get_data_dir(void) {
    const char* xdg_data = g_get_user_data_dir();
    char* dir = g_build_filename(xdg_data, "quadrature", NULL);
    ensure_dir(dir);
    return dir;
}

// Simple hash for generating unique database names
static unsigned int hash_string(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// Generate database path for a source
static char* generate_db_path(library_source_type_t type, const char* path) {
    char* data_dir = get_data_dir();

    if (type == LIBRARY_SOURCE_LOCAL) {
        char* db_path = g_build_filename(data_dir, "library.db", NULL);
        g_free(data_dir);
        return db_path;
    }

    // For remote/portable sources, use a hash-based filename
    char* sources_dir = g_build_filename(data_dir, "sources", NULL);
    ensure_dir(sources_dir);
    g_free(data_dir);

    unsigned int hash = hash_string(path);
    char filename[64];
    snprintf(filename, sizeof(filename), "%08x.db", hash);

    char* db_path = g_build_filename(sources_dir, filename, NULL);
    g_free(sources_dir);

    return db_path;
}

// =============================================================================
// Lifecycle
// =============================================================================

quadrature_result_t library_source_create(library_source_t** out,
                                          library_source_type_t type,
                                          const char* path,
                                          const char* name) {
    if (!out || !path) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

#ifndef QUADRATURE_BUILD_BROADCAST
    // In debug builds, only LOCAL source type is allowed
    if (type != LIBRARY_SOURCE_LOCAL) {
        g_warning("Non-local library sources only available in broadcast build");
        return QUADRATURE_ERROR_INVALID_PARAM;
    }
#endif

    library_source_t* source = calloc(1, sizeof(library_source_t));
    if (!source) {
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    source->type = type;
    source->path = strdup(path);
    source->name = name ? strdup(name) : strdup(path);
    source->db_path = generate_db_path(type, path);
    source->db = NULL;
    source->db_opened = false;

    if (!source->path || !source->name || !source->db_path) {
        library_source_destroy(source);
        return QUADRATURE_ERROR_OUT_OF_MEMORY;
    }

    *out = source;
    return QUADRATURE_OK;
}

void library_source_destroy(library_source_t* source) {
    if (!source) return;

    library_source_close_db(source);
    free(source->path);
    free(source->name);
    free(source->db_path);
    free(source);
}

// =============================================================================
// Properties
// =============================================================================

library_source_type_t library_source_type(const library_source_t* source) {
    return source ? source->type : LIBRARY_SOURCE_LOCAL;
}

const char* library_source_path(const library_source_t* source) {
    return source ? source->path : NULL;
}

const char* library_source_name(const library_source_t* source) {
    return source ? source->name : NULL;
}

const char* library_source_db_path(const library_source_t* source) {
    return source ? source->db_path : NULL;
}

bool library_source_is_online(const library_source_t* source) {
    if (!source) return false;

    // Check if the path exists and is accessible
    struct stat st;
    if (stat(source->path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

bool library_source_is_read_only(const library_source_t* source) {
    if (!source) return true;

    // Check if we can write to the path
    if (access(source->path, W_OK) != 0) {
        return true;
    }

    return false;
}

// =============================================================================
// Database Access
// =============================================================================

quadrature_result_t library_source_open_db(library_source_t* source,
                                           quadrature_db_t** db_out) {
    if (!source || !db_out) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    // Return cached handle if already opened
    if (source->db_opened && source->db) {
        *db_out = source->db;
        return QUADRATURE_OK;
    }

    // Open database
    quadrature_result_t res = db_open(source->db_path, &source->db);
    source->db_opened = true;

    if (res != QUADRATURE_OK) {
        g_warning("Failed to open database for source '%s': %s",
                  source->name, source->db_path);
        return res;
    }

    *db_out = source->db;
    return QUADRATURE_OK;
}

void library_source_close_db(library_source_t* source) {
    if (!source) return;

    if (source->db) {
        db_close(source->db);
        source->db = NULL;
    }
    source->db_opened = false;
}

// =============================================================================
// Type Conversion
// =============================================================================

const char* library_source_type_name(library_source_type_t type) {
    switch (type) {
        case LIBRARY_SOURCE_LOCAL:    return "Local";
        case LIBRARY_SOURCE_REMOTE:   return "Remote";
        case LIBRARY_SOURCE_PORTABLE: return "Portable";
        default:                      return "Unknown";
    }
}
