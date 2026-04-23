/**
 * Quadrature Library Module - Internal Header
 *
 * Single consolidated header for the library browsing subsystem.
 * All data is sourced from the foundation layer library_cache_t.
 */

#ifndef QUADRATURE_UI_LIBRARY_INTERNAL_H
#define QUADRATURE_UI_LIBRARY_INTERNAL_H

#include <gtk/gtk.h>
#include "quadrature/library.h"
#include "quadrature/settings.h"

struct PlaybackIntent;

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
 * Artwork Manager
 *
 * Async thumbnail loading: LRU texture cache + per-library mmapped atlases + worker pool.
 *
 * Album IDs must be global IDs: LIBRARY_MAKE_GLOBAL_ID(lib_index, local_id).
 * The manager decodes lib_index to route lookups to the correct per-library atlas.
 * Library 0 global IDs equal their local IDs (backward compatible for single-library use).
 *
 * Atlas files live at: {library_root}/artwork/{N}px-artwork-{unix_timestamp}.atlas
 * The manager always loads the file with the highest timestamp for each library.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _ArtworkManager ArtworkManager;

/**
 * Source descriptor for one library slot in the artwork manager.
 * Mirrors library_cache_source_t for consistent multi-library addressing.
 */
typedef struct {
    int bitmap_index;          /* Stable library ID (matches library_cache bitmap_index) */
    const char *data_root;     /* Library data root (DB, atlas files, fanart cache) */
    const char *music_root;    /* Music file root (for embedded art fallback); NULL = data_root */
} artwork_manager_source_t;

ArtworkManager *artwork_manager_new(library_cache_t *library,
                                    const artwork_manager_source_t *sources,
                                    int source_count,
                                    int thumb_size, size_t cache_count);
void artwork_manager_free(ArtworkManager *mgr);
void artwork_manager_get_thumbnail(ArtworkManager *mgr, int64_t album_id, GtkWidget *image);
int artwork_manager_get_thumb_size(ArtworkManager *mgr);

/** Set album thumbnail on a GtkImage (pixel size + async load). */
static inline void ui_set_album_thumbnail(ArtworkManager *mgr, GtkWidget *img, int64_t album_id) {
    if (!mgr || !img) return;
    gtk_image_set_pixel_size(GTK_IMAGE(img), artwork_manager_get_thumb_size(mgr));
    artwork_manager_get_thumbnail(mgr, album_id, img);
}
void artwork_manager_reload_library_atlas(ArtworkManager *mgr, int bitmap_index,
                                          const char *atlas_path);
void artwork_manager_add_library(ArtworkManager *mgr, int bitmap_index,
                                  const char *data_root, const char *music_root);
void artwork_manager_remove_library(ArtworkManager *mgr, int bitmap_index);

/* Load full-resolution album art directly from the album directory on disk.
 * Bypasses the thumbnail atlas — intended for detail views where the raw image
 * is displayed large and GTK handles the scaling via GtkPicture.
 * Async: queues work to a worker thread; adds "artwork-loading" CSS class to the
 * art container during load, then sets the texture via idle callback.
 * picture: must be a GtkPicture widget. */
void artwork_manager_get_fullsize_album_art(ArtworkManager *mgr,
                                             int64_t album_id, GtkWidget *picture);

void artwork_manager_get_artist_thumbnail(ArtworkManager *mgr, int64_t artist_id,
                                           GtkWidget *image);

/** Set artist thumbnail on a GtkImage (pixel size + async load from artist atlas). */
static inline void ui_set_artist_thumbnail(ArtworkManager *mgr, GtkWidget *img, int64_t artist_id) {
    if (!mgr || !img) return;
    gtk_image_set_pixel_size(GTK_IMAGE(img), artwork_manager_get_thumb_size(mgr));
    artwork_manager_get_artist_thumbnail(mgr, artist_id, img);
}

void artwork_manager_reload_artist_atlas(ArtworkManager *mgr);

/* Performance stats snapshot (for perf dashboard polling) */
#define ARTWORK_MANAGER_MAX_LIBRARIES 8
typedef struct {
    size_t texture_cache_count;                           /* LRU entries */
    size_t texture_cache_bytes;                           /* count × thumb² × 4 (RGBA) */
    size_t atlas_mmap_bytes[ARTWORK_MANAGER_MAX_LIBRARIES]; /* per-library mmap size */
    int    lib_count;
    size_t total_hits;
    size_t total_misses;
    size_t atlas_hits;
    size_t evictions;
    size_t pending_load_count;
} artwork_manager_stats_t;

