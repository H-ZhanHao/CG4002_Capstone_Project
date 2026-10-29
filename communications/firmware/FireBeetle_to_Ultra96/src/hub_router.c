/**
 * @file hub_router.c
 * @brief Hub-side fan-in for local and forwarded IMU samples.
 *
 * Producers submit complete logical samples to a bounded input queue. The
 * router preserves the original Node ID, sequence, and capture timestamp and
 * forwards samples to the bounded queue consumed by the TCP task.
 *
 * If the outbound queue is full, the oldest queued sample is evicted so that
 * recovery prioritizes recent motion data. Sequence numbers are never reused.
 */

#include "hub_router.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/task.h"

#include "imu_source.h"

#define HUB_ROUTER_INPUT_QUEUE_LENGTH 20
#define HUB_ROUTER_TASK_STACK_SIZE    3072
#define HUB_ROUTER_TASK_PRIORITY      5

static const char *TAG = "hub_router";

static QueueHandle_t router_input_queue = NULL;
static QueueHandle_t router_output_queue = NULL;
static TaskHandle_t router_task_handle = NULL;
static volatile uint32_t dropped_sample_count = 0;
static volatile uint32_t dropped_by_node[7];

/* Fan in producers while evicting oldest outbound data under backpressure. */
static void hub_router_task(void *argument)
{
    (void)argument;

    while (true) {
        imu_sample_t sample;

        if (xQueueReceive(
                router_input_queue,
                &sample,
                portMAX_DELAY
            ) != pdPASS) {

            continue;
        }

        if (sample.node_id < 1 || sample.node_id > 6) {
            ESP_LOGE(
                TAG,
                "Rejected sample with invalid Node ID: %u",
                sample.node_id
            );
            continue;
        }

        if (xQueueSend(router_output_queue, &sample, 0) != pdPASS) {
            imu_sample_t evicted_sample;

            if (xQueueReceive(
                    router_output_queue,
                    &evicted_sample,
                    0
                ) != pdPASS) {

                /* The TCP consumer created space; retry the new sample. */
                if (xQueueSend(router_output_queue, &sample, 0) == pdPASS) {
                    continue;
                }

                evicted_sample = sample;
            }
            else if (xQueueSend(
                         router_output_queue,
                         &sample,
                         0
                     ) != pdPASS) {

                ESP_LOGW(TAG, "Could not retain new sample after eviction");
            }

            dropped_sample_count++;
            dropped_by_node[evicted_sample.node_id]++;

            if (dropped_sample_count == 1 ||
                (dropped_sample_count % 50) == 0) {

                ESP_LOGW(
                    TAG,
                    "Outbound queue full; evicted node=%u sequence=%lu "
                    "total_dropped=%lu node_dropped=%lu; retained node=%u "
                    "sequence=%lu",
                    evicted_sample.node_id,
                    (unsigned long)evicted_sample.sequence,
                    (unsigned long)dropped_sample_count,
                    (unsigned long)dropped_by_node[evicted_sample.node_id],
                    sample.node_id,
                    (unsigned long)sample.sequence
                );
            }
        }
    }
}

/* Create the producer queue and connect it to the transport-owned outbound queue. */
esp_err_t hub_router_start(
    QueueHandle_t outbound_queue,
    QueueHandle_t *input_queue)
{
    if (outbound_queue == NULL || input_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (router_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    router_input_queue = xQueueCreate(
        HUB_ROUTER_INPUT_QUEUE_LENGTH,
        sizeof(imu_sample_t)
    );

    if (router_input_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    router_output_queue = outbound_queue;
    dropped_sample_count = 0;

    for (uint8_t node_id = 1; node_id <= 6; node_id++) {
        dropped_by_node[node_id] = 0;
    }

    BaseType_t result = xTaskCreate(
        hub_router_task,
        "hub_router",
        HUB_ROUTER_TASK_STACK_SIZE,
        NULL,
        HUB_ROUTER_TASK_PRIORITY,
        &router_task_handle
    );

    if (result != pdPASS) {
        vQueueDelete(router_input_queue);
        router_input_queue = NULL;
        router_output_queue = NULL;
        router_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    *input_queue = router_input_queue;

    ESP_LOGI(
        TAG,
        "Hub router started; input_capacity=%u outbound_capacity=%u",
        HUB_ROUTER_INPUT_QUEUE_LENGTH,
        (unsigned int)uxQueueSpacesAvailable(router_output_queue)
    );

    return ESP_OK;
}

uint32_t hub_router_get_dropped_count(void)
{
    return dropped_sample_count;
}

uint32_t hub_router_get_dropped_count_for_node(uint8_t node_id)
{
    if (node_id < 1 || node_id > 6) {
        return 0;
    }

    return dropped_by_node[node_id];
}
