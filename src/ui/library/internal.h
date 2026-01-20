/**
 * Quadrature Library Module - Internal Header
 *
 * Single consolidated header for the library browsing subsystem.
 * Provides: cache, lazy list model, and view factories.
 */

#ifndef QUADRATURE_UI_LIBRARY_INTERNAL_H
#define QUADRATURE_UI_LIBRARY_INTERNAL_H

#include <gtk/gtk.h>
#include "quadrature/database/database.h"

G_BEGIN_DECLS

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LIBRARY_PAGE_SIZE 50
#define LIBRARY_CACHE_MAX_PAGES 100

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Item - Unified GObject for all library entities
 *
 * Single type handles artists, albums, and tracks. Uses tagged fields
 * to avoid multiple GObject type registrations and reduce complexity.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    LIBRARY_ITEM_ARTIST,
    LIBRARY_ITEM_ALBUM,
    LIBRARY_ITEM_TRACK,
} LibraryItemKind;

#define LIBRARY_TYPE_ITEM (library_item_get_type())
G_DECLARE_FINAL_TYPE(LibraryItem, library_item, LIBRARY, ITEM, GObject)

struct _LibraryItem {
    GObject parent;
    LibraryItemKind kind;
    gboolean placeholder;    /* TRUE if awaiting data fetch */
    gboolean is_disc_header; /* TRUE for disc separator rows */

    int64_t id;
    char *name;            /* Artist name, album title, or track title */
    char *secondary;       /* Album: artist name; Track: artist */
    char *tertiary;        /* Track: album name */
    char *path;            /* Track: file path */

    int64_t parent_id;     /* Album: artist_id; Track: album_id */
    int64_t artist_id;     /* Track: artist_id for navigation */
    uint32_t duration_ms;  /* Track only */
    uint16_t track_num;    /* Track only */
    uint16_t disc_num;     /* Track only (default 1) */
    uint16_t year;         /* Album or track */
    size_t count1;         /* Artist: album_count; Album: track_count */
    size_t count2;         /* Artist: track_count */
};

/* Constructors */
LibraryItem *library_item_new_placeholder(LibraryItemKind kind);
LibraryItem *library_item_new_artist(const db_artist_t *a);
LibraryItem *library_item_new_album(const db_album_t *a);
LibraryItem *library_item_new_track(const db_track_t *t);

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Cache - LRU cache for paginated data
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Cache namespaces for composite keys */
typedef enum {
    CACHE_PAGE_ARTIST,      /* Artist pages by offset */
    CACHE_PAGE_ALBUM,       /* Album pages by offset */
    CACHE_PAGE_TRACK,       /* Track pages by offset */
    CACHE_DETAIL_ALBUM,     /* Albums by artist_id */
    CACHE_DETAIL_TRACK,     /* Tracks by album_id */
} CacheNamespace;

typedef struct _LibraryCache LibraryCache;

LibraryCache *library_cache_new(void);
void library_cache_free(LibraryCache *cache);
void library_cache_invalidate(LibraryCache *cache);

/* Page queries - returns cached data or NULL if miss */
gboolean library_cache_get_page(LibraryCache *cache,
                                 LibraryItemKind kind,
                                 size_t offset,
                                 GPtrArray **out,      /* Array of LibraryItem* */
                                 size_t *total);

/* Store page in cache */
void library_cache_put_page(LibraryCache *cache,
                             LibraryItemKind kind,
                             size_t offset,
                             GPtrArray *items,        /* Takes ownership */
                             size_t total);

/* Detail queries (albums by artist, tracks by album) */
gboolean library_cache_get_detail(LibraryCache *cache,
                                   LibraryItemKind kind,
                                   int64_t parent_id,
                                   GPtrArray **out);

void library_cache_put_detail(LibraryCache *cache,
                               LibraryItemKind kind,
                               int64_t parent_id,
                               GPtrArray *items);

/* Unified cache operations (used internally) */
gboolean library_cache_get(LibraryCache *cache, CacheNamespace ns,
                           int64_t key, GPtrArray **out, size_t *total);
void library_cache_put(LibraryCache *cache, CacheNamespace ns,
                       int64_t key, GPtrArray *items, size_t total);

/* Cached totals */
gboolean library_cache_get_total(LibraryCache *cache, LibraryItemKind kind, size_t *total);
void library_cache_set_total(LibraryCache *cache, LibraryItemKind kind, size_t total);

