#include "physical_pipeline.h"

#include <math.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "exercise_bridge.h"
#include "feedback_bridge.h"
#include "feedback_delivery.h"
#include "freertos/task.h"
#include "hardware_bridge.h"
#include "sensor_history.h"
#include "fall_bridge.h"
#include "device_events.h"

#define PIPELINE_TASK_STACK 4096
#define PIPELINE_TASK_PRIORITY 4
#define PIPELINE_COMMAND_QUEUE_LENGTH 8
#define PIPELINE_TRIGGER_QUEUE_LENGTH 2

typedef enum {
    COMMAND_BEGIN, COMMAND_END, COMMAND_ARM, COMMAND_FLUSH, COMMAND_STOP, COMMAND_FEEDBACK
} command_type_t;
typedef struct {
    command_type_t type;
    uint32_t session_id;
    uint32_t boot_id;
    uint32_t command_sequence;
    protocol_feedback_t feedback;
    protocol_arm_t arm;
    protocol_start_flush_t flush;
    QueueHandle_t result_queue;
} pipeline_command_t;
typedef struct { uint8_t result; uint64_t effective_local_us; } command_result_t;

static const char *TAG = "physical_pipeline";
static QueueHandle_t command_queue;
static QueueHandle_t trigger_queue;
static QueueHandle_t feedback_ack_queue;
static feedback_delivery_t feedback_delivery;
static QueueHandle_t sample_queue;
static TaskHandle_t task_handle;
static uint8_t configured_node_id;
static volatile uint32_t observable_overwrites;
/*
 * The 701-entry history is intentionally bounded, but it is much larger than
 * this task's 4 KiB stack. Keep task-owned state in static DRAM so entering the
 * task cannot overwrite FreeRTOS queue control blocks before its first read.
 * Only pipeline_task accesses these objects after startup.
 */
static physical_history_t pipeline_history;
static physical_feeder_t pipeline_feeder;
static device_events_t device_events;
static portMUX_TYPE event_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t board_boot_id;
static uint64_t last_fall_us;

static bool record_fall(bool synthetic)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (configured_node_id != 1 || (last_fall_us && now-last_fall_us < 3000000ULL)) return false;
    last_fall_us = now;
    // Alarm is local and immediate, even if delivery storage is full/offline.
    (void)feedback_bridge_trigger(FEEDBACK_FALL_ALARM, (uint32_t)(now/1000));
    portENTER_CRITICAL(&event_lock);
    bool queued = device_events_push(&device_events, now, synthetic);
    uint64_t id = device_events.next_id;
    portEXIT_CRITICAL(&event_lock);
    ESP_LOGW(TAG, "FALL_STARTED boot=%lu event=%llu synthetic=%d queued=%d duration_ms=3000",
        (unsigned long)board_boot_id, (unsigned long long)id, synthetic, queued);
    return true;
}
static bool inject_fall(void) { return record_fall(true); }

uint32_t physical_pipeline_boot_id(void) { return board_boot_id; }
bool physical_pipeline_event_due(uint64_t now, protocol_device_event_t *event)
{
    portENTER_CRITICAL(&event_lock);
    bool due = device_events_due(&device_events, now, event);
    portEXIT_CRITICAL(&event_lock);
    return due;
}
bool physical_pipeline_event_ack(const protocol_device_event_ack_t *ack)
{
    portENTER_CRITICAL(&event_lock);
    bool accepted = device_events_ack(&device_events, ack);
    portEXIT_CRITICAL(&event_lock);
    return accepted;
}

static bool apply_feedback(uint8_t action, uint64_t now_us, void *context)
{
    (void)context;
    feedback_action_t output;
    switch (action) {
    case PROTOCOL_FEEDBACK_REP_LED: output = FEEDBACK_REP_LED; break;
    case PROTOCOL_FEEDBACK_READY_LED: output = FEEDBACK_READY_LED; break;
    case PROTOCOL_FEEDBACK_IDLE_BUZZ: output = FEEDBACK_IDLE_BUZZ; break;
    default: return false;
    }
    return feedback_bridge_trigger(output, (uint32_t)(now_us / 1000U));
}

