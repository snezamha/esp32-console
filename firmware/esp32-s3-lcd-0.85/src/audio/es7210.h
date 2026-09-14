#pragma once

#include "i2c_device.h"

// ES7210 4-channel microphone ADC. I2S slave, MCLK = 256 × fs, 16-bit.
// Outputs MIC1 on the left and MIC2 on the right slot.
class Es7210 {
 public:
  Es7210(i2c_master_bus_handle_t bus, uint8_t address) : dev_(bus, address) {}

  bool Init(int sample_rate);
  // 0–37.5 dB in 3 dB steps (rounded down)
  void SetGain(float db);

 private:
  I2cDevice dev_;
};
