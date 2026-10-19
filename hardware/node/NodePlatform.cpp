#include "NodePlatform.h"

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

UsbConsole usbConsole;

unsigned long NodePlatform::millis() {
  return static_cast<unsigned long>(esp_timer_get_time() / 1000);
}

unsigned long NodePlatform::micros() {
  return static_cast<unsigned long>(esp_timer_get_time());
}

void NodePlatform::delayMs(unsigned long duration) {
  vTaskDelay(pdMS_TO_TICKS(duration));
}

void NodePlatform::outputPin(int pin) {
  ESP_ERROR_CHECK(gpio_reset_pin(static_cast<gpio_num_t>(pin)));
  ESP_ERROR_CHECK(gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT));
}

void NodePlatform::writePin(int pin, int level) {
  gpio_set_level(static_cast<gpio_num_t>(pin), level);
}

void NodeStream::println(const char* text) {
  printf("%s\r\n", text);
}

void NodeStream::printf(const char* format, ...) {
  char line[384];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (length > 0) {
    write(reinterpret_cast<const uint8_t*>(line),
          static_cast<size_t>(length) < sizeof(line) ? length : sizeof(line) - 1);
  }
}

void UsbConsole::begin(int baud) {
  uart_config_t config = {};
  config.baud_rate = baud;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  // Queue normal telemetry/peak/window bursts instead of waiting for UART wire time.
  // Excessive console output can still block once this finite buffer fills.
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 2048, 0, nullptr, 0));
}

int UsbConsole::read() {
  uint8_t value;
  return uart_read_bytes(UART_NUM_0, &value, 1, 0) == 1 ? value : -1;
}

size_t UsbConsole::write(const uint8_t* bytes, size_t length) {
  const int written = uart_write_bytes(UART_NUM_0, bytes, length);
  return written > 0 ? static_cast<size_t>(written) : 0;
}

namespace {
  constexpr EventBits_t SPP_READY = BIT0;
  constexpr EventBits_t SPP_FAILED = BIT1;
  struct ReceivedByte { uint32_t session; int16_t value; };
  struct OutgoingLine {
    uint32_t session;
    uint16_t length;
    uint8_t bytes[384];
  };
  QueueHandle_t bluetoothInput = nullptr;
  QueueHandle_t bluetoothOutput = nullptr;
  EventGroupHandle_t bluetoothStartup = nullptr;
  std::atomic<uint32_t> clientHandle{0};
  std::atomic<uint32_t> clientSession{0};
  std::atomic<bool> txBusy{false};
  std::atomic<bool> congested{false};
  char bluetoothName[64];
  // Keep the outstanding buffer alive through ESP_SPP_WRITE_EVT.
  OutgoingLine sendingLine;

  void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* parameter) {
    if (event == ESP_BT_GAP_CFM_REQ_EVT) {
      // This device has no display/keypad: SSP Just Works pairing.
      esp_bt_gap_ssp_confirm_reply(parameter->cfm_req.bda, true);
    } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
      esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
      if (parameter->pin_req.min_16_digit) {
        esp_bt_gap_pin_reply(parameter->pin_req.bda, false, 0, pin);
      } else {
        esp_bt_gap_pin_reply(parameter->pin_req.bda, true, 4, pin);
      }
    }
  }

  void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t* parameter) {
    switch (event) {
      case ESP_SPP_INIT_EVT:
        if (parameter->init.status != ESP_SPP_SUCCESS ||
            esp_bt_gap_set_device_name(bluetoothName) != ESP_OK ||
            esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE,
                              0, "ExerciseNode") != ESP_OK) {
          xEventGroupSetBits(bluetoothStartup, SPP_FAILED);
        }
        break;
      case ESP_SPP_START_EVT:
        if (parameter->start.status == ESP_SPP_SUCCESS &&
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE) == ESP_OK) {
          xEventGroupSetBits(bluetoothStartup, SPP_READY);
        } else {
          xEventGroupSetBits(bluetoothStartup, SPP_FAILED);
        }
        break;
      case ESP_SPP_SRV_OPEN_EVT:
        if (parameter->srv_open.status != ESP_SPP_SUCCESS) break;
        if (clientHandle.load() != 0) {
          esp_spp_disconnect(parameter->srv_open.handle); // One laptop per node.
          break;
        }
        xQueueReset(bluetoothInput);
        xQueueReset(bluetoothOutput);
        txBusy.store(false);
        congested.store(false);
        clientSession.fetch_add(1);
        clientHandle.store(parameter->srv_open.handle);
        break;
      case ESP_SPP_CLOSE_EVT:
        if (parameter->close.handle == clientHandle.load()) {
          clientHandle.store(0);
          clientSession.fetch_add(1);
          txBusy.store(false);
          congested.store(false);
          xQueueReset(bluetoothInput);
          xQueueReset(bluetoothOutput);
        }
        break;
      case ESP_SPP_DATA_IND_EVT: {
        if (parameter->data_ind.handle != clientHandle.load()) break;
        const uint32_t session = clientSession.load();
        if (uxQueueSpacesAvailable(bluetoothInput) < parameter->data_ind.len) {
          // Never execute a command assembled from truncated input.
          xQueueReset(bluetoothInput);
          const ReceivedByte overflow = {session, -2};
          xQueueSend(bluetoothInput, &overflow, 0);
          // Drop this whole chunk. Resume only after a fresh line terminator.
          break;
        }
        for (int i = 0; i < parameter->data_ind.len; ++i) {
          const ReceivedByte value = {session, parameter->data_ind.data[i]};
          xQueueSend(bluetoothInput, &value, 0);
        }
        break;
      }
      case ESP_SPP_WRITE_EVT:
        if (parameter->write.handle == clientHandle.load()) {
          congested.store(parameter->write.cong);
          txBusy.store(false);
        }
        break;
      case ESP_SPP_CONG_EVT:
        if (parameter->cong.handle == clientHandle.load()) {
          congested.store(parameter->cong.cong);
        }
        break;
      default:
        break;
    }
  }
}

