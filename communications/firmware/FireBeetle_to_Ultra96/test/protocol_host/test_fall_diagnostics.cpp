#include "feedback_bridge.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int buzzer, buzzerTouches;
static unsigned injected;
static uint32_t nowMs;
static const char *input="";
static void touch(int pin) { if (pin==26) ++buzzerTouches; }
esp_err_t gpio_reset_pin(int pin) { touch(pin); return ESP_OK; }
esp_err_t gpio_set_direction(int pin, int) { touch(pin); return ESP_OK; }
esp_err_t gpio_set_level(int pin, int level) {
  touch(pin); if (pin==26) buzzer=level; return ESP_OK;
}
esp_err_t uart_driver_install(int, int, int, int, void *, int) { return ESP_OK; }
int uart_read_bytes(int, void *out, size_t capacity, int) {
  size_t count=strlen(input); if (count>capacity) count=capacity;
  memcpy(out,input,count); input+=count; return static_cast<int>(count);
}
static bool inject() {
  if (!feedback_bridge_trigger(FEEDBACK_FALL_ALARM, nowMs)) return false;
  ++injected; return true;
}
static void command(const char *line, uint32_t now) {
  input=line; nowMs=now;
  while (*input) feedback_bridge_service(now);
}
int main() {
  assert(feedback_bridge_begin(1)==ESP_OK);
  feedback_bridge_set_fall_injector(inject);
  command("feedback fall-alarm\n", 100);
  assert(buzzer==1 && injected==0); // Output test is never an event.
  command("fall inject\n", 200);
  assert(buzzer==1 && injected==1);
  feedback_bridge_service(3199); assert(buzzer==1);
  feedback_bridge_service(3200); assert(buzzer==0 && injected==1);
  command("fall\n", 4000); assert(injected==1); // Must explicitly say inject.
  buzzerTouches=0;
  assert(feedback_bridge_begin(2)==ESP_OK);
  command("fall inject\n", 5000);
  assert(injected==1 && buzzerTouches==0);
  puts("Fall diagnostics: synthetic command routed, output-only command separate, nonblocking expiry, no wrist GPIO PASS");
}