void artwork_manager_get_stats(ArtworkManager *mgr, artwork_manager_stats_t *out);

/* Access latency histograms for perf dashboard (returns interior pointer — valid
 * for the lifetime of the ArtworkManager). The histogram uses µs-scale buckets. */
struct perf_histogram;  /* forward declare to avoid core/internal.h dependency */
typedef struct perf_histogram perf_histogram_us_t_fwd;
const void *artwork_manager_get_texture_hit_hist(ArtworkManager *mgr);
const void *artwork_manager_get_atlas_decode_hist(ArtworkManager *mgr);

/* ═══════════════════════════════════════════════════════════════════════════
 * QuadScrubber — Index scrubber overlay for library list views
 *
 * Slide-in widget overlaid at halign=END on a GtkOverlay wrapping the
 * list's GtkScrolledWindow. Shows A-Z or year buckets; click/drag scrolls
 * the list to that section.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define QUAD_TYPE_SCRUBBER (quad_scrubber_get_type())
G_DECLARE_FINAL_TYPE(QuadScrubber, quad_scrubber, QUAD, SCRUBBER, GtkWidget)

typedef struct {
    char  *label;    /* "A", "2019", "#", etc. */
    guint  position; /* first item index in GListStore */
    guint  count;    /* number of items in this bucket */
    int    label_w;  /* cached Pango pixel width  (set by quad_scrubber_set_buckets) */
    int    label_h;  /* cached Pango pixel height (set by quad_scrubber_set_buckets) */
    int    label_w_bold; /* cached bold-face pixel width  */
    int    label_h_bold; /* cached bold-face pixel height */
} ScrubberBucket;

void scrubber_bucket_free(ScrubberBucket *bucket);

GtkWidget *quad_scrubber_new(void);
void quad_scrubber_set_list_view(QuadScrubber *self, GtkListView *list_view);
void quad_scrubber_set_vadj     (QuadScrubber *self, GtkAdjustment *vadj);
void quad_scrubber_set_badge    (QuadScrubber *self, GtkWidget *badge);
void quad_scrubber_set_buckets  (QuadScrubber *self, GPtrArray *buckets); /* takes ownership */
void quad_scrubber_set_total    (QuadScrubber *self, guint total_items);
void quad_scrubber_clear        (QuadScrubber *self);

/* ═══════════════════════════════════════════════════════════════════════════
 * Gradient Widgets (gradient_widgets.c)
 *
 * GPU-accelerated gradient overlays for detail view backgrounds.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GdkRGBA top;
    GdkRGBA bottom;
} EdgeColors;

typedef struct {
    GdkRGBA edge_color;     /* Sampled from image edge */
    gboolean from_top;      /* TRUE = page bg at top, image at bottom */
} GradientData;

#define QUAD_TYPE_GRADIENT_FADE (quad_gradient_fade_get_type())
G_DECLARE_FINAL_TYPE(QuadGradientFade, quad_gradient_fade, QUAD, GRADIENT_FADE, GtkWidget)

struct _QuadGradientFade {
    GtkWidget parent;
    GradientData grad;
};

struct _QuadGradientFadeClass {
    GtkWidgetClass parent_class;
};

/* Sample average color from top and bottom N rows of a texture. */
EdgeColors sample_edge_colors(GdkTexture *texture, int num_rows);

/* Set gradient edge color and direction. */
void quad_gradient_fade_set_color(QuadGradientFade *self,
                                   const GdkRGBA *color, gboolean from_top);

/* ═══════════════════════════════════════════════════════════════════════════
 * Row Interaction Handlers
 *
 * Standard callbacks for row interactions. Used by all library row types.
 * Selection is handled by GTK's GtkSelectionModel (automatic).
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    void (*on_activate)(int64_t entity_id, gpointer data);  /* primary: double-click / Enter */
    void (*on_secondary)(int64_t entity_id, gpointer data); /* secondary: right-click */
    void (*on_mbid_navigate)(const char *mbid, const char *name, const char *type, gpointer data);
    gpointer user_data;
    int64_t suppress_id;        /* Artist ID to render inactive (0 = none) */
    const char *suppress_mbid;  /* MBID to render inactive (NULL = none) */
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

