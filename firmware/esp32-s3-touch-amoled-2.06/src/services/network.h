#pragma once

#include <freertos/FreeRTOS.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../net/wireless.h"

// Wi-Fi, Bluetooth advertising and network time, driven by DeviceConfig.
//
// Wi-Fi runs as a small state machine, one thing at a time:
//   Connecting  trying the saved network (then the backup one) for up to 20 s each
//   Connected   joined; a dropped connection starts Connecting again
//   Setup       no network could be joined: the board opens its setup access point
//
// The ESP32 has a single radio, so anything that makes it leave the access point's channel
// (a scan, or the station searching for a network) disconnects a phone that is using the setup
// page. While the setup access point is up the board therefore stays quiet: networks are scanned
// once before the access point starts, a rescan only happens when the page asks for one, and the
// saved network is only retried while nobody is connected to the access point.
class Network {
 public:
  enum class WifiState { Off, Setup, Connecting, Connected };

  static Network& GetInstance() {
    static Network instance;
    return instance;
  }

  void Begin();
  // Re-applies Wi-Fi on/off, Bluetooth and time zone settings. Does not restart a connection.
  void Apply();
  // Call from the main loop.
  void Loop(uint32_t now_ms);

  // Saves credentials and tries them now, keeping the setup access point up for the result.
  void Connect(const std::string& ssid, const std::string& password);
  // Adds/replaces the backup network, tried when the main one is unreachable.
  void SetBackup(const std::string& ssid, const std::string& password);
  void ForgetBackup();
  // [primary ssid, backup ssid], for reporting to the console.
  std::vector<std::string> SavedNetworks() const;

  WifiState State() const;
  // "Off", "Setup", "Connecting", "Connected"
  std::string WifiStatus() const;
  // Joined network while connected, otherwise the saved one.
  std::string WifiSsid() const;
  std::string WifiIp() const;
  int WifiRssi() const;
  bool AccessPointActive() const { return ap_active_; }
  std::string AccessPointIp() const;
  // Why the last attempt failed ("Wrong password", "Network not found", …), or empty.
  const std::string& LastError() const { return error_; }
  // Increments for every attempt started from the setup page, so the page can tell a fresh
  // result from an old one.
  int AttemptId() const { return attempt_id_; }

  // Background scan, shared by the menu and the setup page.
  void StartScan();
  bool IsScanning() const { return scanning_; }
  uint32_t ScanAgeMs() const;
  std::vector<WifiNetwork> ScanResults();
  // Background BLE scan shared with Project Builder; results remain until the next scan.
  void StartBleScan();
  bool IsBleScanning() const { return ble_scanning_; }
  std::vector<BleDevice> BleScanResults();

  bool TimeValid() const;
  // Wi-Fi access point and Bluetooth name.
  std::string DeviceName() const;
  // DHCP / mDNS host name (no dots allowed).
  std::string HostName() const;

 private:
  enum class Phase { Off, Connecting, Connected, Setup };

  Network() = default;
  static void ScanTask(void* arg);
  static void BleScanTask(void* arg);
  void ApplyWifi();
  void ApplyBle();
  void ApplyTimezone();
  void TryNetwork(int index, bool from_page, uint32_t now_ms);
  void AttemptFailed(uint32_t now_ms);
  void OpenSetup(uint32_t now_ms);
  void StopStation();
  void CloseAccessPoint();
  void StartMdns();
  void ScanNow();

  Phase phase_ = Phase::Off;
  uint32_t phase_since_ = 0;
  // 0 = main network, 1 = backup network.
  int network_index_ = 0;
  bool from_page_ = false;
  uint32_t connected_at_ = 0;
  std::string error_;
  int attempt_id_ = 0;

  bool mdns_started_ = false;
  bool ble_advertising_ = false;
  std::atomic<bool> ap_active_{false};
  std::atomic<bool> scanning_{false};
  std::atomic<bool> ble_scanning_{false};
  uint32_t scanned_at_ = 0;
  std::mutex mutex_;
  std::vector<WifiNetwork> scan_results_;
  std::vector<BleDevice> ble_scan_results_;
};
