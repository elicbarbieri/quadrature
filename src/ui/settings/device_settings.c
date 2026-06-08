/**
 * Quadrature Device Settings
 *
 * Device enumeration, format/quantum configuration, channel settings frames.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "internal.h"
#include "../../audio/internal.h"
#include <string.h>


/* ═══════════════════════════════════════════════════════════════════════════
 * Quantum (Buffer Size) Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint32_t quantum_values[] = { 32, 64, 128, 256, 512, 1024, 2048 };
static const int quantum_value_count = 7;

static int quantum_to_index(uint32_t quantum) {
    for (int i = 0; i < quantum_value_count; i++)
        if (quantum_values[i] == quantum) return i;
    /* Fallback: find index of APP_SETTINGS_DEFAULT_QUANTUM */
    for (int i = 0; i < quantum_value_count; i++)
        if (quantum_values[i] == APP_SETTINGS_DEFAULT_QUANTUM) return i;
    return 3;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Per-Channel Device Model Management
 *
 * Each channel dropdown gets its own filtered model that excludes devices
 * assigned to other channels. This prevents duplicate device assignments.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Rebuild all per-channel device models, filtering out devices assigned to other channels.
 * Preserves current selections. Must be called with settings_initializing = TRUE or
 * from within a callback that guards against recursion.
 */
static void rebuild_device_models(UiWindow *w);

static gboolean rebuild_device_models_idle(gpointer user_data) {
    UiWindow *w = UI_WINDOW(user_data);
    w->device_rebuild_idle_id = 0;
    rebuild_device_models(w);
    return G_SOURCE_REMOVE;
}

static void rebuild_device_models(UiWindow *w) {
    if (!w->device_names || w->device_count == 0) return;

    /* First, collect current assignments by device_names index */
    int assigned_by[MAX_CHANNELS];  /* device_names index for each channel, -1 = none */
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        assigned_by[ch] = -1;
        const char *dev = NULL;
        if (w->settings)
            dev = app_settings_get_channel_device(w->settings, ch);
        if (dev && dev[0]) {
            for (int j = 0; j < w->device_count; j++) {
                if (g_strcmp0(w->device_names[j], dev) == 0) {
                    assigned_by[ch] = j;
                    break;
                }
            }
        }
    }

    /* Block device-changed signals during model rebuild to prevent re-entrancy.
     * g_signal_handler_block is the GLib-canonical way — works regardless of
     * internal freeze/thaw or deferred notify::selected emissions. */
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (w->device_drops[ch] && w->device_drop_handler_ids[ch])
            g_signal_handler_block(w->device_drops[ch], w->device_drop_handler_ids[ch]);
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (!w->device_drops[ch]) continue;

        /* Create fresh model */
        GtkStringList *model = gtk_string_list_new(NULL);
        gtk_string_list_append(model, "None");
        w->device_model_map[ch][0] = -1;

        int model_idx = 1;
        int selected = 0;  /* Default to "None" */

        for (int j = 0; j < w->device_count && model_idx < 63; j++) {
            /* Include if: unassigned OR assigned to THIS channel */
            gboolean taken = FALSE;
            for (int other = 0; other < MAX_CHANNELS; other++) {
                if (other != ch && assigned_by[other] == j) {
                    taken = TRUE;
                    break;
                }
            }
            if (taken) continue;

            /* Need to get description — search was done at enum time.
             * We store node_name in device_names; description is in the
             * old shared model. We'll read it from there. */
            /* Build description: for now use the node_name. The actual
             * description was stored in the old shared model entries. */
            /* We need the description. Let's store it. */
            gtk_string_list_append(model, w->device_descs ? w->device_descs[j] : w->device_names[j]);
            w->device_model_map[ch][model_idx] = j;

            if (assigned_by[ch] == j) {
                selected = model_idx;
            }
            model_idx++;
        }

        /* Replace old model */
        g_clear_object(&w->device_models[ch]);
        w->device_models[ch] = model;
        gtk_drop_down_set_model(GTK_DROP_DOWN(w->device_drops[ch]), G_LIST_MODEL(model));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w->device_drops[ch]), (guint)selected);
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        if (w->device_drops[ch] && w->device_drop_handler_ids[ch])
            g_signal_handler_unblock(w->device_drops[ch], w->device_drop_handler_ids[ch]);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Async Pipeline Operations
 *
 * audio_pipeline_set_player_device() and audio_pipeline_set_player_quantum()
 * both acquire pw_thread_loop_lock() internally.  Calling them directly from
 * GTK signal handlers freezes the GTK main loop for the duration of the lock
 * (potentially 100s of ms for USB audio devices).
 *
 * These helpers dispatch the operation onto a GLib thread-pool worker so the
 * GTK event loop stays responsive.  No completion callback is needed — the
 * operations are idempotent and the UI already reflects the desired state.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_pipeline_t *pipeline;
    int               channel;
    char              device_name[256];  /* empty string means NULL (no device) */
    gboolean          is_quantum_op;
    uint32_t          quantum;
} PipelineOpTask;

