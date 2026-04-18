/**
 * Quadrature Search View
 *
 * Search UI, debouncing, credit filtering, and result population.
 * Extracted from window.c for single-responsibility.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "quadrature/library_search.h"
#include <string.h>

static void clear_search_results(GtkWidget *list_box) {
    gtk_list_box_remove_all(GTK_LIST_BOX(list_box));
}

/* Section header creation moved to ui_make_section_header() in row_helpers.c */

/* GtkListBoxUpdateHeaderFunc for the search results list.
 * "quad-section" is stored on the content widget (first child of the row wrapper)
 * BEFORE gtk_list_box_append() is called, because the header_func fires
 * synchronously during the append — before any post-append data-setting would run.
 * Using the header_func API ensures GTK manages header widget clip/layout correctly. */
static void search_section_header_func(GtkListBoxRow *row,
                                        GtkListBoxRow *before,
                                        gpointer       data) {
    (void)data;
    /* Use gtk_list_box_row_get_child() — NOT get_first_child() — because
     * set_header() inserts the header before the child in the widget tree,
     * making get_first_child() return the header widget on re-invocation. */
    GtkWidget *child = gtk_list_box_row_get_child(row);
    const char *section = child ? g_object_get_data(G_OBJECT(child), "quad-section") : NULL;
    if (!section) return;

    const char *prev_section = NULL;
    if (before) {
        GtkWidget *prev_child = gtk_list_box_row_get_child(before);
        prev_section = prev_child ? g_object_get_data(G_OBJECT(prev_child), "quad-section") : NULL;
    }

    if (g_strcmp0(section, prev_section) != 0) {
        /* Reuse existing header if it already matches this section */
        GtkWidget *existing = gtk_list_box_row_get_header(row);
        if (existing) {
            const char *existing_section = g_object_get_data(G_OBJECT(existing), "quad-header-section");
            if (g_strcmp0(section, existing_section) == 0) return;
        }
        GtkWidget *header = ui_make_section_header(section);
        g_object_set_data_full(G_OBJECT(header), "quad-header-section",
                               g_strdup(section), g_free);
        gtk_list_box_row_set_header(row, header);
    } else {
        gtk_list_box_row_set_header(row, NULL);
    }
}


/* Forward declarations */
void do_search(UiWindow *w);

/* ═══════════════════════════════════════════════════════════════════════════
 * Search Filter State
 * ═══════════════════════════════════════════════════════════════════════════ */

void set_search_filter(UiWindow *w, int idx) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->filter_btns[idx]), TRUE);
    w->filter_active = idx;
    do_search(w);
}

static void on_filter_toggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn))
        return;
    UiWindow *w = UI_WINDOW(data);
    for (int i = 0; i < 4; i++) {
        if (GTK_WIDGET(btn) == w->filter_btns[i]) {
            w->filter_active = i;
            do_search(w);
            return;
        }
    }
}

static void on_metadata_toggled(GtkToggleButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    gboolean active = gtk_toggle_button_get_active(btn);
    filter_bar_set_metadata_mode(&w->search_filter_bar, active);
    do_search(w);
}

/* Helper: Focus search entry and select all text */
void focus_search_entry(UiWindow *w) {
    gtk_widget_grab_focus(w->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(w->search_entry), 0, -1);
}

/* ── Metadata search helpers ── */

/**
 * Check if metadata/credit search is active (metadata mode toggle on in search view,
 * or search mode set to Metadata in list views).
 */
static gboolean metadata_search_active(UiWindow *w) {
    if (w->filter_metadata_btn)
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->filter_metadata_btn));
    return filter_bar_get_search_mode(&w->search_filter_bar) == FILTER_SEARCH_METADATA;
}

/* ── Async Credit Search (GTask-based) ─────────────────────────────────── */

/* Input parameters for background credit search (copied from UI state) */
typedef struct {
    library_cache_t *cache;
    char *credit_text;
    char *role_gid;       /* first selected role GID, or NULL */
    uint32_t library_mask;
} CreditSearchInput;

static void credit_search_input_free(CreditSearchInput *in) {
    if (!in) return;
    g_free(in->credit_text);
    g_free(in->role_gid);
    g_free(in);
}

