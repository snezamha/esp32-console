#pragma once

#include <string>

// User settings, persisted in NVS (namespace "ui").
// Theme and brightness live with the display, volume with the audio codec.
struct DeviceConfig {
  int sleep_seconds = 0;      // Screen dims after this idle time; 0 = never
  int power_off_seconds = 0;  // On battery only; 0 = never
  bool rotate = false;        // Rotate the screen 180°
  bool button_sound = true;
  bool led_on = false;
  bool led_feedback = false;  // Glow on press, low battery pulse, charging motion
  int led_brightness = 4;     // 1–8
  int led_color = 0;          // Index into kLedColors (0 = white)
  std::string led_mode = "static";  // LedPattern effect
  int led_speed = 5;                // 1–10
  std::string led_pixels;           // LedPattern pixels; empty = every LED in led_color
  bool wifi_on = true;
  std::string wifi_ssid;
  std::string wifi_password;
  // Backup network, added from the console; tried when the main one is unreachable.
  std::string wifi_ssid2;
  std::string wifi_password2;
  // Deliberately not persisted: BLE is brought up on request and always starts off, so this stays
  // an honest picture of whether the radio is actually running.
  bool ble_on = false;
  bool clock_on = true;
  std::string timezone = "Europe/Berlin";  // IANA id from time_zones.h
  bool battery_percent = true;
  int power_hold_ms = 1500;

  static DeviceConfig& Get() {
    static DeviceConfig config;
    return config;
  }

  void Load();
  void Save();
  // Clears every settings namespace; call before restarting.
  static void FactoryReset();
};