/* Get the filtered model (GtkFilterListModel output — the visible item sequence) */
GListModel *lazy_list_get_filtered_model(LazyList *ll);
GObject *lazy_list_get_selected_item(LazyList *ll);

/* Set/replace the filter (NULL = no filtering). Caller retains ownership. */
void lazy_list_set_filter(LazyList *ll, GtkFilter *filter);

/* Set/replace the sorter (NULL = no sorting). Caller retains ownership. */
void lazy_list_set_sorter(LazyList *ll, GtkSorter *sorter);

/* Attach scroll monitoring to a GtkScrolledWindow */
void lazy_list_connect_scroll(LazyList *ll, GtkScrolledWindow *scroll);

/* ═══════════════════════════════════════════════════════════════════════════
 * Unified Filter Bar
 *
 * Shared filter/sort UI used by Artists, Albums, and Search views.
 * Loads filter_sort_bar.ui (two-row layout with integrated role filter).
 * Owns genre/year/credit filter state and sort selection.
 * Notifies the consumer via on_changed callback when any filter changes.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Search mode for the filter bar search field */
typedef enum {
    FILTER_SEARCH_DEFAULT,     /* Plaintext search across names/titles */
    FILTER_SEARCH_METADATA,    /* Search credits, labels, metadata fields */
} FilterSearchMode;

/* Sort option descriptor for the sort dropdown */
typedef struct {
    const char *label;       /* Display text, e.g. "Name (A-Z)" */
    library_sort_t sort;     /* Enum value */
} FilterBarSortOption;

/* Callback fired when any filter or sort state changes */
typedef void (*filter_bar_changed_cb)(gpointer user_data);

typedef struct {
    /* Widgets (owned by GTK widget tree, not freed here) */
    GtkWidget *bar_widget;         /* Top-level GtkBox from filter_sort_bar.ui */
    GtkWidget *filter_genre;       /* GtkMenuButton */
    GtkWidget *filter_year;        /* GtkMenuButton */
    GtkWidget *filter_role;        /* GtkMenuButton (metadata mode facet) */
    GtkWidget *genre_box;          /* Container box for genre (swapped with role) */
    GtkWidget *year_box;           /* Container box for year (swapped with role) */
    GtkWidget *role_box;           /* Container box for role (hidden by default) */
    GtkWidget *filter_search;      /* GtkSearchEntry (NULL if hidden) */
    GtkWidget *filter_search_row;  /* Row 2 container (hidden in search view) */
    GtkWidget *filter_search_box;  /* Container box for search entry */
    GtkWidget *filter_clear;       /* GtkButton */
    GtkWidget *show_featuring_toggle; /* GtkToggleButton (Artists view only) */
    GtkWidget *sort_box;           /* Container box (hidden when no sort options) */
    GtkWidget *sort_dropdown;      /* GtkMenuButton */
    GtkWidget *search_mode_dropdown; /* GtkMenuButton (Default/Metadata) */

    /* Action groups for popover menus */
    GSimpleActionGroup *sort_actions;

    /* Filter state */
    GHashTable *selected_genres;   /* Set of selected genre strings (owned) */
    uint16_t selected_years_mask;  /* Bitmask: bit 0=2020s .. bit 7=Pre-1960 */
    uint32_t selected_roles_mask;  /* Bitmask: bit 0=All .. bit N=role N */
    guint filter_debounce_timer;
    FilterSearchMode search_mode;  /* Current search mode */

    /* Sort state */
    const FilterBarSortOption *sort_options; /* Array of options (static, not owned) */
    int sort_option_count;
    int current_sort_index;

    /* Genre list (for popover rebuild) */
    GPtrArray *genre_list;         /* Sorted unique genre strings (owned) */

    /* Consumer callback */
    filter_bar_changed_cb on_changed;
    gpointer on_changed_data;

    /* Library cache (for building genre popover) */
    library_cache_t *cache;
} FilterBarState;

/**
 * Initialize a FilterBarState by loading filter_sort_bar.ui.
 * sort_options: array of sort descriptors (NULL = hide sort dropdown).
 * sort_count: number of sort options.
 * on_changed: callback invoked when any filter/sort changes.
 * Returns the top-level GtkBox widget.
 */
GtkWidget *filter_bar_init(FilterBarState *fb, library_cache_t *cache,
                            const FilterBarSortOption *sort_options, int sort_count,
                            filter_bar_changed_cb on_changed, gpointer user_data);

/** Reset all filter state to defaults and invoke on_changed. */
void filter_bar_clear(FilterBarState *fb);

