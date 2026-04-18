/**
 * Quadrature Row Factories
 *
 * Create template-based list rows from LibraryCache data.
 * All row creation functions store entity IDs via g_object_set_data()
 * for handler access. Click handlers attached separately.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <string.h>

#define BADGE_SLOT_MAX 4     /* pre-allocated library badge labels per row */
#define ART_STRIP_MAX_THUMBS 6  /* max pre-allocated art strip slots */
#define ART_STRIP_SPACING 4     /* must match art_strip spacing in .ui template */

/* ═══════════════════════════════════════════════════════════════════════════
 * Pre-allocated Library Badges — setup once, rebind by text + visibility
 *
 * Same pattern as genre pills. Dedup logic runs inline (stack arrays),
 * then sets text on pre-allocated labels. No overflow box, no heap alloc.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Rebind pre-allocated badge labels in a badges box.
 *  Uses MBID index lookup for artists/albums to show cross-library presence.
 *  Precondition: badges_box has BADGE_SLOT_MAX children (GtkLabels). */

/* ═══════════════════════════════════════════════════════════════════════════
 * Art Strip Width-Aware Reflow
 *
 * Uniform item widths → pure integer division, no per-item measurement.
 * Shows the N most recent albums that fit the allocated width, capped at
 * ART_STRIP_MAX_THUMBS pre-allocated slots.
 *
 * Driven by ProportionalBox pre-allocate callback — receives the exact
 * pixel budget BEFORE the child's GtkBox vfunc lays out thumbnails.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Reflow pre-allocated art strip thumbnails to fit the given pixel budget.
 *  Toggles visibility on pre-allocated GtkImage slots — no widget lifecycle.
 *  Expects art_strip to carry "art-strip-album-count" and "art-strip-thumb-px"
 *  via g_object_set_data (set during rebind). */
static void art_strip_reflow(GtkWidget *art_strip, int budget) {
    guint album_count = GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(art_strip), "art-strip-album-count"));
    int thumb_px = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(art_strip), "art-strip-thumb-px"));

    if (album_count == 0 || thumb_px <= 0 || budget <= 0) {
        gtk_widget_set_visible(art_strip, album_count > 0 && thumb_px > 0);
        GtkWidget *img = gtk_widget_get_first_child(art_strip);
        while (img) {
            gtk_widget_set_visible(img, FALSE);
            img = gtk_widget_get_next_sibling(img);
        }
        return;
    }

    /* How many fit? First item has no leading spacing. */
    guint max_fit = (guint)(budget >= thumb_px
        ? 1 + (budget - thumb_px) / (thumb_px + ART_STRIP_SPACING)
        : 0);
    guint show = MIN(album_count, max_fit);

    /* Populated slots are 0..(album_count-1). Show the last `show` of those,
     * hide everything else (leading populated + trailing empty). */
    guint start_visible = album_count - show;
    GtkWidget *img = gtk_widget_get_first_child(art_strip);
    for (guint i = 0; img; i++, img = gtk_widget_get_next_sibling(img))
        gtk_widget_set_visible(img, i >= start_visible && i < album_count);

    gtk_widget_set_visible(art_strip, show > 0);
}

/** ProportionalBox pre-allocate callback for the art strip column. */
static void art_strip_pre_allocate(GtkWidget *col, int width, gpointer user_data) {
    (void)col;
    art_strip_reflow(GTK_WIDGET(user_data), width);
}

static void format_artist_subtitle(const library_artist_info_t *artist,
                                    library_cache_t *cache,
                                    uint32_t library_mask,
                                    char *buf, size_t len) {
    uint32_t albums = 0, appearances = 0;
    library_cache_get_merged_artist_counts(cache, artist->artist_id,
                                            library_mask, &albums, &appearances);
    if (albums > 0 && appearances > 0)
        snprintf(buf, len, "%u album%s \u00b7 Appears on %u track%s",
                 albums, albums == 1 ? "" : "s",
                 appearances, appearances == 1 ? "" : "s");
    else if (albums > 0)
        snprintf(buf, len, "%u album%s", albums, albums == 1 ? "" : "s");
    else if (appearances > 0)
        snprintf(buf, len, "Appears on %u track%s",
                 appearances, appearances == 1 ? "" : "s");
    else
        snprintf(buf, len, "No albums");
}

