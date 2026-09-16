#include "es8311.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr uint8_t REG00_RESET = 0x00;
constexpr uint8_t REG01_CLK_MANAGER = 0x01;
constexpr uint8_t REG02_CLK_MANAGER = 0x02;
constexpr uint8_t REG03_CLK_MANAGER = 0x03;
constexpr uint8_t REG04_CLK_MANAGER = 0x04;
constexpr uint8_t REG05_CLK_MANAGER = 0x05;
constexpr uint8_t REG06_CLK_MANAGER = 0x06;
constexpr uint8_t REG07_CLK_MANAGER = 0x07;
constexpr uint8_t REG08_CLK_MANAGER = 0x08;
constexpr uint8_t REG09_SDPIN = 0x09;
constexpr uint8_t REG0A_SDPOUT = 0x0A;
constexpr uint8_t REG0D_SYSTEM = 0x0D;
constexpr uint8_t REG0E_SYSTEM = 0x0E;
constexpr uint8_t REG12_SYSTEM = 0x12;
constexpr uint8_t REG13_SYSTEM = 0x13;
constexpr uint8_t REG1C_ADC = 0x1C;
constexpr uint8_t REG31_DAC = 0x31;
constexpr uint8_t REG32_DAC = 0x32;
constexpr uint8_t REG37_DAC = 0x37;
}  // namespace

bool Es8311::Init(int sample_rate) {
  (void)sample_rate;  // MCLK = 256 × fs uses the same dividers for every rate

  if (!dev_.WriteReg(REG00_RESET, 0x1F)) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  dev_.WriteReg(REG00_RESET, 0x00);
  dev_.WriteReg(REG00_RESET, 0x80);  // Power on, slave mode

  // Clocks: all on, MCLK from pin
  dev_.WriteReg(REG01_CLK_MANAGER, 0x3F);
  dev_.UpdateReg(REG06_CLK_MANAGER, 0x20, 0x00);  // SCLK not inverted
  // pre_div = 1, pre_multi = 1×
  dev_.UpdateReg(REG02_CLK_MANAGER, 0xF8, 0x00);
  // adc_div = 1, dac_div = 1
  dev_.WriteReg(REG05_CLK_MANAGER, 0x00);
  // fs_mode = single speed, adc_osr = 0x10
  dev_.UpdateReg(REG03_CLK_MANAGER, 0x7F, 0x10);
  // dac_osr = 0x10
  dev_.UpdateReg(REG04_CLK_MANAGER, 0x7F, 0x10);
  // LRCK divider = 256
  dev_.UpdateReg(REG07_CLK_MANAGER, 0x3F, 0x00);
  dev_.WriteReg(REG08_CLK_MANAGER, 0xFF);
  // BCLK divider = 4
  dev_.UpdateReg(REG06_CLK_MANAGER, 0x1F, 0x03);

  // I2S format, 16-bit in and out
  dev_.WriteReg(REG09_SDPIN, 0x0C);
  dev_.WriteReg(REG0A_SDPOUT, 0x0C);

  dev_.WriteReg(REG0D_SYSTEM, 0x01);  // Power up analog circuitry
  dev_.WriteReg(REG0E_SYSTEM, 0x02);  // Enable analog PGA and ADC modulator
  dev_.WriteReg(REG12_SYSTEM, 0x00);  // Power up DAC
  dev_.WriteReg(REG13_SYSTEM, 0x10);  // Enable output to HP drive
  dev_.WriteReg(REG1C_ADC, 0x6A);     // ADC equalizer bypass, cancel DC offset
  dev_.WriteReg(REG37_DAC, 0x08);     // Bypass DAC equalizer

  SetMute(false);
  return true;
}

void Es8311::SetVolume(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  // 0xBF is 0 dB. The previous 100% value (0xFF) applied +32 dB digital gain,
  // overdriving the small speaker amplifier and, on battery, its supply.
  // Keep the former 50% level close to its old value while capping at unity gain.
  dev_.WriteReg(REG32_DAC, volume == 0 ? 0 : 63 + volume * 128 / 100);
}

void Es8311::SetMute(bool mute) { dev_.UpdateReg(REG31_DAC, 0x60, mute ? 0x60 : 0x00); }
