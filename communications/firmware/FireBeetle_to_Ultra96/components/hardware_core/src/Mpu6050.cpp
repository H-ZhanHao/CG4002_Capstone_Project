#include "Mpu6050.h"
#include "Mpu6050Sample.h"
#include "ExerciseConfig.h"
#include "NodePlatform.h"

bool Mpu6050::writeRegister(uint8_t address, uint8_t value) {
  const uint8_t bytes[] = {address, value};
  return i2c_master_transmit(device_, bytes, sizeof(bytes), 50) == ESP_OK;
}

bool Mpu6050::readRegisters(uint8_t address, uint8_t* bytes, size_t size) {
  return i2c_master_transmit_receive(device_, &address, 1, bytes, size, 50) == ESP_OK;
}

bool Mpu6050::begin() {
  i2c_master_bus_config_t bus = {};
  bus.i2c_port = I2C_NUM_0;
  bus.sda_io_num = static_cast<gpio_num_t>(GeneralConfig::I2C_SDA_PIN);
  bus.scl_io_num = static_cast<gpio_num_t>(GeneralConfig::I2C_SCL_PIN);
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus, &bus_) != ESP_OK) return false;

  i2c_device_config_t device = {};
  device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device.device_address = GeneralConfig::MPU6050_ADDRESS;
  device.scl_speed_hz = 100000;
  if (i2c_master_bus_add_device(bus_, &device, &device_) != ESP_OK) return false;

  uint8_t identity = 0;
  if (!readRegisters(0x75, &identity, 1) || identity != 0x68) return false;
  if (!writeRegister(0x6b, 0x80)) return false; // Device reset.
  NodePlatform::delayMs(100);
  if (!writeRegister(0x6b, 0x01)) return false; // Wake, X-gyro PLL clock.
  NodePlatform::delayMs(10);
  // Match the former Adafruit setup: +/-8g, +/-500 deg/s, 21 Hz DLPF.
  return writeRegister(0x6c, 0x00) && writeRegister(0x19, 0x00) &&
         writeRegister(0x1a, 0x04) && writeRegister(0x1b, 0x08) &&
         writeRegister(0x1c, 0x10);
}

bool Mpu6050::read(float acceleration[3], float gyro[3]) {
  uint8_t raw[14];
  if (!readRegisters(0x3b, raw, sizeof(raw))) return false;
  decodeMpu6050Sample(raw, acceleration, gyro);
  return true;
}

