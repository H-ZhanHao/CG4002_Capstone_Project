#include "physical_pipeline.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "bluetooth_bridge.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "exercise_bridge.h"
#include "freertos/task.h"
#include "hardware_bridge.h"
#include "sensor_history.h"

#define PIPELINE_TASK_STACK 4096
#define PIPELINE_TASK_PRIORITY 4
#define PIPELINE_COMMAND_QUEUE_LENGTH 8
#define PIPELINE_TRIGGER_QUEUE_LENGTH 2

typedef enum { COMMAND_BEGIN, COMMAND_END, COMMAND_FLUSH, COMMAND_STOP } command_type_t;
typedef struct {
    command_type_t type;
    uint32_t session_id;
    uint32_t boot_id;
    uint32_t command_sequence;
    protocol_start_flush_t flush;
    QueueHandle_t result_queue;
} pipeline_command_t;
typedef struct { uint8_t result; uint64_t effective_local_us; } command_result_t;

static const char *TAG = "physical_pipeline";
static QueueHandle_t command_queue;
static QueueHandle_t trigger_queue;
static QueueHandle_t sample_queue;
static TaskHandle_t task_handle;
static uint8_t configured_node_id;
static volatile uint32_t observable_overwrites;
/*
 * The 401-entry history is intentionally bounded, but it is much larger than
 * this task's 4 KiB stack. Keep task-owned state in static DRAM so entering the
 * task cannot overwrite FreeRTOS queue control blocks before its first read.
 * Only pipeline_task accesses these objects after startup.
 */
static physical_history_t pipeline_history;
static physical_feeder_t pipeline_feeder;

static void handle_command(const pipeline_command_t *command,
                           physical_history_t *history,
                           physical_feeder_t *feeder,
                           uint32_t *session_id, uint32_t *boot_id,
                           uint32_t *arm_generation)
{
    command_result_t reply = { .effective_local_us = (uint64_t)esp_timer_get_time() };
    if (command->type == COMMAND_BEGIN) {
        *session_id = command->session_id;
        *boot_id = command->boot_id;
        (*arm_generation)++;
        if (*arm_generation == 0) (*arm_generation)++;
        physical_feeder_stop(feeder);
        xQueueReset(sample_queue);
        xQueueReset(trigger_queue);
    } else if (command->session_id != *session_id) {
        reply.result = PROTOCOL_CONTROL_RESULT_STALE_IDENTITY;
    } else if (command->type == COMMAND_END || command->type == COMMAND_STOP) {
        reply.result = feeder->active ? PROTOCOL_CONTROL_RESULT_APPLIED
                                      : PROTOCOL_CONTROL_RESULT_ALREADY_STOPPED;
        physical_feeder_stop(feeder);
        xQueueReset(sample_queue);
        if (command->type == COMMAND_END) {
            *session_id = 0;
            *boot_id = 0;
        } else {
            (*arm_generation)++;
            if (*arm_generation == 0) (*arm_generation)++;
        }
    } else if (command->type == COMMAND_FLUSH) {
        int64_t coordinator_t0 = (int64_t)command->flush.coordinator_t0_us;
        int64_t local_t0 = coordinator_t0 + command->flush.node_minus_coordinator_us;
        reply.effective_local_us = local_t0 > 0 ? (uint64_t)local_t0 : 0;
        if (feeder->active) {
            reply.result = (feeder->command_sequence == command->command_sequence &&
                feeder->trigger_sequence == command->flush.trigger_sequence)
                ? PROTOCOL_CONTROL_RESULT_DUPLICATE : PROTOCOL_CONTROL_RESULT_BUSY;
        } else if (local_t0 <= 0 || command->flush.history_duration_us == 0 ||
                   command->flush.history_duration_us >= (uint64_t)local_t0 ||
                   !physical_feeder_begin(feeder, history,
                       command->command_sequence, command->flush.trigger_sequence,
                       (uint64_t)local_t0 - command->flush.history_duration_us,
                       (uint64_t)local_t0)) {
            reply.result = PROTOCOL_CONTROL_RESULT_INSUFFICIENT_HISTORY;
        } else {
            reply.result = PROTOCOL_CONTROL_RESULT_APPLIED;
        }
    }
    if (command->result_queue != NULL) (void)xQueueSend(command->result_queue, &reply, 0);
}

