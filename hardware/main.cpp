#include "ExerciseNode.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef EXERCISE_NODE_ROLE
#error "Select a hip or wrist PlatformIO environment"
#endif
static_assert(EXERCISE_NODE_ROLE == HIP || EXERCISE_NODE_ROLE == WRIST,
              "Invalid exercise node role");

extern "C" void app_main() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);
  ExerciseNode::begin(static_cast<NodeRole>(EXERCISE_NODE_ROLE));
  for (;;) {
    ExerciseNode::update();
    vTaskDelay(1); // Yield to the radio tasks and idle watchdog between samples.
  }
}
