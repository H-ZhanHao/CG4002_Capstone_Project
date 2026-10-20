/**
 * @file wifi_manager.h
 * @brief ESP32 station-mode Wi-Fi connection management.
 *
 * In hub mode, the manager connects using wifi_secrets.h, reconnects after
 * disconnection, and notifies the application after obtaining IPv4. In leaf
 * mode, it starts only the station radio on a configured ESP-NOW channel.
 *
 * It does not create sockets or depend on the TCP transport module.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Callback invoked whenever the station obtains an IP address.
 *
 * @param context Application-defined context supplied to
 *                wifi_manager_start().
 */
typedef void (*wifi_manager_connected_callback_t)(void *context);

/**
 * @brief Initialize Wi-Fi and begin connecting in station mode.
 *
 * @param connected_callback Optional callback invoked after obtaining an IP.
 * @param callback_context   Context passed to the callback.
 *
 * @return ESP_OK on success or an ESP-IDF error code.
 */
esp_err_t wifi_manager_start(
    wifi_manager_connected_callback_t connected_callback,
    void *callback_context
);

/** Start a station-mode radio on a fixed channel without joining a WLAN. */
esp_err_t wifi_manager_start_leaf(uint8_t channel);

/** Return the current primary Wi-Fi channel. */
esp_err_t wifi_manager_get_channel(uint8_t *channel);

/** Return the station-interface MAC address. */
esp_err_t wifi_manager_get_station_mac(uint8_t mac[6]);

#endif
