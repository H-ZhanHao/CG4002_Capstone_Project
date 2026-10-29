/**
 * @file wifi_manager.c
 * @brief ESP32 station-mode Wi-Fi initialization and reconnection.
 *
 * This module owns ESP-IDF Wi-Fi configuration and Wi-Fi/IP event handling.
 * Credentials are read from the local wifi_secrets.h file.
 *
 * When an IPv4 address is obtained, the manager logs it and invokes the
 * application callback. TCP sockets and application protocol handling remain
 * outside this module.
 */

#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if APP_WIFI_ENTERPRISE
#include "esp_eap_client.h"
#include "wifi_enterprise_secrets.h"
#else
#include "wifi_secrets.h"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifndef APP_WIFI_ENTERPRISE
#define APP_WIFI_ENTERPRISE 0
#endif

static const char *TAG = "wifi_manager";

static wifi_manager_connected_callback_t application_callback = NULL;
static void *application_callback_context = NULL;
static bool wifi_started = false;

/* Convert common ESP-IDF reason codes into operator-readable diagnostics. */
static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return "802.1X authentication failed";
        case WIFI_REASON_NO_AP_FOUND:
            return "SSID not found";
        case WIFI_REASON_AUTH_FAIL:
            return "authentication failed";
        case WIFI_REASON_ASSOC_FAIL:
            return "association failed";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "handshake timeout";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            return "SSID found but security is incompatible";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "SSID found below authentication threshold";
        default:
            return "unclassified";
    }
}

#if APP_WIFI_ENTERPRISE
/* Refuse to start enterprise authentication while template values remain. */
static bool enterprise_credentials_configured(void)
{
    return strncmp(WIFI_EAP_IDENTITY, "REPLACE_WITH_", 13) != 0 &&
           strncmp(WIFI_EAP_USERNAME, "REPLACE_WITH_", 13) != 0 &&
           strncmp(WIFI_EAP_PASSWORD, "REPLACE_WITH_", 13) != 0;
}

/* Configure PEAP credentials and the platform CA bundle before station start. */
static esp_err_t configure_enterprise_authentication(void)
{
    if (!enterprise_credentials_configured()) {
        ESP_LOGE(
            TAG,
            "NUS Wi-Fi credentials are still placeholders in "
            "wifi_enterprise_secrets.h"
        );
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = esp_eap_client_set_identity(
        (const unsigned char *)WIFI_EAP_IDENTITY,
        strlen(WIFI_EAP_IDENTITY)
    );

    if (result != ESP_OK) {
        return result;
    }

    result = esp_eap_client_set_username(
        (const unsigned char *)WIFI_EAP_USERNAME,
        strlen(WIFI_EAP_USERNAME)
    );

    if (result != ESP_OK) {
        return result;
    }

    result = esp_eap_client_set_password(
        (const unsigned char *)WIFI_EAP_PASSWORD,
        strlen(WIFI_EAP_PASSWORD)
    );

    if (result != ESP_OK) {
        return result;
    }

    result = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_eap_client_use_default_cert_bundle(true);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_eap_client_set_domain_name(WIFI_EAP_SERVER_DOMAIN);

    if (result != ESP_OK) {
        return result;
    }

    return esp_wifi_sta_enterprise_enable();
}
#endif

/* Log channel and station MAC needed when provisioning the ESP-NOW peer. */
static void log_station_radio(void)
{
    uint8_t mac[6] = {0};
    uint8_t primary_channel = 0;
    wifi_second_chan_t secondary_channel = WIFI_SECOND_CHAN_NONE;

    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Station MAC: %02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
        );
    }

    if (esp_wifi_get_channel(&primary_channel, &secondary_channel) == ESP_OK) {
        ESP_LOGI(TAG, "Active Wi-Fi channel: %u", primary_channel);
    }
}

/* Own reconnect events and notify the application only after IPv4 is ready. */
static void wifi_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)handler_argument;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        ESP_LOGI(TAG, "Wi-Fi started; connecting...");
        esp_err_t result = esp_wifi_connect();

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not start Wi-Fi connection: %s",
                esp_err_to_name(result)
            );
        }
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected; reason=%d (%s); reconnecting...",
            event->reason,
            wifi_disconnect_reason_name(event->reason)
        );

        esp_err_t result = esp_wifi_connect();

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Could not restart Wi-Fi connection: %s",
                esp_err_to_name(result)
            );
        }
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "Connected! ESP32 IP address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        log_station_radio();

        if (application_callback != NULL) {
            application_callback(application_callback_context);
        }
    }
}

/* Initialize the station once and begin personal or enterprise association. */
esp_err_t wifi_manager_start(
    wifi_manager_connected_callback_t connected_callback,
    void *callback_context)
{
    if (wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }

    application_callback = connected_callback;
    application_callback_context = callback_context;

    esp_err_t result = esp_netif_init();

    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_loop_create_default();

    if (result != ESP_OK) {
        return result;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t initial_configuration =
        WIFI_INIT_CONFIG_DEFAULT();

    result = esp_wifi_init(&initial_configuration);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL
    );

    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL
    );

    if (result != ESP_OK) {
        return result;
    }

    wifi_config_t wifi_configuration = {0};

    snprintf(
        (char *)wifi_configuration.sta.ssid,
        sizeof(wifi_configuration.sta.ssid),
#if APP_WIFI_ENTERPRISE
        "%s",
        WIFI_ENTERPRISE_SSID
#else
        "%s",
        WIFI_SSID
#endif
    );

#if !APP_WIFI_ENTERPRISE
    snprintf(
        (char *)wifi_configuration.sta.password,
        sizeof(wifi_configuration.sta.password),
        "%s",
        WIFI_PASSWORD
    );

    /*
     * WPA2 is the minimum accepted security level.
     * Stronger WPA3 access points are also permitted.
     */
    wifi_configuration.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;
#endif

    wifi_configuration.sta.pmf_cfg.capable = true;
    wifi_configuration.sta.pmf_cfg.required = false;

    result = esp_wifi_set_mode(WIFI_MODE_STA);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_configuration
    );

    if (result != ESP_OK) {
        return result;
    }

#if APP_WIFI_ENTERPRISE
    result = configure_enterprise_authentication();

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not configure WPA2-Enterprise authentication: %s",
            esp_err_to_name(result)
        );
        return result;
    }
#endif

    result = esp_wifi_start();

    if (result != ESP_OK) {
        return result;
    }

    wifi_started = true;
    return ESP_OK;
}

/* Enable only the station radio on a fixed channel for a leaf ESP-NOW role. */
esp_err_t wifi_manager_start_leaf(uint8_t channel)
{
    if (wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel < 1 || channel > 14) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = esp_netif_init();

    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_loop_create_default();

    if (result != ESP_OK) {
        return result;
    }

    wifi_init_config_t initial_configuration = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&initial_configuration);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_set_mode(WIFI_MODE_STA);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_start();

    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (result != ESP_OK) {
        return result;
    }

    wifi_started = true;
    ESP_LOGI(TAG, "Leaf radio started without WLAN association");
    log_station_radio();
    return ESP_OK;
}

esp_err_t wifi_manager_get_channel(uint8_t *channel)
{
    if (!wifi_started || channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_second_chan_t secondary_channel = WIFI_SECOND_CHAN_NONE;
    return esp_wifi_get_channel(channel, &secondary_channel);
}

esp_err_t wifi_manager_get_station_mac(uint8_t mac[6])
{
    if (!wifi_started || mac == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_wifi_get_mac(WIFI_IF_STA, mac);
}
