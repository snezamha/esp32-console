#pragma once

#include <freertos/FreeRTOS.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../net/wireless.h"

// Wi-Fi, Bluetooth advertising and network time, driven by DeviceConfig.
//
// With Wi-Fi on, the board joins the saved network. While it is not connected it also
// opens a setup access point (named after the board) with a web page for choosing the
// network; the access point closes shortly after a connection is made.
class Network {
 public:
  enum class WifiState { Off, Setup, Connecting, Connected };

  static Network& GetInstance() {
    static Network instance;
    return instance;
  }

  void Begin();
  // Re-applies Wi-Fi, Bluetooth and time zone settings.
  void Apply();
  // Call from the main loop: manages the setup access point.
  void Loop(uint32_t now_ms);

  // Saves credentials and starts connecting.
  void Connect(const std::string& ssid, const std::string& password);
  // Adds/replaces the backup network, tried when the main one is unreachable.
  void SetBackup(const std::string& ssid, const std::string& password);
  void ForgetBackup();
  // [primary ssid, backup ssid], for reporting to the console.
  std::vector<std::string> SavedNetworks() const;

  WifiState State() const;
  // "Off", "Setup", "Connecting", "Connected"
  std::string WifiStatus() const;
  std::string WifiSsid() const;
  std::string WifiIp() const;
  int WifiRssi() const;
  bool AccessPointActive() const { return ap_active_; }
  std::string AccessPointIp() const;

  // Background scan, shared by the menu and the setup page.
  void StartScan();
  bool IsScanning() const { return scanning_; }
  uint32_t ScanAgeMs() const;
  std::vector<WifiNetwork> ScanResults();

  bool TimeValid() const;
  // Wi-Fi access point and Bluetooth name.
  std::string DeviceName() const;
  // DHCP / mDNS host name (no dots allowed).
  std::string HostName() const;

 private:
  Network() = default;
  static void ScanTask(void* arg);
  void ApplyWifi();
  void ApplyBle();
  void ApplyTimezone();
  void StartAccessPoint();
  void StopAccessPoint();

  void StartMdns();

  bool wifi_started_ = false;
  bool using_backup_ = false;
  bool mdns_started_ = false;
  bool ble_advertising_ = false;
  std::atomic<bool> ap_active_{false};
  std::atomic<bool> scanning_{false};
  uint32_t connected_since_ = 0;
  uint32_t disconnected_since_ = 0;
  uint32_t scanned_at_ = 0;
  std::mutex mutex_;
  std::vector<WifiNetwork> scan_results_;
};