/* GTask worker thread: runs the credit search off the main thread. */
static void credit_search_thread(GTask *task, gpointer src,
                                  gpointer data, GCancellable *cancel) {
    (void)src;
    CreditSearchInput *in = data;

    if (g_cancellable_is_cancelled(cancel)) return;

    library_credit_search_result_t *result = library_credit_search(
        in->cache, in->credit_text, in->role_gid, in->library_mask);

    if (g_cancellable_is_cancelled(cancel)) {
        library_credit_search_result_free(result);
        return;
    }

    g_task_return_pointer(task, result,
        (GDestroyNotify)library_credit_search_result_free);
}

/* ── Display helpers for search results ── */

static void populate_search_artists(UiWindow *w, GPtrArray *artists) {
    if (!artists || artists->len == 0) return;

    UiRowSizeGroups artist_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
        .col2 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)
    };

    for (guint i = 0; i < artists->len; i++) {
        const library_artist_info_t *artist = g_ptr_array_index(artists, i);
        GtkWidget *row = ui_create_artist_row(artist, w->library_cache, w->artwork_mgr, TRUE, &artist_groups, w->library_mask);
        ui_row_attach_handlers(row, &w->lib_cbs.artist_cbs);
        g_object_set_data(G_OBJECT(row), "quad-section", (gpointer)"Artists");
        if (i == 0)
            gtk_widget_add_css_class(row, "library-row-first");
        if (i == artists->len - 1)
            gtk_widget_add_css_class(row, "library-row-last");
        gtk_list_box_append(GTK_LIST_BOX(w->search_results_list), row);
    }

    g_object_unref(artist_groups.col1);
    g_object_unref(artist_groups.col2);
}

static void populate_search_albums(UiWindow *w, GPtrArray *albums) {
    if (!albums || albums->len == 0) return;

    UiRowSizeGroups album_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
        .col2 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL)
    };

    for (guint i = 0; i < albums->len; i++) {
        const library_album_info_t *album = g_ptr_array_index(albums, i);
        GtkWidget *row = ui_create_album_row(album, w->library_cache, w->artwork_mgr, TRUE,
                                               &w->lib_cbs.artist_cbs, &album_groups, NULL);
        ui_row_attach_handlers(row, &w->lib_cbs.album_cbs);
        g_object_set_data(G_OBJECT(row), "quad-section", (gpointer)"Albums");
        if (i == 0)
            gtk_widget_add_css_class(row, "library-row-first");
        if (i == albums->len - 1)
            gtk_widget_add_css_class(row, "library-row-last");
        gtk_list_box_append(GTK_LIST_BOX(w->search_results_list), row);
    }

    g_object_unref(album_groups.col1);
    g_object_unref(album_groups.col2);
}

static void populate_search_tracks(UiWindow *w, GPtrArray *tracks,
                                    GHashTable *credit_info) {
    if (!tracks || tracks->len == 0) return;

    UiRowSizeGroups track_groups = {
        .col1 = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL),
    };

    for (guint i = 0; i < tracks->len; i++) {
        const library_track_info_t *track = g_ptr_array_index(tracks, i);

        /* Look up credit annotation if credit search is active */
        const UiTrackCreditInfo *credit = NULL;
        UiTrackCreditInfo credit_data;
        if (credit_info) {
            library_credit_info_t *ci = g_hash_table_lookup(credit_info, &track->track_id);
            if (ci) {
                /* Try to resolve MB artist to a library artist_id for navigation */
                int64_t resolved_artist_id = 0;
                if (ci->artist_mbid && w->library_cache) {
                    const GPtrArray *ta = library_cache_get_track_artists(
                        w->library_cache, track->track_id);
                    if (ta) {
                        for (guint j = 0; j < ta->len; j++) {
                            const library_track_artist_t *a = g_ptr_array_index(ta, j);
                            const library_artist_info_t *ai = library_cache_get_artist(
                                w->library_cache, a->artist_id, w->library_mask);
                            if (ai && ai->musicbrainz_id &&
                                g_strcmp0(ai->musicbrainz_id, ci->artist_mbid) == 0) {
                                resolved_artist_id = a->artist_id;
                                break;
                            }
                        }
                    }
                }
                /* Build NULL-terminated roles array — append sentinel once per row.
                 * Not idempotent across repeated reads of the same ci; callers
                 * treat roles as consumed. */
                if (ci->roles->len == 0 ||
                        g_ptr_array_index(ci->roles, ci->roles->len - 1) != NULL)
                    g_ptr_array_add(ci->roles, NULL);
                credit_data = (UiTrackCreditInfo){
                    .roles = (const char *const *)ci->roles->pdata,
                    .role_count = ci->roles->len - 1,  /* exclude sentinel */
                    .artist_name = ci->artist_name,
                    .artist_id = resolved_artist_id,
                    .artist_mbid = ci->artist_mbid,
                };
                credit = &credit_data;
            }
        }

        GtkWidget *row = ui_create_track_row(track, w->library_cache, w->artwork_mgr, TRUE,
                                               &w->lib_cbs.artist_cbs, &w->lib_cbs.album_cbs,
                                               &track_groups, credit);
        ui_row_attach_handlers(row, &w->lib_cbs.track_cbs);
        g_object_set_data(G_OBJECT(row), "quad-section", (gpointer)"Songs");
        if (i == 0)
            gtk_widget_add_css_class(row, "library-row-first");
        if (i == tracks->len - 1)
            gtk_widget_add_css_class(row, "library-row-last");
        gtk_list_box_append(GTK_LIST_BOX(w->search_results_list), row);
    }

    g_object_unref(track_groups.col1);
}

