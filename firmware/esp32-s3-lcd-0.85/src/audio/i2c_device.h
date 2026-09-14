#pragma once

#include <driver/i2c_master.h>

// 8-bit register access to a device on an i2c_master bus.
class I2cDevice {
 public:
  I2cDevice(i2c_master_bus_handle_t bus, uint8_t address) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = address;
    cfg.scl_speed_hz = 100000;
    ok_ = i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
  }
  ~I2cDevice() {
    if (dev_) i2c_master_bus_rm_device(dev_);
  }

  bool WriteReg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return ok_ && i2c_master_transmit(dev_, buf, sizeof(buf), 100) == ESP_OK;
  }

  uint8_t ReadReg(uint8_t reg) {
    uint8_t value = 0;
    if (ok_) i2c_master_transmit_receive(dev_, &reg, 1, &value, 1, 100);
    return value;
  }

  bool UpdateReg(uint8_t reg, uint8_t mask, uint8_t value) {
    return WriteReg(reg, (ReadReg(reg) & ~mask) | (value & mask));
  }

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  bool ok_ = false;
};
