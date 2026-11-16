/**
 * @file wifi_manager.h
 * @brief ESP32 station-mode Wi-Fi connection management.
 *
 * The manager connects using the configured Wi-Fi credentials, reconnects
 * after disconnection, and notifies the application after obtaining IPv4.
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

/** Return the current primary Wi-Fi channel. */
esp_err_t wifi_manager_get_channel(uint8_t *channel);

/** Return the station-interface MAC address. */
esp_err_t wifi_manager_get_station_mac(uint8_t mac[6]);

#endif
