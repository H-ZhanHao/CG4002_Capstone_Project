/**
 * @file main.c
 * @brief Startup for one directly connected Node 1 or Node 2 device.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "app_config.h"
#include "application.h"
#if APP_TRANSPORT_TLS
#if __has_include("demo_tls_config.h")
#include "demo_tls_config.h"
#else
#include "demo_tls_config.example.h"
#endif
#endif
#include "hub_router.h"
#include "imu_source.h"
#include "protocol.h"
#include "physical_pipeline.h"
#include "sensor_source.h"
#include "tcp_client.h"
#include "wifi_manager.h"

#if !APP_WIFI_ASSOCIATION_ONLY
#include "laptop_relay_endpoint_secrets.h"
#define SERVER_IP   LAPTOP_RELAY_SERVER_IP
#define SERVER_PORT LAPTOP_RELAY_SERVER_PORT
#endif

#define HUB_OUTBOUND_QUEUE_LENGTH 50

#define HUB_NODE_ID APP_NODE_ID

#if APP_NODE_ID == 1
#define ASSIGNED_NODE_MASK 0x02
#elif APP_NODE_ID == 2
#define ASSIGNED_NODE_MASK 0x04
#else
#error "Direct node mode currently supports APP_NODE_ID 1 or 2"
#endif

#define ACTIVE_NODE_MASK (1U << HUB_NODE_ID)

#if !APP_WIFI_ASSOCIATION_ONLY
/* Predictable demo identifier; TLS protects it in transit, but it is not a secret. */
static const uint8_t DEVELOPMENT_DEVICE_TOKEN[PROTOCOL_DEVICE_TOKEN_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};
#endif

static const char *TAG = "application";

#if !APP_WIFI_ASSOCIATION_ONLY
static QueueHandle_t hub_outbound_queue = NULL;
#if !APP_PHYSICAL_INTEGRATION
static QueueHandle_t hub_router_input_queue = NULL;
#endif
#endif

/* Start network-dependent hub services only after the station owns an IPv4 address. */
static void wifi_connected(void *context)
{
    (void)context;
#if APP_WIFI_ASSOCIATION_ONLY
    uint8_t channel = 0;
    if (wifi_manager_get_channel(&channel) == ESP_OK) {
        ESP_LOGI(
            TAG,
            "NUS Wi-Fi association test passed; active channel=%u; "
            "TCP remains disabled",
            channel
        );
    }

    return;
#else

    const tcp_client_config_t tcp_config = {
        .sample_queue = hub_outbound_queue,
        .server_ip = SERVER_IP,
        .server_port = SERVER_PORT,
        .hub_node_id = HUB_NODE_ID,
        .firmware_major = 1,
        .firmware_minor = 0,
        .firmware_patch = 0,
        .supported_sampling_rates = PROTOCOL_SAMPLE_RATE_100_HZ,
        .assigned_node_mask = ASSIGNED_NODE_MASK,
        .active_node_mask = ACTIVE_NODE_MASK,
        .device_token = DEVELOPMENT_DEVICE_TOKEN,
#if APP_TRANSPORT_TLS
        .use_tls = true,
        .tls_ca_certificate = DEMO_TLS_CA_CERTIFICATE,
        .tls_server_identity = DEMO_TLS_SERVER_IDENTITY,
#else
        .use_tls = false,
        .tls_ca_certificate = NULL,
        .tls_server_identity = NULL,
#endif
        .get_overflow_count = APP_PHYSICAL_INTEGRATION
            ? physical_pipeline_get_overwrite_count
            : hub_router_get_dropped_count_for_node,
        .physical_samples = APP_PHYSICAL_INTEGRATION,
    };

    esp_err_t result = tcp_client_start(&tcp_config);

    if (result == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "TCP client task is already running");
    }
    else if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not start TCP client: %s",
            esp_err_to_name(result)
        );
    }
#endif
}

/* Assemble the hub pipeline: producers -> router -> TCP/TLS transport. */
static void start_hub(void)
{
#if APP_WIFI_ASSOCIATION_ONLY
    esp_err_t result = wifi_manager_start(wifi_connected, NULL);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not start Wi-Fi association test: %s",
            esp_err_to_name(result)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "Hub Node %u NUS Wi-Fi association-only test started; "
        "sensor capture and TCP are disabled",
        HUB_NODE_ID
    );
    return;
#else
    esp_err_t result = application_start(HUB_NODE_ID);

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(
            TAG,
            "Could not start application task: %s",
            esp_err_to_name(result)
        );
        return;
    }

    hub_outbound_queue = xQueueCreate(HUB_OUTBOUND_QUEUE_LENGTH,
        APP_PHYSICAL_INTEGRATION ? sizeof(protocol_physical_sample_t)
                                 : sizeof(imu_sample_t));

    if (hub_outbound_queue == NULL) {
        ESP_LOGE(TAG, "Could not create Hub outbound queue");
        return;
    }

#if APP_PHYSICAL_INTEGRATION
    result = physical_pipeline_start(hub_outbound_queue, HUB_NODE_ID);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start physical pipeline: %s", esp_err_to_name(result));
        return;
    }
#else
    result = hub_router_start(
        hub_outbound_queue,
        &hub_router_input_queue
    );

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start hub router: %s", esp_err_to_name(result));
        return;
    }
    result = sensor_source_start(hub_router_input_queue, HUB_NODE_ID);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not start Node %u sensor producer: %s",
            HUB_NODE_ID,
            esp_err_to_name(result)
        );
        return;
    }
#endif

    result = wifi_manager_start(wifi_connected, NULL);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Hub Wi-Fi: %s", esp_err_to_name(result));
        return;
    }

    ESP_LOGI(
        TAG,
        "Hub Node %u started; local %s stream ready",
        HUB_NODE_ID,
        APP_PHYSICAL_INTEGRATION ? "continuous 100 Hz physical" : "100 Hz raw"
    );
#endif
}

/* Initialize persistent ESP-IDF state, then start this directly connected node. */
void app_main(void)
{
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }

    ESP_ERROR_CHECK(result);

    start_hub();
}
