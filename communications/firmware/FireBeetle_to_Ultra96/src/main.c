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
#include "espnow_link.h"
#include "espnow_secrets.h"
#include "hub_router.h"
#include "imu_source.h"
#include "protocol.h"
#include "physical_pipeline.h"
#include "sensor_source.h"
#include "tcp_client.h"
#include "wifi_manager.h"

#if APP_NODE_ROLE == APP_ROLE_HUB && !APP_WIFI_ASSOCIATION_ONLY
#include "laptop_relay_endpoint_secrets.h"
#define SERVER_IP   LAPTOP_RELAY_SERVER_IP
#define SERVER_PORT LAPTOP_RELAY_SERVER_PORT
#endif

#define HUB_OUTBOUND_QUEUE_LENGTH 50
#define LEAF_SAMPLE_QUEUE_LENGTH  20

#if APP_NODE_ROLE == APP_ROLE_HUB
#define HUB_NODE_ID APP_NODE_ID

#if APP_NODE_ID == 1
#define ASSIGNED_NODE_MASK 0x02
#elif APP_NODE_ID == 2
#define ASSIGNED_NODE_MASK 0x04
#else
#error "Direct node mode currently supports APP_NODE_ID 1 or 2"
#endif

#if APP_HUB_LOCAL_ONLY
#define ACTIVE_NODE_MASK (1U << HUB_NODE_ID)
#else
#if APP_NODE_ID != 3
#error "ESP-NOW hub mode is currently implemented only for Hub 3"
#endif
#define LEAF_NODE_ID     1
#define ACTIVE_NODE_MASK 0x0A
#endif
#else
/* Preserve the existing Leaf 1 -> Hub 3 build and behavior. */
#define HUB_NODE_ID  3
#define LEAF_NODE_ID 1
#endif

#if APP_NODE_ROLE == APP_ROLE_HUB && !APP_WIFI_ASSOCIATION_ONLY
/* Predictable demo identifier; TLS protects it in transit, but it is not a secret. */
static const uint8_t DEVELOPMENT_DEVICE_TOKEN[PROTOCOL_DEVICE_TOKEN_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};
#endif

static const char *TAG = "application";

#if APP_NODE_ROLE == APP_ROLE_HUB
#if !APP_WIFI_ASSOCIATION_ONLY
static QueueHandle_t hub_outbound_queue = NULL;
#if !APP_PHYSICAL_INTEGRATION
static QueueHandle_t hub_router_input_queue = NULL;
#endif
#endif
#if !APP_HUB_LOCAL_ONLY && !APP_WIFI_ASSOCIATION_ONLY
static bool hub_espnow_started = false;

/* Bind Hub 3's ESP-NOW receiver to the Wi-Fi channel and router input queue. */
static void start_hub_espnow(uint8_t channel)
{
    const espnow_link_config_t espnow_config = {
        .local_node_id = HUB_NODE_ID,
        .peer_node_id = LEAF_NODE_ID,
        .peer_mac = {
            ESPNOW_LEAF1_STA_MAC[0], ESPNOW_LEAF1_STA_MAC[1],
            ESPNOW_LEAF1_STA_MAC[2], ESPNOW_LEAF1_STA_MAC[3],
            ESPNOW_LEAF1_STA_MAC[4], ESPNOW_LEAF1_STA_MAC[5],
        },
        .channel = channel,
        .pmk = ESPNOW_PMK,
        .lmk = ESPNOW_LEAF1_HUB3_LMK,
        .sample_queue = hub_router_input_queue,
    };

    esp_err_t result = espnow_link_start_hub(&espnow_config);

    if (result == ESP_OK) {
        hub_espnow_started = true;
        ESP_LOGI(TAG, "Hub ESP-NOW receiver started on channel %u", channel);
    }
    else if (result == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(
            TAG,
            "Hub ESP-NOW peer is not ready; configure Leaf 1 MAC in "
            "espnow_secrets.h"
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "Could not start Hub ESP-NOW receiver: %s",
            esp_err_to_name(result)
        );
    }
}
#endif

