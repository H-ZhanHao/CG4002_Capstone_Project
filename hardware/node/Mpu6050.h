#pragma once

#include "driver/i2c_master.h"

class Mpu6050 {
 public:
  bool begin();
  bool read(float acceleration[3], float gyro[3]);

 private:
  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t device_ = nullptr;
  bool writeRegister(uint8_t address, uint8_t value);
  bool readRegisters(uint8_t address, uint8_t* bytes, size_t size);
};
