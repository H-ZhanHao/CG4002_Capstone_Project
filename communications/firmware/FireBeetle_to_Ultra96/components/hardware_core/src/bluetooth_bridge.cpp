#include "bluetooth_bridge.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ExerciseProfiles.h"
#include "NodePlatform.h"
#include "esp_log.h"
#include "exercise_bridge.h"

namespace {
constexpr size_t COMMAND_CAPACITY = 128;
constexpr unsigned int POLL_BYTE_BUDGET = 64;
constexpr uint32_t STATUS_INTERVAL_MS = 1000;
constexpr const char *HIP_BLUETOOTH_NAME = "ExerciseHip";
constexpr const char *WRIST_BLUETOOTH_NAME = "ExerciseWrist";
const char *TAG = "bluetooth_bridge";

BluetoothSpp bluetooth;
char commandBuffer[COMMAND_CAPACITY];
size_t commandLength = 0;
bool commandOverflow = false;
bool bridgeStarted = false;
uint8_t localNodeId = 0;
uint32_t observedSession = 0;
uint32_t lastStatusMs = 0;

void sendLine(const char *format, ...) {
  if (!bridgeStarted || !bluetooth.hasClient()) return;
  char line[384];
  va_list arguments;
  va_start(arguments, format);
  const int bodyLength = vsnprintf(line, sizeof(line) - 2, format, arguments);
  va_end(arguments);
  if (bodyLength <= 0) return;
  size_t length = static_cast<size_t>(bodyLength);
  if (length > sizeof(line) - 3) length = sizeof(line) - 3;
  line[length++] = '\r';
  line[length++] = '\n';
  if (bluetooth.write(reinterpret_cast<const uint8_t *>(line), length) != length)
    ESP_LOGW(TAG, "Bluetooth output queue full; status line dropped");
}

const char *stateName(const exercise_bridge_status_t& status) {
  if (status.waiting_for_peer) return "ACTIVATING";
  return status.active ? "COUNTING" : "STANDBY";
}

void sendStatus() {
  exercise_bridge_status_t status;
  exercise_bridge_get_status(&status);
  sendLine("Node: %u | Exercise: %s | Mode: %s | %s | Reps: %lu | LED: %s | Peer queue drops: %lu",
           localNodeId, exerciseName(static_cast<ExerciseType>(status.exercise)),
           status.mode == EXERCISE_BRIDGE_PAUSE_MODE ? "PAUSE" : "CONTINUOUS",
           stateName(status), static_cast<unsigned long>(status.repetitions),
           status.led_on ? "ON" : "OFF",
           static_cast<unsigned long>(status.peer_tx_drops));
}

void normalize(char *text) {
  char *start = text;
  while (*start && isspace(static_cast<unsigned char>(*start))) ++start;
  if (start != text) memmove(text, start, strlen(start) + 1);
  size_t length = strlen(text);
  while (length && isspace(static_cast<unsigned char>(text[length - 1])))
    text[--length] = '\0';
  for (size_t i = 0; i < length; ++i)
    text[i] = static_cast<char>(toupper(static_cast<unsigned char>(text[i])));
}

void processCommand(char *text) {
  normalize(text);
  if (!text[0]) return;
  if (!strcmp(text, "HELP")) {
    sendLine("Commands: ACTIVATE | STANDBY | RESET | EXERCISE:<name> | EXERCISES | MODE:PAUSE | MODE:CONTINUOUS | STATUS | HELP");
    sendLine("IMU history, START_FLUSH, and stream timing are controlled by Ultra96 over TLS.");
    return;
  }
  if (!strcmp(text, "STATUS")) {
    sendStatus();
    return;
  }
  if (!strcmp(text, "EXERCISES")) {
    sendLine(localNodeId == 1
      ? "HIP exercises: PUSHUP | SQUAT | LUNGE | LEG_RAISE"
      : "WRIST exercises: SITUP | JUMPING_JACK");
    return;
  }
  if (!strcmp(text, "ACTIVATE")) {
    if (!exercise_bridge_activate()) sendLine("ERROR: Exercise core is unavailable.");
    else sendStatus();
    return;
  }
  if (!strcmp(text, "STANDBY") || !strcmp(text, "DEACTIVATE")) {
    exercise_bridge_standby();
    sendStatus();
    return;
  }
  if (!strcmp(text, "RESET")) {
    exercise_bridge_reset();
    sendStatus();
    return;
  }
  if (!strncmp(text, "EXERCISE:", 9)) {
    ExerciseType requested;
    if (!parseExercise(text + 9, requested)) {
      sendLine("ERROR: Unknown exercise. Send EXERCISES for valid names.");
    } else if (!exercise_bridge_select_exercise(static_cast<uint8_t>(requested))) {
      sendLine("ERROR: That exercise belongs to the other sensor role.");
    } else {
      sendStatus();
    }
    return;
  }
  const char *mode = !strncmp(text, "MODE:", 5) ? text + 5 : text;
  if (!strcmp(mode, "PAUSE") || !strcmp(mode, "CONTINUOUS")) {
    const uint8_t requested = !strcmp(mode, "PAUSE")
      ? EXERCISE_BRIDGE_PAUSE_MODE : EXERCISE_BRIDGE_CONTINUOUS_MODE;
    if (!exercise_bridge_select_mode(requested)) sendLine("ERROR: Invalid mode.");
    else sendStatus();
    return;
  }
  sendLine("ERROR: Unknown command. Send HELP for commands.");
}
}

