#include "feedback_bridge.h"
#include "FeedbackController.h"
#include "FeedbackDiagnostic.h"
// Use the canonical config; the older communications copy has separate types.
#include "../../../../../../hardware/node/ExerciseConfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef APP_FEEDBACK_DIAGNOSTICS
#define APP_FEEDBACK_DIAGNOSTICS 0
#endif

namespace {
FeedbackController controller;
bool (*fallInjector)(void) = nullptr;
#if APP_FEEDBACK_DIAGNOSTICS
FeedbackDiagnostic diagnostic;
const char *TAG = "feedback";
#endif
int lastLed = -1, lastBuzzer = -1;
void drive() {
  const int led = controller.ledOn() ? GeneralConfig::LED_ON : GeneralConfig::LED_OFF;
  if (led != lastLed) {
    gpio_set_level(static_cast<gpio_num_t>(GeneralConfig::LED_PIN), led);
    lastLed = led;
  }
  if (controller.hasBuzzer()) {
    const int buzzer = controller.buzzerOn() ? 1 : 0;
    if (buzzer != lastBuzzer) {
      gpio_set_level(static_cast<gpio_num_t>(FallConfig::BUZZER_PIN), buzzer);
      lastBuzzer = buzzer;
    }
  }
}
esp_err_t output(int pin) {
  esp_err_t err = gpio_reset_pin(static_cast<gpio_num_t>(pin));
  if (err != ESP_OK) return err;
  // Set the inactive latch before enabling the output driver.
  err = gpio_set_level(static_cast<gpio_num_t>(pin), 0);
  if (err != ESP_OK) return err;
  return gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
}
}

extern "C" esp_err_t feedback_bridge_begin(uint8_t node_id) {
  if (node_id != 1 && node_id != 2) return ESP_ERR_INVALID_ARG;
  controller.begin(node_id);
#if APP_FEEDBACK_DIAGNOSTICS
  diagnostic = FeedbackDiagnostic{};
#endif
  lastLed = lastBuzzer = -1;
  esp_err_t err = output(GeneralConfig::LED_PIN);
  if (err != ESP_OK) return err;
  if (controller.hasBuzzer()) {
    err = output(FallConfig::BUZZER_PIN);
    if (err != ESP_OK) return err;
  }
#if APP_FEEDBACK_DIAGNOSTICS
  // UART0 is the board's USB serial console. Logging remains on the existing
  // console; only this adapter consumes RX. No blocking stdin or extra task.
  static_assert(CONFIG_ESP_CONSOLE_UART_NUM == 0, "Diagnostics require UART0 console");
  err = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
  if (err != ESP_OK) return err;
  ESP_LOGI(TAG, "USB diagnostics: feedback rep|ready|idle|fall-alarm|status; fall inject (synthetic) (115200)");
#endif
  drive();
  return ESP_OK;
}

extern "C" bool feedback_bridge_trigger(feedback_action_t action, uint32_t now_ms) {
  using Action = FeedbackController::Action;
  Action requested;
  switch (action) {
    case FEEDBACK_REP_LED: requested = Action::Rep; break;
    case FEEDBACK_READY_LED: requested = Action::Ready; break;
    case FEEDBACK_IDLE_BUZZ: requested = Action::Idle; break;
    case FEEDBACK_FALL_ALARM: requested = Action::FallAlarm; break;
    default: return false;
  }
  const bool accepted = controller.trigger(requested, now_ms);
  drive();
  return accepted;
}

extern "C" void feedback_bridge_set_fall_injector(bool (*injector)(void)) {
  fallInjector = injector;
}

extern "C" void feedback_bridge_service(uint32_t now_ms) {
  controller.update(now_ms);
  drive(); // Always expire outputs, even with no valid sensor sample.
#if APP_FEEDBACK_DIAGNOSTICS
  uint8_t bytes[32]; // Bounded work per loop, including under continuous input.
  const int count = uart_read_bytes(UART_NUM_0, bytes, sizeof(bytes), 0);
  for (int i = 0; i < count; ++i) {
    using Command = FeedbackDiagnostic::Command;
    const Command command = diagnostic.feed(static_cast<char>(bytes[i]));
    if (command == Command::None) continue;
    if (command == Command::FallEvent) {
      const bool accepted = fallInjector && fallInjector();
      ESP_LOGW(TAG, "SYNTHETIC_FALL %s (test injection, not sensor detection)",
          accepted ? "accepted" : "rejected");
      continue;
    }
    if (command == Command::Invalid) {
      ESP_LOGW(TAG, "Invalid/oversized command: feedback rep|ready|idle|fall-alarm|status; fall inject (synthetic)");
      continue;
    }
    if (command != Command::Status) {
      const feedback_action_t action = command == Command::Rep ? FEEDBACK_REP_LED :
          command == Command::Ready ? FEEDBACK_READY_LED :
          command == Command::Idle ? FEEDBACK_IDLE_BUZZ : FEEDBACK_FALL_ALARM;
      const bool accepted = feedback_bridge_trigger(action, now_ms);
      ESP_LOGI(TAG, "Diagnostic output %s%s", accepted ? "accepted" : "rejected",
          command == Command::FallAlarm ? " (output test only; no fall detected)" : "");
    }
    ESP_LOGI(TAG, "LED=%s on=%d buzzer=%s on=%d supported=%d",
        controller.ledPattern(), controller.ledOn(), controller.buzzerPattern(),
        controller.buzzerOn(), controller.hasBuzzer());
  }
#endif
}
