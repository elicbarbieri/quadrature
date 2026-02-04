/**
 * errors_view.c - Indexer Errors Tree View
 *
 * Hierarchical tree view showing errors grouped by folder structure.
 * Uses GtkTreeListModel for expandable rows with lazy loading on expand.
 */

#include "internal.h"
#include "../internal.h"
#include "quadrature/quadrature_database.h"

#include <glib.h>
#include <string.h>

// =============================================================================
// Error Item GObject (for tree model)
// =============================================================================

typedef enum {
    ERROR_ITEM_FOLDER,   // A directory that contains errors (or subdirs with errors)
    ERROR_ITEM_ERROR,    // An actual error entry
} ErrorItemKind;

#define ERROR_TYPE_ITEM (error_item_get_type())
G_DECLARE_FINAL_TYPE(ErrorItem, error_item, ERROR, ITEM, GObject)

struct _ErrorItem {
    GObject parent;
    ErrorItemKind kind;

    char *path;           // Full path (folder path or error path)
    char *display_name;   // What to show (folder name or error message)
    size_t error_count;   // For folders: count of errors underneath
    int64_t created_at;   // For errors: timestamp
};

G_DEFINE_FINAL_TYPE(ErrorItem, error_item, G_TYPE_OBJECT)

static void error_item_finalize(GObject *obj) {
    ErrorItem *item = (ErrorItem *)obj;
    g_free(item->path);
    g_free(item->display_name);
    G_OBJECT_CLASS(error_item_parent_class)->finalize(obj);
}

static void error_item_class_init(ErrorItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = error_item_finalize;
}

static void error_item_init(ErrorItem *self) {
    self->kind = ERROR_ITEM_FOLDER;
    self->path = NULL;
    self->display_name = NULL;
    self->error_count = 0;
    self->created_at = 0;
}

// =============================================================================
// View Data
// =============================================================================

typedef struct {
    quadrature_db_t *db;
    GtkWidget *container;
    GtkWidget *list_view;
    GtkWidget *title_label;
    GtkWidget *subtitle_label;
    GtkTreeListModel *tree_model;
    GListStore *root_store;

    char *path_filter;  /* Optional path prefix filter (library root) */

    void (*on_navigate_to_path)(const char *path, gpointer data);
    gpointer user_data;
} ErrorsViewData;

static const char *ERRORS_VIEW_DATA_KEY = "errors-view-data";

static void errors_view_data_free(gpointer data) {
    ErrorsViewData *vd = data;
    if (vd) {
        g_free(vd->path_filter);
        g_free(vd);
    }
}

// =============================================================================
// Path Utilities
// =============================================================================

// Get the directory part of a path
static char *get_parent_path(const char *path) {
    if (!path) return NULL;
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) return g_strdup("/");
    return g_strndup(path, last_slash - path);
}

// Get the filename/folder name part of a path
static char *get_basename(const char *path) {
    if (!path) return g_strdup("");
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return g_strdup(path);
    return g_strdup(last_slash + 1);
}

// Check if path is under prefix
static bool path_is_under(const char *path, const char *prefix) {
    if (!prefix || !*prefix) return true;
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0) return false;
    // Must be exact match or followed by /
    return path[prefix_len] == '\0' || path[prefix_len] == '/';
}

// Get the immediate child folder name from path relative to parent
// E.g., parent="/music", path="/music/Artist/Album/file.mp3" -> "Artist"
static char *get_immediate_child(const char *path, const char *parent) {
    if (!path || !parent) return NULL;

    size_t parent_len = strlen(parent);
    if (strncmp(path, parent, parent_len) != 0) return NULL;

    const char *rest = path + parent_len;
    if (*rest == '/') rest++;
    if (*rest == '\0') return NULL;

    const char *next_slash = strchr(rest, '/');
    if (next_slash) {
        return g_strndup(rest, next_slash - rest);
    }
    return NULL;  // No child folder, this is a direct child file
}

// =============================================================================
// Tree Model Creation Callbacks
// =============================================================================