GtkWidget *ui_create_artist_row(const library_artist_info_t *artist,
                                 library_cache_t *cache,
                                 ArtworkManager *art_mgr,
                                 gboolean show_art_strip,
                                 UiRowSizeGroups *size_groups,
                                 uint32_t library_mask) {
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkBuilder *builder;
    GtkWidget *row = ui_builder_load("/org/quadrature/ui/library_artist_row.ui", "row", &builder);

    GtkWidget *artist_art = GTK_WIDGET(gtk_builder_get_object(builder, "artist_art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *subtitle = GTK_WIDGET(gtk_builder_get_object(builder, "subtitle"));
    GtkWidget *art_strip = GTK_WIDGET(gtk_builder_get_object(builder, "art_strip"));
    GtkWidget *col_left = GTK_WIDGET(gtk_builder_get_object(builder, "col_left"));
    GtkWidget *badges_box = GTK_WIDGET(gtk_builder_get_object(builder, "library_badges"));

    g_object_unref(builder);

    /* Load artist thumbnail from artist atlas */
    ui_set_artist_thumbnail(art_mgr, artist_art, artist->artist_id);

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), artist->name);
    }

    /* Library badges */
    if (badges_box && cache) {
        ui_populate_library_badges(badges_box, cache,
                                    artist->artist_id,
                                    BADGE_ENTITY_ARTIST,
                                    col_left);
    }

    if (subtitle) {
        char buf[64];
        format_artist_subtitle(artist, cache, library_mask, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(subtitle), buf);
    }

    if (art_strip) {
        if (show_art_strip && cache && art_mgr) {
            GPtrArray *albums = library_cache_get_albums_by_artist(cache, artist->artist_id, library_mask);
            guint album_count = 0;
            int thumb_px = 0;
            if (albums && albums->len > 0) {
                album_count = MIN(albums->len, ART_STRIP_MAX_THUMBS);
                thumb_px = artwork_manager_get_thumb_size(art_mgr);
                guint start_idx = albums->len > ART_STRIP_MAX_THUMBS
                                  ? albums->len - ART_STRIP_MAX_THUMBS : 0;
                for (guint i = start_idx; i < albums->len; i++) {
                    const library_album_info_t *album = g_ptr_array_index(albums, i);
                    GtkWidget *img = gtk_image_new();
                    gtk_image_set_pixel_size(GTK_IMAGE(img), thumb_px);
                    gtk_widget_add_css_class(img, "album-art-strip-thumb");
                    artwork_manager_get_thumbnail(art_mgr, album->album_id, img);
                    gtk_box_append(GTK_BOX(art_strip), img);
                }
            }
            g_clear_pointer(&albums, g_ptr_array_unref);
            g_object_set_data(G_OBJECT(art_strip), "art-strip-album-count",
                              GUINT_TO_POINTER(album_count));
            g_object_set_data(G_OBJECT(art_strip), "art-strip-thumb-px",
                              GINT_TO_POINTER(thumb_px));
            /* Pre-allocate callback reflows during ProportionalBox size_allocate */
            proportional_box_set_pre_allocate(
                QUADRATURE_PROPORTIONAL_BOX(row), "right",
                art_strip_pre_allocate, art_strip);
        } else {
            gtk_widget_set_visible(art_strip, FALSE);
        }
    }

    if (size_groups) {
        if (size_groups->col1 && title && subtitle) {
            gtk_size_group_add_widget(size_groups->col1, title);
            gtk_size_group_add_widget(size_groups->col1, subtitle);
        }
    }

    g_object_set_data(G_OBJECT(row), "artist-id", GSIZE_TO_POINTER((gsize)artist->artist_id));

    return row;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Artist Row Shell / Rebind (for GtkListView factory recycling)
 *
 * Shell creates the widget tree once (in setup). Rebind populates data
 * into the existing tree (in bind). Avoids re-parsing GtkBuilder XML and
 * allocating ~15 widgets on every scroll.
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *ui_create_artist_row_shell(void) {
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkBuilder *builder;
    GtkWidget *row = ui_builder_load("/org/quadrature/ui/library_artist_row.ui", "row", &builder);

    /* Store widget refs for fast access in rebind */
    g_object_set_data(G_OBJECT(row), "w-artist-art",
                      gtk_builder_get_object(builder, "artist_art"));
    g_object_set_data(G_OBJECT(row), "w-title",
                      gtk_builder_get_object(builder, "title"));
    GtkWidget *badges_box = GTK_WIDGET(gtk_builder_get_object(builder, "library_badges"));
    g_object_set_data(G_OBJECT(row), "w-library-badges", badges_box);
    g_object_set_data(G_OBJECT(row), "w-subtitle",
                      gtk_builder_get_object(builder, "subtitle"));

    GtkWidget *art_strip = GTK_WIDGET(gtk_builder_get_object(builder, "art_strip"));
    g_object_set_data(G_OBJECT(row), "w-art-strip", art_strip);

    /* Pre-allocate art strip image slots — reused in rebind, never destroyed */
    if (art_strip) {
        for (int i = 0; i < ART_STRIP_MAX_THUMBS; i++) {
            GtkWidget *img = gtk_image_new();
            gtk_widget_add_css_class(img, "album-art-strip-thumb");
            gtk_widget_set_visible(img, FALSE);
            gtk_box_append(GTK_BOX(art_strip), img);
        }
        /* Register pre-allocate on ProportionalBox — reflows every size_allocate */
        proportional_box_set_pre_allocate(
            QUADRATURE_PROPORTIONAL_BOX(row), "right",
            art_strip_pre_allocate, art_strip);
    }

    g_object_unref(builder);
    return row;
}

