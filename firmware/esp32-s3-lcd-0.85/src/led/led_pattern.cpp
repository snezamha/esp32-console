#include "led_pattern.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
constexpr const char* kModes[] = {"static", "blink", "breathe", "spin", "chase", "rainbow"};

int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

RgbColor Hue(float hue) {
  hue -= std::floor(hue);
  const float h = hue * 6.0f;
  const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(h) % 6) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
  }
  return {static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255)};
}
}  // namespace

bool LedPattern::ValidMode(const std::string& mode) {
  return std::any_of(std::begin(kModes), std::end(kModes), [&](const char* m) { return mode == m; });
}

std::vector<LedPixel> LedPattern::Parse(const std::string& pixels, int count, RgbColor fallback) {
  std::vector<LedPixel> out(count, LedPixel{fallback, 100, false});
  size_t start = 0;
  for (int i = 0; i < count && start < pixels.size(); i++) {
    size_t end = pixels.find(',', start);
    if (end == std::string::npos) end = pixels.size();
    const std::string item = pixels.substr(start, end - start);
    start = end + 1;
    const size_t first = item.find(':'), second = item.find(':', first + 1);
    if (first != 6 || second == std::string::npos) continue;
    int rgb[6];
    bool valid = true;
    for (int k = 0; k < 6; k++) valid &= (rgb[k] = Hex(item[k])) >= 0;
    if (!valid) continue;
    out[i].color = {static_cast<uint8_t>(rgb[0] << 4 | rgb[1]), static_cast<uint8_t>(rgb[2] << 4 | rgb[3]),
                    static_cast<uint8_t>(rgb[4] << 4 | rgb[5])};
    out[i].level = std::clamp(atoi(item.c_str() + first + 1), 0, 100);
    out[i].blink = item.substr(second + 1) == "1";
  }
  return out;
}

bool LedPattern::Animated(const std::string& mode, const std::vector<LedPixel>& pixels) {
  return mode != "static" || std::any_of(pixels.begin(), pixels.end(), [](const LedPixel& p) { return p.blink && p.level > 0; });
}

std::vector<RgbColor> LedPattern::Render(const std::string& mode, const std::vector<LedPixel>& pixels,
                                         int speed, int brightness, uint32_t now_ms) {
  const int count = static_cast<int>(pixels.size());
  std::vector<RgbColor> frame(count);
  if (!count) return frame;
  // Speed 1 = 2 s per cycle, 10 = 0.2 s.
  const uint32_t period = 2200 - std::clamp(speed, 1, 10) * 200;
  const float phase = static_cast<float>(now_ms % period) / period;
  const bool blink_off = phase >= 0.5f;
  const int global = (1 << std::clamp(brightness, 1, 8)) - 1;  // 1 … 255, perceptual steps
  const float chase_position = static_cast<float>(now_ms % (period * count)) / period;
  const int spin_offset = static_cast<int>(now_ms / std::max<uint32_t>(1, period / count)) % count;

  for (int i = 0; i < count; i++) {
    const LedPixel& source = mode == "spin" ? pixels[(i + count - spin_offset) % count] : pixels[i];
    RgbColor color = source.color;
    float level = source.level / 100.0f;
    if (source.blink && blink_off && mode != "blink") level = 0;
    if (mode == "blink" && blink_off) level = 0;
    if (mode == "breathe") level *= 0.5f - 0.5f * std::cos(phase * 2.0f * static_cast<float>(M_PI));
    if (mode == "chase") {
      float distance = std::fabs(i - chase_position);
      distance = std::min(distance, count - distance);
      level *= std::max(0.0f, 1.0f - distance / 1.5f);
    }
    if (mode == "rainbow") color = Hue(static_cast<float>(i) / count + phase);
    const float scale = level * global / 255.0f;
    frame[i] = {static_cast<uint8_t>(color.red * scale), static_cast<uint8_t>(color.green * scale),
                static_cast<uint8_t>(color.blue * scale)};
  }
  return frame;
}