static GListModel *create_children_model(gpointer item, gpointer user_data) {
    ErrorsViewData *vd = user_data;
    ErrorItem *parent = (ErrorItem *)item;

    // Only folders have children
    if (parent->kind != ERROR_ITEM_FOLDER) {
        return NULL;
    }

    const char *folder_path = parent->path;

    // Query errors under this folder
    db_indexer_error_t *errors = NULL;
    size_t error_count = 0;
    db_get_errors_page(vd->db, folder_path, 0, 10000, &errors, &error_count);

    if (error_count == 0) {
        return NULL;
    }

    // Build a set of immediate subfolders and direct errors
    GHashTable *subfolders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *subfolder_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *direct_errors = g_ptr_array_new();

    for (size_t i = 0; i < error_count; i++) {
        const char *error_path = errors[i].path;
        char *parent_dir = get_parent_path(error_path);

        if (g_strcmp0(parent_dir, folder_path) == 0) {
            // This error is directly in this folder
            g_ptr_array_add(direct_errors, &errors[i]);
        } else {
            // This error is in a subfolder - find immediate child
            char *child_name = get_immediate_child(error_path, folder_path);
            if (child_name) {
                gpointer count_ptr = g_hash_table_lookup(subfolder_counts, child_name);
                size_t count = count_ptr ? GPOINTER_TO_SIZE(count_ptr) : 0;

                char *child_path = g_strdup_printf("%s/%s", folder_path, child_name);
                g_hash_table_replace(subfolders, g_strdup(child_name), child_path);
                g_hash_table_replace(subfolder_counts, g_strdup(child_name), GSIZE_TO_POINTER(count + 1));
                g_free(child_name);
            }
        }
        g_free(parent_dir);
    }

    GListStore *store = g_list_store_new(error_item_get_type());

    // Add subfolders first (sorted)
    GList *folder_names = g_hash_table_get_keys(subfolders);
    folder_names = g_list_sort(folder_names, (GCompareFunc)g_strcmp0);

    for (GList *l = folder_names; l; l = l->next) {
        const char *name = l->data;
        const char *child_path = g_hash_table_lookup(subfolders, name);
        gpointer count_ptr = g_hash_table_lookup(subfolder_counts, name);
        size_t count = count_ptr ? GPOINTER_TO_SIZE(count_ptr) : 0;

        ErrorItem *folder_item = g_object_new(error_item_get_type(), NULL);
        folder_item->kind = ERROR_ITEM_FOLDER;
        folder_item->path = g_strdup(child_path);
        folder_item->display_name = g_strdup(name);
        folder_item->error_count = count;
        g_list_store_append(store, folder_item);
        g_object_unref(folder_item);
    }
    g_list_free(folder_names);

    // Add direct errors
    for (guint i = 0; i < direct_errors->len; i++) {
        db_indexer_error_t *err = g_ptr_array_index(direct_errors, i);
        ErrorItem *err_item = g_object_new(error_item_get_type(), NULL);
        err_item->kind = ERROR_ITEM_ERROR;
        err_item->path = g_strdup(err->path);
        err_item->display_name = g_strdup(err->message);
        err_item->created_at = err->created_at;
        g_list_store_append(store, err_item);
        g_object_unref(err_item);
    }

    g_hash_table_destroy(subfolders);
    g_hash_table_destroy(subfolder_counts);
    g_ptr_array_free(direct_errors, TRUE);
    db_indexer_errors_free(errors, error_count);

    if (g_list_model_get_n_items(G_LIST_MODEL(store)) == 0) {
        g_object_unref(store);
        return NULL;
    }

    return G_LIST_MODEL(store);
}

// =============================================================================
// Row Setup/Bind
// =============================================================================

static void errors_row_setup(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    (void)data;

    GtkWidget *expander = gtk_tree_expander_new();
    gtk_tree_expander_set_indent_for_icon(GTK_TREE_EXPANDER(expander), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "errors-tree-row");

    GtkWidget *icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *badge = gtk_label_new(NULL);
    gtk_widget_add_css_class(badge, "error-badge");
    gtk_box_append(GTK_BOX(box), badge);

    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(li, expander);
}