static void pipeline_op_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    PipelineOpTask *t = data;
    if (t->is_quantum_op) {
        audio_pipeline_set_player_quantum(t->pipeline, t->channel, t->quantum);
    } else {
        audio_pipeline_set_player_device(t->pipeline, t->channel,
                                         t->device_name[0] ? t->device_name : NULL);
    }
    g_task_return_boolean(task, TRUE);
}

static void dispatch_player_device(audio_pipeline_t *pipeline, int channel, const char *name) {
    PipelineOpTask *t = g_new0(PipelineOpTask, 1);
    t->pipeline = pipeline;
    t->channel  = channel;
    if (name)
        g_strlcpy(t->device_name, name, sizeof(t->device_name));
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, t, g_free);
    g_task_run_in_thread(task, pipeline_op_thread);
    g_object_unref(task);
}

static void dispatch_player_quantum(audio_pipeline_t *pipeline, int channel, uint32_t quantum) {
    PipelineOpTask *t = g_new0(PipelineOpTask, 1);
    t->pipeline      = pipeline;
    t->channel       = channel;
    t->is_quantum_op = TRUE;
    t->quantum       = quantum;
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, t, g_free);
    g_task_run_in_thread(task, pipeline_op_thread);
    g_object_unref(task);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Enumeration
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    audio_pipeline_t *pipeline;
    audio_device_list_t devices;
    quadrature_result_t result;
} DeviceEnumData;

static void device_enum_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    DeviceEnumData *d = data;
    d->result = audio_devices_enumerate(d->pipeline, &d->devices);
    g_task_return_pointer(task, d, NULL);
}

