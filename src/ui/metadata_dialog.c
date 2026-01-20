/**
 * metadata_dialog.c - Track metadata popup dialog
 *
 * Shows detailed metadata for a track including file info, audio format,
 * and all metadata tags. Supports Copy JSON to clipboard.
 */

#include "internal.h"
#include "quadrature/database/database.h"

#include <glib.h>
#include <string.h>

// =============================================================================
// Widget Structure
// =============================================================================

struct _UiMetadataDialog {
    GtkWindow parent;

    GtkWidget *copy_json_btn;
    GtkWidget *file_info_box;
    GtkWidget *format_info_box;
    GtkWidget *tags_info_box;

    int64_t track_id;
    char *raw_json;
    char *file_path;
};

G_DEFINE_FINAL_TYPE(UiMetadataDialog, ui_metadata_dialog, GTK_TYPE_WINDOW)

// =============================================================================
// Helpers
// =============================================================================

static GtkWidget *create_info_row(const char *label, const char *value) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *label_widget = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_widget_set_size_request(label_widget, 120, -1);
    gtk_widget_add_css_class(label_widget, "metadata-label");

    GtkWidget *value_widget = gtk_label_new(value ? value : "(none)");
    gtk_label_set_xalign(GTK_LABEL(value_widget), 0);
    gtk_label_set_wrap(GTK_LABEL(value_widget), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(value_widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(value_widget), TRUE);
    gtk_widget_set_hexpand(value_widget, TRUE);
    gtk_widget_add_css_class(value_widget, "metadata-value");

    if (!value || !*value) {
        gtk_widget_add_css_class(value_widget, "dim-label");
    }

    gtk_box_append(GTK_BOX(row), label_widget);
    gtk_box_append(GTK_BOX(row), value_widget);

    return row;
}

static char *format_duration(uint32_t ms) {
    uint32_t total_secs = ms / 1000;
    uint32_t hours = total_secs / 3600;
    uint32_t mins = (total_secs % 3600) / 60;
    uint32_t secs = total_secs % 60;

    if (hours > 0) {
        return g_strdup_printf("%u:%02u:%02u", hours, mins, secs);
    }
    return g_strdup_printf("%u:%02u", mins, secs);
}

static char *format_sample_rate(int32_t rate) {
    if (rate >= 1000) {
        return g_strdup_printf("%.1f kHz", rate / 1000.0);
    }
    return g_strdup_printf("%d Hz", rate);
}

static char *format_bitrate(int32_t kbps) {
    if (kbps > 0) {
        return g_strdup_printf("%d kbps", kbps);
    }
    return g_strdup("VBR");
}

static char *format_channels(int32_t channels) {
    switch (channels) {
        case 1: return g_strdup("Mono");
        case 2: return g_strdup("Stereo");
        case 6: return g_strdup("5.1 Surround");
        case 8: return g_strdup("7.1 Surround");
        default: return g_strdup_printf("%d channels", channels);
    }
}

static void clear_box(GtkWidget *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL) {
        gtk_box_remove(GTK_BOX(box), child);
    }
}

// =============================================================================
// Callbacks
// =============================================================================

static gboolean reset_button_label(gpointer data) {
    GtkButton *button = GTK_BUTTON(data);
    if (GTK_IS_BUTTON(button)) {
        gtk_button_set_label(button, "Copy JSON");
    }
    return G_SOURCE_REMOVE;
}

static void on_copy_json_clicked(GtkButton *button, UiMetadataDialog *self) {
    if (!self->raw_json) return;

    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_set_text(clipboard, self->raw_json);

    // Brief feedback
    gtk_button_set_label(button, "Copied!");
    g_timeout_add(1500, reset_button_label, button);
}

// =============================================================================
// GObject Implementation
// =============================================================================

static void ui_metadata_dialog_dispose(GObject *object) {
    UiMetadataDialog *self = UI_METADATA_DIALOG(object);

    g_free(self->raw_json);
    self->raw_json = NULL;

    g_free(self->file_path);
    self->file_path = NULL;

    G_OBJECT_CLASS(ui_metadata_dialog_parent_class)->dispose(object);
}

static void ui_metadata_dialog_class_init(UiMetadataDialogClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = ui_metadata_dialog_dispose;
}

static void ui_metadata_dialog_init(UiMetadataDialog *self) {
    self->track_id = 0;
    self->raw_json = NULL;
    self->file_path = NULL;

    // Build UI programmatically (simpler than loading template for dialogs)
    gtk_window_set_title(GTK_WINDOW(self), "Track Metadata");
    gtk_window_set_default_size(GTK_WINDOW(self), 450, 500);
    gtk_window_set_modal(GTK_WINDOW(self), FALSE);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(self), main_box);

    // Header bar
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(self), header);

    self->copy_json_btn = gtk_button_new_with_label("Copy JSON");
    gtk_widget_set_tooltip_text(self->copy_json_btn, "Copy all metadata as JSON");
    g_signal_connect(self->copy_json_btn, "clicked",
                     G_CALLBACK(on_copy_json_clicked), self);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), self->copy_json_btn);

    // Scrolled content
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(main_box), scroll);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(content, "view-container");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

    // File section
    GtkWidget *file_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(content), file_section);

    GtkWidget *file_header = gtk_label_new("FILE");
    gtk_label_set_xalign(GTK_LABEL(file_header), 0);
    gtk_widget_add_css_class(file_header, "metadata-section-header");
    gtk_box_append(GTK_BOX(file_section), file_header);

    self->file_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(self->file_info_box, "metadata-section");
    gtk_box_append(GTK_BOX(file_section), self->file_info_box);

    // Format section
    GtkWidget *format_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(content), format_section);

    GtkWidget *format_header = gtk_label_new("FORMAT");
    gtk_label_set_xalign(GTK_LABEL(format_header), 0);
    gtk_widget_add_css_class(format_header, "metadata-section-header");
    gtk_box_append(GTK_BOX(format_section), format_header);

    self->format_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(self->format_info_box, "metadata-section");
    gtk_box_append(GTK_BOX(format_section), self->format_info_box);

    // Tags section
    GtkWidget *tags_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(content), tags_section);

    GtkWidget *tags_header = gtk_label_new("TAGS");
    gtk_label_set_xalign(GTK_LABEL(tags_header), 0);
    gtk_widget_add_css_class(tags_header, "metadata-section-header");
    gtk_box_append(GTK_BOX(tags_section), tags_header);

    self->tags_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(self->tags_info_box, "metadata-section");
    gtk_box_append(GTK_BOX(tags_section), self->tags_info_box);
}

