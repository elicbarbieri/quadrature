/**
 * Quadrature Artwork Manager
 *
 * Async thumbnail loading: LRU texture cache + mmapped atlas + worker pool.
 * Cache hit is synchronous. Miss dispatches to worker thread for PNG decode.
 */

#ifndef QUADRATURE_UI_LIBRARY_ARTWORK_MANAGER_H
#define QUADRATURE_UI_LIBRARY_ARTWORK_MANAGER_H

#include <gtk/gtk.h>
#include "quadrature/quadrature.h"
#include "quadrature/quadrature_library.h"

G_BEGIN_DECLS

typedef struct _ArtworkManager ArtworkManager;

/**
 * Create a new artwork manager.
 *
 * Atlas path is computed internally: $XDG_DATA_HOME/quadrature/art/{cache_size}px/artwork.atlas
 *
 * @param library Library cache (for future use, may be NULL)
 * @param cache_size Thumbnail pixel size (determines atlas path)
 * @param cache_count Maximum number of cached texture entries (0 = default 1000)
 * @return New artwork manager
 */
ArtworkManager *artwork_manager_new(library_cache_t *library, int cache_size, size_t cache_count);

void artwork_manager_free(ArtworkManager *mgr);

/**
 * Async load thumbnail into GtkImage.
 * Cache hit: sets texture synchronously.
 * Cache miss: sets placeholder icon, dispatches worker, updates on main thread.
 *
 * Precondition: mgr != NULL, image != NULL
 *
 * @param mgr Artwork manager
 * @param album_id Album ID to load artwork for
 * @param image GtkImage widget to update
 */
void artwork_manager_get_thumbnail(ArtworkManager *mgr, int64_t album_id, GtkWidget *image);

/**
 * Get the configured thumbnail pixel size.
 *
 * @param mgr Artwork manager
 * @return Thumbnail size in pixels
 */
int artwork_manager_get_thumb_size(ArtworkManager *mgr);

/**
 * Reload atlas from disk and clear texture cache.
 * Call after indexer finishes to pick up new artwork.
 *
 * @param mgr Artwork manager
 */
void artwork_manager_reload_atlas(ArtworkManager *mgr);

/**
 * Prefetch full-size artwork by album ID.
 * Calls LibraryCache to resolve album_id -> path and do the prefetch syscall.
 *
 * @param mgr Artwork manager
 * @param album_id Album ID
 */
void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, int64_t album_id);

G_END_DECLS

#endif /* QUADRATURE_UI_LIBRARY_ARTWORK_MANAGER_H */
