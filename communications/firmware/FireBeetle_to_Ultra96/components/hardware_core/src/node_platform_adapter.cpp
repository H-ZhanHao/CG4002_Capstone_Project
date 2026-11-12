#include "NodePlatform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The canonical MPU6050 driver only needs this platform service. Keep its
// initialization delays without pulling in the standalone Bluetooth runtime.
void NodePlatform::delayMs(unsigned long duration) {
  vTaskDelay(pdMS_TO_TICKS(duration));
}
