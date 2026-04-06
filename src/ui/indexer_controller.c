#include "internal.h"

#include <string.h>

/* =============================================================================
 * Per-scan state
 * ============================================================================= */

/* User data passed to each individual indexer's callback */
typedef struct {
    IndexerController* controller;
    char* library_path;   /* heap-owned copy of the library root path */
} ScanCallbackData;

/* One active library scan slot */
typedef struct {
    indexer_t* indexer;
    char* library_path;          /* heap-owned */
    ScanCallbackData* cb_data;   /* heap-owned; freed when slot is released */
} ActiveScan;

/* =============================================================================
 * Controller struct
 * ============================================================================= */

struct _IndexerController {
    GObject parent_instance;

    /* Configuration */
    int thread_count;
    gboolean process_artwork;
    int art_size;
    gboolean musicbrainz_resolve;
    char* pg_conninfo;
    char* mb_solr_url;
    char* acoustid_pg_conninfo;
    char* acoustid_index_url;
    char* fanart_api_key;
    int max_concurrent;   /* default: 2 */

    /* All library roots for the current batch (for cross-library art reuse) */
    GPtrArray* all_roots; /* char*, owned — set at start(), kept until next start() */

    /* Active scans (array of ActiveScan*, owned) */
    GPtrArray* active;    /* ActiveScan* while running */

    /* Pending paths waiting for a free slot */
    GPtrArray* pending;   /* char* library_paths, owned */

    /* Data root overrides: library_path → data_path (NULL entry = same as library) */
    GHashTable* data_root_map;  /* char* → char*, owned keys + values */

    /* Global state */
    gboolean running;
};

/* =============================================================================
 * Properties & Signals
 * ============================================================================= */

enum {
    PROP_0,
    PROP_RUNNING,
    N_PROPS
};

static GParamSpec* props[N_PROPS];