bool BluetoothSpp::begin(const char* deviceName) {
  snprintf(bluetoothName, sizeof(bluetoothName), "%s", deviceName);
  bluetoothInput = xQueueCreate(512, sizeof(ReceivedByte));
  bluetoothOutput = xQueueCreate(16, sizeof(OutgoingLine));
  bluetoothStartup = xEventGroupCreate();
  if (!bluetoothInput || !bluetoothOutput || !bluetoothStartup) return false;

  esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_init(&controller) != ESP_OK ||
      esp_bt_controller_enable(ESP_BT_MODE_BTDM) != ESP_OK ||
      esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK ||
      esp_bt_gap_register_callback(gapCallback) != ESP_OK ||
      esp_spp_register_callback(sppCallback) != ESP_OK) return false;

  esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
  if (esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability)) != ESP_OK) {
    return false;
  }
  esp_spp_cfg_t config = {};
  config.mode = ESP_SPP_MODE_CB;
  config.enable_l2cap_ertm = false;
  config.tx_buffer_size = 0; // Only used in VFS mode.
  if (esp_spp_enhanced_init(&config) != ESP_OK) return false;
  const EventBits_t result = xEventGroupWaitBits(bluetoothStartup, SPP_READY | SPP_FAILED,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
  return (result & SPP_READY) && !(result & SPP_FAILED);
}

bool BluetoothSpp::hasClient() const { return clientHandle.load() != 0; }
uint32_t BluetoothSpp::connectionId() const { return clientSession.load(); }

int BluetoothSpp::read() {
  ReceivedByte value;
  while (bluetoothInput && xQueueReceive(bluetoothInput, &value, 0) == pdTRUE) {
    if (hasClient() && value.session == clientSession.load()) return value.value;
  }
  return -1;
}

size_t BluetoothSpp::write(const uint8_t* bytes, size_t length) {
  if (!hasClient() || length == 0 || length > sizeof(OutgoingLine::bytes)) return 0;
  OutgoingLine line = {};
  line.session = clientSession.load();
  line.length = static_cast<uint16_t>(length);
  memcpy(line.bytes, bytes, length);
  // Bounded, nonblocking queue: a stalled laptop must not stop motion sampling.
  return xQueueSend(bluetoothOutput, &line, 0) == pdTRUE ? length : 0;
}

void BluetoothSpp::service() {
  if (!hasClient() || congested.load() || txBusy.load()) return;
  while (xQueueReceive(bluetoothOutput, &sendingLine, 0) == pdTRUE) {
    if (sendingLine.session != clientSession.load()) continue;
    const uint32_t handle = clientHandle.load();
    if (!handle) return;
    txBusy.store(true);
    if (esp_spp_write(handle, sendingLine.length, sendingLine.bytes) != ESP_OK) {
      txBusy.store(false);
    }
    return;
  }
}