static void handle_command(const pipeline_command_t *command,
                           physical_history_t *history,
                           physical_feeder_t *feeder,
                           uint32_t *session_id, uint32_t *boot_id,
                           uint32_t *arm_generation, bool *trigger_armed,
                           uint64_t *scheduled_arm_us)
{
    command_result_t reply = { .effective_local_us = (uint64_t)esp_timer_get_time() };
    if (command->type == COMMAND_BEGIN) {
        feedback_delivery_begin(&feedback_delivery, command->session_id, configured_node_id);
        *session_id = command->session_id;
        *boot_id = command->boot_id;
        *arm_generation = 0;
        *trigger_armed = false;
        *scheduled_arm_us = 0;
        physical_feeder_stop(feeder);
        xQueueReset(sample_queue);
        xQueueReset(trigger_queue);
    } else if (command->type == COMMAND_FEEDBACK) {
        physical_feedback_ack_t response = { .session_id = command->session_id };
        response.ack = feedback_delivery_process(&feedback_delivery,
            command->session_id, command->command_sequence, &command->feedback,
            reply.effective_local_us, apply_feedback, NULL);
        // A lost/full ACK queue is safe: retry reuses the cached result.
        (void)xQueueSend(feedback_ack_queue, &response, 0);
    } else if (command->session_id != *session_id) {
        reply.result = PROTOCOL_CONTROL_RESULT_STALE_IDENTITY;
    } else if (command->type == COMMAND_END || command->type == COMMAND_STOP) {
        reply.result = feeder->active ? PROTOCOL_CONTROL_RESULT_APPLIED
                                      : PROTOCOL_CONTROL_RESULT_ALREADY_STOPPED;
        physical_feeder_stop(feeder);
        *trigger_armed = false;
        *scheduled_arm_us = 0;
        xQueueReset(sample_queue);
#if APP_IDLE_RESUME_INTEGRATION
        /* A peak queued before STOP must not reopen the newly armed run. */
        xQueueReset(trigger_queue);
#endif
        if (command->type == COMMAND_END) {
            feedback_delivery_begin(&feedback_delivery, 0, configured_node_id);
            *session_id = 0;
            *boot_id = 0;
        }
    } else if (command->type == COMMAND_ARM) {
        int64_t coordinator_arm_us = (int64_t)command->arm.coordinator_arm_us;
        int64_t local_arm_us = coordinator_arm_us +
            command->arm.node_minus_coordinator_us;
        reply.effective_local_us = local_arm_us > 0 ?
            (uint64_t)local_arm_us : 0;
        if (feeder->active || command->arm.arm_generation == 0 ||
            local_arm_us <= esp_timer_get_time()) {
            reply.result = PROTOCOL_CONTROL_RESULT_INVALID_STATE;
        } else {
            *arm_generation = command->arm.arm_generation;
            *trigger_armed = false;
            *scheduled_arm_us = (uint64_t)local_arm_us;
            xQueueReset(trigger_queue);
            reply.result = PROTOCOL_CONTROL_RESULT_APPLIED;
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
            *trigger_armed = false;
            *scheduled_arm_us = 0;
            xQueueReset(trigger_queue);
        }
    }
    if (command->result_queue != NULL) (void)xQueueSend(command->result_queue, &reply, 0);
}