static void errors_row_bind(GtkListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    (void)data;

    GtkTreeListRow *row = gtk_list_item_get_item(li);
    g_assert(row != NULL);  /* GTK row binding must provide a row */

    ErrorItem *item = gtk_tree_list_row_get_item(row);
    g_assert(item != NULL);  /* Tree list row must have an item */

    GtkWidget *expander = gtk_list_item_get_child(li);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);

    GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);
    GtkWidget *badge = gtk_widget_get_next_sibling(label);

    // Remove CSS classes
    gtk_widget_remove_css_class(box, "error-folder-row");
    gtk_widget_remove_css_class(box, "error-item-row");

    switch (item->kind) {
    case ERROR_ITEM_FOLDER:
        gtk_widget_add_css_class(box, "error-folder-row");
        gtk_widget_set_visible(icon, FALSE);  // No icon for folders
        gtk_label_set_text(GTK_LABEL(label), item->display_name);

        if (item->error_count > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%zu", item->error_count);
            gtk_label_set_text(GTK_LABEL(badge), buf);
            gtk_widget_set_visible(badge, TRUE);
        } else {
            gtk_widget_set_visible(badge, FALSE);
        }
        break;

    case ERROR_ITEM_ERROR:
        gtk_widget_add_css_class(box, "error-item-row");
        gtk_widget_set_visible(icon, TRUE);
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "dialog-warning-symbolic");

        // Show filename and error message
        char *filename = get_basename(item->path);
        char *display = g_strdup_printf("%s: %s", filename, item->display_name);
        gtk_label_set_text(GTK_LABEL(label), display);
        g_free(display);
        g_free(filename);

        gtk_widget_set_visible(badge, FALSE);
        break;
    }

    g_object_unref(item);
}

// =============================================================================
// Activation Handler
// =============================================================================

static void on_errors_activated(GtkListView *lv, guint pos, gpointer data) {
    (void)lv;
    ErrorsViewData *vd = data;

    GtkTreeListRow *row = g_list_model_get_item(
        G_LIST_MODEL(vd->tree_model), pos);
    if (!row) return;

    ErrorItem *item = gtk_tree_list_row_get_item(row);
    if (!item) {
        g_object_unref(row);
        return;
    }

    // For errors, navigate to the file path
    if (item->kind == ERROR_ITEM_ERROR && vd->on_navigate_to_path) {
        vd->on_navigate_to_path(item->path, vd->user_data);
    }

    g_object_unref(item);
    g_object_unref(row);
}

// =============================================================================
// Build Root Model
// =============================================================================

static void build_root_model(ErrorsViewData *vd) {
    g_list_store_remove_all(vd->root_store);

    // Get all errors to find top-level folders
    db_indexer_error_t *errors = NULL;
    size_t error_count = 0;
    db_get_errors_page(vd->db, vd->path_filter, 0, 10000, &errors, &error_count);

    if (error_count == 0) {
        db_indexer_errors_free(errors, error_count);
        return;
    }

    // Determine the base path (path_filter or find common root)
    const char *base_path = vd->path_filter;
    if (!base_path || !*base_path) {
        // Find shortest common prefix of all error paths
        // For simplicity, just use the parent of the first error
        if (error_count > 0 && errors[0].path) {
            // Find a reasonable root - go up to find where paths diverge
            // For now, use an empty string to show all top-level folders
            base_path = "";
        }
    }

    // Build set of immediate children under base_path
    GHashTable *children = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *child_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (size_t i = 0; i < error_count; i++) {
        const char *error_path = errors[i].path;

        // Find the first path component after base_path
        const char *rest = error_path;
        if (base_path && *base_path) {
            if (!path_is_under(error_path, base_path)) continue;
            rest = error_path + strlen(base_path);
            if (*rest == '/') rest++;
        }

        // Get first component
        const char *slash = strchr(rest, '/');
        char *first_component;
        if (slash) {
            first_component = g_strndup(rest, slash - rest);
        } else {
            // This is a file directly under base_path (unlikely for music)
            first_component = g_strdup(rest);
        }

        if (*first_component) {
            gpointer count_ptr = g_hash_table_lookup(child_counts, first_component);
            size_t count = count_ptr ? GPOINTER_TO_SIZE(count_ptr) : 0;

            char *child_path;
            if (base_path && *base_path) {
                child_path = g_strdup_printf("%s/%s", base_path, first_component);
            } else {
                // Find the full path up to this component
                child_path = g_strndup(error_path, (rest - error_path) + strlen(first_component));
            }

            g_hash_table_replace(children, g_strdup(first_component), child_path);
            g_hash_table_replace(child_counts, g_strdup(first_component), GSIZE_TO_POINTER(count + 1));
        }
        g_free(first_component);
    }

    // Add root folders (sorted)
    GList *names = g_hash_table_get_keys(children);
    names = g_list_sort(names, (GCompareFunc)g_strcmp0);

    for (GList *l = names; l; l = l->next) {
        const char *name = l->data;
        const char *path = g_hash_table_lookup(children, name);
        gpointer count_ptr = g_hash_table_lookup(child_counts, name);
        size_t count = count_ptr ? GPOINTER_TO_SIZE(count_ptr) : 0;

        ErrorItem *item = g_object_new(error_item_get_type(), NULL);
        item->kind = ERROR_ITEM_FOLDER;
        item->path = g_strdup(path);
        item->display_name = g_strdup(name);
        item->error_count = count;
        g_list_store_append(vd->root_store, item);
        g_object_unref(item);
    }

    g_list_free(names);
    g_hash_table_destroy(children);
    g_hash_table_destroy(child_counts);
    db_indexer_errors_free(errors, error_count);
}

