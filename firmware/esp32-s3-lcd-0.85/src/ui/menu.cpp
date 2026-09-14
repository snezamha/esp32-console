#include "menu.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>

#include "../display/canvas.h"
#include "../display/lcd_display.h"

namespace {
constexpr int kRowHeight = 12;
constexpr uint32_t kMarqueePauseMs = 900;
constexpr float kMarqueeSpeed = 22.0f;  // px per second
const std::string kEmpty;

// Exponential ease-out step; returns true while still moving.
bool Ease(float& value, float target, float k) {
  if (std::fabs(target - value) < 0.4f) {
    const bool changed = value != target;
    value = target;
    return changed;
  }
  value += (target - value) * k;
  return true;
}
}  // namespace

void MenuView::Open(const std::string& title, std::function<MenuItems()> build) {
  pages_.clear();
  editing_ = false;
  editor_t_ = 0;
  Push(title, build);
  slide_x_ = 0;  // The first page appears in place
}

void MenuView::Close() {
  if (auto page = Current()) remembered_[page->title] = page->selected;
  pages_.clear();
  editing_ = false;
  editor_t_ = 0;
  if (on_change) on_change();
}

void MenuView::Push(const std::string& title, std::function<MenuItems()> build) {
  if (auto page = Current()) remembered_[page->title] = page->selected;
  Page page{title, build, build ? build() : MenuItems{}, 0};
  const auto it = remembered_.find(title);
  if (it != remembered_.end()) {
    page.selected = std::clamp(it->second, 0, std::max(0, static_cast<int>(page.items.size()) - 1));
  }
  pages_.push_back(std::move(page));
  ResetMotion(+1);
  if (on_change) on_change();
}

void MenuView::Pop() {
  editing_ = false;
  if (auto page = Current()) remembered_[page->title] = page->selected;
  pages_.pop_back();
  if (pages_.empty()) {
    if (on_change) on_change();
    return;
  }
  Refresh();
  ResetMotion(-1);
  if (on_change) on_change();
}

void MenuView::Refresh() {
  auto page = Current();
  if (!page || !page->build) return;
  page->items = page->build();
  page->selected = std::min(page->selected, std::max(0, static_cast<int>(page->items.size()) - 1));
}

void MenuView::ResetMotion(int slide_direction) {
  auto page = Current();
  const int selected = page ? page->selected : 0;
  highlight_y_ = selected * kRowHeight;
  const float max_scroll =
      std::max(0, static_cast<int>(page ? page->items.size() : 0) * kRowHeight - content_height_);
  scroll_y_ = std::clamp(highlight_y_ - (content_height_ - kRowHeight) / 2.0f, 0.0f, max_scroll);
  // New pages come in from the right; going back comes in from the left.
  slide_x_ = slide_direction * content_width_ * 0.6f;
  selected_at_ = millis();
  last_animate_ = 0;
}

void MenuView::SetSelected(int index) {
  auto page = Current();
  if (!page) return;
  page->selected = index;
  selected_at_ = millis();
}

void MenuView::Next() {
  auto page = Current();
  if (!page || page->items.empty()) return;
  if (editing_) {
    auto& item = page->items[page->selected];
    if (item.on_adjust) item.on_adjust(+1);
    return;
  }
  SetSelected((page->selected + 1) % page->items.size());
}

void MenuView::Previous() {
  auto page = Current();
  if (!page || page->items.empty()) return;
  if (editing_) {
    auto& item = page->items[page->selected];
    if (item.on_adjust) item.on_adjust(-1);
    return;
  }
  SetSelected((page->selected + page->items.size() - 1) % page->items.size());
}

void MenuView::Select() {
  auto page = Current();
  if (!page || page->items.empty()) return;
  if (editing_) {
    editing_ = false;
    return;
  }

  // Copy: callbacks may rebuild or pop this page.
  MenuItem item = page->items[page->selected];
  switch (item.kind) {
    case MenuItem::Kind::Action:
    case MenuItem::Kind::Toggle:
    case MenuItem::Kind::Choice:
      if (item.on_select) item.on_select();
      if (!pages_.empty()) Refresh();
      break;
    case MenuItem::Kind::Submenu:
      Push(item.submenu_title.empty() ? item.label : item.submenu_title, item.build_submenu);
      break;
    case MenuItem::Kind::Adjust:
      editing_ = true;
      break;
    case MenuItem::Kind::Back:
      Pop();
      break;
    case MenuItem::Kind::Hold:  // Completed by holding Power, see App
    case MenuItem::Kind::Info:
      break;
  }
}

const std::string& MenuView::Title() const { return pages_.empty() ? kEmpty : pages_.back().title; }

const MenuItem* MenuView::SelectedItem() const {
  auto page = Current();
  if (!page || page->items.empty()) return nullptr;
  return &page->items[page->selected];
}

