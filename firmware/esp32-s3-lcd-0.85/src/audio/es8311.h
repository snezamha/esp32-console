#pragma once

#include "i2c_device.h"

// ES8311 mono codec, used here as the speaker DAC. I2S slave, MCLK = 256 × fs, 16-bit.
class Es8311 {
 public:
  Es8311(i2c_master_bus_handle_t bus, uint8_t address) : dev_(bus, address) {}

  bool Init(int sample_rate);
  // 0–100
  void SetVolume(int volume);
  void SetMute(bool mute);

 private:
  I2cDevice dev_;
};
