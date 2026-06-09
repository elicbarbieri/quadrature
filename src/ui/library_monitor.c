/**
 * Library Monitor — detects mount/unmount events for library paths.
 *
 * Uses GVolumeMonitor for instant notification of mount changes, plus a
 * periodic stat() heartbeat as fallback for cases GVolumeMonitor misses
 * (NFS staleness, bind mounts, etc.).
 *
 * Emits "availability-changed" signal when a library path transitions
 * between accessible and inaccessible.  The signal carries (lib_idx, available).
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

#include <glib/gstdio.h>
#include <sys/types.h>
#include <sys/stat.h>

/* =============================================================================
 * Private struct
 * ============================================================================= */

struct _LibraryMonitor {
    GObject parent_instance;

    library_cache_t *cache;   /* borrowed */
    app_settings_t *settings; /* borrowed */

    GVolumeMonitor *vol_monitor; /* system mount monitor */
    gulong sig_mount_added;
    gulong sig_mount_removed;
    gulong sig_mount_pre_unmount;

    guint heartbeat_timer;    /* g_timeout_add ID */
    gboolean *prev_available; /* edge-detection array */
    int prev_slot_count;      /* length of prev_available */
};

/* =============================================================================
 * Signals
 * ============================================================================= */

enum { SIG_AVAILABILITY_CHANGED, N_MONITOR_SIGNALS };

static guint monitor_signals[N_MONITOR_SIGNALS];

G_DEFINE_TYPE(LibraryMonitor, library_monitor, G_TYPE_OBJECT)

/* =============================================================================
 * Core logic — stat() each library path, emit on edge transitions
 * ============================================================================= */

#define HEARTBEAT_INTERVAL_MS 5000

static void
check_all_libraries(LibraryMonitor *self)
{
    if (!self->cache || !self->settings)
        return;

    int count = library_cache_get_library_count(self->cache);

    /* Handle dynamic slot count changes (library added/removed via UI) */
    if (count != self->prev_slot_count) {
        self->prev_available = g_renew(gboolean, self->prev_available, MAX(count, 1));
        for (int i = self->prev_slot_count; i < count; i++)
            self->prev_available[i] = TRUE;
        self->prev_slot_count = count;
    }

    for (int i = 0; i < count; i++) {
        if (i >= (int)self->settings->library_count)
            break;
        const char *path = self->settings->libraries[i].path;
        if (!path)
            continue;

        GStatBuf st;
        gboolean accessible = (g_stat(path, &st) == 0 && S_ISDIR(st.st_mode));

        if (accessible != self->prev_available[i]) {
            self->prev_available[i] = accessible;
            int bitmap_idx = self->settings->libraries[i].library_index;
            library_cache_set_available(self->cache, bitmap_idx, accessible);
            g_signal_emit(
                self, monitor_signals[SIG_AVAILABILITY_CHANGED], 0, bitmap_idx, accessible);
        }
    }
}

/* =============================================================================
 * GVolumeMonitor callbacks — recheck on any mount event
 * ============================================================================= */

static void
on_mount_event(GVolumeMonitor *vm, GMount *mount, gpointer data)
{
    (void)vm;
    (void)mount;
    check_all_libraries(LIBRARY_MONITOR(data));
}

/* =============================================================================
 * Heartbeat timer
 * ============================================================================= */

static gboolean
heartbeat_tick(gpointer data)
{
    check_all_libraries(LIBRARY_MONITOR(data));
    return G_SOURCE_CONTINUE;
}

/* =============================================================================
 * GObject lifecycle
 * ============================================================================= */

static void
library_monitor_dispose(GObject *obj)
{
    LibraryMonitor *self = LIBRARY_MONITOR(obj);

    library_monitor_stop(self);

    G_OBJECT_CLASS(library_monitor_parent_class)->dispose(obj);
}

static void
library_monitor_finalize(GObject *obj)
{
    LibraryMonitor *self = LIBRARY_MONITOR(obj);

    g_free(self->prev_available);
    self->prev_available = NULL;

    G_OBJECT_CLASS(library_monitor_parent_class)->finalize(obj);
}

static void
library_monitor_class_init(LibraryMonitorClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->dispose = library_monitor_dispose;
    obj_class->finalize = library_monitor_finalize;

    /**
     * LibraryMonitor::availability-changed:
     * @monitor: the monitor
     * @lib_idx: library slot index that changed
     * @available: TRUE if now accessible, FALSE if disconnected
     */
    monitor_signals[SIG_AVAILABILITY_CHANGED] = g_signal_new("availability-changed",
                                                             LIBRARY_TYPE_MONITOR,
                                                             G_SIGNAL_RUN_LAST,
                                                             0,    /* class_offset */
                                                             NULL, /* accumulator */
                                                             NULL, /* accu_data */
                                                             NULL, /* c_marshaller (auto) */
                                                             G_TYPE_NONE,
                                                             2,
                                                             G_TYPE_INT,
                                                             G_TYPE_BOOLEAN);
}

static void
library_monitor_init(LibraryMonitor *self)
{
    self->prev_slot_count = 0;
    self->prev_available = NULL;
    self->heartbeat_timer = 0;
    self->vol_monitor = NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

LibraryMonitor *
library_monitor_new(library_cache_t *cache, app_settings_t *settings)
{
    LibraryMonitor *self = g_object_new(LIBRARY_TYPE_MONITOR, NULL);
    self->cache = cache;
    self->settings = settings;
    return self;
}

void
library_monitor_start(LibraryMonitor *self)
{
    g_return_if_fail(LIBRARY_IS_MONITOR(self));

    /* Connect to GVolumeMonitor (singleton, main-thread only) */
    if (!self->vol_monitor) {
        self->vol_monitor = g_volume_monitor_get();
        self->sig_mount_added
            = g_signal_connect(self->vol_monitor, "mount-added", G_CALLBACK(on_mount_event), self);
        self->sig_mount_removed = g_signal_connect(
            self->vol_monitor, "mount-removed", G_CALLBACK(on_mount_event), self);
        self->sig_mount_pre_unmount = g_signal_connect(
            self->vol_monitor, "mount-pre-unmount", G_CALLBACK(on_mount_event), self);
    }

    /* Immediate initial check */
    check_all_libraries(self);

    /* Start periodic heartbeat */
    if (self->heartbeat_timer == 0)
        self->heartbeat_timer = g_timeout_add(HEARTBEAT_INTERVAL_MS, heartbeat_tick, self);
}

void
library_monitor_stop(LibraryMonitor *self)
{
    g_return_if_fail(LIBRARY_IS_MONITOR(self));

    if (self->heartbeat_timer) {
        g_source_remove(self->heartbeat_timer);
        self->heartbeat_timer = 0;
    }

    if (self->vol_monitor) {
        if (self->sig_mount_added)
            g_signal_handler_disconnect(self->vol_monitor, self->sig_mount_added);
        if (self->sig_mount_removed)
            g_signal_handler_disconnect(self->vol_monitor, self->sig_mount_removed);
        if (self->sig_mount_pre_unmount)
            g_signal_handler_disconnect(self->vol_monitor, self->sig_mount_pre_unmount);
        self->sig_mount_added = 0;
        self->sig_mount_removed = 0;
        self->sig_mount_pre_unmount = 0;
        g_clear_object(&self->vol_monitor);
    }
}

void
library_monitor_check_now(LibraryMonitor *self)
{
    g_return_if_fail(LIBRARY_IS_MONITOR(self));
    check_all_libraries(self);
}
