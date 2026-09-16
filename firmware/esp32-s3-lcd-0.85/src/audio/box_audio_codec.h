#pragma once

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <cstdint>

#include "es7210.h"
#include "es8311.h"

// ES8311 speaker + ES7210 microphones sharing one full-duplex I2S port.
// Audio is 16-bit stereo, interleaved L/R.
class BoxAudioCodec {
 public:
  static constexpr int kDefaultVolume = 50;

  BoxAudioCodec(i2c_master_bus_handle_t i2c_bus, int sample_rate, gpio_num_t mclk, gpio_num_t bclk,
                gpio_num_t ws, gpio_num_t dout, gpio_num_t din, gpio_num_t pa_pin,
                uint8_t es8311_addr, uint8_t es7210_addr);

  // Probes both codecs, configures them and starts I2S.
  bool Start();

  bool es8311_found() const { return es8311_found_; }
  bool es7210_found() const { return es7210_found_; }
  bool started() const { return started_; }
  int sample_rate() const { return sample_rate_; }
  int default_sample_rate() const { return default_sample_rate_; }
  bool SetSampleRate(int sample_rate);

  int output_volume() const { return output_volume_; }
  void SetOutputVolume(int volume);
  void SetInputGain(float db);
  // Speaker amplifier on/off.
  void EnableOutput(bool enable);

  // Blocking; `frames` stereo frames. Return frames written / read.
  int Write(const int16_t* samples, int frames, int timeout_ms = 1000);
  int Read(int16_t* samples, int frames, int timeout_ms = 1000);

 private:
  i2c_master_bus_handle_t i2c_bus_;
  int sample_rate_;
  int default_sample_rate_;
  gpio_num_t mclk_, bclk_, ws_, dout_, din_, pa_pin_;
  uint8_t es8311_addr_, es7210_addr_;

  Es8311* es8311_ = nullptr;
  Es7210* es7210_ = nullptr;
  i2s_chan_handle_t tx_ = nullptr;
  i2s_chan_handle_t rx_ = nullptr;

  bool es8311_found_ = false;
  bool es7210_found_ = false;
  bool started_ = false;
  int output_volume_ = kDefaultVolume;
};
