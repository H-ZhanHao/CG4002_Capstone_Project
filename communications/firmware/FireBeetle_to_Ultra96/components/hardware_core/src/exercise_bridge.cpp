#include "exercise_bridge.h"

#include <math.h>
#include <string.h>

#include "ContinuousCounter.h"
#include "ExercisePresets.h"
#include "NodeSession.h"
#include "RepCounter.h"
#include "driver/gpio.h"
#include "esp_log.h"

#ifndef APP_USE_MOCK_IMU
#define APP_USE_MOCK_IMU 0
#endif

namespace {
constexpr size_t PEER_QUEUE_CAPACITY = 12;
constexpr uint8_t NODE_1_HIP = 1;
constexpr uint8_t NODE_2_WRIST = 2;
const char *TAG = "exercise_bridge";

ExerciseSettings settings = getGenericSettings();
RepCounter pauseCounter(settings);
ContinuousCounter continuousCounter;
NodeRole role = WRIST;
ExerciseType exercise = JUMPING_JACK;
CountingMode mode = CONTINUOUS_MODE;
bool started = false;
bool countingStarted = false;
bool filterInitialised = false;
float filteredMagnitude = 0.0f;

LinkPacket peerQueue[PEER_QUEUE_CAPACITY];
size_t peerHead = 0;
size_t peerCount = 0;
uint32_t peerDrops = 0;

uint32_t nowMs = 0;

void logMessage(const char *message) {
  ESP_LOGI(TAG, "%s", message);
}

void queuePeer(const LinkPacket& packet) {
  if (peerCount == PEER_QUEUE_CAPACITY) {
    ++peerDrops;
    return;
  }
  peerQueue[(peerHead + peerCount) % PEER_QUEUE_CAPACITY] = packet;
  ++peerCount;
}

NodeSession session(queuePeer, logMessage);

void writeLed(bool on) {
  gpio_set_level(static_cast<gpio_num_t>(GeneralConfig::LED_PIN),
                 on ? GeneralConfig::LED_ON : GeneralConfig::LED_OFF);
}

void restartCounters() {
  countingStarted = false;
  filterInitialised = false;
  filteredMagnitude = 0.0f;
  session.clearLedFeedback(nowMs);
  writeLed(false);
  if (!pauseCounter.beginPreset(nowMs, ExercisePresets::pause(exercise))) {
    session.standby();
    ESP_LOGE(TAG, "Invalid pause preset for %s", exerciseName(exercise));
    return;
  }
  continuousCounter.beginPreset(GeneralConfig::GYRO_BIAS_RAD_S, nowMs,
                                ExercisePresets::continuous(exercise));
  if (session.localActive()) {
    countingStarted = true;
    session.showLedFeedback(LedFeedback::READY, nowMs);
  }
}

void serviceSession() {
  session.update(nowMs);
  const bool restart = session.consumeRestart();
  if (restart || mode != session.mode()) {
    mode = session.mode();
    restartCounters();
  }
  writeLed(session.feedbackLedOn(nowMs));
}

#if APP_USE_MOCK_IMU
void completeMockActivation() {
  /* Exercise the real exclusive-activation state machine without requiring
   * provisioned peer radios on the no-IMU bench. Physical builds never use
   * this shortcut. */
  session.activate(nowMs);
  if (peerCount == 0) return;
  const LinkPacket request = peerQueue[peerHead];
  peerHead = (peerHead + 1) % PEER_QUEUE_CAPACITY;
  --peerCount;
  if (request.kind != PacketKind::DEACTIVATE) return;
  LinkPacket ack = {};
  ack.magic = request.magic;
  ack.version = request.version;
  ack.kind = PacketKind::STANDBY_ACK;
  ack.boot = request.boot ^ 0xA5A5A5A5U;
  if (!ack.boot) ack.boot = 1;
  ack.targetBoot = request.boot;
  ack.request = request.request;
  ack.role = role == HIP ? WRIST : HIP;
  session.receive(ack, nowMs);
  serviceSession();
  /* No radio is present in this mode. Keep its synthetic control traffic
   * from obscuring genuine bounded-queue diagnostics. */
  peerHead = peerCount = 0;
}
#endif
}

extern "C" esp_err_t exercise_bridge_begin(uint8_t node_id, uint32_t boot_id) {
  if ((node_id != NODE_1_HIP && node_id != NODE_2_WRIST) || boot_id == 0)
    return ESP_ERR_INVALID_ARG;
  role = node_id == NODE_1_HIP ? HIP : WRIST;
  exercise = role == HIP ? SQUAT : JUMPING_JACK;
  mode = ExercisePresets::defaultMode(exercise);
  nowMs = 0;
  peerHead = peerCount = peerDrops = 0;
  ESP_ERROR_CHECK(gpio_reset_pin(static_cast<gpio_num_t>(GeneralConfig::LED_PIN)));
  ESP_ERROR_CHECK(gpio_set_direction(static_cast<gpio_num_t>(GeneralConfig::LED_PIN),
                                    GPIO_MODE_OUTPUT));
  writeLed(false);
  session.begin(role, boot_id, mode);
  restartCounters();
  started = true;
#if APP_USE_MOCK_IMU
  completeMockActivation();
  ESP_LOGI(TAG, "Mock-only local activation applied");
#endif
  ESP_LOGI(TAG, "Node %u local exercise core ready: role=%s exercise=%s mode=%s",
           node_id, role == WRIST ? "WRIST" : "HIP", exerciseName(exercise),
           mode == PAUSE_MODE ? "PAUSE" : "CONTINUOUS");
  return ESP_OK;
}