/**
 * Apply credit search results to the UI (main thread only).
 *
 * The result's track_ids / album_ids are already deduped cross-library by
 * library_credit_search (by RGID|disc|track for tracks, RGID for albums);
 * this function just resolves them to cache-owned info pointers for display.
 */
static void apply_search_with_credits(UiWindow *w,
                                       library_credit_search_result_t *result) {
    if (!result || result->track_ids->len == 0) {
        clear_search_results(w->search_results_list);
        gtk_widget_set_visible(w->search_results_list, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "No credit matches found");
        return;
    }

    GPtrArray *tracks = g_ptr_array_new();
    for (guint i = 0; i < result->track_ids->len; i++) {
        int64_t tid = g_array_index(result->track_ids, int64_t, i);
        const library_track_info_t *info = library_cache_get_track(w->library_cache, tid);
        if (info) g_ptr_array_add(tracks, (gpointer)info);
    }

    if (tracks->len == 0) {
        g_ptr_array_unref(tracks);
        clear_search_results(w->search_results_list);
        gtk_widget_set_visible(w->search_results_list, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "No credit matches found");
        return;
    }

    GPtrArray *albums = g_ptr_array_new();
    for (guint i = 0; i < result->album_ids->len; i++) {
        int64_t aid = g_array_index(result->album_ids, int64_t, i);
        const library_album_info_t *a =
            library_cache_get_album(w->library_cache, aid, w->library_mask);
        if (a) g_ptr_array_add(albums, (gpointer)a);
    }

    gtk_widget_set_visible(w->search_empty_label, FALSE);
    gtk_widget_set_visible(w->search_results_list, TRUE);
    clear_search_results(w->search_results_list);

    populate_search_albums(w, albums);
    populate_search_tracks(w, tracks, result->credit_info);

    g_ptr_array_unref(albums);
    g_ptr_array_unref(tracks);
}

/* GTask completion callback: credit search finished, apply results on main thread */
static void on_credit_search_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    UiWindow *w = UI_WINDOW(data);
    GError *error = NULL;
    library_credit_search_result_t *result = g_task_propagate_pointer(G_TASK(res), &error);

    if (!result) {
        /* Cancelled or failed — don't touch UI if cancelled (superseded by newer search) */
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("credit search failed: %s", error->message);
        g_clear_error(&error);
        return;
    }

    apply_search_with_credits(w, result);
    library_credit_search_result_free(result);
}

