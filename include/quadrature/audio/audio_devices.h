#ifndef QUADRATURE_AUDIO_DEVICES_H
#define QUADRATURE_AUDIO_DEVICES_H

#include "../core/types.h"
#include "audio_pipeline.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio output device information
typedef struct {
    char node_name[256];      // For PW_KEY_TARGET_OBJECT
    char description[256];    // Human-readable name for UI
    char serial[64];          // object.serial for stable identification
    uint32_t id;              // PipeWire node ID
} audio_device_t;

// List of available audio devices
typedef struct {
    audio_device_t *devices;
    int count;
    int capacity;
} audio_device_list_t;

// Enumerate available PipeWire audio sinks
// The pipeline must be started before calling this function
// Caller must free the list with audio_devices_free()
quadrature_result_t audio_devices_enumerate(audio_pipeline_t *pipeline, audio_device_list_t *list);

// Free device list resources
void audio_devices_free(audio_device_list_t *list);

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_AUDIO_DEVICES_H
