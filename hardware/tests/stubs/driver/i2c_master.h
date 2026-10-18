#pragma once
#include <stddef.h>
#include <stdint.h>
// Host tests replace Mpu6050's methods; no ESP-IDF driver is linked.
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
