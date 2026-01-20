/**
 * Quadrature Artwork Manager
 *
 * Two-tier artwork system:
 * - Thumbnails: LRU texture cache + mmapped atlas + worker pool
 * - Full-size: posix_fadvise() hint, then UI loads directly via kernel page cache
 */

#ifndef QUADRATURE_UI_LIBRARY_ARTWORK_MANAGER_H
#define QUADRATURE_UI_LIBRARY_ARTWORK_MANAGER_H

#include <gtk/gtk.h>
#include "quadrature/core/types.h"
#include "quadrature/database/database.h"

G_BEGIN_DECLS

/* ═══════════════════════════════════════════════════════════════════════════
 * Types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _ArtworkManager ArtworkManager;

typedef enum {
    LOAD_PRIORITY_VISIBLE = 0,   /* Currently visible rows */
    LOAD_PRIORITY_PREFETCH = 1,  /* Scroll anticipation */
    LOAD_PRIORITY_LOW = 2,       /* Background fills */
} LoadPriority;

/* Callback for async thumbnail loading */
typedef void (*ArtThumbCallback)(ArtworkManager *mgr, int64_t album_id,
                                  GdkTexture *texture, gpointer user_data);

/* ═══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

ArtworkManager *artwork_manager_new(quadrature_db_t *db, size_t max_entries);
void artwork_manager_free(ArtworkManager *mgr);

/* ═══════════════════════════════════════════════════════════════════════════
 * Thumbnail API (for list views)
 * Load path: cache → atlas → file fallback
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Sync lookup - returns cached texture or NULL (no load triggered) */
GdkTexture *artwork_manager_get_thumb(ArtworkManager *mgr, int64_t album_id);

/* Async load with callback (atlas lookup by album_id) */
void artwork_manager_load_thumb(ArtworkManager *mgr, int64_t album_id,
                                 LoadPriority priority, GCancellable *cancel,
                                 ArtThumbCallback cb, gpointer data);

/* Async load directly into GtkImage widget (atlas lookup by album_id) */
void artwork_manager_load_thumb_into(ArtworkManager *mgr, int64_t album_id,
                                      LoadPriority priority, GtkWidget *image,
                                      GCancellable *cancel);

/* Scroll prefetch - queues thumbnails at LOAD_PRIORITY_PREFETCH */
void artwork_manager_prefetch_thumbs(ArtworkManager *mgr, const int64_t *album_ids, size_t count);
void artwork_manager_cancel_prefetches(ArtworkManager *mgr);

/* ═══════════════════════════════════════════════════════════════════════════
 * Full-Size API (for detail views)
 * Uses kernel page cache - no userspace caching needed
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Hint kernel to prefetch file into page cache (call on album click) */
void artwork_manager_prefetch_fullsize(ArtworkManager *mgr, const char *art_path);

/* UI then loads directly: gtk_picture_set_filename(picture, art_path) */

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Albums Cache (reduces DB queries for art strips)
 * ═══════════════════════════════════════════════════════════════════════════ */

gboolean artwork_manager_get_artist_albums(ArtworkManager *mgr, int64_t artist_id,
                                            int64_t **album_ids, size_t *count);
void artwork_manager_put_artist_albums(ArtworkManager *mgr, int64_t artist_id,
                                        const int64_t *album_ids, size_t count);
void artwork_manager_invalidate_artist_cache(ArtworkManager *mgr);

/* ═══════════════════════════════════════════════════════════════════════════
 * Cache Management
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_clear(ArtworkManager *mgr);
void artwork_manager_invalidate_album(ArtworkManager *mgr, int64_t album_id);
void artwork_manager_reload_atlas(ArtworkManager *mgr);

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

void artwork_manager_get_stats(ArtworkManager *mgr, size_t *hits, size_t *misses,
                                size_t *evictions, size_t *atlas_hits,
                                size_t *load_failures, size_t *load_timeouts);

/* Get load time percentiles (in milliseconds) from recent samples */
void artwork_manager_get_load_time_stats(ArtworkManager *mgr,
                                          double *p50_ms, double *p90_ms, double *p99_ms);

/* Enable periodic stats logging (every 15 seconds) */
void artwork_manager_enable_stats_reporting(ArtworkManager *mgr, gboolean enable);

G_END_DECLS

#endif /* QUADRATURE_UI_LIBRARY_ARTWORK_MANAGER_H */
