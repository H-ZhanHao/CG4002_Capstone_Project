#include "feedback_bridge.h"
#include "exercise_bridge.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned writes, buzzerTouches;
static int led, buzzer;
static const char *input = "";
static void touched(int pin) {
  assert(pin == 2 || pin == 26);
  if (pin == 26) ++buzzerTouches;
}
esp_err_t gpio_reset_pin(int pin) { touched(pin); return ESP_OK; }
esp_err_t gpio_set_direction(int pin, int) { touched(pin); return ESP_OK; }
esp_err_t gpio_set_level(int pin, int level) {
  touched(pin);
  ++writes;
  (pin == 2 ? led : buzzer) = level;
  return ESP_OK;
}
esp_err_t uart_driver_install(int, int rx, int tx, int, void *, int) {
  assert(rx == 256 && tx == 0);
  return ESP_OK;
}
int uart_read_bytes(int, void *out, size_t capacity, int wait) {
  assert(capacity <= 32 && wait == 0);
  size_t count = strlen(input);
  if (count > capacity) count = capacity;
  memcpy(out, input, count);
  input += count;
  return static_cast<int>(count);
}
static void command(const char *text, uint32_t now) {
  input = text;
  while (*input) feedback_bridge_service(now);
}
static void exerciseWithoutOutput(uint8_t node) {
  unsigned before = writes;
  assert(exercise_bridge_begin(node, 1234) == ESP_OK);
  for (unsigned index = 0; index < 400; ++index) {
    float acc[3] = {0, 0, 9.80665f}, gyro[3] = {0, 0, 0};
    if (index >= 260 && index < 320) acc[2] += 1.2f;
    if (index >= 260 && index < 336) gyro[0] = 2;
    else if (index >= 336 && index < 366) gyro[0] = -2;
    exercise_bridge_update(index * 20000ULL, acc, gyro);
  }
  exercise_bridge_status_t status;
  exercise_bridge_get_status(&status);
  assert(status.active && status.repetitions == 1);
  exercise_bridge_reset();
  exercise_bridge_standby();
  assert(writes == before); // Includes activation, ready, a real counted rep, reset/stop.
}
int main() {
  assert(feedback_bridge_begin(1) == ESP_OK);
  assert(led == 0 && buzzer == 0);
  command("feedback active\n", 10);
  assert(led == 1 && buzzer == 0);
  exerciseWithoutOutput(1);
  feedback_bridge_service(10000);
  assert(led == 1); // Activity never expires with time or local reps.
  command("feedback rep\n", 10000);
  assert(led == 1 && buzzer == 1);
  feedback_bridge_service(10100);
  assert(led == 1 && buzzer == 0);
  command("feedback idle\n", 10200);
  assert(led == 0 && buzzer == 0);
  command("feedback fall-alarm\n", 10300);
  command("feedback active\n", 10350);
  feedback_bridge_reset_session();
  assert(led == 0 && buzzer == 1);
  feedback_bridge_service(13300);
  assert(buzzer == 0);
  buzzerTouches = 0;
  assert(feedback_bridge_begin(2) == ESP_OK);
  exerciseWithoutOutput(2);
  command("feedback rep\nfeedback fall-alarm\nfeedback status\n", 14000);
  assert(buzzerTouches == 0 && led == 0);
  command("feedback active\n", 15000);
  feedback_bridge_service(25000);
  assert(led == 1);
  command("feedback idle\n", 25000);
  assert(led == 0 && buzzerTouches == 0);
  puts("GPIO bridge: persistent activity, silent idle, Hip-only rep beeps, local counter independence and fall-preserving session reset passed");
}