static void device_enum_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    UiWindow *w = UI_WINDOW(data);
    GError *err = NULL;
    DeviceEnumData *d = g_task_propagate_pointer(G_TASK(res), &err);

    if (err) { g_error_free(err); return; }

    /* Block callbacks while restoring settings */
    w->settings_initializing = TRUE;

    /* Cancel any stale deferred rebuild — we're doing a full re-enumeration */
    if (w->device_rebuild_idle_id) {
        g_source_remove(w->device_rebuild_idle_id);
        w->device_rebuild_idle_id = 0;
    }

    /* Free old device arrays */
    if (w->device_names) {
        for (int i = 0; i < w->device_count; i++) {
            g_free(w->device_names[i]);
            g_free(w->device_descs[i]);
        }
        g_free(w->device_names);
        g_free(w->device_descs);
    }
    for (int i = 0; i < MAX_CHANNELS; i++)
        g_clear_object(&w->device_models[i]);

    /* Store device names and descriptions */
    if (d->result == QUADRATURE_OK && d->devices.count > 0) {
        w->device_names = g_new0(char*, d->devices.count);
        w->device_descs = g_new0(char*, d->devices.count);
        w->device_count = d->devices.count;
        for (int i = 0; i < d->devices.count; i++) {
            w->device_names[i] = g_strdup(d->devices.devices[i].node_name);
            w->device_descs[i] = g_strdup(d->devices.devices[i].description);
        }
        audio_devices_free(&d->devices);
    } else {
        w->device_names = NULL;
        w->device_descs = NULL;
        w->device_count = 0;
    }

    /* Build per-channel filtered device models */
    rebuild_device_models(w);

    /* Apply saved settings to pipeline and channel strips */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        const char *saved = w->settings ? app_settings_get_channel_device(w->settings, i) : NULL;

        /* Find if saved device exists in enumerated list */
        gboolean found = FALSE;
        if (saved && saved[0]) {
            for (int j = 0; j < w->device_count; j++) {
                if (g_strcmp0(w->device_names[j], saved) == 0) {
                    found = TRUE;
                    break;
                }
            }
        }

        /* Update channel state using layered API */
        if (w->channels[i]) {
            if (!saved || !saved[0]) {
                ui_channel_strip_set_device_name(w->channels[i], NULL);
                ui_channel_strip_set_device_state(w->channels[i], DEVICE_STATE_UNCONFIGURED);
            } else if (!found) {
                ui_channel_strip_set_device_name(w->channels[i], saved);
                ui_channel_strip_set_device_state(w->channels[i], DEVICE_STATE_INVALID);
                /* Clear pipeline target so that when the device reappears the
                 * set_player_device() early-out sees a transition (NULL → name)
                 * and recreates the stream on the correct node. Without this,
                 * PW silently migrates the orphaned stream to the default sink
                 * and the early-out skips recreation on replug. */
                dispatch_player_device(w->pipeline, i, NULL);
            } else {
                ui_channel_strip_set_device_name(w->channels[i], NULL);
                ui_channel_strip_set_device_state(w->channels[i], DEVICE_STATE_VALID);
                /* Dispatch off GTK thread — set_player_device acquires
                 * pw_thread_loop_lock which would block the main loop. */
                dispatch_player_device(w->pipeline, i, saved);
            }
        }

        /* Restore exclusive mode and apply to pipeline */
        if (w->exclusive_checks[i] && w->settings) {
            gboolean excl = app_settings_get_channel_exclusive(w->settings, i);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(w->exclusive_checks[i]), excl);
            if (w->pipeline) {
                audio_pipeline_set_player_exclusive(w->pipeline, i, excl);
            }
        }

        /* Restore format selection */
        if (w->format_drops[i] && w->settings) {
            output_format_t fmt = app_settings_get_channel_format(w->settings, i);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(w->format_drops[i]), (guint)fmt);
        }

        /* Restore GPIO address */
        if (w->gpio_entries[i] && w->settings) {
            const char *gpio = app_settings_get_channel_gpio(w->settings, i);
            gtk_editable_set_text(GTK_EDITABLE(w->gpio_entries[i]), gpio ? gpio : "");

            /* Initialize GPIO handler if address is configured */
            if (gpio && gpio[0] != '\0') {
                restart_gpio_handler(w, i);
            }
        }

        /* Restore quantum selection and apply to pipeline */
        if (w->quantum_drops[i] && w->settings) {
            uint32_t quantum = app_settings_get_channel_quantum(w->settings, i);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(w->quantum_drops[i]), (guint)quantum_to_index(quantum));
            if (w->pipeline) {
                /* Dispatch off GTK thread — same as device above. */
                dispatch_player_quantum(w->pipeline, i, quantum);
            }
        }
    }

    /* Re-enable callbacks */
    w->settings_initializing = FALSE;

    g_free(d);
}

void populate_devices_async(UiWindow *w) {
    if (!w->pipeline) return;
    DeviceEnumData *d = g_new0(DeviceEnumData, 1);
    d->pipeline = w->pipeline;
    GTask *task = g_task_new(NULL, NULL, device_enum_done, w);
    g_task_set_task_data(task, d, NULL);
    g_task_run_in_thread(task, device_enum_thread);
    g_object_unref(task);
}