/* Start network-dependent hub services only after the station owns an IPv4 address. */
static void wifi_connected(void *context)
{
    (void)context;
#if APP_WIFI_ASSOCIATION_ONLY || !APP_HUB_LOCAL_ONLY
    uint8_t channel = 0;
#endif

#if APP_WIFI_ASSOCIATION_ONLY
    if (wifi_manager_get_channel(&channel) == ESP_OK) {
        ESP_LOGI(
            TAG,
            "NUS Wi-Fi association test passed; active channel=%u; "
            "ESP-NOW and TCP remain disabled",
            channel
        );
    }

    return;
#else

#if !APP_HUB_LOCAL_ONLY
    if (wifi_manager_get_channel(&channel) == ESP_OK &&
        !hub_espnow_started) {

        start_hub_espnow(channel);
    }
#endif

    const tcp_client_config_t tcp_config = {
        .sample_queue = hub_outbound_queue,
        .server_ip = SERVER_IP,
        .server_port = SERVER_PORT,
        .hub_node_id = HUB_NODE_ID,
        .firmware_major = 1,
        .firmware_minor = 0,
        .firmware_patch = 0,
        .supported_sampling_rates = APP_PHYSICAL_INTEGRATION
            ? PROTOCOL_SAMPLE_RATE_50_HZ : PROTOCOL_SAMPLE_RATE_100_HZ,
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
        "mock data, ESP-NOW, and TCP are disabled",
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
        "Hub Node %u started; local %s stream ready; ESP-NOW %s",
        HUB_NODE_ID,
        APP_PHYSICAL_INTEGRATION ? "continuous 50 Hz physical" : "100 Hz raw",
        APP_HUB_LOCAL_ONLY ? "disabled" : "enabled"
    );
#endif
}
#else
/* Assemble the leaf pipeline: mock IMU -> encrypted ESP-NOW transmitter. */
static void start_leaf(void)
{
    QueueHandle_t leaf_sample_queue = xQueueCreate(
        LEAF_SAMPLE_QUEUE_LENGTH,
        sizeof(imu_sample_t)
    );

    if (leaf_sample_queue == NULL) {
        ESP_LOGE(TAG, "Could not create Leaf sample queue");
        return;
    }

    esp_err_t result = wifi_manager_start_leaf(ESPNOW_WIFI_CHANNEL);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Leaf Wi-Fi radio: %s", esp_err_to_name(result));
        return;
    }

    const espnow_link_config_t espnow_config = {
        .local_node_id = LEAF_NODE_ID,
        .peer_node_id = HUB_NODE_ID,
        .peer_mac = {
            ESPNOW_HUB3_STA_MAC[0], ESPNOW_HUB3_STA_MAC[1],
            ESPNOW_HUB3_STA_MAC[2], ESPNOW_HUB3_STA_MAC[3],
            ESPNOW_HUB3_STA_MAC[4], ESPNOW_HUB3_STA_MAC[5],
        },
        .channel = ESPNOW_WIFI_CHANNEL,
        .pmk = ESPNOW_PMK,
        .lmk = ESPNOW_LEAF1_HUB3_LMK,
        .sample_queue = leaf_sample_queue,
    };

    result = espnow_link_start_leaf(&espnow_config);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Could not start Leaf ESP-NOW transmitter: %s; configure Hub "
            "3 MAC and channel in espnow_secrets.h",
            esp_err_to_name(result)
        );
        return;
    }

    result = sensor_source_start(leaf_sample_queue, LEAF_NODE_ID);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Node 1 mock producer: %s", esp_err_to_name(result));
        return;
    }

    ESP_LOGI(
        TAG,
        "Leaf Node 1 started on channel %u; WLAN association and TCP are disabled",
        ESPNOW_WIFI_CHANNEL
    );
}
#endif

/* Initialize persistent ESP-IDF state, then enter the compile-time selected role. */
void app_main(void)
{
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }

    ESP_ERROR_CHECK(result);

#if APP_NODE_ROLE == APP_ROLE_HUB
    start_hub();
#else
    start_leaf();
#endif
}
