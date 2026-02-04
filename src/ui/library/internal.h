/**
 * Quadrature Library Module - Internal Header
 *
 * Single consolidated header for the library browsing subsystem.
 * All data is sourced from the foundation layer library_cache_t.
 */

#ifndef QUADRATURE_UI_LIBRARY_INTERNAL_H
#define QUADRATURE_UI_LIBRARY_INTERNAL_H

#include <gtk/gtk.h>
#include "quadrature/quadrature_library.h"

G_BEGIN_DECLS

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LIBRARY_PAGE_SIZE 50

/* ═══════════════════════════════════════════════════════════════════════════
 * Entity Kind Enum (for navigation and view type identification)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    LIBRARY_ITEM_ARTIST,
    LIBRARY_ITEM_ALBUM,
    LIBRARY_ITEM_TRACK,
} LibraryItemKind;

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
    DETAIL_STATE_ALBUM,
    DETAIL_STATE_ARTIST,
} DetailState;

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Interaction Handlers
 *
 * Standard callbacks for row interactions. Used by all library row types.
 * Selection is handled by GTK's GtkSelectionModel (automatic).
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    void (*on_activate)(int64_t entity_id, gpointer data);  /* primary: double-click / Enter */
    void (*on_secondary)(int64_t entity_id, gpointer data); /* secondary: right-click */
    gpointer user_data;
} RowCallbacks;

/* ═══════════════════════════════════════════════════════════════════════════
 * GObject Items for GListStore (cache-owned data wrappers)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define QUAD_TYPE_ARTIST_ITEM (quad_artist_item_get_type())
G_DECLARE_FINAL_TYPE(QuadArtistItem, quad_artist_item, QUAD, ARTIST_ITEM, GObject)
struct _QuadArtistItem { GObject parent; const library_artist_info_t *info; };
QuadArtistItem *quad_artist_item_new(const library_artist_info_t *info);

#define QUAD_TYPE_ALBUM_ITEM (quad_album_item_get_type())
G_DECLARE_FINAL_TYPE(QuadAlbumItem, quad_album_item, QUAD, ALBUM_ITEM, GObject)
struct _QuadAlbumItem { GObject parent; const library_album_info_t *info; };
QuadAlbumItem *quad_album_item_new(const library_album_info_t *info);

/* ═══════════════════════════════════════════════════════════════════════════
 * LazyList — Virtualized list with scroll monitoring
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _LazyList LazyList;

/* Factory callbacks provided by the consumer */
typedef struct {
    void (*setup)(GtkListItemFactory *f, GtkListItem *li, gpointer data);
    void (*bind)(GtkListItemFactory *f, GtkListItem *li, gpointer data);
    void (*unbind)(GtkListItemFactory *f, GtkListItem *li, gpointer data);
    void (*activate)(guint position, gpointer data);
    gpointer user_data;
} LazyListCallbacks;

/* Create lazy list. item_type is the GObject type stored in the GListStore. */
LazyList *lazy_list_new(GType item_type, const LazyListCallbacks *cbs);

/* Destroy */
void lazy_list_free(LazyList *ll);

/* Get the GtkListView widget (to add as child of scroll window) */
GtkWidget *lazy_list_get_widget(LazyList *ll);

/* Get the GListStore for population */
GListStore *lazy_list_get_store(LazyList *ll);

/* Attach scroll monitoring to a GtkScrolledWindow */
void lazy_list_connect_scroll(LazyList *ll, GtkScrolledWindow *scroll);

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
    /* High-level actions */
    void (*on_navigate)(LibraryItemKind kind, int64_t id, gpointer data);
    void (*on_play)(const char *path, const char *title,
                    const char *artist, const char *album,
                    int64_t track_id, gpointer data);
    void (*on_back)(gpointer data);
    void (*on_album_loaded)(int64_t album_id, const LibraryTrackInfo *tracks,
                            size_t count, gpointer data);
    void (*on_track_info)(int64_t track_id, gpointer data);
    void (*on_load_to_channel)(int channel, int64_t track_id, gpointer data); /* 1-4 hotkeys */

    /* Row-level callbacks for each entity type */
    RowCallbacks track_cbs;        /* Search/list tracks: activate navigates to album */
    RowCallbacks album_track_cbs;  /* Album detail tracks: no activate (already viewing album) */
    RowCallbacks album_cbs;
    RowCallbacks artist_cbs;

    gpointer user_data;
} LibraryCallbacks;

/* Main list views */
GtkWidget *library_view_new(LibraryItemKind kind,
                             library_cache_t *cache,
                             ArtworkManager *art_mgr,
                             const LibraryCallbacks *cbs);

/* Unified Detail View - single widget handling empty/album/artist states */
GtkWidget *library_unified_detail_view_new(library_cache_t *cache,
                                            ArtworkManager *art_mgr,
                                            const LibraryCallbacks *cbs);

void library_unified_detail_navigate_to_artist(GtkWidget *view, int64_t artist_id,
                                                const char *source_view);
void library_unified_detail_navigate_to_album(GtkWidget *view, int64_t album_id,
                                               const char *source_view,
                                               int64_t select_track_id);

gboolean library_unified_detail_go_back(GtkWidget *view);
void library_unified_detail_clear_nav(GtkWidget *view);
DetailState library_unified_detail_get_state(GtkWidget *view);
GtkWidget *library_unified_detail_get_track_list(GtkWidget *view);

/* Refresh current view data */
void library_view_refresh(GtkWidget *view);

/* Clear all filters (genre/year/search text) and repopulate */
void library_view_clear_filters(GtkWidget *view);

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
