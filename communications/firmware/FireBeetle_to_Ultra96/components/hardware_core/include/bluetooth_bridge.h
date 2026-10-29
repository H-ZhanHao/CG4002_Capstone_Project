#ifndef BLUETOOTH_BRIDGE_H
#define BLUETOOTH_BRIDGE_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the hardware team's Classic Bluetooth SPP transport. */
esp_err_t bluetooth_bridge_begin(uint8_t node_id);

/* Service bounded SPP I/O and apply commands from the physical-pipeline task. */
void bluetooth_bridge_poll(uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
