#include "led_ring.h"

#include <esp_log.h>

#define TAG "LedRing"

LedRing::LedRing(gpio_num_t gpio, uint16_t count)
    : gpio_(gpio), count_(count), pixels_(count), symbols_(count * 24) {
  if (!rmtInit(gpio_, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_2, 10 * 1000 * 1000)) {
    ESP_LOGE(TAG, "RMT init failed on GPIO %d", gpio_);
  }
  Show();
}

void LedRing::SetPixel(int index, RgbColor color) {
  if (index < 0 || index >= count_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pixels_[index] = color;
}

void LedRing::SetAll(RgbColor color) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& p : pixels_) p = color;
}

void LedRing::Show() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t i = 0;
  for (const auto& p : pixels_) {
    const uint8_t bytes[3] = {p.green, p.red, p.blue};
    for (uint8_t byte : bytes) {
      for (int bit = 7; bit >= 0; bit--) {
        const bool one = byte & (1 << bit);
        symbols_[i].level0 = 1;
        symbols_[i].duration0 = one ? 8 : 4;  // 800 / 400 ns high
        symbols_[i].level1 = 0;
        symbols_[i].duration1 = one ? 4 : 8;
        i++;
      }
    }
  }
  rmtWrite(gpio_, symbols_.data(), i, RMT_WAIT_FOR_EVER);
}
