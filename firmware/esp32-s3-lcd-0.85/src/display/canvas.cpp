#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "font5x7.h"

void Canvas::SetClip(int x, int y, int w, int h) {
  clip_x0_ = std::max(x, 0);
  clip_y0_ = std::max(y, 0);
  clip_x1_ = std::min(x + w, width_);
  clip_y1_ = std::min(y + h, height_);
}

void Canvas::RestoreClip(const Clip& clip) {
  clip_x0_ = clip.x0;
  clip_y0_ = clip.y0;
  clip_x1_ = clip.x1;
  clip_y1_ = clip.y1;
}

void Canvas::IntersectClip(int x, int y, int w, int h) {
  clip_x0_ = std::max(clip_x0_, x);
  clip_y0_ = std::max(clip_y0_, y);
  clip_x1_ = std::min(clip_x1_, x + w);
  clip_y1_ = std::min(clip_y1_, y + h);
}

void Canvas::Fill(uint16_t color) { FillRect(0, 0, width_, height_, color); }

void Canvas::Pixel(int x, int y, uint16_t color) {
  if (x < clip_x0_ || y < clip_y0_ || x >= clip_x1_ || y >= clip_y1_) return;
  const size_t i = (static_cast<size_t>(y) * width_ + x) * 2;
  buf_[i] = color >> 8;
  buf_[i + 1] = color & 0xFF;
}

void Canvas::FillRect(int x, int y, int w, int h, uint16_t color) {
  const int x0 = std::max(x, clip_x0_), x1 = std::min(x + w, clip_x1_);
  const int y0 = std::max(y, clip_y0_), y1 = std::min(y + h, clip_y1_);
  for (int yy = y0; yy < y1; yy++) {
    uint8_t* p = buf_ + (static_cast<size_t>(yy) * width_ + x0) * 2;
    for (int xx = x0; xx < x1; xx++) {
      *p++ = color >> 8;
      *p++ = color & 0xFF;
    }
  }
}

void Canvas::BlitRow(int x, int y, int w, const uint8_t* pixels) {
  if (y < clip_y0_ || y >= clip_y1_) return;
  const int x0 = std::max(x, clip_x0_), x1 = std::min(x + w, clip_x1_);
  if (x0 >= x1) return;
  memcpy(buf_ + (static_cast<size_t>(y) * width_ + x0) * 2, pixels + (x0 - x) * 2,
         static_cast<size_t>(x1 - x0) * 2);
}

void Canvas::Rect(int x, int y, int w, int h, uint16_t color) {
  FillRect(x, y, w, 1, color);
  FillRect(x, y + h - 1, w, 1, color);
  FillRect(x, y, 1, h, color);
  FillRect(x + w - 1, y, 1, h, color);
}

void Canvas::FillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  r = std::min({r, w / 2, h / 2});
  FillRect(x, y + r, w, h - 2 * r, color);
  for (int dy = 0; dy < r; dy++) {
    const int yy = r - dy;
    const int dx = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - yy * yy)) + 0.5f);
    FillRect(x + dx, y + dy, w - 2 * dx, 1, color);
    FillRect(x + dx, y + h - 1 - dy, w - 2 * dx, 1, color);
  }
}

void Canvas::RoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  r = std::min({r, w / 2, h / 2});
  FillRect(x + r, y, w - 2 * r, 1, color);
  FillRect(x + r, y + h - 1, w - 2 * r, 1, color);
  FillRect(x, y + r, 1, h - 2 * r, color);
  FillRect(x + w - 1, y + r, 1, h - 2 * r, color);
  if (r > 0) {
    Arc(x + r, y + r, r, 180, 270, 1, color);
    Arc(x + w - 1 - r, y + r, r, 270, 360, 1, color);
    Arc(x + r, y + h - 1 - r, r, 90, 180, 1, color);
    Arc(x + w - 1 - r, y + h - 1 - r, r, 0, 90, 1, color);
  }
}

void Canvas::FillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  const int min_y = std::min({y0, y1, y2}), max_y = std::max({y0, y1, y2});
  for (int y = min_y; y <= max_y; y++) {
    float left = 1e9f, right = -1e9f;
    const int pts[3][2] = {{x0, y0}, {x1, y1}, {x2, y2}};
    for (int i = 0; i < 3; i++) {
      const auto* a = pts[i];
      const auto* b = pts[(i + 1) % 3];
      if ((y < a[1] && y < b[1]) || (y > a[1] && y > b[1])) continue;
      if (a[1] == b[1]) {
        left = std::min({left, static_cast<float>(a[0]), static_cast<float>(b[0])});
        right = std::max({right, static_cast<float>(a[0]), static_cast<float>(b[0])});
      } else {
        const float x = a[0] + (b[0] - a[0]) * static_cast<float>(y - a[1]) / (b[1] - a[1]);
        left = std::min(left, x);
        right = std::max(right, x);
      }
    }
    if (left <= right) {
      const int l = static_cast<int>(std::lround(left));
      FillRect(l, y, static_cast<int>(std::lround(right)) - l + 1, 1, color);
    }
  }
}