void ui_rebind_artist_row(GtkWidget *row,
                           const library_artist_info_t *artist,
                           library_cache_t *cache,
                           ArtworkManager *art_mgr,
                           uint32_t library_mask) {
    GtkWidget *artist_art = g_object_get_data(G_OBJECT(row), "w-artist-art");
    GtkWidget *title      = g_object_get_data(G_OBJECT(row), "w-title");
    GtkWidget *badges_box = g_object_get_data(G_OBJECT(row), "w-library-badges");
    GtkWidget *subtitle   = g_object_get_data(G_OBJECT(row), "w-subtitle");
    GtkWidget *art_strip  = g_object_get_data(G_OBJECT(row), "w-art-strip");

    /* Artist thumbnail */
    ui_set_artist_thumbnail(art_mgr, artist_art, artist->artist_id);

    /* Title */
    if (title)
        gtk_label_set_text(GTK_LABEL(title), artist->name);

    /* Library badges */
    ui_populate_library_badges(badges_box, cache,
                               artist->artist_id, BADGE_ENTITY_ARTIST, NULL);

    /* Subtitle: album/track counts */
    if (subtitle) {
        char buf[64];
        format_artist_subtitle(artist, cache, library_mask, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(subtitle), buf);
    }

    /* Art strip: populate pre-allocated slots with album data, reflow for width */
    if (art_strip) {
        guint album_count = 0;
        int thumb_px = 0;
        if (cache && art_mgr) {
            GPtrArray *albums = library_cache_get_albums_by_artist(cache, artist->artist_id, library_mask);
            if (albums && albums->len > 0) {
                album_count = MIN(albums->len, ART_STRIP_MAX_THUMBS);
                thumb_px = artwork_manager_get_thumb_size(art_mgr);
                /* Fill slots from the end: most recent albums in rightmost slots.
                 * Reflow will hide leading slots that don't fit the width. */
                guint start_idx = albums->len > ART_STRIP_MAX_THUMBS
                                  ? albums->len - ART_STRIP_MAX_THUMBS : 0;
                guint slot = 0;
                GtkWidget *img = gtk_widget_get_first_child(art_strip);
                for (guint i = start_idx; i < albums->len && img; i++, slot++) {
                    const library_album_info_t *album = g_ptr_array_index(albums, i);
                    gtk_image_set_pixel_size(GTK_IMAGE(img), thumb_px);
                    artwork_manager_get_thumbnail(art_mgr, album->album_id, img);
                    img = gtk_widget_get_next_sibling(img);
                }
                /* Hide any trailing unused slots */
                for (; img; img = gtk_widget_get_next_sibling(img))
                    gtk_widget_set_visible(img, FALSE);
            }
            g_clear_pointer(&albums, g_ptr_array_unref);
        }
        /* Store metadata for reflow (used by art_strip_reflow via pre-allocate) */
        g_object_set_data(G_OBJECT(art_strip), "art-strip-album-count",
                          GUINT_TO_POINTER(album_count));
        g_object_set_data(G_OBJECT(art_strip), "art-strip-thumb-px",
                          GINT_TO_POINTER(thumb_px));
        /* Pre-allocate callback runs during ProportionalBox size_allocate —
         * no manual reflow needed here. */
    }

    /* Update entity ID */
    g_object_set_data(G_OBJECT(row), "artist-id", GSIZE_TO_POINTER((gsize)artist->artist_id));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Album Row Shell / Rebind (for GtkListView factory recycling)
 * ═══════════════════════════════════════════════════════════════════════════ */

GtkWidget *ui_create_album_row_shell(void) {
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkBuilder *builder;
    GtkWidget *row = ui_builder_load("/org/quadrature/ui/library_album_row.ui", "row", &builder);

    /* Store widget refs for fast access in rebind */
    g_object_set_data(G_OBJECT(row), "w-art",
                      gtk_builder_get_object(builder, "art"));
    g_object_set_data(G_OBJECT(row), "w-title",
                      gtk_builder_get_object(builder, "title"));
    g_object_set_data(G_OBJECT(row), "w-count",
                      gtk_builder_get_object(builder, "count"));
    g_object_set_data(G_OBJECT(row), "w-year",
                      gtk_builder_get_object(builder, "year"));
    GtkWidget *primary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "primary_artists_box"));
    g_object_set_data(G_OBJECT(row), "w-primary-artists", primary_artists_box);
    GtkWidget *col_right = GTK_WIDGET(gtk_builder_get_object(builder, "col_right"));
    g_object_set_data(G_OBJECT(row), "w-col-right", col_right);

    /* Replace template GtkBox with QuadratureOverflowBox for genre pills */
    GtkWidget *template_genres = GTK_WIDGET(gtk_builder_get_object(builder, "genres_box"));
    GtkWidget *genres_box = ui_genre_pills_new(4);
    if (col_right && template_genres) {
        gtk_box_insert_child_after(GTK_BOX(col_right), genres_box, NULL);
        gtk_box_remove(GTK_BOX(col_right), template_genres);
    }
    g_object_set_data(G_OBJECT(row), "w-genres", genres_box);
    GtkWidget *badges_box = GTK_WIDGET(gtk_builder_get_object(builder, "library_badges"));
    g_object_set_data(G_OBJECT(row), "w-library-badges", badges_box);
    g_object_set_data(G_OBJECT(row), "w-credit-annotation",
                      gtk_builder_get_object(builder, "credit_annotation"));

    /* Pre-allocate artist button + label — toggled in rebind, never destroyed */
    if (primary_artists_box) {
        GtkWidget *btn = gtk_button_new_with_label("");
        gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
        gtk_widget_add_css_class(btn, "artist-btn");
        gtk_widget_set_visible(btn, FALSE);
        gtk_box_append(GTK_BOX(primary_artists_box), btn);
        g_object_set_data(G_OBJECT(row), "w-artist-btn", btn);

        GtkWidget *lbl = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(lbl, "library-row-subtitle");
        gtk_widget_set_visible(lbl, FALSE);
        gtk_box_append(GTK_BOX(primary_artists_box), lbl);
        g_object_set_data(G_OBJECT(row), "w-artist-label", lbl);
    }

    g_object_unref(builder);
    return row;
}

void ui_rebind_album_row(GtkWidget *row,
                          const library_album_info_t *album,
                          library_cache_t *cache,
                          ArtworkManager *art_mgr,
                          gboolean show_count,
                          RowCallbacks *artist_cbs) {
    GtkWidget *art                = g_object_get_data(G_OBJECT(row), "w-art");
    GtkWidget *title              = g_object_get_data(G_OBJECT(row), "w-title");
    GtkWidget *count              = g_object_get_data(G_OBJECT(row), "w-count");
    GtkWidget *year               = g_object_get_data(G_OBJECT(row), "w-year");
    GtkWidget *primary_artists_box = g_object_get_data(G_OBJECT(row), "w-primary-artists");
    GtkWidget *genres_box         = g_object_get_data(G_OBJECT(row), "w-genres");
    GtkWidget *badges_box         = g_object_get_data(G_OBJECT(row), "w-library-badges");
    GtkWidget *credit_annotation  = g_object_get_data(G_OBJECT(row), "w-credit-annotation");

    /* Album art */
    ui_set_album_thumbnail(art_mgr, art, album->album_id);

    /* Title */
    if (title)
        gtk_label_set_text(GTK_LABEL(title), album->title);

    /* Track count */
    if (count) {
        if (show_count && album->track_count > 0) {
            char buf[16];
            ui_format_track_count(buf, sizeof(buf), album->track_count);
            gtk_label_set_text(GTK_LABEL(count), buf);
            gtk_widget_set_visible(count, TRUE);
        } else {
            gtk_widget_set_visible(count, FALSE);
        }
    }

    /* Year */
    ui_set_year_label(year, album->year);

    /* Primary artist: reuse pre-allocated button/label, toggle visibility */
    if (primary_artists_box) {
        GtkWidget *artist_btn   = g_object_get_data(G_OBJECT(row), "w-artist-btn");
        GtkWidget *artist_label = g_object_get_data(G_OBJECT(row), "w-artist-label");

        gboolean has_name = album->artist_name && album->artist_name[0];
        gboolean is_va    = has_name && ui_is_various_artists(album->artist_name);
        gboolean use_btn  = has_name && artist_cbs && !is_va;

        if (artist_btn) {
            if (use_btn) {
                gtk_button_set_label(GTK_BUTTON(artist_btn), album->artist_name);
                g_object_set_data(G_OBJECT(artist_btn), "artist-id",
                                  GSIZE_TO_POINTER((gsize)album->artist_id));
                /* Connect handler once (idempotent: check for existing) */
                if (!g_object_get_data(G_OBJECT(artist_btn), "artist-callbacks")) {
                    RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
                    *cbs_copy = *artist_cbs;
                    g_object_set_data_full(G_OBJECT(artist_btn), "artist-callbacks",
                                           cbs_copy, g_free);
                    g_signal_connect(artist_btn, "clicked",
                                     G_CALLBACK(on_artist_button_clicked), cbs_copy);
                }
            }
            gtk_widget_set_visible(artist_btn, use_btn);
        }
        if (artist_label) {
            if (has_name && !use_btn) {
                gtk_label_set_text(GTK_LABEL(artist_label), album->artist_name);
                if (is_va)
                    gtk_widget_add_css_class(artist_label, "text-dim");
                else
                    gtk_widget_remove_css_class(artist_label, "text-dim");
            }
            gtk_widget_set_visible(artist_label, has_name && !use_btn);
        }
    }

    /* Genre pills — set text on pre-allocated labels */
    ui_genre_pills_bind(genres_box, album->genres);

    /* Library badges */
    if (cache) {
        ui_populate_library_badges(badges_box, cache,
                                   album->album_id, BADGE_ENTITY_ALBUM, NULL);
    }

    /* Credit annotation: clear for list rows (credits only used in detail views) */
    if (credit_annotation) {
        quadrature_overflow_box_clear_all(QUADRATURE_OVERFLOW_BOX(credit_annotation));
        gtk_widget_set_visible(credit_annotation, FALSE);
    }

    /* Update entity IDs */
    g_object_set_data(G_OBJECT(row), "album-id", GSIZE_TO_POINTER((gsize)album->album_id));

    /* First track ID for keyboard shortcuts — cached on album info during warming */
    g_object_set_data(G_OBJECT(row), "first-track-id",
                      GSIZE_TO_POINTER((gsize)album->first_track_id));
}

