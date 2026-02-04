/**
 * Quadrature - Core Types and Error Codes
 *
 * This is the foundational header that all other Quadrature modules depend on.
 * It defines result codes, channel states, and common types.
 */

#ifndef QUADRATURE_H
#define QUADRATURE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Result Codes
 * ============================================================================= */

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

/* =============================================================================
 * Channel States
 * ============================================================================= */

typedef enum {
    CHANNEL_STOPPED = 0,
    CHANNEL_PLAYING,
    CHANNEL_PAUSED,
    CHANNEL_ERROR
} channel_state_t;

/* =============================================================================
 * Shuttle/Speed Control Modes
 * ============================================================================= */

typedef enum {
    SHUTTLE_MODE_OFF = 0,      /* Speed locked at 1.0x, no processing */
    SHUTTLE_MODE_KEYLOCK,      /* Variable speed, pitch preserved (Rubberband) */
    SHUTTLE_MODE_PITCHED       /* Variable speed, pitch shifts (Turntable) */
} shuttle_mode_t;

/* =============================================================================
 * Constants
 * ============================================================================= */

#define QUADRATURE_MAX_CHANNELS 4
#define QUADRATURE_MAX_FILENAME_LENGTH 512

#ifdef __cplusplus
}
#endif

#endif /* QUADRATURE_H */