static void on_exclusive_toggled(GtkCheckButton *btn, gpointer data) {
    UiWindow *w = UI_WINDOW(data);
    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(btn) == w->exclusive_checks[i]) { ch = i; break; }
    }
    if (ch < 0) return;

    gboolean excl = gtk_check_button_get_active(btn);
    if (w->settings) {
        app_settings_set_channel_exclusive(w->settings, ch, excl);
        settings_save_debounced(w);
    }
    if (w->pipeline) {
        audio_pipeline_set_player_exclusive(w->pipeline, ch, excl);
        /* Recreate stream to apply exclusive change — dispatch off the GTK thread */
        const char *dev = app_settings_get_channel_device(w->settings, ch);
        if (dev && dev[0])
            dispatch_player_device(w->pipeline, ch, dev);
    }
}

static void on_device_changed(GtkDropDown *drop, GParamSpec *p, gpointer data) {
    (void)p;
    UiWindow *w = UI_WINDOW(data);

    /* Skip save during initialization - settings are being restored, not changed */
    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(drop) == w->device_drops[i]) { ch = i; break; }
    }
    if (ch < 0 || !w->pipeline) return;

    /* Map from per-channel model index to actual device name */
    guint sel = gtk_drop_down_get_selected(drop);
    const char *name = NULL;
    if (sel > 0) {
        int dev_idx = w->device_model_map[ch][sel];
        if (dev_idx >= 0 && dev_idx < w->device_count)
            name = w->device_names[dev_idx];
    }

    dispatch_player_device(w->pipeline, ch, name);

    if (w->channels[ch]) {
        ui_channel_strip_set_device_name(w->channels[ch], NULL);
        ui_channel_strip_set_device_state(w->channels[ch],
            name ? DEVICE_STATE_VALID : DEVICE_STATE_UNCONFIGURED);
    }

    if (w->settings) {
        app_settings_set_channel_device(w->settings, ch, name);
        settings_save_debounced(w);
    }

    /* Defer rebuild so we don't replace the dropdown model mid-emission of notify::selected */
    if (w->device_rebuild_idle_id == 0)
        w->device_rebuild_idle_id = g_idle_add(rebuild_device_models_idle, w);
}

static void on_format_changed(GtkDropDown *drop, GParamSpec *p, gpointer data) {
    (void)p;
    UiWindow *w = UI_WINDOW(data);

    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(drop) == w->format_drops[i]) { ch = i; break; }
    }
    if (ch < 0) return;

    guint sel = gtk_drop_down_get_selected(drop);
    if (w->settings && sel < OUTPUT_FORMAT_COUNT) {
        app_settings_set_channel_format(w->settings, ch, (output_format_t)sel);
        settings_save_debounced(w);
    }
}

static void on_quantum_changed(GtkDropDown *drop, GParamSpec *p, gpointer data) {
    (void)p;
    UiWindow *w = UI_WINDOW(data);

    if (w->settings_initializing) return;

    int ch = -1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (GTK_WIDGET(drop) == w->quantum_drops[i]) { ch = i; break; }
    }
    if (ch < 0) return;

    guint sel = gtk_drop_down_get_selected(drop);
    if (sel >= (guint)quantum_value_count) return;

    uint32_t quantum = quantum_values[sel];
    if (w->settings) {
        app_settings_set_channel_quantum(w->settings, ch, quantum);
        settings_save_debounced(w);
    }
    if (w->pipeline)
        dispatch_player_quantum(w->pipeline, ch, quantum);
}

/* Forward declaration for GPIO handler restart */