GtkWidget *ui_create_album_row(const library_album_info_t *album,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_count,
                                RowCallbacks *artist_cbs,
                                UiRowSizeGroups *size_groups,
                                const UiAlbumCreditInfo *credit) {
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkBuilder *builder;
    GtkWidget *row = ui_builder_load("/org/quadrature/ui/library_album_row.ui", "row", &builder);

    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *col_left = GTK_WIDGET(gtk_builder_get_object(builder, "col_left"));
    GtkWidget *col_right = GTK_WIDGET(gtk_builder_get_object(builder, "col_right"));
    GtkWidget *badges_box = GTK_WIDGET(gtk_builder_get_object(builder, "library_badges"));
    GtkWidget *primary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "primary_artists_box"));
    GtkWidget *count = GTK_WIDGET(gtk_builder_get_object(builder, "count"));
    GtkWidget *year = GTK_WIDGET(gtk_builder_get_object(builder, "year"));
    GtkWidget *template_genres = GTK_WIDGET(gtk_builder_get_object(builder, "genres_box"));
    GtkWidget *credit_annotation = GTK_WIDGET(gtk_builder_get_object(builder, "credit_annotation"));

    g_object_unref(builder);

    /* Replace template GtkBox with QuadratureOverflowBox for genre pills */
    GtkWidget *genres_box = ui_genre_pills_new(4);
    if (col_right && template_genres) {
        gtk_box_insert_child_after(GTK_BOX(col_right), genres_box, NULL);
        gtk_box_remove(GTK_BOX(col_right), template_genres);
    }

    /* Load album art */
    ui_set_album_thumbnail(art_mgr, art, album->album_id);

    /* Album title (top row) */
    if (title) {
        gtk_label_set_text(GTK_LABEL(title), album->title);
    }

    /* Top-right metadata: track count and year */
    if (count) {
        if (show_count && album->track_count > 0) {
            char buf[16];
            ui_format_track_count(buf, sizeof(buf), album->track_count);
            gtk_label_set_text(GTK_LABEL(count), buf);
        } else {
            gtk_widget_set_visible(count, FALSE);
        }
    }

    ui_set_year_label(year, album->year);

    /* Bottom-left: primary artist - clickable button or dimmed label */
    if (primary_artists_box && album->artist_name && album->artist_name[0]) {
        if (artist_cbs && !ui_is_various_artists(album->artist_name)) {
            /* Clickable artist button for navigation */
            GtkWidget *btn = create_artist_button(album->artist_id, album->artist_name, artist_cbs);
            gtk_box_append(GTK_BOX(primary_artists_box), btn);
        } else {
            /* Plain label for Various Artists or when no callbacks */
            GtkWidget *artist_label = gtk_label_new(album->artist_name);
            gtk_label_set_xalign(GTK_LABEL(artist_label), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(artist_label), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(artist_label, "library-row-subtitle");
            if (ui_is_various_artists(album->artist_name))
                gtk_widget_add_css_class(artist_label, "text-dim");
            gtk_box_append(GTK_BOX(primary_artists_box), artist_label);
        }
    }

    /* Bottom-right: genre pills */
    ui_genre_pills_bind(genres_box, album->genres);

    /* Add to size groups for column alignment */
    if (size_groups) {
        if (size_groups->col1 && title)
            gtk_size_group_add_widget(size_groups->col1, title);
    }

    /* Library badges — bottom-left, consistent across all row types */
    if (badges_box && cache) {
        ui_populate_library_badges(badges_box, cache,
                                    album->album_id,
                                    BADGE_ENTITY_ALBUM,
                                    col_left);
    }

    /* Credit annotation: artist button (pinned) + role pills (items) + "…" */
    if (credit_annotation && credit && credit->artist_name && credit->role_count > 0) {
        QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(credit_annotation);
        quadrature_overflow_box_clear_all(ofb);

        GtkWidget *btn;
        if (credit->artist_id > 0 && artist_cbs) {
            btn = create_artist_button(credit->artist_id,
                                       credit->artist_name, artist_cbs);
        } else {
            btn = create_artist_button(0, credit->artist_name, NULL);
            gtk_widget_set_sensitive(btn, FALSE);
        }
        quadrature_overflow_box_append(ofb, btn);
        quadrature_overflow_box_set_pinned_count(ofb, 1);

        populate_credit_pills(credit_annotation,
                              (const char *const *)credit->roles,
                              credit->role_count);

        gtk_widget_set_visible(credit_annotation, TRUE);
    }

    /* Store album ID for handler access */
    g_object_set_data(G_OBJECT(row), "album-id", GSIZE_TO_POINTER((gsize)album->album_id));

    /* First track ID for keyboard shortcuts — cached on album info */
    g_object_set_data(G_OBJECT(row), "first-track-id",
                      GSIZE_TO_POINTER((gsize)album->first_track_id));

    return row;
}