/** Free owned resources (hash tables, genre list, timers). */
void filter_bar_destroy(FilterBarState *fb);

/** Check if any filter is active (genre, year, search text, role, or search mode). */
gboolean filter_bar_is_active(const FilterBarState *fb);

/** Get the current sort enum value. */
library_sort_t filter_bar_get_sort(const FilterBarState *fb);

/** Get search text (NULL if empty or search field is hidden). */
const char *filter_bar_get_search_text(const FilterBarState *fb);

/** Get current search mode (Default or Metadata). */
FilterSearchMode filter_bar_get_search_mode(const FilterBarState *fb);

/** Get selected roles bitmask (0 = no filter / all roles). */
uint32_t filter_bar_get_selected_roles_mask(const FilterBarState *fb);

/**
 * Get GIDs of all selected roles as a NULL-terminated array.
 * Caller must g_free the returned array (not the strings — they are static).
 * Returns NULL if no roles selected.
 */
const char **filter_bar_get_selected_role_gids(const FilterBarState *fb, int *count_out);

/** Build db_search_opts_t from current genre/year state. Caller must g_free returned genres array. */
db_search_opts_t filter_bar_build_search_opts(const FilterBarState *fb, const char ***genres_out, size_t *genre_count_out);

/** Whether the "Show Featuring" toggle is active (TRUE = show all, default). */
gboolean filter_bar_get_show_featuring(const FilterBarState *fb);

/** Hide the search row (for search view which has its own primary search bar). */
void filter_bar_hide_search(FilterBarState *fb);

/** Set metadata mode — swaps Genre/Year for Role selector. */
void filter_bar_set_metadata_mode(FilterBarState *fb, gboolean metadata);

/** Rebuild genre popover from library cache (call after cache warming). */
void filter_bar_rebuild_genre_popover(FilterBarState *fb);

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
    void (*on_play)(const struct PlaybackIntent *intent, gpointer data);
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Navigation Stack - For detail view back navigation
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    NAV_ENTRY_VIEW,        /* Main view (Artists, Albums, Search) */
    NAV_ENTRY_ARTIST,      /* Artist detail */
    NAV_ENTRY_ALBUM,       /* Album detail */
    NAV_ENTRY_META_ARTIST, /* Metadata-only artist (credits only, no library presence) */
} NavEntryType;

typedef struct {
    NavEntryType type;
    int64_t id;              /* Artist/album ID (0 for main views) */
    char *view_name;         /* Source view name for back label */
    double scroll_pos;       /* Scroll position to restore */
    /* Meta artist fields (only for NAV_ENTRY_META_ARTIST) */
    char *meta_artist_mbid;
    char *meta_artist_name;
    char *meta_artist_type;
} NavEntry;

/* ═══════════════════════════════════════════════════════════════════════════
 * Unified Detail View - Single widget for empty/album/artist states
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    DETAIL_STATE_ALBUM,
    DETAIL_STATE_ARTIST,
    DETAIL_STATE_META_ARTIST,  /* Credits-only artist (no main DB counterpart) */
} DetailState;

/* Opaque type — see ui/row_helpers.c */
typedef struct _SelectionGroup SelectionGroup;

#define MAX_NAV_STACK 24

typedef struct {
    library_cache_t *cache;
    ArtworkManager *art_mgr;
    LibraryCallbacks cbs;
    app_settings_t *settings;
    uint32_t library_mask;      /* Current library filter bitmask */

    DetailState state;
    int64_t current_id;
    int64_t album_artist_id;
    int64_t merged_rep_album_id; /* Representative album ID when viewing a merged group (0 if none) */

    NavEntry nav_stack[MAX_NAV_STACK];
    int nav_depth;

    GtkWidget *container;
    GtkWidget *back_header;
    GtkWidget *back_button;
    GtkWidget *back_label;
    GtkWidget *header_artist_name;
    GtkWidget *content_stack;
    GtkWidget *album_header_spacer;
    GtkWidget *artist_header_spacer;
    gulong     header_height_signal;

    GtkWidget *album_card_container;
    GtkWidget *album_card_inner;

    GtkWidget *artist_name;
    GtkWidget *artist_stats;
    GtkWidget *artist_library_toggles;
    GtkWidget *albums_section;
    GtkWidget *artist_albums_container;
    GtkWidget *appears_on_section;
    GtkWidget *appears_on_albums_revealer;
    GtkWidget *appears_on_tracks_revealer;
    GtkWidget *appears_on_albums;
    GtkWidget *appears_on_tracks;
    GtkWidget *toggle_albums_btn;
    GtkWidget *toggle_tracks_btn;

    GtkWidget *artist_banner;
    GtkWidget *artist_banner_overlay;
    QuadGradientFade *artist_banner_fade_bottom;

    GtkWidget *about_section;
    GtkWidget *about_background_image;
    QuadGradientFade *about_fade_top;
    QuadGradientFade *about_fade_bottom;
    GtkWidget *about_bio_text;
    GtkWidget *about_wiki_link;
    char *about_wiki_url;

    SelectionGroup *sel_group;

    char *meta_artist_mbid;
    char *meta_artist_name;

    GCancellable *banner_cancel;  /* Cancels async banner load on navigation away */
    GCancellable *bio_bg_cancel;  /* Cancels async bio background load on navigation away */
} UnifiedDetailData;

