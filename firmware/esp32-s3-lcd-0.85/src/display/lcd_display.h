#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <string>

#include "canvas.h"

struct Theme {
  const char* name;
  uint16_t background;
  uint16_t text;
  uint16_t content_background;
  uint16_t muted;
  uint16_t selected;
  uint16_t low_battery;
  uint16_t ok;
  uint16_t fail;
  uint16_t running;
  uint16_t info;
};

struct BatteryStatus {
  int level = 100;
  bool charging = false;
  bool discharging = false;
  bool available = false;
};

struct ConnectivityStatus {
  bool wifi_on = false;
  int wifi_bars = -1;  // -1 = not connected, 1–3 = signal strength
  bool ble_on = false;
};

// Status UI for the 128×128 panel:
//   top bar      Wi-Fi + Bluetooth icons, status or notification text, mute + battery icons
//   content      drawn by the content renderer
//   popup        low battery warning
class LcdDisplay {
 public:
  using ContentRenderer = std::function<void(Canvas& canvas, int x, int y, int width, int height,
                                             const Theme& theme)>;

  LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
             int height, int offset_x, int offset_y);
  ~LcdDisplay();

  void SetupUI();
  void SetStatus(const char* status);
  void ShowNotification(const char* notification, int duration_ms = 3000);
  void ShowNotification(const std::string& notification, int duration_ms = 3000) {
    ShowNotification(notification.c_str(), duration_ms);
  }
  // `update_all` is true on the first call, before battery readings settle.
  void UpdateStatusBar(bool update_all = false);
  void SetTheme(const char* theme_name);
  const char* GetTheme() const { return theme_->name; }
  void SetRotate180(bool rotate);
  void SetShowBatteryPercent(bool show);

  void SetBatteryProvider(std::function<BatteryStatus()> provider) { battery_provider_ = provider; }
  void SetVolumeProvider(std::function<int()> provider) { volume_provider_ = provider; }
  void SetConnectivityProvider(std::function<ConnectivityStatus()> provider) {
    connectivity_provider_ = provider;
  }
  void SetContentRenderer(ContentRenderer renderer) { content_renderer_ = renderer; }
  // Drawn last over the whole screen (splash, hold-to-confirm ring).
  void SetOverlayRenderer(ContentRenderer renderer) { overlay_renderer_ = renderer; }
  const Theme& theme() const { return *theme_; }

  const std::string& status() const { return status_; }
  // Marks the screen for redraw on the next Loop().
  void Invalidate() { dirty_ = true; }
  // Redraws and flushes if anything changed. Call from the main loop.
  void Loop();

 private:
  void Render();
  void Flush();
  void DrawTopBar();
  void DrawBatteryIcon(int x, int y);
  void DrawMuteIcon(int x, int y);
  void DrawWifiIcon(int x, int y);
  void DrawBluetoothIcon(int x, int y);
  void DrawLowBatteryPopup();

  esp_lcd_panel_io_handle_t panel_io_;
  esp_lcd_panel_handle_t panel_;
  int width_;
  int height_;
  uint8_t* buffer_ = nullptr;
  uint8_t* rotated_buffer_ = nullptr;
  bool rotate180_ = false;
  bool show_battery_percent_ = false;
  int battery_level_ = -1;
  Canvas* canvas_ = nullptr;
  SemaphoreHandle_t flush_done_ = nullptr;

  const Theme* theme_;
  bool dirty_ = true;
  bool ui_ready_ = false;

  std::string status_;
  std::string notification_;
  uint32_t notification_until_ = 0;

  int battery_icon_level_ = -1;  // 0–7, 8 = charging
  bool muted_ = false;
  bool low_battery_popup_ = false;

  std::function<BatteryStatus()> battery_provider_;
  std::function<int()> volume_provider_;
  std::function<ConnectivityStatus()> connectivity_provider_;
  ConnectivityStatus connectivity_;
  ContentRenderer content_renderer_;
  ContentRenderer overlay_renderer_;
  uint32_t last_charge_frame_ = 0;
};