GtkWidget *ui_create_track_row(const library_track_info_t *track,
                                library_cache_t *cache,
                                ArtworkManager *art_mgr,
                                gboolean show_album_info,
                                RowCallbacks *artist_cbs,
                                RowCallbacks *album_cbs,
                                UiRowSizeGroups *size_groups,
                                const UiTrackCreditInfo *credit) {
    /* Ensure QuadratureProportionalBox is registered before GtkBuilder runs */
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkBuilder *builder;
    GtkWidget *row = ui_builder_load("/org/quadrature/ui/library_track_row.ui", "row", &builder);

    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "art"));
    GtkWidget *col_left = GTK_WIDGET(gtk_builder_get_object(builder, "col_left"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *album_box = GTK_WIDGET(gtk_builder_get_object(builder, "album_box"));
    GtkWidget *primary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "primary_artists_box"));
    GtkWidget *secondary_artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "secondary_artists_box"));
    GtkWidget *year = GTK_WIDGET(gtk_builder_get_object(builder, "year"));
    GtkWidget *duration = GTK_WIDGET(gtk_builder_get_object(builder, "duration"));
    GtkWidget *badges_box = GTK_WIDGET(gtk_builder_get_object(builder, "library_badges"));
    GtkWidget *credit_annotation = GTK_WIDGET(gtk_builder_get_object(builder, "credit_annotation"));
    GtkWidget *credit_artist_btn = GTK_WIDGET(gtk_builder_get_object(builder, "credit_artist_btn"));
    GtkWidget *credit_artist_label = GTK_WIDGET(gtk_builder_get_object(builder, "credit_artist_label"));

    g_object_unref(builder);


    /* Load album art */
    ui_set_album_thumbnail(art_mgr, art, track->album_id);

    /* Title */
    if (title) {
        gtk_label_set_text(GTK_LABEL(title), track->title);
    }

    /* Album button (if show_album_info enabled) */
    if (album_box) {
        if (show_album_info && track->album_title && track->album_title[0]) {
            GtkBuilder *album_builder;
            GtkWidget *album_btn = ui_builder_load("/org/quadrature/ui/album_button.ui", "album_btn", &album_builder);
            GtkWidget *album_label = GTK_WIDGET(gtk_builder_get_object(album_builder, "album_label"));
            g_object_unref(album_builder);

            if (album_label) {
                gtk_label_set_text(GTK_LABEL(album_label), track->album_title);
            }

            /* Store album ID */
            g_object_set_data(G_OBJECT(album_btn), "album-id", GSIZE_TO_POINTER((gsize)track->album_id));

            /* Attach click handler */
            if (album_cbs) {
                RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
                *cbs_copy = *album_cbs;
                g_object_set_data_full(G_OBJECT(album_btn), "album-callbacks", cbs_copy, g_free);
                g_signal_connect(album_btn, "clicked", G_CALLBACK(on_album_button_clicked), cbs_copy);
            }

            gtk_box_append(GTK_BOX(album_box), album_btn);
            gtk_widget_set_visible(album_box, TRUE);
        } else {
            gtk_widget_set_visible(album_box, FALSE);
        }
    }

    /* Populate artist buttons — pass (cache, track_id) as a lookup key so
     * the tick/map callbacks always re-fetch from the live cache rather than
     * holding a raw pointer freed by library_cache_clear().
     * populate_artist_buttons() hides the box automatically when no
     * artists of the requested role are found.
     *
     * Constraint widgets: col_left / col_right receive hard allocations from
     * ProportionalBox.  The inner artist boxes are nested deeper, so their
     * own gtk_widget_get_width() may not reflect the real space budget until
     * after the first layout pass.  Passing the column slot as the constraint
     * ensures the overflow logic uses the correct width from frame one. */
    if (primary_artists_box) {
        populate_artist_buttons(primary_artists_box, cache, track->track_id,
                               LIBRARY_ARTIST_ROLE_PRIMARY, artist_cbs, FALSE);
    }

    if (secondary_artists_box) {
        populate_artist_buttons(secondary_artists_box, cache, track->track_id,
                               LIBRARY_ARTIST_ROLE_FEATURING, artist_cbs, TRUE);
    }

    /* Credit annotation (optional — for artist detail / credit search views) */
    if (credit_annotation && credit && credit->roles && credit->role_count > 0 && credit->artist_name) {
        if (credit_artist_label)
            gtk_label_set_text(GTK_LABEL(credit_artist_label), credit->artist_name);

        /* Wire artist button callback: prefer library ID, fall back to MBID */
        if (credit_artist_btn) {
            gboolean suppressed = FALSE;

            /* Suppress if this credit artist is the one we're currently viewing */
            if (artist_cbs && credit->artist_id > 0 &&
                artist_cbs->suppress_id == credit->artist_id) {
                suppressed = TRUE;
            } else if (artist_cbs && credit->artist_mbid &&
                       artist_cbs->suppress_mbid &&
                       g_strcmp0(credit->artist_mbid, artist_cbs->suppress_mbid) == 0) {
                suppressed = TRUE;
            }

            if (suppressed) {
                gtk_widget_set_sensitive(credit_artist_btn, FALSE);
            } else if (credit->artist_id > 0 && artist_cbs && artist_cbs->on_activate) {
                g_object_set_data(G_OBJECT(credit_artist_btn), "artist-id",
                                  GSIZE_TO_POINTER((gsize)credit->artist_id));
                RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
                *cbs_copy = *artist_cbs;
                g_object_set_data_full(G_OBJECT(credit_artist_btn), "artist-callbacks",
                                       cbs_copy, g_free);
                g_signal_connect(credit_artist_btn, "clicked",
                                 G_CALLBACK(on_artist_button_clicked), cbs_copy);
            } else if (credit->artist_mbid && artist_cbs && artist_cbs->on_mbid_navigate) {
                g_object_set_data_full(G_OBJECT(credit_artist_btn), "artist-mbid",
                                       g_strdup(credit->artist_mbid), g_free);
                g_object_set_data_full(G_OBJECT(credit_artist_btn), "artist-name",
                                       g_strdup(credit->artist_name), g_free);
                if (credit->artist_type)
                    g_object_set_data_full(G_OBJECT(credit_artist_btn), "artist-type",
                                           g_strdup(credit->artist_type), g_free);
                RowCallbacks *cbs_copy = g_new0(RowCallbacks, 1);
                *cbs_copy = *artist_cbs;
                g_object_set_data_full(G_OBJECT(credit_artist_btn), "artist-callbacks",
                                       cbs_copy, g_free);
                g_signal_connect(credit_artist_btn, "clicked",
                                 G_CALLBACK(on_credit_mbid_navigate), cbs_copy);
            } else {
                gtk_widget_set_sensitive(credit_artist_btn, FALSE);
            }
        }

        /* Role pills as items + "…" overflow — the credit_artist_btn is
         * pinned[0] declared in the track row template. */
        populate_credit_pills(credit_annotation,
                              (const char *const *)credit->roles,
                              credit->role_count);

        gtk_widget_set_visible(credit_annotation, TRUE);
    }

    /* Year */
    if (year) {
        if (track->year > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", track->year);
            gtk_label_set_text(GTK_LABEL(year), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(year), "");
        }
    }

    /* Duration */
    if (duration) {
        char buf[16];
        ui_format_duration(track->duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(duration), buf);
    }

    /* Add to size groups for column alignment */
    if (size_groups) {
        if (size_groups->col1 && title && primary_artists_box) {
            gtk_size_group_add_widget(size_groups->col1, title);
            gtk_size_group_add_widget(size_groups->col1, primary_artists_box);
        }
        /* col2 intentionally unused: ProportionalBox allocates col_right to
         * exactly (1 - left_ratio) of flexible space on every row, so a
         * horizontal size group here is both redundant and would fight the
         * widget's own size_allocate, causing visible layout jitter. */
    }

    /* Library badges — bottom-left, consistent with album/artist rows */
    if (badges_box && cache) {
        ui_populate_library_badges(badges_box, cache,
                                    track->track_id,
                                    BADGE_ENTITY_TRACK,
                                    col_left);
    }

    /* Store track ID for handler access (path resolved on demand) */
    g_object_set_data(G_OBJECT(row), "track-id", GSIZE_TO_POINTER((gsize)track->track_id));

    return row;
}

