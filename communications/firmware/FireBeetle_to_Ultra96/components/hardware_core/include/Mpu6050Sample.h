#pragma once
#include <stdint.h>

// MPU6050 burst at ACCEL_XOUT_H: acceleration, temperature, then angular rate.
// Configuration must be +/-8g and +/-500 deg/s.
inline void decodeMpu6050Sample(const uint8_t raw[14], float acceleration[3], float gyro[3]) {
  for (int axis = 0; axis < 3; ++axis) {
    const int a = axis * 2;
    const int g = 8 + axis * 2;
    const int16_t accelerationRaw = static_cast<int16_t>((raw[a] << 8) | raw[a + 1]);
    const int16_t gyroRaw = static_cast<int16_t>((raw[g] << 8) | raw[g + 1]);
    acceleration[axis] = accelerationRaw * (9.80665f / 4096.0f);
    gyro[axis] = gyroRaw * (0.017453292519943295f / 65.5f);
  }
}

