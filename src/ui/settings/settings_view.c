/**
 * Quadrature Settings View
 *
 * General settings, MusicBrainz configuration, artwork size, and help view builder.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "internal.h"
#include <string.h>


static const int art_size_values[] = { 48, 64, 96, 128 };
static const int art_size_count = G_N_ELEMENTS(art_size_values);

static int art_size_to_index(int size) {
    for (int i = 0; i < art_size_count; i++)
        if (art_size_values[i] == size) return i;
    return 0;
}

static void on_spectrum_toggled(GtkCheckButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    gboolean on = gtk_check_button_get_active(btn);
    ui_window_set_spectrum_visible(w, on);
    if (w->settings) {
        w->settings->show_spectrum = on;
        settings_save_debounced(w);
    }
}

static void on_art_size_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing) return;

    guint idx = gtk_drop_down_get_selected(dropdown);
    if (idx >= (guint)art_size_count) return;
    int new_size = art_size_values[idx];

    if (w->settings && w->settings->art_thumb_size != new_size) {
        w->settings->art_thumb_size = new_size;
        settings_save_debounced(w);

        char msg[64];
        snprintf(msg, sizeof(msg), "Thumbnail size set to %dpx — re-index to apply", new_size);
        ui_window_show_toast(w, msg, TOAST_INFO, 3000);
    }
}

static void on_mb_resolve_toggled(GtkCheckButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing) return;

    gboolean active = gtk_check_button_get_active(btn);
    if (w->settings) {
        w->settings->musicbrainz_resolve = active;
        settings_save_debounced(w);
    }
    if (w->indexer)
        indexer_controller_set_musicbrainz_resolve(w->indexer, active);
}

static void on_pg_conninfo_changed(GtkEditable *editable, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing) return;

    const char *text = gtk_editable_get_text(editable);
    if (w->settings) {
        g_free(w->settings->musicbrainz_pg_conninfo);
        w->settings->musicbrainz_pg_conninfo = g_strdup(text);
        settings_save_debounced(w);
    }
    if (w->indexer)
        indexer_controller_set_pg_conninfo(w->indexer, text);
}

static void on_fanart_api_key_changed(GtkEditable *editable, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing) return;

    const char *text = gtk_editable_get_text(editable);
    if (w->settings) {
        g_free(w->settings->fanart_api_key);
        w->settings->fanart_api_key = g_strdup(text);
        settings_save_debounced(w);
    }
    if (w->indexer)
        indexer_controller_set_fanart_api_key(w->indexer, text);
}

static void on_mb_solr_url_changed(GtkEditable *editable, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing) return;

    const char *text = gtk_editable_get_text(editable);
    if (w->settings) {
        g_free(w->settings->mb_solr_url);
        w->settings->mb_solr_url = g_strdup(text);
        settings_save_debounced(w);
    }
    if (w->indexer)
        indexer_controller_set_mb_solr_url(w->indexer, text);
}

/* Generic settings callbacks — field targeted via g_object_set_data("field-offset") */

static void on_bool_setting_toggled(GtkCheckButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing || !w->settings) return;

    size_t offset = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "field-offset"));
    *(gboolean *)((char *)w->settings + offset) = gtk_check_button_get_active(btn);
    settings_save_debounced(w);
}

static void on_string_setting_changed(GtkEditable *editable, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing || !w->settings) return;

    size_t offset = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(editable), "field-offset"));
    char **field = (char **)((char *)w->settings + offset);
    g_free(*field);
    *field = g_strdup(gtk_editable_get_text(editable));
    settings_save_debounced(w);
}

/* Bind helpers — connect a builder widget to a settings field in one call */

static void bind_bool_toggle(GtkBuilder *b, const char *id, UiWindow *w,
                              size_t offset, gboolean value) {
    GtkWidget *cb = GTK_WIDGET(gtk_builder_get_object(b, id));
    if (!cb) return;
    g_object_set_data(G_OBJECT(cb), "field-offset", GSIZE_TO_POINTER(offset));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), value);
    g_signal_connect(cb, "toggled", G_CALLBACK(on_bool_setting_toggled), w);
}

static void bind_string_entry(GtkBuilder *b, const char *id, UiWindow *w,
                               size_t offset, const char *value) {
    GtkWidget *entry = GTK_WIDGET(gtk_builder_get_object(b, id));
    if (!entry) return;
    g_object_set_data(G_OBJECT(entry), "field-offset", GSIZE_TO_POINTER(offset));
    if (value)
        gtk_editable_set_text(GTK_EDITABLE(entry), value);
    g_signal_connect(entry, "changed", G_CALLBACK(on_string_setting_changed), w);
}