// =============================================================================
// Public API
// =============================================================================

GtkWidget *ui_metadata_dialog_new(GtkWindow *parent) {
    UiMetadataDialog *dialog = g_object_new(UI_TYPE_METADATA_DIALOG, NULL);

    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    }

    return GTK_WIDGET(dialog);
}

void ui_metadata_dialog_set_track(UiMetadataDialog *dialog,
                                   const db_track_t *track,
                                   const db_track_metadata_t *metadata) {
    if (!dialog || !track) return;

    dialog->track_id = track->id;

    // Store raw JSON for copy
    g_free(dialog->raw_json);
    dialog->raw_json = metadata && metadata->raw_json ?
                       g_strdup(metadata->raw_json) : g_strdup("{}");

    // Store file path
    g_free(dialog->file_path);
    dialog->file_path = track->path ? g_strdup(track->path) : NULL;

    // Update window title
    char *title = g_strdup_printf("Metadata: %s", track->title ? track->title : "Unknown");
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    g_free(title);

    // Clear existing content
    clear_box(dialog->file_info_box);
    clear_box(dialog->format_info_box);
    clear_box(dialog->tags_info_box);

    // File info
    gtk_box_append(GTK_BOX(dialog->file_info_box),
                   create_info_row("Path", track->path));

    // Format info
    if (metadata) {
        char *duration = format_duration(track->duration_ms);
        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Duration", duration));
        g_free(duration);

        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Codec", metadata->codec));

        char *sample_rate = format_sample_rate(metadata->sample_rate);
        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Sample Rate", sample_rate));
        g_free(sample_rate);

        char *bitrate = format_bitrate(metadata->bitrate);
        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Bitrate", bitrate));
        g_free(bitrate);

        char *channels = format_channels(metadata->channels);
        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Channels", channels));
        g_free(channels);

        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Embedded Art",
                                       metadata->has_embedded_art ? "Yes" : "No"));
    } else {
        char *duration = format_duration(track->duration_ms);
        gtk_box_append(GTK_BOX(dialog->format_info_box),
                       create_info_row("Duration", duration));
        g_free(duration);
    }

    // Tags
    gtk_box_append(GTK_BOX(dialog->tags_info_box),
                   create_info_row("Title", track->title));
    gtk_box_append(GTK_BOX(dialog->tags_info_box),
                   create_info_row("Artist", track->artist));
    gtk_box_append(GTK_BOX(dialog->tags_info_box),
                   create_info_row("Album", track->album));

    if (metadata && metadata->album_artist && *metadata->album_artist) {
        gtk_box_append(GTK_BOX(dialog->tags_info_box),
                       create_info_row("Album Artist", metadata->album_artist));
    }

    char *track_str = NULL;
    if (metadata && metadata->track_total > 0) {
        track_str = g_strdup_printf("%d / %d", track->track_num, metadata->track_total);
    } else if (track->track_num > 0) {
        track_str = g_strdup_printf("%d", track->track_num);
    }
    gtk_box_append(GTK_BOX(dialog->tags_info_box),
                   create_info_row("Track", track_str));
    g_free(track_str);

    char *disc_str = NULL;
    if (metadata && metadata->disc_total > 0 && track->disc_num > 0) {
        disc_str = g_strdup_printf("%d / %d", track->disc_num, metadata->disc_total);
    } else if (track->disc_num > 1) {
        disc_str = g_strdup_printf("%d", track->disc_num);
    }
    if (disc_str) {
        gtk_box_append(GTK_BOX(dialog->tags_info_box),
                       create_info_row("Disc", disc_str));
        g_free(disc_str);
    }

    if (track->year > 0) {
        char *year_str = g_strdup_printf("%d", track->year);
        gtk_box_append(GTK_BOX(dialog->tags_info_box),
                       create_info_row("Year", year_str));
        g_free(year_str);
    }

    if (metadata) {
        if (metadata->genre && *metadata->genre) {
            gtk_box_append(GTK_BOX(dialog->tags_info_box),
                           create_info_row("Genre", metadata->genre));
        }

        if (metadata->comment && *metadata->comment) {
            gtk_box_append(GTK_BOX(dialog->tags_info_box),
                           create_info_row("Comment", metadata->comment));
        }

        if (metadata->compilation) {
            gtk_box_append(GTK_BOX(dialog->tags_info_box),
                           create_info_row("Compilation", "Yes"));
        }
    }
}