/* Statistics (interval-based: counters reset after each reporting cycle) */
void library_cache_enable_stats_reporting(LibraryCache *cache, gboolean enable);
void library_cache_get_stats(LibraryCache *cache, size_t *hits, size_t *misses,
                              size_t *evictions);

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Model - Lazy-loading GListModel
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LIBRARY_TYPE_MODEL (library_model_get_type())
G_DECLARE_FINAL_TYPE(LibraryModel, library_model, LIBRARY, MODEL, GObject)

LibraryModel *library_model_new(LibraryItemKind kind,
                                 quadrature_db_t *db,
                                 LibraryCache *cache);

void library_model_refresh(LibraryModel *model);
void library_model_prefetch(LibraryModel *model, size_t offset);
LibraryItemKind library_model_get_kind(LibraryModel *model);
void library_model_set_sort(LibraryModel *model, db_sort_t sort);
db_sort_t library_model_get_sort(LibraryModel *model);

/* ═══════════════════════════════════════════════════════════════════════════
 * Artwork Manager (see artwork_manager.h for full API)
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "artwork_manager.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Navigation Stack - For detail view back navigation
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    NAV_ENTRY_VIEW,    /* Main view (Artists, Albums, Search) */
    NAV_ENTRY_ARTIST,  /* Artist detail */
    NAV_ENTRY_ALBUM,   /* Album detail */
} NavEntryType;

typedef struct {
    NavEntryType type;
    int64_t id;              /* Artist/album ID (0 for main views) */
    char *view_name;         /* Source view name for back label */
    double scroll_pos;       /* Scroll position to restore */
} NavEntry;

/* ═══════════════════════════════════════════════════════════════════════════
 * Unified Detail View - Single widget for empty/album/artist states
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DETAIL_STATE_EMPTY,
    DETAIL_STATE_ALBUM,
    DETAIL_STATE_ARTIST,
} DetailState;

/* ═══════════════════════════════════════════════════════════════════════════
 * Library Views - GTK4 list views for browsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Track info for preloading (minimal struct to pass track info) */
typedef struct {
    int64_t track_id;
    const char *path;
} LibraryTrackInfo;

/* Callbacks for view interactions */
typedef struct {
    void (*on_navigate)(LibraryItemKind kind, int64_t id, gpointer data);
    void (*on_play)(const char *path, const char *title,
                    const char *artist, const char *album,
                    int64_t track_id, gpointer data);
    void (*on_back)(gpointer data);
    /* Called when album detail is loaded - use for audio preloading */
    void (*on_album_loaded)(int64_t album_id, const LibraryTrackInfo *tracks,
                            size_t count, gpointer data);
    /* Called when info button is clicked on track row */
    void (*on_track_info)(int64_t track_id, gpointer data);
    gpointer user_data;
} LibraryCallbacks;

/* Main list views (lazy loading) */
GtkWidget *library_view_new(LibraryItemKind kind,
                             quadrature_db_t *db,
                             LibraryCache *cache,
                             ArtworkManager *art_mgr,
                             const LibraryCallbacks *cbs);

/* Detail views (direct queries) - DEPRECATED, use unified detail view */
GtkWidget *library_detail_view_new(LibraryItemKind kind,
                                    quadrature_db_t *db,
                                    LibraryCache *cache,
                                    ArtworkManager *art_mgr,
                                    const LibraryCallbacks *cbs);

void library_detail_view_set_id(GtkWidget *view, int64_t id);

/* Unified Detail View - single widget handling empty/album/artist states */
GtkWidget *library_unified_detail_view_new(quadrature_db_t *db,
                                            LibraryCache *cache,
                                            ArtworkManager *art_mgr,
                                            const LibraryCallbacks *cbs);

void library_unified_detail_navigate_to_artist(GtkWidget *view, int64_t artist_id,
                                                const char *source_view);
void library_unified_detail_navigate_to_album(GtkWidget *view, int64_t album_id,
                                               const char *source_view);
void library_unified_detail_show_empty(GtkWidget *view);
gboolean library_unified_detail_go_back(GtkWidget *view);
DetailState library_unified_detail_get_state(GtkWidget *view);

/* Refresh current view data */
void library_view_refresh(GtkWidget *view);

/* ═══════════════════════════════════════════════════════════════════════════
 * Errors View - Tree view of indexer errors by folder structure
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *errors_view_new(quadrature_db_t *db);
void errors_view_refresh(GtkWidget *view);
void errors_view_set_callbacks(GtkWidget *view,
                                void (*on_path)(const char *path, gpointer data),
                                gpointer user_data);
size_t errors_view_get_count(GtkWidget *view);
void errors_view_set_path_filter(GtkWidget *view, const char *path_filter);

G_END_DECLS

#endif /* QUADRATURE_UI_LIBRARY_INTERNAL_H */
