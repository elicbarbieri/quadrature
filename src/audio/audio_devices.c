#include "internal.h"

#include <glib.h>
#include <spa/utils/dict.h>

#include <stdlib.h>
#include <string.h>

// Timeout for device enumeration (seconds)
#define DEVICE_ENUM_TIMEOUT_SEC 2

// Context for device enumeration
typedef struct {
    audio_device_list_t *list;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_thread_loop *loop;  // For signaling when done
    int pending_sync;
    int done;
} enum_context_t;

static void device_list_add(audio_device_list_t *list, const audio_device_t *device) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        audio_device_t *new_devices = realloc(list->devices, new_cap * sizeof(audio_device_t));
        if (!new_devices) return;
        list->devices = new_devices;
        list->capacity = new_cap;
    }
    list->devices[list->count++] = *device;
}

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
                                   const char *type, uint32_t version,
                                   const struct spa_dict *props) {
    (void)permissions;
    (void)version;
    enum_context_t *ctx = data;

    if (props == NULL || type == NULL)
        return;

    // Only care about nodes
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    // Check if this is an audio sink
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Audio/Sink") != 0)
        return;

    // Get device info
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);

    if (!node_name)
        return;

    audio_device_t device = {0};
    device.id = id;

    strncpy(device.node_name, node_name, sizeof(device.node_name) - 1);

    if (description) {
        strncpy(device.description, description, sizeof(device.description) - 1);
    } else {
        // Fall back to node name if no description
        strncpy(device.description, node_name, sizeof(device.description) - 1);
    }

    if (serial) {
        strncpy(device.serial, serial, sizeof(device.serial) - 1);
    }

    device_list_add(ctx->list, &device);

    g_debug("Found audio sink: %s (%s)", device.description, device.node_name);
}

static void registry_event_global_remove(void *data, uint32_t id) {
    (void)data;
    (void)id;
    // We don't need to handle removal during enumeration
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static void on_core_done(void *data, uint32_t id, int seq) {
    enum_context_t *ctx = data;
    if (id == PW_ID_CORE && seq == ctx->pending_sync) {
        ctx->done = 1;
        pw_thread_loop_signal(ctx->loop, false);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
};

quadrature_result_t audio_devices_enumerate(audio_pipeline_t *pipeline, audio_device_list_t *list) {
    if (!pipeline || !list)
        return QUADRATURE_ERROR_INVALID_PARAM;

    // Validate pipeline is started
    if (!atomic_load(&pipeline->system_active)) {
        g_warning("Device enumeration: pipeline not started");
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Initialize list
    memset(list, 0, sizeof(*list));

    // Get PipeWire core from pipeline (need access to internal fields)
    // We need the core and loop from the pipeline structure
    struct pw_core *core = pipeline->core;
    struct pw_thread_loop *loop = pipeline->loop;

    if (!core || !loop)
        return QUADRATURE_ERROR_INTERNAL;

    enum_context_t ctx = {0};
    ctx.list = list;
    ctx.loop = loop;

    // Lock the thread loop
    pw_thread_loop_lock(loop);

    // Get registry
    ctx.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!ctx.registry) {
        pw_thread_loop_unlock(loop);
        return QUADRATURE_ERROR_INTERNAL;
    }

    // Add registry listener
    pw_registry_add_listener(ctx.registry, &ctx.registry_listener, &registry_events, &ctx);

    // Add core listener for sync
    struct spa_hook core_listener;
    pw_core_add_listener(core, &core_listener, &core_events, &ctx);

    // Sync to get all current nodes
    ctx.pending_sync = pw_core_sync(core, PW_ID_CORE, 0);

    // Wait for sync to complete with timeout
    while (!ctx.done) {
        int wait_result = pw_thread_loop_timed_wait(loop, DEVICE_ENUM_TIMEOUT_SEC);
        if (wait_result != 0) {
            g_warning("Device enumeration timed out after %d seconds", DEVICE_ENUM_TIMEOUT_SEC);
            break;  // Return partial results
        }
    }

    // Cleanup
    spa_hook_remove(&core_listener);
    spa_hook_remove(&ctx.registry_listener);
    pw_proxy_destroy((struct pw_proxy *)ctx.registry);

    pw_thread_loop_unlock(loop);

    g_message("Enumerated %d audio output devices", list->count);
    return QUADRATURE_OK;
}

void audio_devices_free(audio_device_list_t *list) {
    if (!list)
        return;

    free(list->devices);
    list->devices = NULL;
    list->count = 0;
    list->capacity = 0;
}
