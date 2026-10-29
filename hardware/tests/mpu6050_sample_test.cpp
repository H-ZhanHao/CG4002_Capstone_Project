#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "Mpu6050Sample.h"

int main() {
  // +1g, -1g, zero; unrelated temperature; +100, -100, zero degrees/s.
  const uint8_t sample[14] = {
    0x10, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x12, 0x34,
    0x19, 0x96, 0xe6, 0x6a, 0x00, 0x00
  };
  float acceleration[3], gyro[3];
  decodeMpu6050Sample(sample, acceleration, gyro);
  assert(fabsf(acceleration[0] - 9.80665f) < 0.0001f);
  assert(fabsf(acceleration[1] + 9.80665f) < 0.0001f);
  assert(acceleration[2] == 0);
  assert(fabsf(gyro[0] - 1.745329252f) < 0.0001f);
  assert(fabsf(gyro[1] + 1.745329252f) < 0.0001f);
  assert(gyro[2] == 0);

  const uint8_t extremes[14] = {
    0x80, 0x00, 0x7f, 0xff, 0xff, 0xff, 0, 0,
    0x80, 0x00, 0x7f, 0xff, 0xff, 0xff
  };
  decodeMpu6050Sample(extremes, acceleration, gyro);
  assert(fabsf(acceleration[0] + 78.4532f) < 0.001f);
  assert(acceleration[1] > 78.45f && acceleration[1] < 78.454f);
  assert(acceleration[2] < 0 && acceleration[2] > -0.003f);
  assert(gyro[0] < -8.73f && gyro[0] > -8.74f);
  assert(gyro[1] > 8.73f && gyro[1] < 8.74f);
  assert(gyro[2] < 0 && gyro[2] > -0.001f);
  puts("MPU6050 units, signed samples and register offsets passed");
}
