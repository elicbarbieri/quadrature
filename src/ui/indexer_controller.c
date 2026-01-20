#include "indexer_controller.h"
#include "quadrature/indexer/indexer.h"
#include "quadrature/database/database.h"

#include <string.h>

struct _IndexerController {
    GObject parent_instance;

    quadrature_db_t* db;
    indexer_t* indexer;

    // Configuration
    int thread_count;
    gboolean process_artwork;
    int art_size;
    char* art_cache_dir;

    // Current state (for property binding)
    double progress;
    gboolean running;
    char current_item[256];
    char status[64];

    // Latest progress snapshot
    indexer_progress_t last_progress;
};

// Properties
enum {
    PROP_0,
    PROP_PROGRESS,
    PROP_RUNNING,
    PROP_CURRENT_ITEM,
    PROP_STATUS,
    N_PROPS
};

static GParamSpec* props[N_PROPS];

// Signals
enum {
    SIGNAL_STARTED,
    SIGNAL_PROGRESS,
    SIGNAL_COMPLETED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(IndexerController, indexer_controller, G_TYPE_OBJECT)

// --- Idle callback for thread-safe UI updates ---

typedef struct {
    IndexerController* controller;
    indexer_event_t event;
    indexer_progress_t progress;
} IdleCallbackData;

static gboolean on_indexer_event_idle(gpointer user_data) {
    IdleCallbackData* data = user_data;
    IndexerController* self = data->controller;

    if (!INDEXER_IS_CONTROLLER(self)) {
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    // Update internal state from progress
    self->last_progress = data->progress;
    self->progress = data->progress.progress * 100.0;  // Convert to percentage

    if (data->progress.current_path) {
        strncpy(self->current_item, data->progress.current_path, sizeof(self->current_item) - 1);
        self->current_item[sizeof(self->current_item) - 1] = '\0';
    } else {
        self->current_item[0] = '\0';
    }

    switch (data->event) {
        case INDEXER_STARTED:
            self->running = TRUE;
            strncpy(self->status, "Scanning", sizeof(self->status));
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
            g_signal_emit(self, signals[SIGNAL_STARTED], 0);
            break;

        case INDEXER_PROGRESS:
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PROGRESS]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_ITEM]);
            g_signal_emit(self, signals[SIGNAL_PROGRESS], 0, &data->progress);
            break;

        case INDEXER_COMPLETED:
            self->running = FALSE;
            self->progress = 100.0;
            self->current_item[0] = '\0';
            strncpy(self->status, "Complete", sizeof(self->status));
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PROGRESS]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_ITEM]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
            g_signal_emit(self, signals[SIGNAL_COMPLETED], 0, TRUE, &data->progress);
            break;

        case INDEXER_CANCELLED:
            self->running = FALSE;
            self->current_item[0] = '\0';
            strncpy(self->status, "Cancelled", sizeof(self->status));
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_ITEM]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
            g_signal_emit(self, signals[SIGNAL_COMPLETED], 0, FALSE, &data->progress);
            break;

        case INDEXER_ERROR:
            self->running = FALSE;
            strncpy(self->status, "Error", sizeof(self->status));
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);
            g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
            g_signal_emit(self, signals[SIGNAL_COMPLETED], 0, FALSE, &data->progress);
            break;
    }

    g_free(data);
    return G_SOURCE_REMOVE;
}

// --- Callback from indexer thread ---

static void on_indexer_callback(indexer_event_t event,
                                 const indexer_progress_t* progress,
                                 void* user_data) {
    IndexerController* self = INDEXER_CONTROLLER(user_data);

    // Schedule UI update on main thread
    IdleCallbackData* data = g_new(IdleCallbackData, 1);
    data->controller = self;
    data->event = event;
    if (progress) {
        data->progress = *progress;
    } else {
        memset(&data->progress, 0, sizeof(data->progress));
    }

    g_idle_add(on_indexer_event_idle, data);
}

// --- GObject Implementation ---

