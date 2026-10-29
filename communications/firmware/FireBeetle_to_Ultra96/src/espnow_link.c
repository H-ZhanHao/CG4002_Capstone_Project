/**
 * @file espnow_link.c
 * @brief Stage 5 encrypted ESP-NOW leaf-to-hub data path.
 */

#include "espnow_link.h"

#include <stdbool.h>
#include <string.h>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"

#include "imu_source.h"
#include "protocol.h"

#define ESPNOW_TASK_STACK_SIZE       4096
#define ESPNOW_TASK_PRIORITY         5
#define ESPNOW_RX_QUEUE_LENGTH       20
#define ESPNOW_MAX_DATAGRAM_SIZE     250
#define ESPNOW_SEND_TIMEOUT_MS       100
#define LEAF_SEQUENCE_RESET_AFTER_US 3000000

typedef struct {
    uint8_t sender_mac[6];
    uint16_t length;
    uint8_t data[ESPNOW_MAX_DATAGRAM_SIZE];
} espnow_rx_packet_t;

static const char *TAG = "espnow_link";
static espnow_link_config_t link_config;
static QueueHandle_t receive_queue = NULL;
static TaskHandle_t link_task_handle = NULL;
static volatile esp_now_send_status_t latest_send_status =
    ESP_NOW_SEND_FAIL;
static volatile uint32_t send_failure_count = 0;
static volatile uint32_t receive_drop_count = 0;
static volatile uint32_t invalid_frame_count = 0;
static bool link_started = false;

/* Treat an all-zero peer address as an unprovisioned configuration. */
static bool mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero_mac[6] = {0};
    return memcmp(mac, zero_mac, sizeof(zero_mac)) == 0;
}

/* Initialize ESP-NOW with the shared PMK and peer-specific LMK. */
static esp_err_t configure_encrypted_peer(
    const espnow_link_config_t *config)
{
    if (mac_is_zero(config->peer_mac)) {
        ESP_LOGE(
            TAG,
            "Peer MAC is not configured; copy the station MAC from the "
            "other board's startup log"
        );
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = esp_now_init();

    if (result != ESP_OK) {
        return result;
    }

    result = esp_now_set_pmk(config->pmk);

    if (result != ESP_OK) {
        esp_now_deinit();
        return result;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, config->peer_mac, sizeof(peer.peer_addr));
    memcpy(peer.lmk, config->lmk, sizeof(peer.lmk));
    peer.channel = config->channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;

    result = esp_now_add_peer(&peer);

    if (result != ESP_OK) {
        esp_now_deinit();
        return result;
    }

    ESP_LOGI(
        TAG,
        "Encrypted peer configured: node=%u "
        "%02X:%02X:%02X:%02X:%02X:%02X channel=%u",
        config->peer_node_id,
        config->peer_mac[0], config->peer_mac[1], config->peer_mac[2],
        config->peer_mac[3], config->peer_mac[4], config->peer_mac[5],
        config->channel
    );
    return ESP_OK;
}

/* Wake the leaf task with the asynchronous radio delivery result. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void send_callback(
    const esp_now_send_info_t *send_info,
    esp_now_send_status_t status)
{
    (void)send_info;
#else
static void send_callback(
    const uint8_t *mac_address,
    esp_now_send_status_t status)
{
    (void)mac_address;
#endif
    latest_send_status = status;

    if (link_task_handle != NULL) {
        xTaskNotifyGive(link_task_handle);
    }
}

/* Keep recent radio data when the callback-to-task queue is saturated. */
static bool enqueue_received_drop_oldest(const espnow_rx_packet_t *packet)
{
    if (xQueueSend(receive_queue, packet, 0) == pdPASS) {
        return true;
    }

    espnow_rx_packet_t evicted;

    if (xQueueReceive(receive_queue, &evicted, 0) == pdPASS) {
        receive_drop_count++;
    }

    if (xQueueSend(receive_queue, packet, 0) == pdPASS) {
        return true;
    }

    receive_drop_count++;
    return false;
}

/* Copy callback-owned bytes into bounded storage before returning to Wi-Fi code. */
static void receive_datagram(
    const uint8_t *sender_mac,
    const uint8_t *data,
    int data_length)
{
    if (sender_mac == NULL || data == NULL || data_length <= 0 ||
        data_length > ESPNOW_MAX_DATAGRAM_SIZE) {

        invalid_frame_count++;
        return;
    }

    espnow_rx_packet_t packet = {
        .length = (uint16_t)data_length,
    };
    memcpy(packet.sender_mac, sender_mac, sizeof(packet.sender_mac));
    memcpy(packet.data, data, (size_t)data_length);
    enqueue_received_drop_oldest(&packet);
}

/* Adapt ESP-IDF callback signatures to the same version-independent receiver. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void receive_callback(
    const esp_now_recv_info_t *receive_info,
    const uint8_t *data,
    int data_length)
{
    if (receive_info == NULL) {
        invalid_frame_count++;
        return;
    }

    receive_datagram(receive_info->src_addr, data, data_length);
}
#else
static void receive_callback(
    const uint8_t *sender_mac,
    const uint8_t *data,
    int data_length)
{
    receive_datagram(sender_mac, data, data_length);
}
#endif

/* Encode leaf samples with Session zero and send them to the configured hub. */
static void leaf_transmit_task(void *argument)
{
    (void)argument;
    uint8_t packet[PROTOCOL_DATA_PACKET_SIZE];

    while (true) {
        imu_sample_t sample;

        if (xQueueReceive(
                link_config.sample_queue,
                &sample,
                portMAX_DELAY
            ) != pdPASS) {

            continue;
        }

        protocol_encode_data(packet, &sample, 0);
        ulTaskNotifyTake(pdTRUE, 0);
        latest_send_status = ESP_NOW_SEND_FAIL;

        esp_err_t result = esp_now_send(
            link_config.peer_mac,
            packet,
            sizeof(packet)
        );

        if (result != ESP_OK ||
            ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(ESPNOW_SEND_TIMEOUT_MS)
            ) == 0 ||
            latest_send_status != ESP_NOW_SEND_SUCCESS) {

            send_failure_count++;

            if (send_failure_count == 1 ||
                (send_failure_count % 50) == 0) {

                ESP_LOGW(
                    TAG,
                    "ESP-NOW DATA delivery failed: node=%u sequence=%lu "
                    "failures=%lu",
                    sample.node_id,
                    (unsigned long)sample.sequence,
                    (unsigned long)send_failure_count
                );
            }
        }
        else if ((sample.sequence % 50) == 0) {
            ESP_LOGI(
                TAG,
                "Sent encrypted DATA: node=%u sequence=%lu queue=%u",
                sample.node_id,
                (unsigned long)sample.sequence,
                (unsigned int)uxQueueMessagesWaiting(
                    link_config.sample_queue
                )
            );
        }
    }
}

