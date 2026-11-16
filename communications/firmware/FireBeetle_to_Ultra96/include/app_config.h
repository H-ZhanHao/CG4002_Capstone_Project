/**
 * @file app_config.h
 * @brief Compile-time configuration for directly connected sensor nodes.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* PlatformIO environments supply the protocol Node ID. */
#ifndef APP_NODE_ID
#error "APP_NODE_ID must be supplied by the PlatformIO environment"
#endif

/* Diagnostic build: join Wi-Fi but do not start the application transport. */
#ifndef APP_WIFI_ASSOCIATION_ONLY
#define APP_WIFI_ASSOCIATION_ONLY 0
#endif

/* Select authenticated TLS instead of development plain TCP. */
#ifndef APP_TRANSPORT_TLS
#define APP_TRANSPORT_TLS 0
#endif

/* Physical MPU6050 is the normal source. Set to 1 only for bench testing. */
#ifndef APP_USE_MOCK_IMU
#define APP_USE_MOCK_IMU 0
#endif

/* Continuous physical-unit history/trigger/flush integration. */
#ifndef APP_PHYSICAL_INTEGRATION
#define APP_PHYSICAL_INTEGRATION 0
#endif

/* Discard pre-STOP peak notifications for idle-pause auto-resume builds. */
#ifndef APP_IDLE_RESUME_INTEGRATION
#define APP_IDLE_RESUME_INTEGRATION 0
#endif

/* Opt-in live-only START_FLUSH joins; enabled only in the new profiles. */
#ifndef APP_ONE_NODE
#define APP_ONE_NODE 0
#endif

/* Set to 0 to suspend sensor fall detection and synthetic fall events. */
#ifndef APP_FALL_DETECTION
#define APP_FALL_DETECTION 1
#endif

/* Enable server terminal control independently of the detection setting at boot. */
#ifndef APP_FALL_RUNTIME_CONTROL
#define APP_FALL_RUNTIME_CONTROL 0
#endif

/* Sampling measurements are enabled only by the dedicated diagnostic builds. */
#ifndef APP_SAMPLING_DIAGNOSTICS
#define APP_SAMPLING_DIAGNOSTICS 0
#endif

#if APP_NODE_ID < 1 || APP_NODE_ID > 6
#error "APP_NODE_ID must be in the range 1 to 6"
#endif

#endif
