#ifndef PHYSICAL_PIPELINE_H
#define PHYSICAL_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"

typedef struct {
    uint32_t sequence;
    uint64_t detected_timestamp_us;
    protocol_activity_trigger_t trigger;
} physical_trigger_event_t;

esp_err_t physical_pipeline_start(QueueHandle_t output_queue, uint8_t node_id);
bool physical_pipeline_begin_session(uint32_t session_id, uint32_t boot_id);
void physical_pipeline_end_session(uint32_t session_id);
uint8_t physical_pipeline_start_flush(uint32_t session_id,
    uint32_t command_sequence, const protocol_start_flush_t *flush,
    uint64_t *effective_local_us);
uint8_t physical_pipeline_stop_flush(uint32_t session_id,
    uint64_t *effective_local_us);
bool physical_pipeline_receive_trigger(physical_trigger_event_t *event,
    TickType_t ticks_to_wait);
uint32_t physical_pipeline_get_overwrite_count(uint8_t node_id);

#endif