static void indexer_controller_get_property(GObject* object, guint prop_id,
                                             GValue* value, GParamSpec* pspec) {
    IndexerController* self = INDEXER_CONTROLLER(object);

    switch (prop_id) {
        case PROP_PROGRESS:
            g_value_set_double(value, self->progress);
            break;
        case PROP_RUNNING:
            g_value_set_boolean(value, self->running);
            break;
        case PROP_CURRENT_ITEM:
            g_value_set_string(value, self->current_item);
            break;
        case PROP_STATUS:
            g_value_set_string(value, self->status);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void indexer_controller_dispose(GObject* object) {
    IndexerController* self = INDEXER_CONTROLLER(object);

    if (self->indexer) {
        indexer_destroy(self->indexer);
        self->indexer = NULL;
    }

    g_free(self->art_cache_dir);
    self->art_cache_dir = NULL;

    G_OBJECT_CLASS(indexer_controller_parent_class)->dispose(object);
}

static void indexer_controller_class_init(IndexerControllerClass* klass) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = indexer_controller_get_property;
    object_class->dispose = indexer_controller_dispose;

    // Properties
    props[PROP_PROGRESS] = g_param_spec_double(
        "progress", "Progress", "Progress percentage (0-100)",
        0.0, 100.0, 0.0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    props[PROP_RUNNING] = g_param_spec_boolean(
        "running", "Running", "Whether indexing is in progress",
        FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    props[PROP_CURRENT_ITEM] = g_param_spec_string(
        "current-item", "Current Item", "Currently processing item",
        "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    props[PROP_STATUS] = g_param_spec_string(
        "status", "Status", "Current status description",
        "Idle", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, props);

    // Signals
    signals[SIGNAL_STARTED] = g_signal_new(
        "started",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_PROGRESS] = g_signal_new(
        "progress",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_POINTER);

    signals[SIGNAL_COMPLETED] = g_signal_new(
        "completed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_POINTER);
}

static void indexer_controller_init(IndexerController* self) {
    self->thread_count = 0;  // Auto
    self->process_artwork = TRUE;
    self->art_size = 300;
    self->progress = 0.0;
    self->running = FALSE;
    strncpy(self->status, "Idle", sizeof(self->status));
}

// --- Public API ---

IndexerController* indexer_controller_new(quadrature_db_t* db) {
    IndexerController* self = g_object_new(INDEXER_TYPE_CONTROLLER, NULL);
    self->db = db;
    return self;
}

void indexer_controller_set_thread_count(IndexerController* self, int thread_count) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    self->thread_count = thread_count;
}

void indexer_controller_set_process_artwork(IndexerController* self, gboolean enable) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    self->process_artwork = enable;
}

void indexer_controller_set_art_size(IndexerController* self, int size) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    self->art_size = size > 0 ? size : 300;
}

static gboolean ensure_indexer(IndexerController* self) {
    if (self->indexer) return TRUE;

    indexer_config_t config = {
        .thread_count = self->thread_count,
        .process_artwork = self->process_artwork,
        .art_size = self->art_size,
        .art_dir = self->art_cache_dir,
        .callback = on_indexer_callback,
        .user_data = self
    };

    if (indexer_create(&self->indexer, &config) != QUADRATURE_OK) {
        g_critical("Failed to create indexer");
        return FALSE;
    }

    return TRUE;
}

gboolean indexer_controller_start(IndexerController* self,
                                   const char** paths, gsize path_count) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), FALSE);
    g_return_val_if_fail(paths != NULL && path_count > 0, FALSE);

    if (!ensure_indexer(self)) return FALSE;

    if (self->running) {
        g_warning("Indexer already running");
        return FALSE;
    }

    self->progress = 0.0;
    self->current_item[0] = '\0';
    strncpy(self->status, "Starting", sizeof(self->status));

    if (indexer_scan(self->indexer, self->db, paths, path_count) != QUADRATURE_OK) {
        strncpy(self->status, "Error", sizeof(self->status));
        return FALSE;
    }

    return TRUE;
}

gboolean indexer_controller_start_all(IndexerController* self) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), FALSE);

    if (!ensure_indexer(self)) return FALSE;

    if (self->running) {
        g_warning("Indexer already running");
        return FALSE;
    }

    // Get watch paths from database
    db_watch_path_t* watch_paths = NULL;
    size_t path_count = 0;
    if (db_get_watch_paths(self->db, &watch_paths, &path_count) != QUADRATURE_OK ||
        path_count == 0) {
        g_warning("No watch paths configured");
        return FALSE;
    }

    // Extract path strings for the indexer
    const char** paths = g_malloc(path_count * sizeof(const char*));
    for (size_t i = 0; i < path_count; i++) {
        paths[i] = watch_paths[i].path;
    }

    self->progress = 0.0;
    self->current_item[0] = '\0';
    strncpy(self->status, "Starting", sizeof(self->status));

    quadrature_result_t res = indexer_scan(self->indexer, self->db, paths, path_count);

    g_free(paths);
    db_free_watch_paths(watch_paths, path_count);

    if (res != QUADRATURE_OK) {
        strncpy(self->status, "Error", sizeof(self->status));
        return FALSE;
    }

    return TRUE;
}

gboolean indexer_controller_add_path(IndexerController* self, const char* path) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    // Add to watch paths
    if (db_add_watch_path(self->db, path) != QUADRATURE_OK) {
        g_critical("Failed to add watch path: %s", path);
        return FALSE;
    }

    // Start indexing just this path
    const char* paths[] = { path };
    return indexer_controller_start(self, paths, 1);
}

void indexer_controller_cancel(IndexerController* self) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    if (self->indexer) {
        indexer_cancel(self->indexer);
    }
}

gboolean indexer_controller_is_running(IndexerController* self) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), FALSE);
    return self->running;
}

double indexer_controller_get_progress(IndexerController* self) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), 0.0);
    return self->progress;
}

const char* indexer_controller_get_current_item(IndexerController* self) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), NULL);
    return self->current_item[0] ? self->current_item : NULL;
}

void indexer_controller_get_progress_info(IndexerController* self,
                                           indexer_progress_t* progress) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_return_if_fail(progress != NULL);

    if (self->indexer) {
        indexer_get_progress(self->indexer, progress);
    } else {
        memset(progress, 0, sizeof(*progress));
    }
}

const char* indexer_controller_get_status(IndexerController* self) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), "Unknown");
    return self->status;
}
