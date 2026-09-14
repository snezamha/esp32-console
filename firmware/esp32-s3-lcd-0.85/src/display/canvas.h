#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// RGB565 color from 0xRRGGBB.
constexpr uint16_t Color(uint32_t hex) {
  const uint8_t r = hex >> 16, g = hex >> 8, b = hex;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Framebuffer with drawing primitives. Pixels are stored big-endian RGB565,
// the byte order SPI panels expect.
class Canvas {
 public:
  Canvas(int width, int height, uint8_t* buffer)
      : width_(width), height_(height), buf_(buffer), clip_x1_(width), clip_y1_(height) {}

  int width() const { return width_; }
  int height() const { return height_; }
  uint8_t* data() { return buf_; }

  // Restricts all drawing to a rectangle; ResetClip() restores the full canvas.
  struct Clip {
    int x0, y0, x1, y1;
  };
  void SetClip(int x, int y, int w, int h);
  void ResetClip() { SetClip(0, 0, width_, height_); }
  Clip GetClip() const { return {clip_x0_, clip_y0_, clip_x1_, clip_y1_}; }
  void RestoreClip(const Clip& clip);
  // Narrows the current clip to the intersection with a rectangle.
  void IntersectClip(int x, int y, int w, int h);

  void Fill(uint16_t color);
  void Pixel(int x, int y, uint16_t color);
  void FillRect(int x, int y, int w, int h, uint16_t color);
  void Rect(int x, int y, int w, int h, uint16_t color);
  void FillRoundRect(int x, int y, int w, int h, int r, uint16_t color);
  void FillCircle(int cx, int cy, int r, uint16_t color);
  void Ring(int cx, int cy, int r, int thickness, uint16_t color);
  // Hollow rounded rectangle, 1 px border.
  void RoundRect(int x, int y, int w, int h, int r, uint16_t color);
  // Solid triangle.
  void FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
  // Angles in degrees, 0 = right, 90 = down.
  void Arc(int cx, int cy, int r, int start_deg, int end_deg, int thickness, uint16_t color);
  void Line(int x0, int y0, int x1, int y1, int thickness, uint16_t color);

  // 5×7 font. Returns the x after the text.
  int Text(int x, int y, const char* text, uint16_t color, int scale = 1);
  static int TextWidth(const char* text, int scale = 1);
  static int LineHeight(int scale = 1) { return 9 * scale; }
  void TextCentered(int cx, int y, const char* text, uint16_t color, int scale = 1);
  // Word-wraps to `max_width`; returns the lines.
  static std::vector<std::string> Wrap(const std::string& text, int max_width, int scale = 1);

 private:
  int width_;
  int height_;
  uint8_t* buf_;
  int clip_x0_ = 0, clip_y0_ = 0, clip_x1_, clip_y1_;
};