void do_search(UiWindow *w) {
    if (!w->library_cache) return;

    /* Cancel any in-flight credit search */
    if (w->credit_search_cancel) {
        g_cancellable_cancel(w->credit_search_cancel);
        g_clear_object(&w->credit_search_cancel);
    }

    const char *query = gtk_editable_get_text(GTK_EDITABLE(w->search_entry));
    gboolean has_main_query = query && strlen(query) >= 1;
    gboolean has_credit = metadata_search_active(w);

    if (!has_main_query && !has_credit) {
        clear_search_results(w->search_results_list);
        gtk_widget_set_visible(w->search_results_list, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "Type to search...");
        return;
    }

    if (has_credit) {
        /* ── Async credit search: dispatch to worker thread ── */
        int role_count = 0;
        const char **role_gids = filter_bar_get_selected_role_gids(
            &w->search_filter_bar, &role_count);

        CreditSearchInput *input = g_new0(CreditSearchInput, 1);
        input->cache = w->library_cache;
        input->credit_text = g_strdup(query);
        input->role_gid = (role_gids && role_count > 0) ? g_strdup(role_gids[0]) : NULL;
        input->library_mask = w->library_mask;
        g_free(role_gids);

        /* Show searching indicator */
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "Searching credits...");
        gtk_widget_set_visible(w->search_empty_label, TRUE);

        w->credit_search_cancel = g_cancellable_new();
        GTask *task = g_task_new(NULL, w->credit_search_cancel,
                                  on_credit_search_done, w);
        g_task_set_task_data(task, input, (GDestroyNotify)credit_search_input_free);
        g_task_run_in_thread(task, credit_search_thread);
        g_object_unref(task);
        return;
    }

    /* ── Non-credit search: runs synchronously (library_cache_search is fast) ── */

    library_search_filter_t filter = LIBRARY_SEARCH_FILTER_ALL;
    size_t limit = 0;
    switch (w->filter_active) {
        case 1: filter = LIBRARY_SEARCH_FILTER_ARTISTS; break;
        case 2: filter = LIBRARY_SEARCH_FILTER_ALBUMS; break;
        case 3: filter = LIBRARY_SEARCH_FILTER_TRACKS; break;
    }

    const char **genre_arr = NULL;
    size_t genre_count = 0;
    db_search_opts_t search_opts = filter_bar_build_search_opts(
        &w->search_filter_bar, &genre_arr, &genre_count);
    const db_search_opts_t *opts_ptr = (genre_count > 0 || search_opts.year_mask) ? &search_opts : NULL;

    library_search_results_t *results = library_cache_search(
        w->library_cache, query, filter, limit, opts_ptr,
        w->library_mask);
    g_free(genre_arr);

    if (!results) {
        gtk_widget_set_visible(w->search_results_list, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "Search failed");
        return;
    }

    gboolean has_artists = results->artists && results->artists->len > 0;
    gboolean has_albums = results->albums && results->albums->len > 0;
    gboolean has_tracks = results->tracks && results->tracks->len > 0;

    if (!has_artists && !has_albums && !has_tracks) {
        library_search_results_free(results);
        clear_search_results(w->search_results_list);
        gtk_widget_set_visible(w->search_results_list, FALSE);
        gtk_widget_set_visible(w->search_empty_label, TRUE);
        gtk_label_set_text(GTK_LABEL(w->search_empty_label), "No results found");
        return;
    }

    gtk_widget_set_visible(w->search_empty_label, FALSE);
    gtk_widget_set_visible(w->search_results_list, TRUE);
    clear_search_results(w->search_results_list);

    populate_search_artists(w, results->artists);
    populate_search_albums(w, results->albums);
    populate_search_tracks(w, results->tracks, NULL);

    library_search_results_free(results);
}

static gboolean on_search_debounce(gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    w->search_debounce_timer = 0;
    do_search(w);
    return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    UiWindow *w = UI_WINDOW(data);

    /* Cancel previous timer */
    if (w->search_debounce_timer) {
        g_source_remove(w->search_debounce_timer);
        w->search_debounce_timer = 0;
    }

    /* Start new debounce timer (200ms) */
    w->search_debounce_timer = g_timeout_add(200, on_search_debounce, w);
}