GtkWidget *make_settings_view(UiWindow *w) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/settings_view.ui");

    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "settings_view"));
    g_object_ref(view);

    /* ── Audio Channels ── */
    GtkWidget *channel_frames_box = GTK_WIDGET(gtk_builder_get_object(builder, "channel_frames_box"));
    for (int i = 0; i < MAX_CHANNELS; i++) {
        GtkWidget *frame = make_channel_settings_frame(w, i);
        gtk_box_append(GTK_BOX(channel_frames_box), frame);
    }

    /* ── Appearance ── */
    GtkWidget *spectrum_checkbox = GTK_WIDGET(gtk_builder_get_object(builder, "spectrum_checkbox"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(spectrum_checkbox), w->show_spectrum);
    g_signal_connect(spectrum_checkbox, "toggled", G_CALLBACK(on_spectrum_toggled), w);

    GtkWidget *art_size_dropdown = GTK_WIDGET(gtk_builder_get_object(builder, "art_size_dropdown"));
    if (art_size_dropdown) {
        int current = w->settings ? w->settings->art_thumb_size : 48;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(art_size_dropdown), art_size_to_index(current));
        g_signal_connect(art_size_dropdown, "notify::selected", G_CALLBACK(on_art_size_changed), w);
    }

    /* ── Integrations: toggles ── */
    /* MusicBrainz resolve keeps its own callback (notifies indexer controller) */
    GtkWidget *mb_resolve_checkbox = GTK_WIDGET(gtk_builder_get_object(builder, "mb_resolve_checkbox"));
    if (mb_resolve_checkbox) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(mb_resolve_checkbox),
                                    w->settings ? w->settings->musicbrainz_resolve : FALSE);
        g_signal_connect(mb_resolve_checkbox, "toggled", G_CALLBACK(on_mb_resolve_toggled), w);
    }

    app_settings_t *s = w->settings;
    bind_bool_toggle(builder, "fanart_resolve_checkbox", w,
                     offsetof(app_settings_t, fanart_resolve),
                     s ? s->fanart_resolve : FALSE);
    bind_bool_toggle(builder, "acoustid_fingerprint_checkbox", w,
                     offsetof(app_settings_t, acoustid_fingerprint),
                     s ? s->acoustid_fingerprint : FALSE);

    /* ── Integrations: connection strings ── */
    /* MB PG + fanart API key keep their own callbacks (notify indexer controller) */
    GtkWidget *pg_conninfo_entry = GTK_WIDGET(gtk_builder_get_object(builder, "pg_conninfo_entry"));
    if (pg_conninfo_entry) {
        if (s && s->musicbrainz_pg_conninfo)
            gtk_editable_set_text(GTK_EDITABLE(pg_conninfo_entry), s->musicbrainz_pg_conninfo);
        g_signal_connect(pg_conninfo_entry, "changed", G_CALLBACK(on_pg_conninfo_changed), w);
    }

    GtkWidget *mb_solr_entry = GTK_WIDGET(gtk_builder_get_object(builder, "mb_solr_entry"));
    if (mb_solr_entry) {
        if (s && s->mb_solr_url)
            gtk_editable_set_text(GTK_EDITABLE(mb_solr_entry), s->mb_solr_url);
        g_signal_connect(mb_solr_entry, "changed", G_CALLBACK(on_mb_solr_url_changed), w);
    }

    GtkWidget *fanart_api_key_entry = GTK_WIDGET(gtk_builder_get_object(builder, "fanart_api_key_entry"));
    if (fanart_api_key_entry) {
        if (s && s->fanart_api_key)
            gtk_editable_set_text(GTK_EDITABLE(fanart_api_key_entry), s->fanart_api_key);
        g_signal_connect(fanart_api_key_entry, "changed", G_CALLBACK(on_fanart_api_key_changed), w);
    }

    bind_string_entry(builder, "acoustid_pg_entry", w,
                      offsetof(app_settings_t, acoustid_pg_conninfo),
                      s ? s->acoustid_pg_conninfo : NULL);
    bind_string_entry(builder, "acoustid_index_entry", w,
                      offsetof(app_settings_t, acoustid_index_url),
                      s ? s->acoustid_index_url : NULL);

    g_object_unref(builder);
    return view;
}

GtkWidget *make_help_view(void) {
    GtkBuilder *builder = gtk_builder_new_from_resource("/org/quadrature/ui/help_view.ui");
    GtkWidget *view = GTK_WIDGET(gtk_builder_get_object(builder, "help_view"));
    g_object_ref(view);  /* prevent destruction when builder is freed */
    g_object_unref(builder);
    return view;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UI Building
 * ═══════════════════════════════════════════════════════════════════════════ */
