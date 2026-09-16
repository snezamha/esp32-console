#pragma once

#include <string>

// User settings, persisted in NVS (namespace "ui").
struct DeviceConfig {
  bool wifi_on = true;
  std::string wifi_ssid;
  std::string wifi_password;
  // Backup network, added from the console; tried when the main one is unreachable.
  std::string wifi_ssid2;
  std::string wifi_password2;
  // Deliberately not persisted: BLE is brought up on request and always starts off, so this stays
  // an honest picture of whether the radio is actually running.
  bool ble_on = false;
  std::string timezone = "Europe/Berlin";  // IANA id from time_zones.h

  static DeviceConfig& Get() {
    static DeviceConfig config;
    return config;
  }

  void Load();
  void Save();
  // Clears every settings namespace; call before restarting.
  static void FactoryReset();
};
