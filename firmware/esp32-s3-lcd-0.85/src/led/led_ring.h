#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

#include <mutex>
#include <vector>

struct RgbColor {
  uint8_t red = 0, green = 0, blue = 0;
};

// WS2812 ring, GRB wire format, driven through RMT at 10 MHz.
class LedRing {
 public:
  LedRing(gpio_num_t gpio, uint16_t count);

  int count() const { return count_; }
  void SetPixel(int index, RgbColor color);
  void SetAll(RgbColor color);
  void Clear() { SetAll({}); }
  // Sends the pixel buffer to the ring.
  void Show();

 private:
  gpio_num_t gpio_;
  int count_;
  std::mutex mutex_;
  std::vector<RgbColor> pixels_;
  std::vector<rmt_data_t> symbols_;
};