/* detail_view.c — shared with credits_view.c */
void on_album_card_artist_navigate(GtkButton *btn, gpointer data);

/* Forward declaration — full definition in src/ui/internal.h */
typedef struct UiRowSizeGroups UiRowSizeGroups;

/* credits_view.c — info button wiring and MB credits popover builders */
void wire_info_buttons(GtkWidget *card, UnifiedDetailData *ud);
GHashTable *collect_credit_album_roles(UnifiedDetailData *ud,
                                        const char *artist_mbid,
                                        const char *artist_name,
                                        int64_t viewed_artist_id,
                                        GHashTable *skip_track_ids);
guint append_credit_rows(UnifiedDetailData *ud,
                         const char *artist_mbid,
                         const char *artist_name,
                         int64_t viewed_artist_id,
                         GHashTable *skip_track_ids,
                         GHashTable *skip_album_mbids,
                         UiRowSizeGroups *track_groups,
                         UiRowSizeGroups *album_groups);

/* Main list views */
GtkWidget *library_view_new(LibraryItemKind kind,
                             library_cache_t *cache,
                             ArtworkManager *art_mgr,
                             const LibraryCallbacks *cbs,
                             app_settings_t *settings);

/* Unified Detail View - single widget handling empty/album/artist states */
GtkWidget *library_unified_detail_view_new(library_cache_t *cache,
                                            ArtworkManager *art_mgr,
                                            const LibraryCallbacks *cbs,
                                            app_settings_t *settings);

void library_unified_detail_navigate_to_artist(GtkWidget *view, int64_t artist_id,
                                                const char *source_view);
void library_unified_detail_navigate_to_album(GtkWidget *view, int64_t album_id,
                                               const char *source_view,
                                               int64_t select_track_id);
void library_unified_detail_navigate_to_meta_artist(GtkWidget *view,
                                                      const char *artist_mbid,
                                                      const char *artist_name,
                                                      const char *artist_type);

gboolean library_unified_detail_go_back(GtkWidget *view);
void library_unified_detail_reload(GtkWidget *view);
void library_unified_detail_set_library_mask(GtkWidget *view, uint32_t mask);
void library_unified_detail_clear_nav(GtkWidget *view);
DetailState library_unified_detail_get_state(GtkWidget *view);
int64_t library_unified_detail_get_current_entity_id(GtkWidget *view);
GtkWidget *library_unified_detail_get_track_list(GtkWidget *view);

/* Refresh current view data */
void library_view_refresh(GtkWidget *view);

/* Get first_track_id of the selected album in a library view (0 if none) */
int64_t library_view_get_selected_track_id(GtkWidget *view);

/* Clear all filters (genre/year/search text) and repopulate */
void library_view_clear_filters(GtkWidget *view);

/* ═══════════════════════════════════════════════════════════════════════════
 * Errors View - Tree view of indexer errors by folder structure
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *errors_view_new(quadrature_db_t *db);
void errors_view_set_db(GtkWidget *view, quadrature_db_t *db);
void errors_view_refresh(GtkWidget *view);
void errors_view_set_callbacks(GtkWidget *view,
                                void (*on_path)(const char *path, gpointer data),
                                gpointer user_data);
size_t errors_view_get_count(GtkWidget *view);
void errors_view_set_path_filter(GtkWidget *view, const char *path_filter);

G_END_DECLS

#endif /* QUADRATURE_UI_LIBRARY_INTERNAL_H */