static void pipeline_task(void *unused)
{
    (void)unused;
    uint32_t session_id = 0, boot_id = 0, arm_generation = 0, sequence = 0;
    physical_history_clear(&pipeline_history);
    physical_feeder_stop(&pipeline_feeder);
    uint64_t next_capture_us = (uint64_t)esp_timer_get_time();
    for (;;) {
        pipeline_command_t command;
        while (xQueueReceive(command_queue, &command, 0) == pdPASS)
            handle_command(&command, &pipeline_history, &pipeline_feeder,
                           &session_id, &boot_id, &arm_generation);
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        bluetooth_bridge_poll(now_us);
        if (now_us >= next_capture_us) {
            protocol_physical_sample_t captured = {0};
            hardware_bridge_sample_t hardware_sample;
            hardware_bridge_peak_t peak;
            if (hardware_bridge_capture(sequence, &hardware_sample, &peak)) {
                exercise_bridge_update(hardware_sample.timestamp_us,
                    hardware_sample.acceleration, hardware_sample.gyro);
                captured.node_id = configured_node_id;
                captured.sequence = sequence;
                captured.timestamp_us = hardware_sample.timestamp_us;
                captured.ax = hardware_sample.acceleration[0];
                captured.ay = hardware_sample.acceleration[1];
                captured.az = hardware_sample.acceleration[2];
                captured.gx = hardware_sample.gyro[0];
                captured.gy = hardware_sample.gyro[1];
                captured.gz = hardware_sample.gyro[2];
#if APP_USE_MOCK_IMU
                captured.status = PROTOCOL_STATUS_MOCK_DATA;
#endif
                if (physical_history_push(&pipeline_history, &captured)) sequence++;
                if (session_id != 0 && peak.peak_timestamp_us != 0) {
                    physical_trigger_event_t event = {
                        .sequence = captured.sequence,
                        .detected_timestamp_us = peak.detected_timestamp_us,
                        .trigger = { .boot_id = boot_id,
                            .arm_generation = arm_generation,
                            .peak_timestamp_us = peak.peak_timestamp_us },
                    };
                    (void)xQueueSend(trigger_queue, &event, 0);
                }
            }
            next_capture_us += PHYSICAL_SAMPLE_PERIOD_US;
            if (next_capture_us <= now_us) next_capture_us = now_us + PHYSICAL_SAMPLE_PERIOD_US;
        }
        protocol_physical_sample_t outgoing;
        if (physical_feeder_next(&pipeline_feeder, &pipeline_history, &outgoing)) {
            if (xQueueSend(sample_queue, &outgoing, 0) == pdPASS)
                (void)physical_feeder_mark_sent(&pipeline_feeder,
                                                outgoing.sequence);
            else
                (void)physical_feeder_retry(&pipeline_feeder,
                                            outgoing.sequence);
        }
        /* Expose only an actual history-to-live delivery gap. Routine eviction
         * from the bounded rolling history is expected before a trigger. */
        observable_overwrites = pipeline_feeder.lost_before_send;
        vTaskDelay(1);
    }
}

