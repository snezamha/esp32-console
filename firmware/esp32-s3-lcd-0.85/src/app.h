#pragma once

#include <Arduino.h>

#include <string>

#include "services/console_client.h"
#include "ui/menu.h"

enum class MenuKey;
class Canvas;
struct Theme;

// Home screen, settings menu (Power click), hardware check (Vol+ and Vol- together)
// and serial commands.
class App {
 public:
  static App& GetInstance() {
    static App instance;
    return instance;
  }

  void Start();
  void Loop();

 private:
  App() = default;

  void ApplySettings();
  void OnButtonPressed(uint8_t bit);
  void OnMenuKey(MenuKey key);
  void OnClockTick();
  void UpdateStatus();
  void UpdatePowerHold(uint32_t now);
  // Restarts if free heap stays critically low: cheaper than chasing a slow leak, and every
  // setting is in NVS, so nothing is lost. Skipped mid update (a restart would abort the OTA).
  void CheckHeap(uint32_t now);
  void DrawHome(Canvas& canvas, int x, int y, int width, int height, const Theme& theme);
  std::string HomeSignature() const;
  void DrawOverlay(Canvas& canvas, int width, int height, const Theme& theme);

  MenuItems BuildMainMenu();
  MenuItems BuildDisplayMenu();
  MenuItems BuildSoundMenu();
  MenuItems BuildConnectivityMenu();
  MenuItems BuildConsoleMenu();
  MenuItems BuildNetworksMenu();
  MenuItems BuildTimeZoneMenu();
  MenuItems BuildDeviceMenu();
  MenuItems BuildBatteryMenu();
  MenuItems BuildSdCardMenu();
  MenuItems BuildAboutMenu();
  MenuItems BuildHwCheckMenu();
  MenuItems BuildConfirmMenu(const char* action_label, std::function<void()> action);

  void HandleSerial();
  void HandleCommand(const std::string& line);
  void PrintInfo();
  // Settings, hardware check results and Wi-Fi networks reported to the console (changes hold
  // the long poll open), telemetry that always goes up, settings received from the console, and
  // console commands (only those not handled inside ConsoleClient, i.e. not "ota").
  std::string ReportState();
  std::string ReportTelemetry();
  void ApplyRemoteSettings(const ConsoleClient::Values& values);
  std::string HandleConsoleCommand(const ConsoleClient::Command& command);
  // Beeps and flashes the LED ring so the device can be found.
  void Identify();

  MenuView menu_;
  uint8_t buttons_pressed_ = 0;
  uint32_t last_clock_tick_ = 0;
  uint32_t last_live_redraw_ = 0;
  uint32_t last_input_ = 0;
  uint32_t last_menu_refresh_ = 0;
  uint32_t splash_until_ = 0;
  uint32_t low_heap_since_ = 0;
  // Power hold ring: 0 = hidden, otherwise 0–1 progress.
  float hold_progress_ = 0;
  std::string hold_label_;
  bool hold_done_ = false;
  std::string serial_line_;
  std::string home_signature_;
};