static void pipeline_task(void *unused)
{
    (void)unused;
    uint32_t session_id = 0, boot_id = 0, arm_generation = 0, sequence = 0;
    bool trigger_armed = false;
    uint64_t scheduled_arm_us = 0;
    physical_history_clear(&pipeline_history);
    physical_feeder_stop(&pipeline_feeder);
    uint64_t next_capture_us = (uint64_t)esp_timer_get_time();
    uint32_t expired_before = 0, overflow_before = 0;
    for (;;) {
        feedback_bridge_service((uint32_t)((uint64_t)esp_timer_get_time() / 1000U));
        pipeline_command_t command;
        while (xQueueReceive(command_queue, &command, 0) == pdPASS)
            handle_command(&command, &pipeline_history, &pipeline_feeder,
                           &session_id, &boot_id, &arm_generation,
                           &trigger_armed, &scheduled_arm_us);
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (!trigger_armed && scheduled_arm_us != 0 &&
            now_us >= scheduled_arm_us) {
            hardware_bridge_reset_peak_detector();
            xQueueReset(trigger_queue);
            trigger_armed = true;
            scheduled_arm_us = 0;
        }
        if (now_us >= next_capture_us) {
            protocol_physical_sample_t captured = {0};
            hardware_bridge_sample_t hardware_sample;
            hardware_bridge_peak_t peak;
            if (hardware_bridge_capture(sequence, &hardware_sample, &peak)) {
                if (fall_bridge_update(hardware_sample.acceleration,
                        hardware_sample.timestamp_us, true)) (void)record_fall(false);
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
                if (session_id != 0 && trigger_armed &&
                    peak.peak_timestamp_us != 0) {
                    physical_trigger_event_t event = {
                        .sequence = captured.sequence,
                        .detected_timestamp_us = peak.detected_timestamp_us,
                        .trigger = { .boot_id = boot_id,
                            .arm_generation = arm_generation,
                            .peak_timestamp_us = peak.peak_timestamp_us },
                    };
                    if (xQueueSend(trigger_queue, &event, 0) == pdPASS)
                        trigger_armed = false;
                }
            } else {
                (void)fall_bridge_update(NULL, now_us, false);
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
        portENTER_CRITICAL(&event_lock);
        device_events_expire(&device_events, now_us);
        uint32_t expired = device_events.expired, overflow = device_events.overflow;
        portEXIT_CRITICAL(&event_lock);
        if (expired != expired_before || overflow != overflow_before) {
            ESP_LOGW(TAG, "DEVICE_EVENT_LOSS expired=%lu overflow=%lu",
                (unsigned long)expired, (unsigned long)overflow);
            expired_before = expired; overflow_before = overflow;
        }
        vTaskDelay(1);
    }
}

esp_err_t physical_pipeline_start(QueueHandle_t output_queue, uint8_t node_id)
{
    if (output_queue == NULL || node_id < 1 || node_id > 2) return ESP_ERR_INVALID_ARG;
    if (task_handle != NULL) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(hardware_bridge_begin(), TAG,
        "hardware core initialization");
    ESP_RETURN_ON_ERROR(feedback_bridge_begin(node_id), TAG,
        "feedback initialization");
    uint32_t exercise_boot_id = esp_random();
    if (exercise_boot_id == 0) exercise_boot_id = 1;
    ESP_RETURN_ON_ERROR(exercise_bridge_begin(node_id, exercise_boot_id), TAG,
        "exercise core initialization");
    command_queue = xQueueCreate(PIPELINE_COMMAND_QUEUE_LENGTH, sizeof(pipeline_command_t));
    trigger_queue = xQueueCreate(PIPELINE_TRIGGER_QUEUE_LENGTH, sizeof(physical_trigger_event_t));
    feedback_ack_queue = xQueueCreate(PIPELINE_COMMAND_QUEUE_LENGTH, sizeof(physical_feedback_ack_t));
    if (command_queue == NULL || trigger_queue == NULL || feedback_ack_queue == NULL) return ESP_ERR_NO_MEM;
    sample_queue = output_queue;
    configured_node_id = node_id;
    do { board_boot_id = esp_random(); } while (!board_boot_id);
    device_events_init(&device_events, board_boot_id);
    fall_bridge_begin(node_id);
    feedback_bridge_set_fall_injector(inject_fall);
    if (xTaskCreate(pipeline_task, "physical_pipeline", PIPELINE_TASK_STACK, NULL,
        PIPELINE_TASK_PRIORITY, &task_handle) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Node %u continuous 100 Hz %s pipeline started", node_id,
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

uint8_t physical_pipeline_arm(uint32_t session_id,
    const protocol_arm_t *arm, uint64_t *effective_local_us)
{
    if (arm == NULL) return PROTOCOL_CONTROL_RESULT_INVALID_STATE;
    pipeline_command_t command = { .type = COMMAND_ARM,
        .session_id = session_id, .arm = *arm };
    command_result_t result = { .result = PROTOCOL_CONTROL_RESULT_INVALID_STATE };
    if (!submit_sync(&command, &result)) return PROTOCOL_CONTROL_RESULT_INVALID_STATE;
    if (effective_local_us != NULL) *effective_local_us = result.effective_local_us;
    return result.result;
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

// Asynchronous, bounded request/result queues. No per-request reply queue can
// be deleted while a delayed pipeline command still references it.
bool physical_pipeline_submit_feedback(uint32_t session, uint32_t sequence,
                                       const protocol_feedback_t *feedback)
{
    if (command_queue == NULL || feedback == NULL || session == 0) return false;
    pipeline_command_t command = { .type = COMMAND_FEEDBACK, .session_id = session,
        .command_sequence = sequence, .feedback = *feedback };
    return xQueueSend(command_queue, &command, 0) == pdPASS;
}

bool physical_pipeline_receive_feedback_ack(physical_feedback_ack_t *response)
{
    return feedback_ack_queue != NULL && response != NULL &&
        xQueueReceive(feedback_ack_queue, response, 0) == pdPASS;
}
