/**
 * metadata_dialog.c - Track metadata popup dialog
 *
 * Shows track metadata. Auto-destroys when unfocused.
 */

#include "internal.h"
#include <string.h>

struct _UiMetadataDialog {
    GtkWindow parent;
    GtkWidget *content_box;
};

G_DEFINE_FINAL_TYPE(UiMetadataDialog, ui_metadata_dialog, GTK_TYPE_WINDOW)

static GtkWidget *
create_row(const char *label, const char *value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "metadata-dialog-row");

    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_set_size_request(lbl, 100, -1);
    gtk_widget_add_css_class(lbl, "dim-label");

    GtkWidget *val = gtk_label_new(value ? value : "—");
    gtk_label_set_xalign(GTK_LABEL(val), 0);
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_set_hexpand(val, TRUE);

    gtk_box_append(GTK_BOX(row), lbl);
    gtk_box_append(GTK_BOX(row), val);
    return row;
}

static gboolean
on_focus_out(GtkEventControllerFocus *ctrl, gpointer data)
{
    (void)ctrl;
    GtkWindow *win = GTK_WINDOW(data);
    gtk_window_destroy(win);
    return FALSE;
}

static void
ui_metadata_dialog_class_init(UiMetadataDialogClass *klass)
{
    (void)klass;
}

static void
ui_metadata_dialog_init(UiMetadataDialog *self)
{
    gtk_window_set_title(GTK_WINDOW(self), "Track Info");
    gtk_window_set_default_size(GTK_WINDOW(self), 400, -1);
    gtk_window_set_resizable(GTK_WINDOW(self), FALSE);

    self->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(self->content_box, "view-container");
    gtk_widget_add_css_class(self->content_box, "metadata-dialog-content");
    gtk_window_set_child(GTK_WINDOW(self), self->content_box);

    /* Auto-destroy on focus loss */
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_out), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);
}

GtkWidget *
ui_metadata_dialog_new(GtkWindow *parent,
                       const library_track_info_t *track,
                       const char *resolved_path)
{
    UiMetadataDialog *dlg = g_object_new(UI_TYPE_METADATA_DIALOG, NULL);

    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    }

    if (!track)
        return GTK_WIDGET(dlg);

    /* Title */
    char *title = g_strdup_printf("Info: %s", track->title ? track->title : "Unknown");
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    g_free(title);

    /* Populate */
    gtk_box_append(GTK_BOX(dlg->content_box), create_row("Title", track->title));
    gtk_box_append(GTK_BOX(dlg->content_box), create_row("Artist", track->artist_display));
    gtk_box_append(GTK_BOX(dlg->content_box), create_row("Album", track->album_title));

    if (track->year > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", track->year);
        gtk_box_append(GTK_BOX(dlg->content_box), create_row("Year", buf));
    }

    if (track->track_num > 0) {
        char buf[24];
        if (track->disc_num > 1)
            snprintf(buf, sizeof(buf), "%u (Disc %u)", track->track_num, track->disc_num);
        else
            snprintf(buf, sizeof(buf), "%u", track->track_num);
        gtk_box_append(GTK_BOX(dlg->content_box), create_row("Track", buf));
    }

    char dur_buf[16];
    ui_format_duration(track->duration_ms, dur_buf, sizeof(dur_buf));
    gtk_box_append(GTK_BOX(dlg->content_box), create_row("Duration", dur_buf));

    gtk_box_append(GTK_BOX(dlg->content_box), create_row("Path", resolved_path));

    return GTK_WIDGET(dlg);
}
