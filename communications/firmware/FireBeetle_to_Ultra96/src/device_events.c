#include "device_events.h"
#include <string.h>
void device_events_init(device_events_t *s, uint32_t boot) {
    memset(s, 0, sizeof(*s)); s->boot_id = boot;
}
void device_events_expire(device_events_t *s, uint64_t now) {
    for (unsigned i=0; i<DEVICE_EVENTS_CAPACITY; ++i) {
        device_event_slot_t *slot = &s->slots[i];
        if (slot->used && now >= slot->expires_us) {
            slot->used = false; ++s->expired;
        }
    }
}
bool device_events_push(device_events_t *s, uint64_t now, bool synthetic) {
    device_events_expire(s, now);
    if (!s->boot_id || !now || s->next_id == UINT64_MAX) return false;
    for (unsigned i=0; i<DEVICE_EVENTS_CAPACITY; ++i) {
        if (s->slots[i].used) continue;
        s->slots[i] = (device_event_slot_t){
            .used = true, .event = { .boot_id=s->boot_id,
                .event_id=++s->next_id, .occurred_us=now, .duration_ms=3000,
                .kind=PROTOCOL_EVENT_FALL_STARTED, .synthetic=synthetic },
            .expires_us=now+DEVICE_EVENT_TTL_US, .retry_us=now };
        return true;
    }
    ++s->overflow; return false; // Preserve older pending events; drop newest.
}
bool device_events_due(device_events_t *s, uint64_t now, protocol_device_event_t *out) {
    device_events_expire(s, now);
    device_event_slot_t *selected = NULL;
    for (unsigned i=0; i<DEVICE_EVENTS_CAPACITY; ++i) {
        device_event_slot_t *slot = &s->slots[i];
        if (!slot->used || now < slot->retry_us || slot->attempts >= DEVICE_EVENT_MAX_ATTEMPTS) continue;
        if (!selected || slot->event.event_id < selected->event.event_id) selected = slot;
    }
    if (!selected) return false;
    *out = selected->event; ++selected->attempts;
    selected->retry_us = now + DEVICE_EVENT_RETRY_US;
    return true;
}
bool device_events_ack(device_events_t *s, const protocol_device_event_ack_t *a) {
    if (a->boot_id != s->boot_id || a->result > PROTOCOL_EVENT_ACK_DUPLICATE) return false;
    for (unsigned i=0; i<DEVICE_EVENTS_CAPACITY; ++i) {
        if (s->slots[i].used && s->slots[i].event.event_id == a->event_id &&
            s->slots[i].attempts) { s->slots[i].used=false; return true; }
    }
    return false;
}