extern "C" void exercise_bridge_update(uint64_t timestamp_us,
    const float acceleration[3], const float gyro[3]) {
  if (!started || acceleration == nullptr || gyro == nullptr) return;
  nowMs = static_cast<uint32_t>(timestamp_us / 1000U);
  serviceSession();
  if (!session.localActive() || !countingStarted) return;

  float magnitudeSquared = 0.0f;
  float correctedGyroSquared = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    if (!isfinite(acceleration[axis]) || !isfinite(gyro[axis])) return;
    magnitudeSquared += acceleration[axis] * acceleration[axis];
    const float corrected = gyro[axis] - GeneralConfig::GYRO_BIAS_RAD_S[axis];
    correctedGyroSquared += corrected * corrected;
  }
  const float magnitude = sqrtf(magnitudeSquared);
  if (!filterInitialised) {
    filteredMagnitude = magnitude;
    filterInitialised = true;
  } else {
    filteredMagnitude = GeneralConfig::FILTER_ALPHA * magnitude +
      (1.0f - GeneralConfig::FILTER_ALPHA) * filteredMagnitude;
  }
  const float accelerationMotion = fabsf(filteredMagnitude - pauseCounter.baselineMagnitude());
  const bool stationary = accelerationMotion < settings.calibrationEndGate &&
    sqrtf(correctedGyroSquared) < ModeConfig::QUIET_GYRO_LIMIT;
  if (mode == PAUSE_MODE) pauseCounter.update(filteredMagnitude, nowMs);
  else continuousCounter.update(gyro, stationary, nowMs);

  const bool counted = mode == PAUSE_MODE
    ? pauseCounter.consumeRepEvent() : continuousCounter.consumeRepEvent();
  if (counted) {
    session.showLedFeedback(LedFeedback::REP, nowMs);
    writeLed(true);
    ESP_LOGI(TAG, "REP exercise=%s count=%d", exerciseName(exercise),
             mode == PAUSE_MODE ? pauseCounter.repetitionCount()
                                : continuousCounter.repetitionCount());
  }
}

extern "C" bool exercise_bridge_activate(void) {
  if (!started) return false;
  session.activate(nowMs);
  return true;
}

extern "C" void exercise_bridge_standby(void) {
  if (!started) return;
  session.standby();
  countingStarted = false;
  writeLed(false);
}

extern "C" void exercise_bridge_reset(void) {
  if (!started) return;
  if (session.waiting()) session.standby();
  restartCounters();
}

extern "C" bool exercise_bridge_select_exercise(uint8_t requested) {
  if (!started || requested >= EXERCISE_COUNT) return false;
  const ExerciseType next = static_cast<ExerciseType>(requested);
  if (!ExercisePresets::allowed(role, next)) return false;
  if (next == exercise) return true;
  exercise = next;
  session.selectExerciseMode(ExercisePresets::defaultMode(exercise), nowMs);
  serviceSession();
  if (!session.localActive()) restartCounters();
  return true;
}

extern "C" bool exercise_bridge_select_mode(uint8_t requested) {
  if (!started || (requested != PAUSE_MODE && requested != CONTINUOUS_MODE))
    return false;
  session.setMode(static_cast<CountingMode>(requested), nowMs);
  serviceSession();
  return true;
}

extern "C" bool exercise_bridge_receive_peer(
    const exercise_bridge_peer_packet_t *packet) {
  if (!started || packet == nullptr) return false;
  static_assert(sizeof(LinkPacket) == sizeof(packet->bytes), "peer packet ABI changed");
  LinkPacket decoded;
  memcpy(&decoded, packet->bytes, sizeof(decoded));
  session.receive(decoded, nowMs);
  serviceSession();
  return true;
}

extern "C" bool exercise_bridge_take_peer(exercise_bridge_peer_packet_t *packet) {
  if (!started || packet == nullptr || peerCount == 0) return false;
  memcpy(packet->bytes, &peerQueue[peerHead], sizeof(packet->bytes));
  peerHead = (peerHead + 1) % PEER_QUEUE_CAPACITY;
  --peerCount;
  return true;
}

extern "C" void exercise_bridge_get_status(exercise_bridge_status_t *status) {
  if (status == nullptr) return;
  memset(status, 0, sizeof(*status));
  status->repetitions = static_cast<uint32_t>(mode == PAUSE_MODE
    ? pauseCounter.repetitionCount() : continuousCounter.repetitionCount());
  status->peer_tx_drops = peerDrops;
  status->exercise = static_cast<uint8_t>(exercise);
  status->mode = static_cast<uint8_t>(mode);
  status->gyro_axis = static_cast<int8_t>(continuousCounter.axis());
  status->active = session.localActive();
  status->waiting_for_peer = session.waiting();
  status->led_on = session.feedbackLedOn(nowMs);
}
