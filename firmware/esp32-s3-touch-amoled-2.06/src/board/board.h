#pragma once

#include "../../config.h"  // Defines XPOWERS_CHIP_AXP2101, read by XPowersLib.h below.

#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <XPowersLib.h>

#include <functional>
#include <memory>
#include <string>

#include "../common/button.h"

// Waveshare ESP32-S3-Touch-AMOLED-2.06 peripherals: CO5300 AMOLED (QSPI) + FT3168 touch (LVGL
// drives both, see ui/ui.cpp), AXP2101 PMIC (battery + power off) and the boot button.
class Board {
 public:
  static Board& GetInstance() {
    static Board instance;
    return instance;
  }

  void Initialize();
  // Polls the boot button; call from the main loop.
  void Loop();

  std::string GetBoardType() const { return "esp32-s3-touch-amoled-2.06"; }
  Arduino_CO5300* GetDisplay() { return display_; }
  Arduino_IIC* GetTouch() { return touch_.get(); }
  XPowersPMU& GetPower() { return power_; }

  bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
  // Cuts board power; USB stays powered (the chip re-boots when the USB source resets it).
  void PowerOff();
  void ShowNotification(const std::string& text, int duration_ms = 3000);

  void SetBootClickHandler(std::function<void()> handler) { boot_click_handler_ = handler; }
  // Held 3s: factory reset (clears Wi-Fi/console pairing) and restarts.
  void SetFactoryResetHandler(std::function<void()> handler) { factory_reset_handler_ = handler; }

 private:
  Board() = default;
  Board(const Board&) = delete;
  Board& operator=(const Board&) = delete;

  void InitializeDisplay();
  void InitializeTouch();
  void InitializePower();

  Arduino_CO5300* display_ = nullptr;
  std::shared_ptr<Arduino_IIC_DriveBus> iic_bus_;
  std::unique_ptr<Arduino_IIC> touch_;
  XPowersPMU power_;
  bool power_ok_ = false;
  Button* boot_button_ = nullptr;
  std::function<void()> boot_click_handler_;
  std::function<void()> factory_reset_handler_;
};
