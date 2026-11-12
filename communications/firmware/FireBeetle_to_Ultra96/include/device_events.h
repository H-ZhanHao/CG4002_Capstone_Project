#pragma once
#include "protocol.h"
#define DEVICE_EVENTS_CAPACITY 8
#define DEVICE_EVENT_TTL_US 30000000ULL
#define DEVICE_EVENT_RETRY_US 500000ULL
#define DEVICE_EVENT_MAX_ATTEMPTS 60
typedef struct {
    bool used;
    protocol_device_event_t event;
    uint64_t expires_us, retry_us;
    unsigned attempts;
} device_event_slot_t;
typedef struct {
    device_event_slot_t slots[DEVICE_EVENTS_CAPACITY];
    uint32_t boot_id;
    uint64_t next_id;
    uint32_t overflow, expired;
} device_events_t;
// Caller serializes these bounded in-memory operations; never hold its lock
// during logging, GPIO work or socket I/O. Session changes do not clear events.
void device_events_init(device_events_t *state, uint32_t boot_id);
bool device_events_push(device_events_t *state, uint64_t now_us, bool synthetic);
bool device_events_due(device_events_t *state, uint64_t now_us, protocol_device_event_t *event);
bool device_events_ack(device_events_t *state, const protocol_device_event_ack_t *ack);
void device_events_expire(device_events_t *state, uint64_t now_us);