static void on_search_activate(GtkSearchEntry *entry, gpointer data) {
    (void)entry;
    UiWindow *w = UI_WINDOW(data);

    /* Cancel debounce and search immediately */
    if (w->search_debounce_timer) {
        g_source_remove(w->search_debounce_timer);
        w->search_debounce_timer = 0;
    }
    do_search(w);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Search View Key Handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Search entry: Down arrow moves focus to first selectable result */
static gboolean on_search_entry_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                             guint keycode, GdkModifierType state,
                                             gpointer data) {
    (void)ctl; (void)keycode; (void)state;
    UiWindow *w = UI_WINDOW(data);

    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (gtk_widget_get_visible(w->search_results_list)) {
            GtkWidget *child = gtk_widget_get_first_child(w->search_results_list);
            while (child) {
                if (GTK_IS_LIST_BOX_ROW(child) &&
                    gtk_list_box_row_get_selectable(GTK_LIST_BOX_ROW(child))) {
                    gtk_list_box_select_row(GTK_LIST_BOX(w->search_results_list),
                                           GTK_LIST_BOX_ROW(child));
                    gtk_widget_grab_focus(child);
                    return TRUE;
                }
                child = gtk_widget_get_next_sibling(child);
            }
        }
        return TRUE;
    }

    return FALSE;
}

static const char *SEARCH_SECTION_ORDER[] = { "Artists", "Albums", "Songs", NULL };

/* Select and focus the first selectable row whose "quad-section" matches
 * section_name. Returns TRUE if a row was found. */
static gboolean search_jump_to_section(UiWindow *w, const char *section_name) {
    GtkWidget *child = gtk_widget_get_first_child(w->search_results_list);
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child) &&
            gtk_list_box_row_get_selectable(GTK_LIST_BOX_ROW(child))) {
            const char *sec = g_object_get_data(G_OBJECT(child), "quad-section");
            if (g_strcmp0(sec, section_name) == 0) {
                gtk_list_box_select_row(GTK_LIST_BOX(w->search_results_list),
                                        GTK_LIST_BOX_ROW(child));
                gtk_widget_grab_focus(child);
                return TRUE;
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
    return FALSE;
}

/* Search results: Escape returns to search entry; Ctrl+Down/Up jumps sections */
static gboolean on_search_results_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                               guint keycode, GdkModifierType state,
                                               gpointer data) {
    (void)ctl; (void)keycode;
    UiWindow *w = UI_WINDOW(data);

    if (keyval == GDK_KEY_Escape) {
        focus_search_entry(w);
        return TRUE;
    }

    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down ||
         keyval == GDK_KEY_Up   || keyval == GDK_KEY_KP_Up)) {
        gboolean going_down = (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down);
        GtkListBoxRow *selected = gtk_list_box_get_selected_row(
            GTK_LIST_BOX(w->search_results_list));
        const char *current = selected
            ? g_object_get_data(G_OBJECT(selected), "quad-section")
            : NULL;

        /* Find the adjacent section in the ordered list */
        const char *target = NULL;
        for (int i = 0; SEARCH_SECTION_ORDER[i]; i++) {
            if (g_strcmp0(SEARCH_SECTION_ORDER[i], current) == 0) {
                target = going_down ? SEARCH_SECTION_ORDER[i + 1]
                                    : (i > 0 ? SEARCH_SECTION_ORDER[i - 1] : NULL);
                break;
            }
        }
        /* If nothing is selected yet, Ctrl+Down lands on the first section */
        if (!current && going_down)
            target = SEARCH_SECTION_ORDER[0];

        if (target)
            search_jump_to_section(w, target);
        return TRUE;
    }

    return FALSE;
}

/* Filter panel: Down arrow snaps focus to first selectable search result */
static gboolean on_filter_panel_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                             guint keycode, GdkModifierType state,
                                             gpointer data) {
    (void)ctl; (void)keycode; (void)state;
    UiWindow *w = UI_WINDOW(data);

    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (gtk_widget_get_visible(w->search_results_list)) {
            GtkWidget *child = gtk_widget_get_first_child(w->search_results_list);
            while (child) {
                if (GTK_IS_LIST_BOX_ROW(child) &&
                    gtk_list_box_row_get_selectable(GTK_LIST_BOX_ROW(child))) {
                    gtk_list_box_select_row(GTK_LIST_BOX(w->search_results_list),
                                           GTK_LIST_BOX_ROW(child));
                    gtk_widget_grab_focus(child);
                    return TRUE;
                }
                child = gtk_widget_get_next_sibling(child);
            }
        }
        return TRUE;
    }

    return FALSE;
}

