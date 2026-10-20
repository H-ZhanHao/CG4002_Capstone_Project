/**
 * @file mock_imu.c
 * @brief Deterministic IMU source for development without physical sensors.
 *
 * Samples are generated every 20 ms using vTaskDelayUntil() to maintain an
 * absolute 50 Hz schedule. Sequence numbers and timestamps are assigned at
 * capture time, before the sample enters the queue.
 *
 * If the queue is full, the oldest queued sample is dropped so that recent
 * motion data is retained. The producer never waits for network work.
 */

#include "mock_imu.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "imu_source.h"
#include "protocol.h"

#define MOCK_SAMPLE_PERIOD_MS 20
#define MOCK_TASK_STACK_SIZE  3072
#define MOCK_TASK_PRIORITY    4
/* The project uses a 100 Hz FreeRTOS tick, so delays below 10 ms round to 0
 * and leave this priority-4 task continuously runnable. One full tick lets
 * app startup, Wi-Fi, and IDLE0 run while capture is stopped or paused. */
#define MOCK_ARM_POLL_MS      10

static const char *TAG = "mock_imu";

typedef struct {
    QueueHandle_t output_queue;
    TaskHandle_t task_handle;
    uint8_t node_id;
    volatile bool paused;
    volatile bool armed;
    volatile bool running;
    int64_t scheduled_start_us;
    volatile uint32_t dropped_sample_count;
} mock_source_t;

static mock_source_t sources[7];
static portMUX_TYPE source_state_lock = portMUX_INITIALIZER_UNLOCKED;

/* Preserve recent motion by evicting one oldest sample instead of blocking. */
static bool queue_sample_drop_oldest(
    QueueHandle_t queue,
    const imu_sample_t *sample,
    imu_sample_t *evicted_sample)
{
    if (xQueueSend(queue, sample, 0) == pdPASS) {
        return false;
    }

    if (xQueueReceive(queue, evicted_sample, 0) != pdPASS) {
        return xQueueSend(queue, sample, 0) != pdPASS;
    }

    if (xQueueSend(queue, sample, 0) == pdPASS) {
        return true;
    }

    /* A concurrent producer filled the newly created slot. */
    return true;
}

/* Generate deterministic samples on an absolute 20 ms schedule. */
static void mock_imu_task(void *argument)
{
    mock_source_t *source = (mock_source_t *)argument;
    uint32_t sequence = 0;
    TickType_t next_wake_time = xTaskGetTickCount();

    ESP_LOGI(
        TAG,
        "Node %u mock producer ready; waiting for scheduled START",
        source->node_id
    );

    while (true) {
        bool report_scheduled_start = false;
        bool armed;
        bool running;
        bool paused;
        int64_t scheduled_start_us;
        portENTER_CRITICAL(&source_state_lock);
        armed = source->armed;
        running = source->running;
        paused = source->paused;
        scheduled_start_us = source->scheduled_start_us;
        portEXIT_CRITICAL(&source_state_lock);

        if (armed && !running) {
            int64_t remaining_us = scheduled_start_us - esp_timer_get_time();
            if (remaining_us > 2000) {
                TickType_t sleep_ticks = pdMS_TO_TICKS(
                    (remaining_us - 1500) / 1000);
                if (sleep_ticks > 0) vTaskDelay(sleep_ticks);
                else taskYIELD();
                continue;
            }
            if (remaining_us > 0) {
                esp_rom_delay_us((uint32_t)remaining_us);
            }
        }

        if (armed && !running && esp_timer_get_time() >= scheduled_start_us) {
            portENTER_CRITICAL(&source_state_lock);
            if (source->armed && !source->running &&
                source->scheduled_start_us == scheduled_start_us) {
                source->armed = false;
                source->running = true;
                sequence = 0;
                next_wake_time = xTaskGetTickCount();
                running = true;
                report_scheduled_start = true;
            }
            portEXIT_CRITICAL(&source_state_lock);
        }

        if (!running || paused) {
            next_wake_time = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(MOCK_ARM_POLL_MS));
            continue;
        }

        imu_sample_t sample = {
            .node_id = source->node_id,
            .flags = 0,
            .sequence = sequence,
            .timestamp_us = (uint64_t)esp_timer_get_time(),

            .ax = (int16_t)(1000 + (sequence % 200)),
            .ay = (int16_t)(-500 + (sequence % 100)),
            .az = 16384,
            .gx = (int16_t)(sequence % 100),
            .gy = -(int16_t)(sequence % 100),
            .gz = 0,

            .battery_mv = 3800,
            .status = PROTOCOL_STATUS_MOCK_DATA,
        };

        imu_sample_t evicted_sample = {0};

        if (queue_sample_drop_oldest(
                source->output_queue,
                &sample,
                &evicted_sample)) {

            uint8_t dropped_node_id = evicted_sample.node_id;

            if (dropped_node_id >= 1 && dropped_node_id <= 6) {
                sources[dropped_node_id].dropped_sample_count++;
            }
            else {
                source->dropped_sample_count++;
                dropped_node_id = source->node_id;
            }

            uint32_t dropped_count =
                sources[dropped_node_id].dropped_sample_count;

            if (dropped_count == 1 || (dropped_count % 50) == 0) {

                ESP_LOGW(
                    TAG,
                    "Sample queue full; evicted node=%u sequence=%lu "
                    "node_dropped=%lu; retained new node=%u sequence=%lu",
                    dropped_node_id,
                    (unsigned long)evicted_sample.sequence,
                    (unsigned long)dropped_count,
                    sample.node_id,
                    (unsigned long)sample.sequence
                );
            }
        }

        if (report_scheduled_start) {
            ESP_LOGI(TAG,
                "Node %u scheduled START captured sequence=0 at %llu us",
                source->node_id,
                (unsigned long long)sample.timestamp_us);
        }

        if ((sequence % 50) == 0) {
            ESP_LOGI(
                TAG,
                "Produced: node=%u sequence=%lu "
                "timestamp_us=%llu queue=%u",
                sample.node_id,
                (unsigned long)sample.sequence,
                (unsigned long long)sample.timestamp_us,
                (unsigned int)uxQueueMessagesWaiting(source->output_queue)
            );
        }

        /*
         * Increment even if the sample was dropped so downstream sequence
         * gaps expose queue overflow.
         */
        sequence++;

        vTaskDelayUntil(
            &next_wake_time,
            pdMS_TO_TICKS(MOCK_SAMPLE_PERIOD_MS)
        );
    }
}