extern "C" esp_err_t bluetooth_bridge_begin(uint8_t node_id) {
  if (node_id < 1 || node_id > 2) return ESP_ERR_INVALID_ARG;
  localNodeId = node_id;
  commandLength = 0;
  commandOverflow = false;
  observedSession = 0;
  lastStatusMs = 0;
  const char *deviceName = node_id == 1
    ? HIP_BLUETOOTH_NAME : WRIST_BLUETOOTH_NAME;
  if (!bluetooth.begin(deviceName)) {
    ESP_LOGE(TAG, "Could not start Classic Bluetooth SPP");
    return ESP_FAIL;
  }
  bridgeStarted = true;
  ESP_LOGI(TAG, "Classic Bluetooth SPP ready as %s", deviceName);
  return ESP_OK;
}

extern "C" void bluetooth_bridge_poll(uint64_t now_us) {
  if (!bridgeStarted) return;
  bluetooth.service();
  const uint32_t session = bluetooth.connectionId();
  if (session != observedSession) {
    commandLength = 0;
    commandOverflow = false;
    observedSession = session;
    if (bluetooth.hasClient()) {
      sendLine("[Laptop Bluetooth connected]");
      sendStatus();
    }
  }

  unsigned int budget = POLL_BYTE_BUDGET;
  while (budget-- && bluetooth.hasClient()) {
    const int value = bluetooth.read();
    if (value == -2) {
      commandLength = 0;
      commandOverflow = true;
      continue;
    }
    if (value < 0) break;
    if (value == '\r' || value == '\n') {
      if (commandOverflow) sendLine("ERROR: Command too long. Send HELP for commands.");
      else if (commandLength) {
        commandBuffer[commandLength] = '\0';
        processCommand(commandBuffer);
      }
      commandLength = 0;
      commandOverflow = false;
      continue;
    }
    if (!commandOverflow) {
      if (commandLength + 1 >= sizeof(commandBuffer)) commandOverflow = true;
      else commandBuffer[commandLength++] = static_cast<char>(value);
    }
  }

  const uint32_t nowMs = static_cast<uint32_t>(now_us / 1000U);
  if (bluetooth.hasClient() && nowMs - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = nowMs;
    sendStatus();
  }
  bluetooth.service();
}