enum {
    SIGNAL_STARTED,
    SIGNAL_PROGRESS,
    SIGNAL_LIBRARY_UPDATED,
    SIGNAL_ARTWORK_UPDATED,
    SIGNAL_COMPLETED,
    SIGNAL_ALL_COMPLETED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(IndexerController, indexer_controller, G_TYPE_OBJECT)

/* =============================================================================
 * Idle callback — marshals worker-thread events to the main thread
 * ============================================================================= */

typedef struct {
    IndexerController* controller;
    char* library_path;       /* heap-owned */
    indexer_event_t event;
    indexer_progress_t progress;
} IdleCallbackData;

/* Forward declaration */
static void try_start_pending(IndexerController* self);

static gboolean on_indexer_event_idle(gpointer user_data) {
    IdleCallbackData* data = user_data;
    IndexerController* self = data->controller;

    /* Controller was disposed while this idle callback was queued — drop it.
     * The ref we hold keeps the GObject memory valid so this check is safe. */
    if (!INDEXER_IS_CONTROLLER(self)) {
        g_free(data->library_path);
        g_object_unref(self);
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    const char* lib = data->library_path;

    switch (data->event) {
        case INDEXER_STARTED:
            g_signal_emit(self, signals[SIGNAL_STARTED], 0, lib);
            break;

        case INDEXER_PROGRESS:
            g_signal_emit(self, signals[SIGNAL_PROGRESS], 0, lib, &data->progress);
            break;

        case INDEXER_LIBRARY_UPDATED:
            g_signal_emit(self, signals[SIGNAL_LIBRARY_UPDATED], 0, lib, &data->progress);
            break;

        case INDEXER_ARTWORK_UPDATED:
            g_signal_emit(self, signals[SIGNAL_ARTWORK_UPDATED], 0, lib, &data->progress);
            break;

        case INDEXER_COMPLETED:
        case INDEXER_CANCELLED:
        case INDEXER_ERROR: {
            gboolean ok = (data->event == INDEXER_COMPLETED);
            g_signal_emit(self, signals[SIGNAL_COMPLETED], 0, lib, ok, &data->progress);

            /* Remove the finished scan from the active list */
            for (guint i = 0; i < self->active->len; i++) {
                ActiveScan* scan = g_ptr_array_index(self->active, i);
                if (strcmp(scan->library_path, lib) == 0) {
                    /* Destroy the indexer and free the slot */
                    indexer_destroy(scan->indexer);
                    g_free(scan->library_path);
                    g_free(scan->cb_data->library_path);
                    g_free(scan->cb_data);
                    g_free(scan);
                    g_ptr_array_remove_index_fast(self->active, i);
                    break;
                }
            }

            /* Start any queued libraries now that a slot is free */
            try_start_pending(self);

            /* If nothing active and nothing pending, we're done */
            if (self->active->len == 0 && self->pending->len == 0) {
                self->running = FALSE;
                g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);
                g_signal_emit(self, signals[SIGNAL_ALL_COMPLETED], 0);
            }
            break;
        }
    }

    g_free(data->library_path);
    g_object_unref(self);
    g_free(data);
    return G_SOURCE_REMOVE;
}

/* =============================================================================
 * Indexer thread callback
 * ============================================================================= */

static void on_indexer_callback(indexer_event_t event,
                                 const indexer_progress_t* progress,
                                 void* user_data) {
    ScanCallbackData* scan_data = user_data;

    IdleCallbackData* data = g_new(IdleCallbackData, 1);
    data->controller = g_object_ref(scan_data->controller);
    data->library_path = g_strdup(scan_data->library_path);
    data->event = event;
    if (progress) {
        data->progress = *progress;
    } else {
        memset(&data->progress, 0, sizeof(data->progress));
    }

    g_idle_add(on_indexer_event_idle, data);
}

/* =============================================================================
 * Helpers
 * ============================================================================= */

/* Build an indexer_t with current config for a new library scan */
static indexer_t* create_indexer_for_scan(IndexerController* self, ScanCallbackData* cb_data) {
    /* Build list of *other* library roots (exclude the one being scanned) */
    GPtrArray* others = g_ptr_array_new();
    for (guint i = 0; i < self->all_roots->len; i++) {
        const char* root = g_ptr_array_index(self->all_roots, i);
        if (strcmp(root, cb_data->library_path) != 0)
            g_ptr_array_add(others, (gpointer)root);
    }

    indexer_config_t config = {
        .thread_count = self->thread_count,
        .process_artwork = self->process_artwork,
        .art_size = self->art_size,
        .callback = on_indexer_callback,
        .user_data = cb_data,
        .mb_resolve = self->musicbrainz_resolve,
        .pg_conninfo = self->pg_conninfo,
        .mb_solr_url = self->mb_solr_url,
        .acoustid_pg_conninfo = self->acoustid_pg_conninfo,
        .acoustid_index_url = self->acoustid_index_url,
        .fanart_api_key = self->fanart_api_key,
        .other_library_roots = (const char* const*)others->pdata,
        .other_library_roots_count = others->len,
    };

    indexer_t* indexer = NULL;
    quadrature_result_t res = indexer_create(&indexer, &config);
    g_ptr_array_free(others, TRUE);  /* strings owned by all_roots, not freed here */

    if (res != QUADRATURE_OK) {
        g_critical("indexer_controller: failed to create indexer for %s", cb_data->library_path);
        return NULL;
    }
    return indexer;
}

/* Start as many pending libraries as we have free slots */
static void try_start_pending(IndexerController* self) {
    while ((int)self->active->len < self->max_concurrent && self->pending->len > 0) {
        char* lib_path = g_ptr_array_steal_index(self->pending, 0);

        ScanCallbackData* cb_data = g_new(ScanCallbackData, 1);
        cb_data->controller = self;
        cb_data->library_path = lib_path;  /* ownership transferred */

        indexer_t* indexer = create_indexer_for_scan(self, cb_data);
        if (!indexer) {
            g_free(cb_data->library_path);
            g_free(cb_data);
            continue;
        }

        const char* data_root = g_hash_table_lookup(self->data_root_map, lib_path);
        if (indexer_scan(indexer, lib_path, data_root) != QUADRATURE_OK) {
            g_warning("indexer_controller: indexer_scan failed for %s", lib_path);
            indexer_destroy(indexer);
            g_free(cb_data->library_path);
            g_free(cb_data);
            continue;
        }

        ActiveScan* scan = g_new(ActiveScan, 1);
        scan->indexer = indexer;
        scan->library_path = g_strdup(lib_path);
        scan->cb_data = cb_data;
        g_ptr_array_add(self->active, scan);
    }
}

/* =============================================================================
 * GObject implementation
 * ============================================================================= */

static void indexer_controller_get_property(GObject* object, guint prop_id,
                                             GValue* value, GParamSpec* pspec) {
    IndexerController* self = INDEXER_CONTROLLER(object);
    switch (prop_id) {
        case PROP_RUNNING:
            g_value_set_boolean(value, self->running);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void active_scan_free(gpointer p) {
    ActiveScan* scan = p;
    if (scan) {
        indexer_destroy(scan->indexer);
        g_free(scan->library_path);
        if (scan->cb_data) {
            g_free(scan->cb_data->library_path);
            g_free(scan->cb_data);
        }
        g_free(scan);
    }
}

static void indexer_controller_dispose(GObject* object) {
    IndexerController* self = INDEXER_CONTROLLER(object);

    /* Cancel all running indexers so worker threads stop */
    for (guint i = 0; i < self->active->len; i++) {
        ActiveScan* scan = g_ptr_array_index(self->active, i);
        indexer_cancel(scan->indexer);
    }
    /* Wait for them to finish before freeing */
    for (guint i = 0; i < self->active->len; i++) {
        ActiveScan* scan = g_ptr_array_index(self->active, i);
        indexer_wait(scan->indexer);
    }
    g_ptr_array_set_free_func(self->active, active_scan_free);
    g_ptr_array_free(self->active, TRUE);
    self->active = NULL;

    g_ptr_array_free(self->pending, TRUE);
    self->pending = NULL;

    g_ptr_array_free(self->all_roots, TRUE);
    self->all_roots = NULL;

    g_hash_table_destroy(self->data_root_map);
    self->data_root_map = NULL;

    g_free(self->pg_conninfo);
    self->pg_conninfo = NULL;

    g_free(self->mb_solr_url);
    self->mb_solr_url = NULL;

    g_free(self->acoustid_pg_conninfo);
    self->acoustid_pg_conninfo = NULL;

    g_free(self->acoustid_index_url);
    self->acoustid_index_url = NULL;

    g_free(self->fanart_api_key);
    self->fanart_api_key = NULL;

    G_OBJECT_CLASS(indexer_controller_parent_class)->dispose(object);
}

static void indexer_controller_class_init(IndexerControllerClass* klass) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = indexer_controller_get_property;
    object_class->dispose = indexer_controller_dispose;

    props[PROP_RUNNING] = g_param_spec_boolean(
        "running", "Running", "Whether any library scan is in progress",
        FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, props);

    /**
     * IndexerController::started:
     * @library_path: The library root that started scanning
     */
    signals[SIGNAL_STARTED] = g_signal_new(
        "started",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * IndexerController::progress:
     * @library_path: The library root being updated
     * @progress: Pointer to indexer_progress_t (valid only during signal emission)
     */
    signals[SIGNAL_PROGRESS] = g_signal_new(
        "progress",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);

    /**
     * IndexerController::library-updated:
     * @library_path: The library root whose SQLite metadata changed
     * @progress: Pointer to indexer_progress_t (valid only during signal emission)
     *
     * Emitted whenever the library database changes in a way that requires
     * the library cache to be cleared and re-warmed. Fired after:
     *   - Phases 1-3 (initial scan + metadata)
     *   - Phase 6 (MusicBrainz enrichment committed)
     */
    signals[SIGNAL_LIBRARY_UPDATED] = g_signal_new(
        "library-updated",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);

    /**
     * IndexerController::artwork-updated:
     * @library_path: The library root whose artwork atlas changed
     * @progress: Pointer to indexer_progress_t with atlas_path set
     *
     * Emitted whenever an artwork atlas is written. Fired after:
     *   - Phase 4 (album artwork atlas)
     *   - Phase 7 (artist thumbnail atlas, fanart.tv)
     * The UI should reload the atlas texture and refresh views.
     */
    signals[SIGNAL_ARTWORK_UPDATED] = g_signal_new(
        "artwork-updated",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);

    /**
     * IndexerController::completed:
     * @library_path: The library root that finished
     * @ok: TRUE on success, FALSE on cancel or error
     * @progress: Pointer to final indexer_progress_t
     */
    signals[SIGNAL_COMPLETED] = g_signal_new(
        "completed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_POINTER);

    /**
     * IndexerController::all-completed:
     * Emitted once when every library in the batch is done.
     */
    signals[SIGNAL_ALL_COMPLETED] = g_signal_new(
        "all-completed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void indexer_controller_init(IndexerController* self) {
    self->thread_count = 0;
    self->process_artwork = TRUE;
    self->art_size = 48;
    self->max_concurrent = 2;
    self->running = FALSE;

    self->all_roots = g_ptr_array_new_with_free_func(g_free);
    self->active = g_ptr_array_new();
    self->pending = g_ptr_array_new_with_free_func(g_free);
    self->data_root_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

IndexerController* indexer_controller_new(void) {
    return g_object_new(INDEXER_TYPE_CONTROLLER, NULL);
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
    self->art_size = size > 0 ? size : 48;
}

void indexer_controller_set_max_concurrent(IndexerController* self, int max_concurrent) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    self->max_concurrent = CLAMP(max_concurrent, 1, 8);
}

gboolean indexer_controller_start(IndexerController* self,
                                   const char** library_roots,
                                   const char** data_roots,
                                   gsize path_count) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), FALSE);
    g_return_val_if_fail(library_roots != NULL && path_count > 0, FALSE);

    if (self->running) {
        g_warning("indexer_controller_start: already running");
        return FALSE;
    }

    /* Remember all roots for cross-library artist art reuse */
    g_ptr_array_set_size(self->all_roots, 0);
    for (gsize i = 0; i < path_count; i++)
        g_ptr_array_add(self->all_roots, g_strdup(library_roots[i]));

    /* Store data root overrides */
    g_hash_table_remove_all(self->data_root_map);
    if (data_roots) {
        for (gsize i = 0; i < path_count; i++) {
            if (data_roots[i] && strcmp(data_roots[i], library_roots[i]) != 0)
                g_hash_table_insert(self->data_root_map,
                                    g_strdup(library_roots[i]),
                                    g_strdup(data_roots[i]));
        }
    }

    /* Populate the pending queue */
    g_ptr_array_set_size(self->pending, 0);
    for (gsize i = 0; i < path_count; i++) {
        g_ptr_array_add(self->pending, g_strdup(library_roots[i]));
    }

    self->running = TRUE;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);

    /* Kick off up to max_concurrent scans immediately */
    try_start_pending(self);

    /* If nothing could start (all indexer_scan calls failed), clean up */
    if (self->active->len == 0 && self->pending->len == 0) {
        self->running = FALSE;
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RUNNING]);
        return FALSE;
    }

    return TRUE;
}