void Canvas::FillCircle(int cx, int cy, int r, uint16_t color) {
  for (int dy = -r; dy <= r; dy++) {
    const int dx = static_cast<int>(std::sqrt(static_cast<float>(r * r - dy * dy)) + 0.5f);
    FillRect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
  }
}

void Canvas::Ring(int cx, int cy, int r, int thickness, uint16_t color) {
  Arc(cx, cy, r, 0, 360, thickness, color);
}

void Canvas::Arc(int cx, int cy, int r, int start_deg, int end_deg, int thickness, uint16_t color) {
  const float r_out = r + thickness / 2.0f;
  const float r_in = r - thickness / 2.0f;
  const int extent = static_cast<int>(std::ceil(r_out));
  for (int dy = -extent; dy <= extent; dy++) {
    for (int dx = -extent; dx <= extent; dx++) {
      const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      if (d < r_in || d > r_out) continue;
      float a = std::atan2(static_cast<float>(dy), static_cast<float>(dx)) * 180.0f / M_PI;
      if (a < 0) a += 360.0f;
      const bool inside = start_deg <= end_deg ? (a >= start_deg && a <= end_deg)
                                               : (a >= start_deg || a <= end_deg);
      if (inside || end_deg - start_deg >= 360) Pixel(cx + dx, cy + dy, color);
    }
  }
}

void Canvas::Line(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
  const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
  const int half = thickness / 2;
  for (int i = 0; i <= steps; i++) {
    const float t = steps ? static_cast<float>(i) / steps : 0.0f;
    const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t));
    const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
    FillRect(x - half, y - half, thickness, thickness, color);
  }
}

int Canvas::Text(int x, int y, const char* text, uint16_t color, int scale) {
  for (; *text; text++) {
    char c = *text;
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = kFont5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
      for (int row = 0; row < 7; row++) {
        if (glyph[col] & (1 << row)) FillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
    x += 6 * scale;
  }
  return x;
}

int Canvas::TextWidth(const char* text, int scale) {
  const int len = static_cast<int>(strlen(text));
  return len ? (len * 6 - 1) * scale : 0;
}

void Canvas::BeginFrame(uint32_t now_ms) {
  frame_time_ = now_ms;
  ++frame_;
  marquee_active_ = false;
}

void Canvas::EndFrame() {
  for (auto it = marquees_.begin(); it != marquees_.end();) {
    if (it->second.frame != frame_) it = marquees_.erase(it);
    else ++it;
  }
}

void Canvas::TextMarquee(int x, int y, int width, const char* text, uint16_t color,
                         int scale, bool centered) {
  if (width <= 0 || scale <= 0 || !text || !*text) return;
  const Clip saved = GetClip();
  IntersectClip(x, y, width, 7 * scale);
  if (clip_x1_ <= clip_x0_ || clip_y1_ <= clip_y0_) {
    RestoreClip(saved);
    return;
  }
  const int text_width = TextWidth(text, scale);
  if (text_width <= width) {
    Text(centered ? x + (width - text_width) / 2 : x, y, text, color, scale);
  } else {
    auto& state = marquees_[std::make_tuple(x, y, width, scale)];
    if (state.text != text || state.frame + 1 < frame_) {
      state.text = text;
      state.started_at = frame_time_;
    }
    state.frame = frame_;
    // Pause at each end so both the beginning and the final characters are readable.
    constexpr uint32_t pause_ms = 1200;
    constexpr uint32_t pixels_per_second = 24;
    const int travel = text_width - width;
    const uint32_t move_ms = (static_cast<uint32_t>(travel) * 1000 + pixels_per_second - 1) / pixels_per_second;
    const uint32_t phase = (frame_time_ - state.started_at) % (pause_ms * 2 + move_ms);
    const int shift = phase <= pause_ms ? 0 :
        std::min(travel, static_cast<int>((phase - pause_ms) * pixels_per_second / 1000));
    Text(x - shift, y, text, color, scale);
    marquee_active_ = true;
  }
  RestoreClip(saved);
}

void Canvas::TextCentered(int cx, int y, const char* text, uint16_t color, int scale) {
  const int half = std::min(cx - clip_x0_, clip_x1_ - cx);
  TextMarquee(cx - half, y, half * 2, text, color, scale, true);
}

std::vector<std::string> Canvas::Wrap(const std::string& text, int max_width, int scale) {
  const size_t max_chars = std::max(1, (max_width / scale + 1) / 6);
  std::vector<std::string> lines;
  std::string line;
  size_t pos = 0;

  while (pos < text.size()) {
    size_t end = text.find_first_of(" \n", pos);
    if (end == std::string::npos) end = text.size();
    std::string word = text.substr(pos, end - pos);

    while (word.size() > max_chars) {  // Hard-split long words
      if (!line.empty()) {
        lines.push_back(line);
        line.clear();
      }
      lines.push_back(word.substr(0, max_chars));
      word.erase(0, max_chars);
    }
    if (line.empty()) {
      line = word;
    } else if (line.size() + 1 + word.size() <= max_chars) {
      line += " " + word;
    } else {
      lines.push_back(line);
      line = word;
    }
    if (end < text.size() && text[end] == '\n') {
      lines.push_back(line);
      line.clear();
    }
    pos = end + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}