/* Validate a node slot and start exactly one mock producer for it. */
esp_err_t mock_imu_start(
    QueueHandle_t sample_queue,
    uint8_t node_id)
{
    if (sample_queue == NULL || node_id < 1 || node_id > 6) {
        return ESP_ERR_INVALID_ARG;
    }

    mock_source_t *source = &sources[node_id];

    if (source->task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    source->output_queue = sample_queue;
    source->node_id = node_id;
    source->paused = false;
    source->armed = false;
    source->running = false;
    source->scheduled_start_us = 0;
    source->dropped_sample_count = 0;

    BaseType_t result = xTaskCreate(
        mock_imu_task,
        "mock_imu",
        MOCK_TASK_STACK_SIZE,
        source,
        MOCK_TASK_PRIORITY,
        &source->task_handle
    );

    if (result != pdPASS) {
        source->output_queue = NULL;
        source->node_id = 0;
        source->task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* Pause generation for benchmark isolation without deleting the producer task. */
esp_err_t mock_imu_set_paused(uint8_t node_id, bool paused)
{
    if (node_id < 1 || node_id > 6) {
        return ESP_ERR_INVALID_ARG;
    }

    mock_source_t *source = &sources[node_id];

    if (source->task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    source->paused = paused;

    ESP_LOGI(
        TAG,
        "Node %u mock producer %s",
        node_id,
        paused ? "paused" : "resumed"
    );

    return ESP_OK;
}

esp_err_t mock_imu_schedule_start(uint8_t node_id, int64_t local_start_us)
{
    if (node_id < 1 || node_id > 6 || local_start_us <= esp_timer_get_time()) {
        return ESP_ERR_INVALID_ARG;
    }
    mock_source_t *source = &sources[node_id];
    if (source->task_handle == NULL) return ESP_ERR_INVALID_STATE;

    xQueueReset(source->output_queue);
    portENTER_CRITICAL(&source_state_lock);
    source->scheduled_start_us = local_start_us;
    source->armed = true;
    source->running = false;
    source->paused = false;
    portEXIT_CRITICAL(&source_state_lock);
    ESP_LOGI(TAG, "Node %u armed for local time %lld us",
        node_id, (long long)local_start_us);
    return ESP_OK;
}

esp_err_t mock_imu_stop(uint8_t node_id)
{
    if (node_id < 1 || node_id > 6) return ESP_ERR_INVALID_ARG;
    mock_source_t *source = &sources[node_id];
    if (source->task_handle == NULL) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&source_state_lock);
    source->armed = false;
    source->running = false;
    source->paused = false;
    source->scheduled_start_us = 0;
    portEXIT_CRITICAL(&source_state_lock);
    xQueueReset(source->output_queue);
    ESP_LOGI(TAG, "Node %u producer stopped", node_id);
    return ESP_OK;
}

uint32_t mock_imu_get_dropped_count(void)
{
    uint32_t total = 0;

    for (uint8_t node_id = 1; node_id <= 6; node_id++) {
        total += sources[node_id].dropped_sample_count;
    }

    return total;
}