GtkWidget *ui_create_album_detail_track_item(const library_track_info_t *track,
                                               library_cache_t *cache,
                                               RowCallbacks *artist_cbs,
                                               int64_t album_artist_id) {
    g_type_ensure(QUADRATURE_TYPE_PROPORTIONAL_BOX);

    GtkBuilder *builder;
    GtkWidget *row = ui_builder_load("/org/quadrature/ui/album_detail_track_item.ui", "row", &builder);

    GtkWidget *track_num = GTK_WIDGET(gtk_builder_get_object(builder, "track_num"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "title"));
    GtkWidget *artists_box = GTK_WIDGET(gtk_builder_get_object(builder, "artists_box"));
    GtkWidget *duration = GTK_WIDGET(gtk_builder_get_object(builder, "duration"));
    GtkWidget *info_btn = GTK_WIDGET(gtk_builder_get_object(builder, "info_btn"));

    g_object_unref(builder);

    if (track_num) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", track->track_num);
        gtk_label_set_text(GTK_LABEL(track_num), buf);
    }

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), track->title);
    }

    /* Combined artists box: primary buttons + "ft" + featuring buttons.
     * Suppress primary artist when it matches the album artist — the album
     * header already shows who the album artist is.  Only show non-album-
     * artist primaries (e.g. "Baths" on an ODESZA album). */
    if (artists_box) {
        gboolean show_primary = (album_artist_id == 0) ||
                                (track->artist_id != album_artist_id);
        populate_artist_buttons_combined(artists_box, cache,
                                         track->track_id, artist_cbs,
                                         show_primary);
    }

    if (duration) {
        char buf[16];
        ui_format_duration(track->duration_ms, buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(duration), buf);
    }

    /* Store track data for handlers (path resolved on demand via track_id) */
    g_object_set_data(G_OBJECT(row), "track-id", GSIZE_TO_POINTER((gsize)track->track_id));
    g_object_set_data(G_OBJECT(row), "disc-num", GSIZE_TO_POINTER((gsize)track->disc_num));
    g_object_set_data(G_OBJECT(row), "track-num", GSIZE_TO_POINTER((gsize)track->track_num));
    g_object_set_data(G_OBJECT(row), "library-index", GSIZE_TO_POINTER((gsize)track->library_index));

    /* Expose info button for signal connection by detail view */
    if (info_btn)
        g_object_set_data(G_OBJECT(row), "info-btn", info_btn);

    return row;
}

/* GtkListBoxUpdateHeaderFunc — inserts disc headers between disc boundaries.
 * Each track content widget stores "quad-disc" (uint16_t disc_num via GUINT_TO_POINTER).
 * Only fires for multi-disc albums (set via gtk_list_box_set_header_func). */
static void disc_header_func(GtkListBoxRow *row,
                              GtkListBoxRow *before,
                              gpointer       data) {
    (void)data;
    GtkWidget *child = gtk_list_box_row_get_child(row);
    gpointer disc_ptr = child ? g_object_get_data(G_OBJECT(child), "quad-disc") : NULL;
    if (!disc_ptr) return;

    uint16_t disc = GPOINTER_TO_UINT(disc_ptr);
    uint16_t prev_disc = 0;
    if (before) {
        GtkWidget *prev_child = gtk_list_box_row_get_child(before);
        gpointer prev_ptr = prev_child ? g_object_get_data(G_OBJECT(prev_child), "quad-disc") : NULL;
        if (prev_ptr) prev_disc = GPOINTER_TO_UINT(prev_ptr);
    }

    if (disc != prev_disc) {
        GtkWidget *existing = gtk_list_box_row_get_header(row);
        if (existing) {
            gpointer existing_disc = g_object_get_data(G_OBJECT(existing), "quad-header-disc");
            if (existing_disc && GPOINTER_TO_UINT(existing_disc) == disc) return;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "Disc %u", disc);
        GtkWidget *header = ui_make_section_header(buf);
        g_object_set_data(G_OBJECT(header), "quad-header-disc", GUINT_TO_POINTER(disc));
        gtk_list_box_row_set_header(row, header);
    } else {
        gtk_list_box_row_set_header(row, NULL);
    }
}

