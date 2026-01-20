#ifndef QUADRATURE_TYPES_H
#define QUADRATURE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef enum {
    QUADRATURE_OK = 0,
    QUADRATURE_ERROR_INVALID_PARAM,
    QUADRATURE_ERROR_OUT_OF_MEMORY,
    QUADRATURE_ERROR_AUDIO_INIT,
    QUADRATURE_ERROR_FILE_NOT_FOUND,
    QUADRATURE_ERROR_UNSUPPORTED_FORMAT,
    QUADRATURE_ERROR_DEVICE_BUSY,
    QUADRATURE_ERROR_TIMEOUT,
    QUADRATURE_ERROR_CANCELLED,
    QUADRATURE_ERROR_INTERNAL
} quadrature_result_t;

// Channel states
typedef enum {
    CHANNEL_STOPPED = 0,
    CHANNEL_PLAYING,
    CHANNEL_PAUSED,
    CHANNEL_LOADING,
    CHANNEL_ERROR
} channel_state_t;

// Shuttle/speed control modes
typedef enum {
    SHUTTLE_MODE_OFF = 0,      // Speed locked at 1.0x, no processing
    SHUTTLE_MODE_KEYLOCK,      // Variable speed, pitch preserved (Rubberband)
    SHUTTLE_MODE_PITCHED       // Variable speed, pitch shifts (Turntable)
} shuttle_mode_t;

#define MAX_CHANNELS 4
#define MAX_FILENAME_LENGTH 512

#ifdef __cplusplus
}
#endif

#endif // QUADRATURE_TYPES_H