/* Authenticate peer identity, decode DATA, and preserve capture metadata. */
static void hub_receive_task(void *argument)
{
    (void)argument;
    bool have_sequence = false;
    uint32_t last_sequence = 0;
    int64_t last_valid_data_us = 0;

    while (true) {
        espnow_rx_packet_t packet;

        if (xQueueReceive(
                receive_queue,
                &packet,
                portMAX_DELAY
            ) != pdPASS) {

            continue;
        }

        protocol_header_t header;
        imu_sample_t sample;
        bool valid =
            memcmp(
                packet.sender_mac,
                link_config.peer_mac,
                sizeof(packet.sender_mac)
            ) == 0 &&
            packet.length == PROTOCOL_DATA_PACKET_SIZE &&
            protocol_decode_data(
                packet.data,
                packet.length,
                &header,
                &sample
            ) &&
            header.node_id == link_config.peer_node_id &&
            header.session_id == 0;

        if (!valid) {
            invalid_frame_count++;

            if (invalid_frame_count == 1 ||
                (invalid_frame_count % 50) == 0) {

                ESP_LOGW(
                    TAG,
                    "Rejected ESP-NOW frame: sender="
                    "%02X:%02X:%02X:%02X:%02X:%02X "
                    "length=%u invalid=%lu",
                    packet.sender_mac[0], packet.sender_mac[1],
                    packet.sender_mac[2], packet.sender_mac[3],
                    packet.sender_mac[4], packet.sender_mac[5],
                    packet.length,
                    (unsigned long)invalid_frame_count
                );
            }
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        bool reset_allowed = have_sequence &&
            (now_us - last_valid_data_us) >=
                LEAF_SEQUENCE_RESET_AFTER_US;

        if (have_sequence && sample.sequence <= last_sequence &&
            !reset_allowed) {

            invalid_frame_count++;
            ESP_LOGW(
                TAG,
                "Rejected stale/duplicate DATA: node=%u previous=%lu "
                "received=%lu",
                sample.node_id,
                (unsigned long)last_sequence,
                (unsigned long)sample.sequence
            );
            continue;
        }

        if (reset_allowed && sample.sequence <= last_sequence) {
            ESP_LOGI(
                TAG,
                "Accepted Node %u sequence reset after link inactivity: "
                "previous=%lu received=%lu",
                sample.node_id,
                (unsigned long)last_sequence,
                (unsigned long)sample.sequence
            );
        }
        else if (have_sequence && sample.sequence > last_sequence + 1) {
            ESP_LOGW(
                TAG,
                "ESP-NOW sequence gap: node=%u expected=%lu received=%lu",
                sample.node_id,
                (unsigned long)(last_sequence + 1),
                (unsigned long)sample.sequence
            );
        }

        last_sequence = sample.sequence;
        last_valid_data_us = now_us;
        have_sequence = true;

        if (xQueueSend(link_config.sample_queue, &sample, 0) != pdPASS) {
            imu_sample_t evicted;

            if (xQueueReceive(
                    link_config.sample_queue,
                    &evicted,
                    0
                ) == pdPASS) {

                receive_drop_count++;
            }

            if (xQueueSend(
                    link_config.sample_queue,
                    &sample,
                    0
                ) != pdPASS) {

                receive_drop_count++;
            }
        }

        if ((sample.sequence % 50) == 0) {
            ESP_LOGI(
                TAG,
                "Accepted encrypted DATA: node=%u sequence=%lu queue=%u",
                sample.node_id,
                (unsigned long)sample.sequence,
                (unsigned int)uxQueueMessagesWaiting(
                    link_config.sample_queue
                )
            );
        }
    }
}

/* Validate shared role settings and install the encrypted peer exactly once. */
static esp_err_t start_common(const espnow_link_config_t *config)
{
    if (config == NULL || config->sample_queue == NULL ||
        config->pmk == NULL || config->lmk == NULL ||
        config->local_node_id < 1 || config->local_node_id > 6 ||
        config->peer_node_id < 1 || config->peer_node_id > 6 ||
        config->channel < 1 || config->channel > 14) {

        return ESP_ERR_INVALID_ARG;
    }

    if (link_started) {
        return ESP_ERR_INVALID_STATE;
    }

    link_config = *config;
    send_failure_count = 0;
    receive_drop_count = 0;
    invalid_frame_count = 0;
    return configure_encrypted_peer(config);
}

/* Start the queue-consuming transmitter for a leaf build. */
esp_err_t espnow_link_start_leaf(const espnow_link_config_t *config)
{
    esp_err_t result = start_common(config);

    if (result != ESP_OK) {
        return result;
    }

    result = esp_now_register_send_cb(send_callback);

    if (result != ESP_OK) {
        esp_now_deinit();
        return result;
    }

    if (xTaskCreate(
            leaf_transmit_task,
            "espnow_tx",
            ESPNOW_TASK_STACK_SIZE,
            NULL,
            ESPNOW_TASK_PRIORITY,
            &link_task_handle
        ) != pdPASS) {

        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }

    link_started = true;
    return ESP_OK;
}

/* Start the callback queue and validating receiver task for a hub build. */
esp_err_t espnow_link_start_hub(const espnow_link_config_t *config)
{
    esp_err_t result = start_common(config);

    if (result != ESP_OK) {
        return result;
    }

    receive_queue = xQueueCreate(
        ESPNOW_RX_QUEUE_LENGTH,
        sizeof(espnow_rx_packet_t)
    );

    if (receive_queue == NULL) {
        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }

    result = esp_now_register_recv_cb(receive_callback);

    if (result != ESP_OK) {
        vQueueDelete(receive_queue);
        receive_queue = NULL;
        esp_now_deinit();
        return result;
    }

    if (xTaskCreate(
            hub_receive_task,
            "espnow_rx",
            ESPNOW_TASK_STACK_SIZE,
            NULL,
            ESPNOW_TASK_PRIORITY,
            &link_task_handle
        ) != pdPASS) {

        vQueueDelete(receive_queue);
        receive_queue = NULL;
        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }

    link_started = true;
    return ESP_OK;
}

/* Expose cumulative link diagnostics without exposing mutable transport state. */
uint32_t espnow_link_get_send_failure_count(void)
{
    return send_failure_count;
}

uint32_t espnow_link_get_receive_drop_count(void)
{
    return receive_drop_count;
}

uint32_t espnow_link_get_invalid_frame_count(void)
{
    return invalid_frame_count;
}