bool MenuView::Animate(uint32_t now_ms) {
  auto page = Current();
  if (!page) return false;
  const float dt =
      last_animate_ == 0 ? 0.016f : std::min<uint32_t>(now_ms - last_animate_, 50) / 1000.0f;
  last_animate_ = now_ms;

  const float target_highlight = page->selected * kRowHeight;
  const float max_scroll =
      std::max(0, static_cast<int>(page->items.size()) * kRowHeight - content_height_);
  const float target_scroll =
      std::clamp(target_highlight - (content_height_ - kRowHeight) / 2.0f, 0.0f, max_scroll);

  // ~95% of the distance in about 170 ms; the page slide and editor are a bit faster.
  const float k = 1.0f - std::exp(-dt * 18.0f);
  const float k_fast = 1.0f - std::exp(-dt * 22.0f);
  bool moving = false;
  moving |= Ease(highlight_y_, target_highlight, k);
  moving |= Ease(scroll_y_, target_scroll, k);
  moving |= Ease(slide_x_, 0.0f, k_fast);
  const float editor_target = editing_ ? 1.0f : 0.0f;
  if (std::fabs(editor_t_ - editor_target) < 0.02f) {
    moving |= editor_t_ != editor_target;
    editor_t_ = editor_target;
  } else {
    editor_t_ += (editor_target - editor_t_) * k_fast;
    moving = true;
  }
  return moving || marquee_active_;
}

void MenuView::Draw(Canvas& c, int x, int y, int w, int h, const Theme& theme) {
  auto page = Current();
  if (!page) return;
  content_width_ = w;
  content_height_ = h;
  marquee_active_ = false;

  const int count = static_cast<int>(page->items.size());
  const int offset_x = static_cast<int>(std::lround(slide_x_));
  const int origin = y - static_cast<int>(std::lround(scroll_y_));
  const int list_w = w - 4;  // Room for the scrollbar
  const int lx = x + offset_x;

  // Highlight glides between rows.
  const int highlight_top = origin + static_cast<int>(std::lround(highlight_y_));
  c.FillRoundRect(lx + 2, highlight_top, list_w - 2, kRowHeight, 3, theme.selected);

  for (int i = 0; i < count; i++) {
    const int row_y = origin + i * kRowHeight;
    if (row_y + kRowHeight <= y || row_y >= y + h) continue;
    DrawRow(c, page->items[i], i, lx, row_y, list_w, theme);
  }

  // Scrollbar
  const int total = count * kRowHeight;
  if (total > h) {
    const int thumb_h = std::max(8, h * h / total);
    const int thumb_y = y + static_cast<int>((h - thumb_h) * scroll_y_ / (total - h));
    c.FillRoundRect(x + w - 3, thumb_y, 2, thumb_h, 1, theme.muted);
  }

  if (editor_t_ > 0.01f && page->selected < count) {
    DrawEditor(c, page->items[page->selected], x, y, w, h, theme);
  }
}