esp_err_t physical_pipeline_start(QueueHandle_t output_queue, uint8_t node_id)
{
    if (output_queue == NULL || node_id < 1 || node_id > 2) return ESP_ERR_INVALID_ARG;
    if (task_handle != NULL) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(hardware_bridge_begin(), TAG,
        "hardware core initialization");
    uint32_t exercise_boot_id = esp_random();
    if (exercise_boot_id == 0) exercise_boot_id = 1;
    ESP_RETURN_ON_ERROR(exercise_bridge_begin(node_id, exercise_boot_id), TAG,
        "exercise core initialization");
    esp_err_t bluetooth_result = bluetooth_bridge_begin(node_id);
    if (bluetooth_result != ESP_OK)
        ESP_LOGW(TAG, "Bluetooth unavailable; TLS sampling will continue: %s",
            esp_err_to_name(bluetooth_result));
    command_queue = xQueueCreate(PIPELINE_COMMAND_QUEUE_LENGTH, sizeof(pipeline_command_t));
    trigger_queue = xQueueCreate(PIPELINE_TRIGGER_QUEUE_LENGTH, sizeof(physical_trigger_event_t));
    if (command_queue == NULL || trigger_queue == NULL) return ESP_ERR_NO_MEM;
    sample_queue = output_queue;
    configured_node_id = node_id;
    if (xTaskCreate(pipeline_task, "physical_pipeline", PIPELINE_TASK_STACK, NULL,
        PIPELINE_TASK_PRIORITY, &task_handle) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Node %u continuous 50 Hz %s pipeline started", node_id,
        APP_USE_MOCK_IMU ? "mock" : "MPU6050");
    return ESP_OK;
}

static bool submit_sync(pipeline_command_t *command, command_result_t *result)
{
    QueueHandle_t replies = xQueueCreate(1, sizeof(command_result_t));
    if (replies == NULL) return false;
    command->result_queue = replies;
    bool ok = xQueueSend(command_queue, command, pdMS_TO_TICKS(100)) == pdPASS &&
        xQueueReceive(replies, result, pdMS_TO_TICKS(250)) == pdPASS;
    vQueueDelete(replies);
    return ok;
}

bool physical_pipeline_begin_session(uint32_t session_id, uint32_t boot_id)
{
    pipeline_command_t command = { .type = COMMAND_BEGIN,
        .session_id = session_id, .boot_id = boot_id };
    command_result_t result;
    return session_id != 0 && boot_id != 0 && submit_sync(&command, &result);
}

void physical_pipeline_end_session(uint32_t session_id)
{
    pipeline_command_t command = { .type = COMMAND_END, .session_id = session_id };
    command_result_t result;
    (void)submit_sync(&command, &result);
}

uint8_t physical_pipeline_start_flush(uint32_t session_id,
    uint32_t command_sequence, const protocol_start_flush_t *flush,
    uint64_t *effective_local_us)
{
    pipeline_command_t command = { .type = COMMAND_FLUSH,
        .session_id = session_id, .command_sequence = command_sequence,
        .flush = *flush };
    command_result_t result = { .result = PROTOCOL_CONTROL_RESULT_INVALID_STATE };
    if (!submit_sync(&command, &result)) return PROTOCOL_CONTROL_RESULT_INVALID_STATE;
    if (effective_local_us != NULL) *effective_local_us = result.effective_local_us;
    return result.result;
}

uint8_t physical_pipeline_stop_flush(uint32_t session_id, uint64_t *effective_local_us)
{
    pipeline_command_t command = { .type = COMMAND_STOP, .session_id = session_id };
    command_result_t result = { .result = PROTOCOL_CONTROL_RESULT_INVALID_STATE };
    if (!submit_sync(&command, &result)) return PROTOCOL_CONTROL_RESULT_INVALID_STATE;
    if (effective_local_us != NULL) *effective_local_us = result.effective_local_us;
    return result.result;
}

bool physical_pipeline_receive_trigger(physical_trigger_event_t *event,
    TickType_t ticks_to_wait)
{
    return trigger_queue != NULL && event != NULL &&
        xQueueReceive(trigger_queue, event, ticks_to_wait) == pdPASS;
}

uint32_t physical_pipeline_get_overwrite_count(uint8_t node_id)
{
    return node_id == configured_node_id ? observable_overwrites : 0;
}
