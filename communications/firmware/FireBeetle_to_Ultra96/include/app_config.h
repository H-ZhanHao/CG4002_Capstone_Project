/**
 * @file app_config.h
 * @brief Compile-time role selection for the Stage 5 two-board build.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define APP_ROLE_HUB  1
#define APP_ROLE_LEAF 2

/* PlatformIO environments supply the physical role and protocol Node ID. */
#ifndef APP_NODE_ROLE
#error "APP_NODE_ROLE must be supplied by the PlatformIO environment"
#endif

#ifndef APP_NODE_ID
#error "APP_NODE_ID must be supplied by the PlatformIO environment"
#endif

/* Diagnostic build: join Wi-Fi but do not start the application transport. */
#ifndef APP_WIFI_ASSOCIATION_ONLY
#define APP_WIFI_ASSOCIATION_ONLY 0
#endif

/* Hub build with only its local sensor source and no ESP-NOW leaf. */
#ifndef APP_HUB_LOCAL_ONLY
#define APP_HUB_LOCAL_ONLY 0
#endif

/* Select authenticated TLS instead of development plain TCP. */
#ifndef APP_TRANSPORT_TLS
#define APP_TRANSPORT_TLS 0
#endif

/* Physical MPU6050 is the normal source. Set to 1 only for bench testing. */
#ifndef APP_USE_MOCK_IMU
#define APP_USE_MOCK_IMU 0
#endif

#if APP_NODE_ROLE != APP_ROLE_HUB && APP_NODE_ROLE != APP_ROLE_LEAF
#error "APP_NODE_ROLE must be APP_ROLE_HUB or APP_ROLE_LEAF"
#endif

#if APP_NODE_ID < 1 || APP_NODE_ID > 6
#error "APP_NODE_ID must be in the range 1 to 6"
#endif

#endif