void indexer_controller_cancel(IndexerController* self) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));

    /* Clear pending so no new scans start */
    g_ptr_array_set_size(self->pending, 0);

    /* Cancel all active scans */
    for (guint i = 0; i < self->active->len; i++) {
        ActiveScan* scan = g_ptr_array_index(self->active, i);
        indexer_cancel(scan->indexer);
    }
}

void indexer_controller_cancel_library(IndexerController* self, const char* library_path) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_return_if_fail(library_path != NULL);

    /* Remove from pending queue if not yet started */
    for (guint i = 0; i < self->pending->len; i++) {
        const char* path = g_ptr_array_index(self->pending, i);
        if (strcmp(path, library_path) == 0) {
            g_ptr_array_remove_index(self->pending, i);
            break;
        }
    }

    /* Cancel active scan if running */
    for (guint i = 0; i < self->active->len; i++) {
        ActiveScan* scan = g_ptr_array_index(self->active, i);
        if (strcmp(scan->library_path, library_path) == 0) {
            indexer_cancel(scan->indexer);
            break;
        }
    }
}

gboolean indexer_controller_is_running(IndexerController* self) {
    g_return_val_if_fail(INDEXER_IS_CONTROLLER(self), FALSE);
    return self->running;
}

void indexer_controller_set_musicbrainz_resolve(IndexerController* self, gboolean enable) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    self->musicbrainz_resolve = enable;
}

void indexer_controller_set_pg_conninfo(IndexerController* self, const char* conninfo) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_free(self->pg_conninfo);
    self->pg_conninfo = conninfo ? g_strdup(conninfo) : NULL;
}

void indexer_controller_set_mb_solr_url(IndexerController* self, const char* url) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_free(self->mb_solr_url);
    self->mb_solr_url = (url && url[0]) ? g_strdup(url) : NULL;
}

void indexer_controller_set_acoustid_pg_conninfo(IndexerController* self, const char* conninfo) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_free(self->acoustid_pg_conninfo);
    self->acoustid_pg_conninfo = conninfo ? g_strdup(conninfo) : NULL;
}

void indexer_controller_set_acoustid_index_url(IndexerController* self, const char* url) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_free(self->acoustid_index_url);
    self->acoustid_index_url = (url && url[0]) ? g_strdup(url) : NULL;
}

void indexer_controller_set_fanart_api_key(IndexerController* self, const char* api_key) {
    g_return_if_fail(INDEXER_IS_CONTROLLER(self));
    g_free(self->fanart_api_key);
    self->fanart_api_key = (api_key && api_key[0]) ? g_strdup(api_key) : NULL;
}
