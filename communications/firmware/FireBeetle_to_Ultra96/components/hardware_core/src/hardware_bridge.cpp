#include "hardware_bridge.h"

#include <math.h>
#include <string.h>

#include "ExerciseConfig.h"
#include "GenericPeakDetector.h"
#include "Mpu6050.h"
#include "esp_timer.h"

#ifndef APP_USE_MOCK_IMU
#define APP_USE_MOCK_IMU 0
#endif

namespace {
Mpu6050 mpu;
GenericPeakDetector detector;

uint64_t reconstructTimestampUs(uint64_t detectedUs, uint32_t eventMs) {
  const uint32_t detectedMs = static_cast<uint32_t>(detectedUs / 1000U);
  const uint32_t elapsedMs = detectedMs - eventMs;
  return detectedUs - static_cast<uint64_t>(elapsedMs) * 1000U;
}

#if APP_USE_MOCK_IMU
void makeMock(uint32_t index, hardware_bridge_sample_t& sample) {
  const float phase = static_cast<float>(index % 400U);
  sample.acceleration[0] = 0.02f * sinf(phase * 0.1f);
  sample.acceleration[1] = 0.02f * cosf(phase * 0.1f);
  sample.acceleration[2] = 9.80665f;
  sample.gyro[0] = sample.gyro[1] = sample.gyro[2] = 0.0f;
  const uint32_t cycle = index % 400U;
  /* One deterministic, counter-valid movement every eight seconds. The
   * acceleration portion is long enough for pause-mode presets; the gyro
   * portion supplies axis discovery followed by a complete two-direction
   * cycle for continuous mode. Its decline also remains a generic trigger. */
  if (cycle >= 260U && cycle < 320U) sample.acceleration[2] += 1.2f;
  if (cycle >= 260U && cycle < 336U) sample.gyro[0] = 2.0f;
  else if (cycle >= 336U && cycle < 366U) sample.gyro[0] = -2.0f;
}
#endif
}

extern "C" esp_err_t hardware_bridge_begin(void) {
  detector.reset();
#if APP_USE_MOCK_IMU
  return ESP_OK;
#else
  return mpu.begin() ? ESP_OK : ESP_ERR_NOT_FOUND;
#endif
}

extern "C" void hardware_bridge_reset_peak_detector(void) {
  detector.reset();
}

extern "C" bool hardware_bridge_capture(uint32_t sample_index,
    hardware_bridge_sample_t *sample, hardware_bridge_peak_t *peak) {
  if (sample == nullptr || peak == nullptr) return false;
  memset(sample, 0, sizeof(*sample));
  memset(peak, 0, sizeof(*peak));
  sample->timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
#if APP_USE_MOCK_IMU
  makeMock(sample_index, *sample);
#else
  if (!mpu.read(sample->acceleration, sample->gyro)) return false;
#endif

  SensorSample detectorSample = {};
  detectorSample.timestampMs = static_cast<uint32_t>(sample->timestamp_us / 1000U);
  memcpy(detectorSample.acceleration, sample->acceleration,
         sizeof(detectorSample.acceleration));
  memcpy(detectorSample.gyro, sample->gyro, sizeof(detectorSample.gyro));
  GenericPeakEvent event;
  if (!detector.update(detectorSample, event)) return true;
  peak->peak_timestamp_us = reconstructTimestampUs(sample->timestamp_us, event.peakMs);
  peak->detected_timestamp_us =
    reconstructTimestampUs(sample->timestamp_us, event.detectedMs);
  peak->acceleration_mps2 = event.accelerationMps2;
  peak->gyro_rad_s = event.gyroRadS;
  return true;
}