/* Format a release date string for display.
 * "2024-03-15" → "March 15, 2024"
 * "2024-03"    → "March 2024"
 * "2024"       → "2024"
 * Returns a g_malloc'd string. */
char *ui_format_release_date(const char *date) {
    static const char *months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    if (!date || !date[0]) return NULL;

    int y = 0, m = 0, d = 0;
    int parts = sscanf(date, "%d-%d-%d", &y, &m, &d);

    if (parts >= 3 && m >= 1 && m <= 12 && d >= 1)
        return g_strdup_printf("%s %d, %d", months[m - 1], d, y);
    if (parts >= 2 && m >= 1 && m <= 12)
        return g_strdup_printf("%s %d", months[m - 1], y);
    if (parts >= 1 && y > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", y);
        return g_strdup(buf);
    }
    return NULL;
}

/* Callback: open album directory in file manager */
static void on_path_btn_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *dir_path = g_object_get_data(G_OBJECT(btn), "dir-path");
    if (!dir_path) return;

    /* Hyprland: launch as centered floating window */
    if (g_getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        char *quoted = g_shell_quote(dir_path);
        char *rule = g_strdup_printf("[float;center;size 60%% 70%%] nautilus %s",
                                     quoted);
        char *argv[] = {"hyprctl", "dispatch", "exec", rule, NULL};
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, NULL, NULL);
        g_free(quoted);
        g_free(rule);
        return;
    }

    GFile *file = g_file_new_for_path(dir_path);
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_file_launcher_launch(launcher, GTK_WINDOW(toplevel), NULL, NULL, NULL);
    g_object_unref(launcher);
    g_object_unref(file);
}