GtkWidget *make_channel_settings_frame(UiWindow *w, int channel) {
    GtkBuilder *builder = gtk_builder_new_from_resource(
        "/org/quadrature/ui/channel_settings_frame.ui");

    GtkWidget *frame = GTK_WIDGET(gtk_builder_get_object(builder, "channel_frame"));
    g_object_ref(frame);

    /* Title is the only per-channel text */
    char title[32];
    snprintf(title, sizeof(title), "Channel %d", channel + 1);
    gtk_frame_set_label(GTK_FRAME(frame), title);

    /* Extract widget refs for signal wiring + settings restore */
    w->device_drops[channel] = GTK_WIDGET(gtk_builder_get_object(builder, "device_dropdown"));
    w->gpio_entries[channel] = GTK_WIDGET(gtk_builder_get_object(builder, "gpio_entry"));
    w->exclusive_checks[channel] = GTK_WIDGET(gtk_builder_get_object(builder, "exclusive_check"));
    w->format_drops[channel] = GTK_WIDGET(gtk_builder_get_object(builder, "format_dropdown"));
    w->quantum_drops[channel] = GTK_WIDGET(gtk_builder_get_object(builder, "quantum_dropdown"));

    /* Shared models (built once, reused across channels) */
    if (!w->format_model) {
        w->format_model = gtk_string_list_new(NULL);
        for (int i = 0; i < OUTPUT_FORMAT_COUNT; i++)
            gtk_string_list_append(w->format_model, app_settings_format_name((output_format_t)i));
    }
    if (!w->quantum_model) {
        w->quantum_model = gtk_string_list_new(NULL);
        for (int i = 0; i < quantum_value_count; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", quantum_values[i]);
            gtk_string_list_append(w->quantum_model, buf);
        }
    }

    gtk_drop_down_set_model(GTK_DROP_DOWN(w->format_drops[channel]),
                            G_LIST_MODEL(w->format_model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->format_drops[channel]),
                               OUTPUT_FORMAT_16BIT_48000);

    gtk_drop_down_set_model(GTK_DROP_DOWN(w->quantum_drops[channel]),
                            G_LIST_MODEL(w->quantum_model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->quantum_drops[channel]),
                               (guint)quantum_to_index(APP_SETTINGS_DEFAULT_QUANTUM));

    /* Signal connections */
    w->device_drop_handler_ids[channel] =
        g_signal_connect(w->device_drops[channel], "notify::selected",
                         G_CALLBACK(on_device_changed), w);
    g_signal_connect(w->gpio_entries[channel], "changed",
                     G_CALLBACK(on_gpio_changed), w);
    g_signal_connect(w->exclusive_checks[channel], "toggled",
                     G_CALLBACK(on_exclusive_toggled), w);
    g_signal_connect(w->format_drops[channel], "notify::selected",
                     G_CALLBACK(on_format_changed), w);
    g_signal_connect(w->quantum_drops[channel], "notify::selected",
                     G_CALLBACK(on_quantum_changed), w);

    g_object_unref(builder);
    return frame;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Device Hot-Plug Monitor (UI Side)
 *
 * audio_devices_monitor_start() invokes on_pw_device_changed() from the
 * PipeWire thread.  We marshal back to the GTK main thread via
 * g_main_context_invoke(), then debounce with a 300 ms GLib timeout before
 * triggering a full re-enumeration.  This collapses the burst of events that
 * arrives when a multi-node USB device connects.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEVICE_HOTPLUG_DEBOUNCE_MS 300

static gboolean device_hotplug_timeout(gpointer user_data) {
    UiWindow *w = UI_WINDOW(user_data);
    w->device_hotplug_timer_id = 0;
    populate_devices_async(w);
    return G_SOURCE_REMOVE;
}

static gboolean device_hotplug_idle(gpointer user_data) {
    UiWindow *w = UI_WINDOW(user_data);

    /* If the window has already been disposed, bail out */
    if (!w->pipeline)
        return G_SOURCE_REMOVE;

    /* Cancel any existing debounce timer and start a fresh one */
    if (w->device_hotplug_timer_id) {
        g_source_remove(w->device_hotplug_timer_id);
        w->device_hotplug_timer_id = 0;
    }
    w->device_hotplug_timer_id =
        g_timeout_add(DEVICE_HOTPLUG_DEBOUNCE_MS, device_hotplug_timeout, w);
    return G_SOURCE_REMOVE;
}

/* Called from the PipeWire thread — marshal to GTK main thread */
static void on_pw_device_changed(void *user_data) {
    g_main_context_invoke(NULL, device_hotplug_idle, user_data);
}

void setup_device_monitor(UiWindow *w) {
    if (!w->pipeline) return;
    audio_devices_monitor_start(w->pipeline, on_pw_device_changed, w);
}

void teardown_device_monitor(UiWindow *w) {
    if (!w->pipeline) return;
    audio_devices_monitor_stop(w->pipeline);
}
