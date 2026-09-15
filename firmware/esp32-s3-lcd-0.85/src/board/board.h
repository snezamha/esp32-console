#pragma once

#include <driver/i2c_master.h>

#include <functional>
#include <string>

#include "../audio/box_audio_codec.h"
#include "../common/backlight.h"
#include "../common/button.h"
#include "../display/lcd_display.h"
#include "../led/led_ring.h"
#include "../storage/sd_card.h"
#include "power_manager.h"

// Toggle: Vol+ and Vol- together. Up / Down / Select: Vol+ / Vol- / Power click.
enum class MenuKey { Toggle, Up, Down, Select };

// Waveshare ESP32-S3-LCD-0.85 peripherals, configured from config.h.
class Board {
 public:
  // Power held this long shows the hold ring; releasing afterwards cancels instead of clicking.
  static constexpr uint16_t kPowerHoldRingDelayMs = 300;

  static Board& GetInstance() {
    static Board instance;
    return instance;
  }

  void Initialize();
  // Polls buttons; call from the main loop.
  void Loop();
  // Call once per second. `busy` keeps the screen awake and the board on.
  void OnClockTick(bool busy);
  // Resets the idle timer and restores the backlight after auto sleep.
  void WakeUp();
  bool IsSleeping() const { return sleeping_; }
  // Applies LED ring and power button settings from DeviceConfig.
  void ApplyLed();
  // Redraws an animated LED design (effects or blinking LEDs); no-op for static designs.
  void AnimateLed(uint32_t now_ms);
  void ApplyPowerHold();

  std::string GetBoardType() const { return "esp32-s3-lcd-0.85"; }
  PowerManager* GetPowerManager() { return power_manager_; }
  LcdDisplay* GetDisplay() { return display_; }
  Backlight* GetBacklight();
  LedRing* GetLedRing() { return led_ring_; }
  BoxAudioCodec* GetAudioCodec() { return audio_codec_; }
  SdCard* GetSdCard() { return sd_card_; }

  Button& GetPowerButton() { return pwr_button_; }
  Button& GetVolumeUpButton() { return volume_up_button_; }
  Button& GetVolumeDownButton() { return volume_down_button_; }

  // While the menu is open Vol+ / Vol- navigate instead of changing the volume.
  // Power click always sends Select; a press that wakes the screen is ignored.
  void SetMenuKeyHandler(std::function<void(MenuKey)> handler) { menu_key_handler_ = handler; }
  void SetMenuOpen(bool open) { menu_open_ = open; }
  // Return true to keep a Power long press from switching the board off.
  void SetPowerHoldInterceptor(std::function<bool()> interceptor) {
    power_hold_interceptor_ = interceptor;
  }
  // Power is held after the boot press was released (i.e. it may power off).
  bool IsPowerHoldActive() const {
    return pwr_button_armed_ && pwr_button_.IsPressed() && !pwr_button_.IsCancelled();
  }
  bool IsMenuOpen() const { return menu_open_; }

  bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
  void PowerOff();

 private:
  Board();
  Board(const Board&) = delete;
  Board& operator=(const Board&) = delete;

  void InitializePowerManager();
  void InitializeI2c();
  void InitializeSpi();
  void InitializeLcdDisplay();
  void InitializeButtons();

  Button pwr_button_;
  Button volume_up_button_;
  Button volume_down_button_;
  bool pwr_button_armed_ = false;
  bool volume_combo_latched_ = false;
  bool menu_open_ = false;
  std::function<void(MenuKey)> menu_key_handler_;
  std::function<bool()> power_hold_interceptor_;

  i2c_master_bus_handle_t i2c_bus_ = nullptr;
  PowerManager* power_manager_ = nullptr;
  LcdDisplay* display_ = nullptr;
  LedRing* led_ring_ = nullptr;
  uint32_t led_frame_ms_ = 0;
  BoxAudioCodec* audio_codec_ = nullptr;
  SdCard* sd_card_ = nullptr;
  int idle_seconds_ = 0;
  bool sleeping_ = false;
};