GtkWidget *ui_create_album_detail_card(const library_album_info_t *album,
                                        const GPtrArray *tracks,
                                        library_cache_t *cache,
                                        ArtworkManager *art_mgr,
                                        guint max_preview_tracks,
                                        RowCallbacks *track_cbs,
                                        RowCallbacks *artist_cbs) {
    /* Fetch enriched release info (label, release date) from metadata DB */
    db_meta_release_t *meta_release = NULL;
    int lib_idx = album->library_index;
    if (lib_idx < 0) lib_idx = LIBRARY_GLOBAL_ID_LIB(album->album_id);
    if (album->musicbrainz_release_id && album->musicbrainz_release_id[0] && lib_idx >= 0) {
        quadrature_meta_db_t *meta_db = library_cache_get_dbs(cache, lib_idx).meta;
        if (meta_db)
            db_meta_get_release(meta_db, album->musicbrainz_release_id, &meta_release);
    }

    GtkBuilder *builder;
    GtkWidget *card = ui_builder_load("/org/quadrature/ui/album_card.ui", "album_card", &builder);

    /* Get widget references */
    GtkWidget *art = GTK_WIDGET(gtk_builder_get_object(builder, "card_art"));
    GtkWidget *title = GTK_WIDGET(gtk_builder_get_object(builder, "card_title"));
    GtkWidget *artist_link = GTK_WIDGET(gtk_builder_get_object(builder, "card_artist_link"));
    GtkWidget *release_info = GTK_WIDGET(gtk_builder_get_object(builder, "card_release_info"));
    GtkWidget *label_w = GTK_WIDGET(gtk_builder_get_object(builder, "card_label"));
    GtkWidget *stats = GTK_WIDGET(gtk_builder_get_object(builder, "card_stats"));
    GtkWidget *template_genres = GTK_WIDGET(gtk_builder_get_object(builder, "card_genres"));
    GtkWidget *path_btn = GTK_WIDGET(gtk_builder_get_object(builder, "card_path_btn"));
    GtkWidget *track_list = GTK_WIDGET(gtk_builder_get_object(builder, "track_list"));

    g_object_unref(builder);

    /* Populate album metadata */
    if (title) {
        gtk_label_set_text(GTK_LABEL(title), album->title);
    }

    /* Populate artist link button */
    if (artist_link) {
        gtk_button_set_label(GTK_BUTTON(artist_link), album->artist_name);
        gtk_widget_set_tooltip_text(artist_link, "View artist");
        /* Store artist ID for caller to access */
        g_object_set_data(G_OBJECT(artist_link), "artist-id",
                         GSIZE_TO_POINTER((gsize)album->artist_id));
    }

    /* Release info: "Type · Date" or just date or just type */
    if (release_info) {
        const char *type = (meta_release && meta_release->release_type &&
                            meta_release->release_type[0])
                           ? meta_release->release_type : NULL;
        char *date_str = NULL;
        if (meta_release && meta_release->release_date)
            date_str = ui_format_release_date(meta_release->release_date);
        if (!date_str && album->year > 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", album->year);
            date_str = g_strdup(buf);
        }

        if (type && date_str) {
            char *combined = g_strdup_printf("%s \u00b7 %s", type, date_str);
            gtk_label_set_text(GTK_LABEL(release_info), combined);
            gtk_widget_set_visible(release_info, TRUE);
            g_free(combined);
        } else if (type) {
            gtk_label_set_text(GTK_LABEL(release_info), type);
            gtk_widget_set_visible(release_info, TRUE);
        } else if (date_str) {
            gtk_label_set_text(GTK_LABEL(release_info), date_str);
            gtk_widget_set_visible(release_info, TRUE);
        } else {
            gtk_widget_set_visible(release_info, FALSE);
        }
        g_free(date_str);
    }

    if (stats) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u track%s",
                 album->track_count, album->track_count == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(stats), buf);
    }

    /* Record label */
    if (label_w && meta_release && meta_release->label && meta_release->label[0]) {
        char *label_text = g_strdup_printf("Label: %s", meta_release->label);
        gtk_label_set_text(GTK_LABEL(label_w), label_text);
        gtk_widget_set_visible(label_w, TRUE);
        g_free(label_text);
    }

    /* Genre pills: multi-row wrapping overflow layout.
     * Single QuadratureOverflowBox with max-rows=3 handles wrapping + ellipsis. */
    if (template_genres && album->genres && album->genres[0]) {
        g_type_ensure(QUADRATURE_TYPE_OVERFLOW_BOX);
        gchar **raw = g_strsplit(album->genres, ";", 0);
        GPtrArray *clean = g_ptr_array_new();
        for (guint i = 0; raw[i]; i++) {
            g_strstrip(raw[i]);
            if (raw[i][0]) g_ptr_array_add(clean, raw[i]);
        }

        QuadratureOverflowBox *ofb = QUADRATURE_OVERFLOW_BOX(
            g_object_new(QUADRATURE_TYPE_OVERFLOW_BOX,
                         "spacing", 6, "row-spacing", 4, "max-rows", 3, NULL));
        for (guint i = 0; i < clean->len; i++) {
            GtkWidget *pill = gtk_label_new(g_ptr_array_index(clean, i));
            gtk_widget_add_css_class(pill, "genre-pill");
            quadrature_overflow_box_append(ofb, pill);
        }
        GtkWidget *overflow = gtk_label_new("…");
        gtk_widget_add_css_class(overflow, "genre-pill");
        quadrature_overflow_box_append(ofb, overflow);
        quadrature_overflow_box_set_item_count(ofb, clean->len);
        gtk_box_append(GTK_BOX(template_genres), GTK_WIDGET(ofb));

        g_ptr_array_free(clean, FALSE);
        g_strfreev(raw);
        gtk_widget_set_visible(template_genres, TRUE);
    }

    /* Album path button */
    if (path_btn && tracks && tracks->len > 0) {
        const library_track_info_t *first = g_ptr_array_index(tracks, 0);
        char *full_path = library_cache_resolve_track_path(cache, first->track_id);
        char *dir = full_path ? g_path_get_dirname(full_path) : NULL;
        g_free(full_path);
        if (dir) {
            GtkWidget *path_label = gtk_label_new(dir);
            gtk_label_set_ellipsize(GTK_LABEL(path_label), PANGO_ELLIPSIZE_MIDDLE);
            gtk_label_set_xalign(GTK_LABEL(path_label), 0);
            gtk_button_set_child(GTK_BUTTON(path_btn), path_label);
            gtk_widget_set_tooltip_text(path_btn, dir);
            g_object_set_data_full(G_OBJECT(path_btn), "dir-path", dir, g_free);
            g_signal_connect(path_btn, "clicked", G_CALLBACK(on_path_btn_clicked), NULL);
            gtk_widget_set_visible(path_btn, TRUE);
        }
    }

    /* Load full-resolution album art directly from disk (detail views bypass atlas) */
    if (art && art_mgr) {
        artwork_manager_get_fullsize_album_art(art_mgr, album->album_id, art);
    }

    /* Populate track list with automatic disc headers */
    if (track_list && tracks && tracks->len > 0) {
        guint track_count = tracks->len;
        guint preview_count = (max_preview_tracks > 0 && track_count > max_preview_tracks)
                              ? max_preview_tracks : track_count;

        /* Connect row-activated for Enter key / double-click */
        g_signal_connect(track_list, "row-activated", G_CALLBACK(ui_list_box_row_activated), NULL);

        /* Detect if multi-disc — install header_func if so */
        uint16_t max_disc = 1;
        for (guint i = 0; i < track_count; i++) {
            const library_track_info_t *t = g_ptr_array_index(tracks, i);
            if (t->disc_num > max_disc) max_disc = t->disc_num;
        }
        gboolean multi_disc = (max_disc > 1);
        if (multi_disc)
            gtk_list_box_set_header_func(GTK_LIST_BOX(track_list), disc_header_func, NULL, NULL);

        uint16_t current_disc = 0;
        GtkWidget *prev_content = NULL;  /* previous track content for section position classes */
        for (guint i = 0; i < preview_count; i++) {
            const library_track_info_t *track = g_ptr_array_index(tracks, i);

            /* Track disc boundary for section position classes */
            if (multi_disc && track->disc_num != current_disc) {
                /* Close previous disc section */
                if (prev_content)
                    gtk_widget_add_css_class(prev_content, "library-row-last");
                prev_content = NULL;

                current_disc = track->disc_num;
            }

            /* Create track row using helper (stores track-id) */
            GtkWidget *content = ui_create_album_detail_track_item(track, cache, artist_cbs, album->artist_id);
            if (track_cbs) {
                ui_row_attach_handlers(content, track_cbs);
            }

            /* Wrap in GtkListBoxRow from template - CANNOT avoid C wrapping
             * Reason: Dynamic number of tracks, must be created in loop */
            GtkBuilder *row_builder;
            GtkWidget *row = ui_builder_load("/org/quadrature/ui/album_detail_track_row_wrapper.ui", "track_row", &row_builder);
            g_object_unref(row_builder);

            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
            if (multi_disc)
                g_object_set_data(G_OBJECT(content), "quad-disc", GUINT_TO_POINTER(track->disc_num));
            gtk_list_box_append(GTK_LIST_BOX(track_list), row);

            /* Section position classes for library-list styling */
            if (!prev_content)
                gtk_widget_add_css_class(content, "library-row-first");
            prev_content = content;
        }
        /* Close final section */
        if (prev_content)
            gtk_widget_add_css_class(prev_content, "library-row-last");
    }

    /* Store album ID and MusicBrainz release MBID for handler access */
    g_object_set_data(G_OBJECT(card), "album-id", GSIZE_TO_POINTER((gsize)album->album_id));
    if (album->musicbrainz_release_id && album->musicbrainz_release_id[0])
        g_object_set_data_full(G_OBJECT(card), "release-mbid",
                               g_strdup(album->musicbrainz_release_id), g_free);

    db_meta_release_free(meta_release);
    return card;
}