void MenuView::DrawRow(Canvas& c, const MenuItem& item, int index, int x, int row_y, int list_w,
                       const Theme& theme) {
  const int text_y = row_y + (kRowHeight - 7) / 2;
  const auto page = Current();
  const bool selected = page && index == page->selected;

  // Rows nudge right as the highlight passes over them.
  const float closeness =
      std::max(0.0f, 1.0f - std::fabs(index * kRowHeight - highlight_y_) / kRowHeight);
  const int nudge = static_cast<int>(std::lround(closeness * 2));

  // Leading icon, status dot or back arrow
  int label_x = x + 6 + nudge;
  if (item.icon) {
    item.icon(c, x + 5 + nudge, row_y + (kRowHeight - 9) / 2, theme.text);
    label_x = x + 17 + nudge;
  } else if (item.color) {
    c.FillCircle(x + 7 + nudge, row_y + kRowHeight / 2, 2, item.color(theme));
    label_x = x + 12 + nudge;
  } else if (item.kind == MenuItem::Kind::Back) {
    c.FillTriangle(x + 5 + nudge, row_y + 6, x + 8 + nudge, row_y + 3, x + 8 + nudge, row_y + 9,
                   theme.muted);
    label_x = x + 12 + nudge;
  }

  // Trailing element; its left edge limits the label.
  const int right = x + list_w - 3;
  int trailing_left = right;
  const int level = item.level ? item.level() : -1;

  if (item.kind == MenuItem::Kind::Toggle) {
    const bool on = item.toggle && item.toggle();
    const int sw = 15, sh = 8;
    const int sx = right - sw, sy = row_y + (kRowHeight - sh) / 2;
    c.FillRoundRect(sx, sy, sw, sh, sh / 2, on ? theme.ok : theme.muted);
    c.FillCircle(on ? sx + sw - 4 : sx + 3, sy + sh / 2, 2, Color(0xFFFFFF));
    trailing_left = sx;
  } else if (level >= 0) {
    const int bar_w = 38;
    c.Rect(right - bar_w, row_y + 3, bar_w, kRowHeight - 6, theme.muted);
    c.FillRect(right - bar_w + 1, row_y + 4, (bar_w - 2) * std::min(level, 100) / 100,
               kRowHeight - 8, item.color ? item.color(theme) : theme.info);
    trailing_left = right - bar_w;
  } else if (item.value) {
    const std::string value = item.value().substr(0, 9);
    const uint16_t color = item.color ? item.color(theme) : theme.muted;
    int vx = right - Canvas::TextWidth(value.c_str());
    if (item.kind == MenuItem::Kind::Choice) {
      // ◂ value ▸ : Power cycles the value.
      const int arrows_color = selected ? theme.text : theme.muted;
      c.FillTriangle(right, row_y + 6, right - 2, row_y + 4, right - 2, row_y + 8, arrows_color);
      vx -= 5;
      c.FillTriangle(vx - 5, row_y + 6, vx - 3, row_y + 4, vx - 3, row_y + 8, arrows_color);
      c.Text(vx, text_y, value.c_str(), color);
      trailing_left = vx - 6;
    } else {
      c.Text(vx, text_y, value.c_str(), color);
      trailing_left = vx;
    }
  } else if (item.kind == MenuItem::Kind::Submenu) {
    c.FillTriangle(right, row_y + 6, right - 3, row_y + 3, right - 3, row_y + 9, theme.muted);
    trailing_left = right - 3;
  }

  // Label, scrolling when it does not fit and the row is selected.
  const int label_room = trailing_left - 3 - label_x;
  const int label_w = Canvas::TextWidth(item.label.c_str());
  const uint16_t label_color = item.kind == MenuItem::Kind::Hold ? theme.fail : theme.text;
  if (label_w <= label_room || label_room <= 0) {
    c.Text(label_x, text_y, item.label.c_str(), label_color);
    return;
  }

  int shift = 0;
  if (selected && !editing_) {
    const uint32_t elapsed = millis() - selected_at_;
    const int travel = label_w - label_room + 6;
    if (elapsed > kMarqueePauseMs) {
      // Scroll to the end, pause, then jump back.
      const float t = (elapsed - kMarqueePauseMs) / 1000.0f * kMarqueeSpeed;
      const float cycle = travel + kMarqueeSpeed * 1.2f;
      const float pos = std::fmod(t, cycle);
      shift = static_cast<int>(std::min(pos, static_cast<float>(travel)));
    }
    marquee_active_ = true;
  }
  const Canvas::Clip saved = c.GetClip();
  c.IntersectClip(label_x, row_y, label_room, kRowHeight);
  c.Text(label_x - shift, text_y, item.label.c_str(), label_color);
  c.RestoreClip(saved);
}

void MenuView::DrawEditor(Canvas& c, const MenuItem& item, int x, int y, int w, int h,
                          const Theme& theme) {
  // Card grows from the center.
  const float t = std::clamp(editor_t_, 0.0f, 1.0f);
  const int full_w = w - 12, full_h = 70;
  const int cw = static_cast<int>(full_w * (0.6f + 0.4f * t));
  const int ch = static_cast<int>(full_h * t);
  const int cx = x + (w - cw) / 2;
  const int cy = y + (h - ch) / 2;
  if (ch < 6) return;

  c.FillRoundRect(cx, cy, cw, ch, 6, theme.background);
  c.RoundRect(cx, cy, cw, ch, 6, theme.info);
  if (t < 0.7f) return;

  const Canvas::Clip saved = c.GetClip();
  c.IntersectClip(cx + 1, cy + 1, cw - 2, ch - 2);

  const int mid = cx + cw / 2;
  c.TextCentered(mid, cy + 6, item.label.c_str(), theme.muted);

  const std::string value = item.value ? item.value() : "";
  c.TextCentered(mid, cy + 20, value.c_str(), theme.text, 2);

  // − and + hints at the sides of the value
  const int hint_y = cy + 26;
  c.FillRect(cx + 8, hint_y, 7, 2, theme.info);
  c.FillRect(cx + cw - 15, hint_y, 7, 2, theme.info);
  c.FillRect(cx + cw - 12, hint_y - 3, 2, 8, theme.info);

  if (item.progress) {
    const int p = std::clamp(item.progress(), 0, 100);
    const int bx = cx + 10, bw = cw - 20, by = cy + 42;
    c.FillRoundRect(bx, by, bw, 6, 3, theme.selected);
    if (p > 0) c.FillRoundRect(bx, by, std::max(6, bw * p / 100), 6, 3, theme.info);
  }
  c.TextCentered(mid, cy + ch - 12, "OK: Power", theme.muted);

  c.RestoreClip(saved);
}