/* Clear genre/year/text filters in the search view (preserves type toggle) */
void clear_search_view_filters(UiWindow *w) {
    gtk_editable_set_text(GTK_EDITABLE(w->search_entry), "");
    if (w->search_debounce_timer) {
        g_source_remove(w->search_debounce_timer);
        w->search_debounce_timer = 0;
    }
    if (w->filter_metadata_btn)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->filter_metadata_btn), FALSE);
    filter_bar_clear(&w->search_filter_bar);
    focus_search_entry(w);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * View Builders
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Filter bar on_changed callback for search view: triggers do_search */
static void on_search_filter_bar_changed(gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    do_search(w);
}

GtkWidget *make_search_view(UiWindow *w) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/search_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "search_view"));
    g_object_ref(view);

    /* Get widget references */
    w->search_entry = GTK_WIDGET(gtk_builder_get_object(builder, "search_entry"));
    w->filter_btns[0] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_all"));
    w->filter_btns[1] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_artists"));
    w->filter_btns[2] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_albums"));
    w->filter_btns[3] = GTK_WIDGET(gtk_builder_get_object(builder, "filter_songs"));
    w->filter_metadata_btn = GTK_WIDGET(gtk_builder_get_object(builder, "filter_metadata"));
    w->search_empty_label = GTK_WIDGET(gtk_builder_get_object(builder, "search_empty_label"));
    w->search_results_list = GTK_WIDGET(gtk_builder_get_object(builder, "search_results_list"));
    gtk_list_box_set_header_func(GTK_LIST_BOX(w->search_results_list),
                                 search_section_header_func, NULL, NULL);

    /* Smooth scroll for search results */
    GtkWidget *search_scroll = GTK_WIDGET(gtk_builder_get_object(builder, "search_scroll"));
    if (search_scroll)
        ui_smooth_scroll_attach(GTK_SCROLLED_WINDOW(search_scroll));

    /* Initialize shared filter bar (no sort dropdown for search view) */
    GtkWidget *filter_bar_widget = filter_bar_init(&w->search_filter_bar,
                                                     w->library_cache,
                                                     NULL, 0,
                                                     on_search_filter_bar_changed, w);
    filter_bar_hide_search(&w->search_filter_bar);

    /* Insert filter bar between top row and results.
     * Search view hides the filter bar's own search row (it has its own search entry). */
    GtkWidget *search_top_row = GTK_WIDGET(gtk_builder_get_object(builder, "search_top_row"));
    gtk_box_insert_child_after(GTK_BOX(view), filter_bar_widget, search_top_row);

    /* Connect signals */
    g_signal_connect(w->search_entry, "search-changed", G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->search_entry, "activate", G_CALLBACK(on_search_activate), w);
    g_signal_connect(w->search_results_list, "row-activated", G_CALLBACK(ui_list_box_row_activated), NULL);

    /* Group search-type toggles so only one can be active (indices 0-3) */
    for (int i = 1; i < 4; i++)
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(w->filter_btns[i]),
                                    GTK_TOGGLE_BUTTON(w->filter_btns[0]));
    for (int i = 0; i < 4; i++)
        g_signal_connect(w->filter_btns[i], "toggled", G_CALLBACK(on_filter_toggled), w);

    /* Metadata toggle (independent, not in radio group) */
    g_signal_connect(w->filter_metadata_btn, "toggled", G_CALLBACK(on_metadata_toggled), w);

    /* Key controller: Down arrow in search entry moves to results */
    GtkEventController *entry_key_ctl = gtk_event_controller_key_new();
    g_signal_connect(entry_key_ctl, "key-pressed", G_CALLBACK(on_search_entry_key_pressed), w);
    gtk_widget_add_controller(w->search_entry, entry_key_ctl);

    /* Key controller: Escape returns to search entry */
    GtkEventController *results_key_ctl = gtk_event_controller_key_new();
    g_signal_connect(results_key_ctl, "key-pressed", G_CALLBACK(on_search_results_key_pressed), w);
    gtk_widget_add_controller(w->search_results_list, results_key_ctl);

    /* Key controller: Down arrow in filter panel snaps to first search result (capture phase) */
    GtkEventController *filter_key_ctl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(filter_key_ctl, GTK_PHASE_CAPTURE);
    g_signal_connect(filter_key_ctl, "key-pressed", G_CALLBACK(on_filter_panel_key_pressed), w);
    gtk_widget_add_controller(w->search_filter_bar.bar_widget, filter_key_ctl);

    g_object_unref(builder);
    return view;
}