// =============================================================================
// Public API
// =============================================================================

GtkWidget *errors_view_new(quadrature_db_t *db) {
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(container, 16);
    gtk_widget_set_margin_end(container, 16);
    gtk_widget_set_margin_top(container, 8);
    gtk_widget_set_margin_bottom(container, 16);
    gtk_widget_add_css_class(container, "errors-view-container");

    ErrorsViewData *vd = g_new0(ErrorsViewData, 1);
    vd->db = db;
    vd->container = container;
    vd->path_filter = NULL;
    g_object_set_data_full(G_OBJECT(container), ERRORS_VIEW_DATA_KEY, vd, errors_view_data_free);

    // Subtitle (error count) - title is in overlay header
    vd->subtitle_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(vd->subtitle_label), 0);
    gtk_widget_add_css_class(vd->subtitle_label, "library-subtitle");
    gtk_box_append(GTK_BOX(container), vd->subtitle_label);
    vd->title_label = NULL;  // No longer used

    // Root store with top-level folders
    vd->root_store = g_list_store_new(error_item_get_type());

    // Tree list model with lazy children
    vd->tree_model = gtk_tree_list_model_new(
        G_LIST_MODEL(vd->root_store),
        FALSE,  // passthrough
        FALSE,  // autoexpand - let user expand
        create_children_model,
        vd,
        NULL);

    // Selection model
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(vd->tree_model));
    gtk_single_selection_set_autoselect(sel, FALSE);

    // Factory
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(errors_row_setup), vd);
    g_signal_connect(factory, "bind", G_CALLBACK(errors_row_bind), vd);

    // List view
    vd->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
    gtk_widget_add_css_class(vd->list_view, "errors-tree");
    g_signal_connect(vd->list_view, "activate", G_CALLBACK(on_errors_activated), vd);

    // Scrolled window
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), vd->list_view);
    gtk_box_append(GTK_BOX(container), scroll);

    return container;
}

void errors_view_refresh(GtkWidget *view) {
    ErrorsViewData *vd = g_object_get_data(G_OBJECT(view), ERRORS_VIEW_DATA_KEY);
    g_assert(vd != NULL);       /* View must have been created via errors_view_new */
    g_assert(vd->db != NULL);   /* Database must be valid */

    // Get total error count
    size_t total_count = 0;
    db_get_error_count(vd->db, vd->path_filter, &total_count);

    // Update subtitle
    char buf[64];
    if (total_count == 0) {
        snprintf(buf, sizeof(buf), "No errors found");
    } else {
        snprintf(buf, sizeof(buf), "%zu error%s found",
                 total_count, total_count == 1 ? "" : "s");
    }
    gtk_label_set_text(GTK_LABEL(vd->subtitle_label), buf);

    // Rebuild root model
    build_root_model(vd);
}

void errors_view_set_callbacks(GtkWidget *view,
                                void (*on_path)(const char *path, gpointer data),
                                gpointer user_data) {
    ErrorsViewData *vd = g_object_get_data(G_OBJECT(view), ERRORS_VIEW_DATA_KEY);
    g_assert(vd != NULL);  /* View must have been created via errors_view_new */

    vd->on_navigate_to_path = on_path;
    vd->user_data = user_data;
}

size_t errors_view_get_count(GtkWidget *view) {
    ErrorsViewData *vd = g_object_get_data(G_OBJECT(view), ERRORS_VIEW_DATA_KEY);
    g_assert(vd != NULL);       /* View must have been created via errors_view_new */
    g_assert(vd->db != NULL);   /* Database must be valid */

    size_t count = 0;
    db_get_error_count(vd->db, vd->path_filter, &count);
    return count;
}

void errors_view_set_path_filter(GtkWidget *view, const char *path_filter) {
    ErrorsViewData *vd = g_object_get_data(G_OBJECT(view), ERRORS_VIEW_DATA_KEY);
    g_assert(vd != NULL);  /* View must have been created via errors_view_new */

    g_free(vd->path_filter);
    vd->path_filter = path_filter ? g_strdup(path_filter) : NULL;
}
