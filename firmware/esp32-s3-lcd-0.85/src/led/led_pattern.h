#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "led_ring.h"

// The console's LED ring design: one color, level and blink flag per LED plus a ring effect.
// Pixels are stored as `rrggbb:level:blink` (level 0–100, blink 0/1), comma-separated, index 0
// first and continuing clockwise. An empty string fills every LED with `fallback` at full level.
// The web console simulator (src/lib/led-pattern.ts) renders the same algorithm.
struct LedPixel {
  RgbColor color;
  int level = 100;
  bool blink = false;
};

class LedPattern {
 public:
  // Effects: static, blink, breathe, spin, chase, rainbow.
  static bool ValidMode(const std::string& mode);
  static std::vector<LedPixel> Parse(const std::string& pixels, int count, RgbColor fallback);
  // Whether frames change over time (the ring then needs periodic redraws).
  static bool Animated(const std::string& mode, const std::vector<LedPixel>& pixels);
  // `speed` 1–10, `brightness` 1–8 (the global LED brightness setting).
  static std::vector<RgbColor> Render(const std::string& mode, const std::vector<LedPixel>& pixels,
                                      int speed, int brightness, uint32_t now_ms);
};
