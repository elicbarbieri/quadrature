#ifndef QUADRATURE_LIBRARY_SOURCE_H
#define QUADRATURE_LIBRARY_SOURCE_H

#include "quadrature/core/types.h"
#include "quadrature/database/database.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Library Source Abstraction
 *
 * Provides a uniform interface for different music library sources:
 * - LOCAL: Local filesystem (always available)
 * - REMOTE: Network mount (NFS, SMB) - Broadcast build only
 * - PORTABLE: Removable drive (USB) - Broadcast build only
 *
 * Each source has its own database and can be independently online/offline.
 */

typedef enum {
    LIBRARY_SOURCE_LOCAL,      // Local filesystem
    LIBRARY_SOURCE_REMOTE,     // Network mount (Broadcast only)
    LIBRARY_SOURCE_PORTABLE    // Removable drive (Broadcast only)
} library_source_type_t;

typedef struct library_source library_source_t;

// =============================================================================
// Lifecycle
// =============================================================================

/**
 * Create a new library source.
 *
 * @param out Output pointer
 * @param type Source type
 * @param path Path to music library root
 * @param name Human-readable name for this source
 * @return QUADRATURE_OK on success
 */
quadrature_result_t library_source_create(library_source_t** out,
                                          library_source_type_t type,
                                          const char* path,
                                          const char* name);

/**
 * Destroy a library source.
 *
 * @param source Source to destroy
 */
void library_source_destroy(library_source_t* source);

// =============================================================================
// Properties
// =============================================================================

/**
 * Get the source type.
 */
library_source_type_t library_source_type(const library_source_t* source);

/**
 * Get the music library root path.
 */
const char* library_source_path(const library_source_t* source);

/**
 * Get the human-readable source name.
 */
const char* library_source_name(const library_source_t* source);

/**
 * Get the database path for this source.
 *
 * For LOCAL sources: ~/.local/share/quadrature/library.db
 * For REMOTE/PORTABLE: ~/.local/share/quadrature/sources/<hash>.db
 */
const char* library_source_db_path(const library_source_t* source);

/**
 * Check if the source is currently online/accessible.
 */
bool library_source_is_online(const library_source_t* source);

/**
 * Check if the source is read-only.
 */
bool library_source_is_read_only(const library_source_t* source);

// =============================================================================
// Database Access
// =============================================================================

/**
 * Open the database for this source.
 *
 * Opens or creates the database file for this source.
 * The database is cached - multiple calls return the same handle.
 *
 * @param source Library source
 * @param db_out Output database handle
 * @return QUADRATURE_OK on success
 */
quadrature_result_t library_source_open_db(library_source_t* source,
                                           quadrature_db_t** db_out);

/**
 * Close the database for this source.
 *
 * @param source Library source
 */
void library_source_close_db(library_source_t* source);

// =============================================================================
// Type Conversion
// =============================================================================

/**
 * Get source type name as string.
 */
const char* library_source_type_name(library_source_type_t type);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_LIBRARY_SOURCE_H
