#ifndef QUADRATURE_CONFIG_H
#define QUADRATURE_CONFIG_H

/**
 * Build Configuration
 *
 * Compile-time feature flags based on build mode:
 *
 * DEBUG Build (QUADRATURE_BUILD_DEBUG):
 *   - Single local library source
 *   - No network features
 *   - No daemon mode
 *   - No fanotify watching
 *
 * BROADCAST Build (QUADRATURE_BUILD_BROADCAST):
 *   - Multiple library sources (local, remote, portable)
 *   - Network mount support (NFS, SMB)
 *   - fanotify filesystem watching
 *   - Daemon mode support
 *   - Replication support
 */

// Default to DEBUG if neither is defined
#if !defined(QUADRATURE_BUILD_DEBUG) && !defined(QUADRATURE_BUILD_BROADCAST)
#define QUADRATURE_BUILD_DEBUG
#endif

// Feature flags
#ifdef QUADRATURE_BUILD_BROADCAST
    #define QUADRATURE_FEATURE_MULTIPLE_SOURCES  1
    #define QUADRATURE_FEATURE_NETWORK_MOUNTS    1
    #define QUADRATURE_FEATURE_FANOTIFY          1
    #define QUADRATURE_FEATURE_DAEMON            1
    #define QUADRATURE_FEATURE_REPLICATION       1
#else
    #define QUADRATURE_FEATURE_MULTIPLE_SOURCES  0
    #define QUADRATURE_FEATURE_NETWORK_MOUNTS    0
    #define QUADRATURE_FEATURE_FANOTIFY          0
    #define QUADRATURE_FEATURE_DAEMON            0
    #define QUADRATURE_FEATURE_REPLICATION       0
#endif

// Version info
#define QUADRATURE_VERSION_MAJOR 0
#define QUADRATURE_VERSION_MINOR 1
#define QUADRATURE_VERSION_PATCH 0
#define QUADRATURE_VERSION_STRING "0.1.0"

// Default paths
#ifndef QUADRATURE_DATA_DIR
    #define QUADRATURE_DATA_DIR "~/.local/share/quadrature"
#endif

#ifndef QUADRATURE_CONFIG_DIR
    #define QUADRATURE_CONFIG_DIR "~/.config/quadrature"
#endif

// Build mode name
#ifdef QUADRATURE_BUILD_BROADCAST
    #define QUADRATURE_BUILD_MODE_NAME "Broadcast"
#else
    #define QUADRATURE_BUILD_MODE_NAME "Debug"
#endif

#endif // QUADRATURE_CONFIG_H
