#include "es7210.h"

namespace {
constexpr uint8_t REG00_RESET = 0x00;
constexpr uint8_t REG01_CLOCK_OFF = 0x01;
constexpr uint8_t REG02_MAINCLK = 0x02;
constexpr uint8_t REG04_LRCK_DIVH = 0x04;
constexpr uint8_t REG05_LRCK_DIVL = 0x05;
constexpr uint8_t REG06_POWER_DOWN = 0x06;
constexpr uint8_t REG07_OSR = 0x07;
constexpr uint8_t REG08_MODE_CONFIG = 0x08;
constexpr uint8_t REG09_TIME_CONTROL0 = 0x09;
constexpr uint8_t REG0A_TIME_CONTROL1 = 0x0A;
constexpr uint8_t REG11_SDP_INTERFACE1 = 0x11;
constexpr uint8_t REG12_SDP_INTERFACE2 = 0x12;
constexpr uint8_t REG20_ADC34_HPF2 = 0x20;
constexpr uint8_t REG21_ADC34_HPF1 = 0x21;
constexpr uint8_t REG22_ADC12_HPF1 = 0x22;
constexpr uint8_t REG23_ADC12_HPF2 = 0x23;
constexpr uint8_t REG40_ANALOG = 0x40;
constexpr uint8_t REG41_MIC12_BIAS = 0x41;
constexpr uint8_t REG42_MIC34_BIAS = 0x42;
constexpr uint8_t REG43_MIC1_GAIN = 0x43;
constexpr uint8_t REG44_MIC2_GAIN = 0x44;
constexpr uint8_t REG45_MIC3_GAIN = 0x45;
constexpr uint8_t REG46_MIC4_GAIN = 0x46;
constexpr uint8_t REG47_MIC1_POWER = 0x47;
constexpr uint8_t REG48_MIC2_POWER = 0x48;
constexpr uint8_t REG49_MIC3_POWER = 0x49;
constexpr uint8_t REG4A_MIC4_POWER = 0x4A;
constexpr uint8_t REG4B_MIC12_POWER = 0x4B;
constexpr uint8_t REG4C_MIC34_POWER = 0x4C;
}  // namespace

bool Es7210::Init(int sample_rate) {
  (void)sample_rate;  // MCLK = 256 × fs uses the same dividers for every rate

  if (!dev_.WriteReg(REG00_RESET, 0xFF)) return false;
  dev_.WriteReg(REG00_RESET, 0x41);
  dev_.WriteReg(REG01_CLOCK_OFF, 0x1F);
  dev_.WriteReg(REG09_TIME_CONTROL0, 0x30);  // Chip state cycle
  dev_.WriteReg(REG0A_TIME_CONTROL1, 0x30);  // Power-on state cycle
  dev_.WriteReg(REG23_ADC12_HPF2, 0x2A);
  dev_.WriteReg(REG22_ADC12_HPF1, 0x0A);
  dev_.WriteReg(REG20_ADC34_HPF2, 0x0A);
  dev_.WriteReg(REG21_ADC34_HPF1, 0x2A);
  dev_.UpdateReg(REG08_MODE_CONFIG, 0x01, 0x00);  // Slave
  dev_.WriteReg(REG40_ANALOG, 0xC3);
  dev_.WriteReg(REG41_MIC12_BIAS, 0x70);  // 2.87 V
  dev_.WriteReg(REG42_MIC34_BIAS, 0x70);

  // Clocks: adc_div = 1, doubler + DLL on, OSR 32, LRCK divider 256
  dev_.WriteReg(REG02_MAINCLK, 0xC1);
  dev_.WriteReg(REG07_OSR, 0x20);
  dev_.WriteReg(REG04_LRCK_DIVH, 0x01);
  dev_.WriteReg(REG05_LRCK_DIVL, 0x00);

  // I2S format, 16-bit, no TDM
  dev_.WriteReg(REG11_SDP_INTERFACE1, 0x60);
  dev_.WriteReg(REG12_SDP_INTERFACE2, 0x00);

  // Select MIC1 + MIC2
  for (uint8_t reg = REG43_MIC1_GAIN; reg <= REG46_MIC4_GAIN; reg++) dev_.UpdateReg(reg, 0x10, 0x00);
  dev_.WriteReg(REG4B_MIC12_POWER, 0xFF);
  dev_.WriteReg(REG4C_MIC34_POWER, 0xFF);
  dev_.UpdateReg(REG01_CLOCK_OFF, 0x0B, 0x00);
  dev_.WriteReg(REG4B_MIC12_POWER, 0x00);
  dev_.UpdateReg(REG43_MIC1_GAIN, 0x10, 0x10);
  dev_.UpdateReg(REG44_MIC2_GAIN, 0x10, 0x10);

  // Start
  dev_.WriteReg(REG06_POWER_DOWN, 0x00);
  dev_.WriteReg(REG40_ANALOG, 0x43);
  dev_.WriteReg(REG47_MIC1_POWER, 0x08);
  dev_.WriteReg(REG48_MIC2_POWER, 0x08);
  dev_.WriteReg(REG49_MIC3_POWER, 0x08);
  dev_.WriteReg(REG4A_MIC4_POWER, 0x08);
  dev_.WriteReg(REG00_RESET, 0x71);
  dev_.WriteReg(REG00_RESET, 0x41);

  SetGain(30);
  return true;
}

void Es7210::SetGain(float db) {
  if (db < 0) db = 0;
  if (db > 37.5f) db = 37.5f;
  const uint8_t step = static_cast<uint8_t>(db / 3.0f + 0.01f);  // 0x00 = 0 dB … 0x0E ≈ 37.5 dB
  dev_.UpdateReg(REG43_MIC1_GAIN, 0x0F, step);
  dev_.UpdateReg(REG44_MIC2_GAIN, 0x0F, step);
}
