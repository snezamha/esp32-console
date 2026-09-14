#include "lcd_display.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>

#include "../common/settings.h"
#include "../lang.h"

#define TAG "LcdDisplay"

namespace {

const Theme kLightTheme = {
    .name = "light",
    .background = Color(0xFFFFFF),
    .text = Color(0x000000),
    .content_background = Color(0xE0E0E0),
    .muted = Color(0x707070),
    .selected = Color(0xC8C8D0),
    .low_battery = Color(0x000000),
    .ok = Color(0x138A3E),
    .fail = Color(0xC62828),
    .running = Color(0xB06E00),
    .info = Color(0x2F5BC8),
};

const Theme kDarkTheme = {
    .name = "dark",
    .background = Color(0x000000),
    .text = Color(0xFFFFFF),
    .content_background = Color(0x1F1F1F),
    .muted = Color(0x8A8A8A),
    .selected = Color(0x34343F),
    .low_battery = Color(0xFF0000),
    .ok = Color(0x3CC878),
    .fail = Color(0xF05050),
    .running = Color(0xF0C040),
    .info = Color(0x80A0FF),
};

const Theme* FindTheme(const std::string& name) {
  return name == "dark" ? &kDarkTheme : &kLightTheme;
}

constexpr int kTopBarHeight = 16;
constexpr int kSpacing = 2;

}  // namespace

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height, int offset_x, int offset_y)
    : panel_io_(panel_io), panel_(panel), width_(width), height_(height) {
  Settings settings("display");
  theme_ = FindTheme(settings.GetString("theme", "dark"));

  if (offset_x != 0 || offset_y != 0) {
    esp_lcd_panel_set_gap(panel_, offset_x, offset_y);
  }

  buffer_ = static_cast<uint8_t*>(
      heap_caps_malloc(width_ * height_ * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  assert(buffer_ != nullptr);
  canvas_ = new Canvas(width_, height_, buffer_);
  rotated_buffer_ = static_cast<uint8_t*>(
      heap_caps_malloc(width_ * height_ * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

  flush_done_ = xSemaphoreCreateBinary();
  const esp_lcd_panel_io_callbacks_t callbacks = {
      .on_color_trans_done =
          [](esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) -> bool {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woken);
        return woken == pdTRUE;
      },
  };
  esp_lcd_panel_io_register_event_callbacks(panel_io_, &callbacks, flush_done_);

  // Paint the background before turning the panel on to avoid showing garbage.
  canvas_->Fill(theme_->background);
  Flush();
  esp_lcd_panel_disp_on_off(panel_, true);
}

LcdDisplay::~LcdDisplay() {
  delete canvas_;
  heap_caps_free(buffer_);
  heap_caps_free(rotated_buffer_);
  vSemaphoreDelete(flush_done_);
}

void LcdDisplay::SetupUI() {
  if (ui_ready_) return;
  ui_ready_ = true;
  status_ = Lang::Strings::INITIALIZING;
  dirty_ = true;
}

void LcdDisplay::SetStatus(const char* status) {
  if (status_ == status) return;
  status_ = status;
  dirty_ = true;
}

void LcdDisplay::ShowNotification(const char* notification, int duration_ms) {
  notification_ = notification;
  // Long messages need enough time for one complete pass through the narrow status bar.
  const int travel = std::max(0, Canvas::TextWidth(notification) - 16);
  const int readable_ms = travel ? 2400 + (travel * 1000 + 23) / 24 : 0;
  notification_until_ = millis() + std::max(duration_ms, readable_ms);
  dirty_ = true;
}

void LcdDisplay::SetTheme(const char* theme_name) {
  theme_ = FindTheme(theme_name);
  Settings settings("display", true);
  settings.SetString("theme", theme_->name);
  dirty_ = true;
}

void LcdDisplay::SetRotate180(bool rotate) {
  if (rotate180_ == rotate) return;
  rotate180_ = rotate;
  dirty_ = true;
}

void LcdDisplay::SetShowBatteryPercent(bool show) {
  if (show_battery_percent_ == show) return;
  show_battery_percent_ = show;
  dirty_ = true;
}

void LcdDisplay::UpdateStatusBar(bool update_all) {
  if (connectivity_provider_) {
    const ConnectivityStatus status = connectivity_provider_();
    if (status.wifi_on != connectivity_.wifi_on || status.wifi_bars != connectivity_.wifi_bars ||
        status.ble_on != connectivity_.ble_on) {
      connectivity_ = status;
      dirty_ = true;
    }
  }

  if (volume_provider_) {
    const bool muted = volume_provider_() == 0;
    if (muted != muted_) {
      muted_ = muted;
      dirty_ = true;
    }
  }

  if (!battery_provider_) return;
  const BatteryStatus battery = battery_provider_();
  if (!battery.available) return;
  if (battery.level != battery_level_) {
    battery_level_ = battery.level;
    dirty_ = true;
  }

  // Eight icon steps: 0 = empty … 7 = full; 8 = charging.
  int icon = 8;
  if (!battery.charging) {
    icon = battery.level <= 0 ? 0 : (battery.level >= 100 ? 7 : 1 + ((battery.level - 1) * 6 / 99));
  }
  if (icon != battery_icon_level_) {
    battery_icon_level_ = icon;
    dirty_ = true;
  }

  // Low battery popup is checked on clock ticks only; readings are not ready at boot.
  if (!update_all) {
    const bool show = icon == 0 && battery.discharging;
    if (show != low_battery_popup_) {
      low_battery_popup_ = show;
      dirty_ = true;
    }
  }
}

void LcdDisplay::Loop() {
  if (canvas_ && canvas_->HasMarquee() && millis() - last_marquee_frame_ >= 33) {
    last_marquee_frame_ = millis();
    dirty_ = true;
  }
  // Charging animation in the battery icon.
  if (battery_icon_level_ == 8 && millis() - last_charge_frame_ >= 350) {
    last_charge_frame_ = millis();
    dirty_ = true;
  }
  if (!notification_.empty() && millis() >= notification_until_) {
    notification_.clear();
    dirty_ = true;
  }
  if (dirty_ && ui_ready_) {
    dirty_ = false;
    Render();
    Flush();
  }
}

void LcdDisplay::Flush() {
  uint8_t* frame = buffer_;
  if (rotate180_ && rotated_buffer_) {
    // 180° rotation: reverse the pixel order, keeping each pixel's byte order.
    const size_t pixels = static_cast<size_t>(width_) * height_;
    for (size_t i = 0; i < pixels; i++) {
      const size_t from = (pixels - 1 - i) * 2;
      rotated_buffer_[i * 2] = buffer_[from];
      rotated_buffer_[i * 2 + 1] = buffer_[from + 1];
    }
    frame = rotated_buffer_;
  }
  xSemaphoreTake(flush_done_, 0);
  if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, width_, height_, frame) == ESP_OK) {
    xSemaphoreTake(flush_done_, pdMS_TO_TICKS(200));
  }
}

void LcdDisplay::Render() {
  auto& c = *canvas_;
  c.BeginFrame(millis());
  c.Fill(theme_->background);
  c.FillRect(0, kTopBarHeight, width_, height_ - kTopBarHeight, theme_->content_background);

  if (content_renderer_) {
    c.SetClip(0, kTopBarHeight, width_, height_ - kTopBarHeight);
    content_renderer_(c, 0, kTopBarHeight, width_, height_ - kTopBarHeight, *theme_);
    c.ResetClip();
  }
  DrawTopBar();
  if (low_battery_popup_) DrawLowBatteryPopup();
  if (overlay_renderer_) overlay_renderer_(c, 0, 0, width_, height_, *theme_);
  c.EndFrame();
}

void LcdDisplay::DrawTopBar() {
  auto& c = *canvas_;
  const int icon_y = (kTopBarHeight - 9) / 2;

  // Right icons: mute, battery
  int x = width_ - kSpacing * 2;
  if (battery_icon_level_ >= 0) {
    x -= 15;
    DrawBatteryIcon(x, icon_y);
  }
  if (show_battery_percent_ && battery_level_ >= 0) {
    const std::string percent = std::to_string(battery_level_) + "%";
    x -= Canvas::TextWidth(percent.c_str()) + 2;
    c.Text(x, (kTopBarHeight - 7) / 2, percent.c_str(), theme_->text);
  }
  if (muted_) {
    x -= 11;
    DrawMuteIcon(x, icon_y);
  }

  // Left icons: Wi-Fi, Bluetooth
  int left = kSpacing * 2;
  if (connectivity_.wifi_on) {
    DrawWifiIcon(left, icon_y);
    left += 11 + kSpacing;
  }
  if (connectivity_.ble_on) {
    DrawBluetoothIcon(left, icon_y);
    left += 6 + kSpacing;
  }

  // Centered status, or notification while one is active, kept clear of the icons
  const std::string& text = notification_.empty() ? status_ : notification_;
  const int room = x - kSpacing - left;
  const int text_width = Canvas::TextWidth(text.c_str());
  if (text_width > room) {
    c.TextMarquee(left, (kTopBarHeight - 7) / 2, room, text.c_str(), theme_->text);
  } else {
    int text_x = (width_ - text_width) / 2;
    text_x = std::max(left, std::min(text_x, x - kSpacing - text_width));
    c.TextMarquee(text_x, (kTopBarHeight - 7) / 2, room, text.c_str(), theme_->text);
  }
}

void LcdDisplay::DrawBatteryIcon(int x, int y) {
  auto& c = *canvas_;
  const uint16_t color = theme_->text;
  // 13×9 body + 2×5 tip
  c.Rect(x, y, 13, 9, color);
  c.FillRect(x + 13, y + 2, 2, 5, color);

  if (battery_icon_level_ == 8) {  // Charging: fill sweeps up in steps
    const int step = (millis() / 350) % 4;
    const int fill = step * 3;
    if (fill > 0) c.FillRect(x + 2, y + 2, fill, 5, color);
    return;
  }
  const int fill = battery_icon_level_ * 9 / 7;  // 0–9 px
  if (fill > 0) c.FillRect(x + 2, y + 2, fill, 5, color);
}

void LcdDisplay::DrawMuteIcon(int x, int y) {
  auto& c = *canvas_;
  const uint16_t color = theme_->text;
  // Speaker
  c.FillRect(x, y + 3, 2, 3, color);
  c.Line(x + 2, y + 3, x + 4, y + 1, 1, color);
  c.Line(x + 2, y + 5, x + 4, y + 7, 1, color);
  c.FillRect(x + 4, y + 1, 1, 7, color);
  // Cross
  c.Line(x + 6, y + 2, x + 9, y + 6, 1, color);
  c.Line(x + 9, y + 2, x + 6, y + 6, 1, color);
}

void LcdDisplay::DrawWifiIcon(int x, int y) {
  auto& c = *canvas_;
  // 11×9 fan: a dot plus three arcs, lit by signal strength.
  const int cx = x + 5;
  const int cy = y + 8;
  const int bars = connectivity_.wifi_bars;
  auto color = [&](int bar) { return bars >= bar ? theme_->text : theme_->muted; };
  c.FillRect(cx - 1, cy - 1, 2, 2, bars >= 1 ? theme_->text : theme_->muted);
  c.Arc(cx, cy, 3, 225, 315, 1, color(1));
  c.Arc(cx, cy, 5, 225, 315, 1, color(2));
  c.Arc(cx, cy, 7, 225, 315, 1, color(3));
}

void LcdDisplay::DrawBluetoothIcon(int x, int y) {
  auto& c = *canvas_;
  const uint16_t color = theme_->text;
  // 5×9 rune
  c.Line(x + 2, y, x + 2, y + 8, 1, color);
  c.Line(x + 2, y, x + 4, y + 2, 1, color);
  c.Line(x + 4, y + 2, x, y + 6, 1, color);
  c.Line(x + 2, y + 8, x + 4, y + 6, 1, color);
  c.Line(x + 4, y + 6, x, y + 2, 1, color);
}

void LcdDisplay::DrawLowBatteryPopup() {
  auto& c = *canvas_;
  const int w = width_ * 9 / 10;
  const int h = Canvas::LineHeight() * 2;
  const int x = (width_ - w) / 2;
  const int y = height_ - h - kSpacing * 4;
  c.FillRoundRect(x, y, w, h, kSpacing * 4, theme_->low_battery);
  c.TextMarquee(x + 4, y + (h - 7) / 2, w - 8, Lang::Strings::BATTERY_NEED_CHARGE, Color(0xFFFFFF), 1, true);
}
