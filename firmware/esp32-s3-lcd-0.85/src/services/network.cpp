#include "network.h"

#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <ctime>

#include "../board/board.h"
#include "device_config.h"
#include "time_zones.h"

namespace {
// Keep the setup access point this long after connecting, so the page can show the result.
constexpr uint32_t kApLingerMs = 30000;
// Reopen the setup access point after losing the network for this long.
constexpr uint32_t kApReopenMs = 20000;
// Try the backup network after the main one has been unreachable this long.
constexpr uint32_t kBackupAfterMs = 15000;
}  // namespace

void Network::Begin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  Apply();
}

void Network::Apply() {
  ApplyTimezone();
  ApplyWifi();
  ApplyBle();
}

void Network::ApplyWifi() {
  auto& config = DeviceConfig::Get();

  if (!config.wifi_on) {
    if (wifi_started_) {
      if (mdns_started_) MDNS.end();
      mdns_started_ = false;
      StopAccessPoint();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifi_started_ = false;
    }
    return;
  }

  if (!wifi_started_) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HostName().c_str());
    wifi_started_ = true;
    disconnected_since_ = millis();
  }

  if (config.wifi_ssid.empty()) {
    StartAccessPoint();
    return;
  }
  using_backup_ = false;
  if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != config.wifi_ssid.c_str()) {
    WiFi.begin(config.wifi_ssid.c_str(), config.wifi_password.c_str());
  }
}

void Network::StartMdns() {
  if (mdns_started_) MDNS.end();
  mdns_started_ = MDNS.begin(HostName().c_str());
  if (mdns_started_) MDNS.addService("http", "tcp", 80);
}

void Network::StartAccessPoint() {
  if (ap_active_) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(DeviceName().c_str());
  ap_active_ = true;
}

void Network::StopAccessPoint() {
  if (!ap_active_) return;
  WiFi.softAPdisconnect(true);
  if (DeviceConfig::Get().wifi_on) WiFi.mode(WIFI_STA);
  ap_active_ = false;
}

void Network::Loop(uint32_t now_ms) {
  auto& config = DeviceConfig::Get();
  if (!config.wifi_on || !wifi_started_) return;

  if (WiFi.status() == WL_CONNECTED) {
    disconnected_since_ = 0;
    if (connected_since_ == 0) {
      connected_since_ = now_ms;
      StartMdns();  // Needs the station interface to be up
    }
    if (ap_active_ && now_ms - connected_since_ > kApLingerMs) StopAccessPoint();
  } else {
    connected_since_ = 0;
    if (disconnected_since_ == 0) disconnected_since_ = now_ms;
    if (!using_backup_ && !config.wifi_ssid2.empty() && now_ms - disconnected_since_ > kBackupAfterMs) {
      using_backup_ = true;
      WiFi.begin(config.wifi_ssid2.c_str(), config.wifi_password2.c_str());
    }
    if (!ap_active_ && (config.wifi_ssid.empty() || now_ms - disconnected_since_ > kApReopenMs)) {
      StartAccessPoint();
    }
  }
}

void Network::SetBackup(const std::string& ssid, const std::string& password) {
  auto& config = DeviceConfig::Get();
  config.wifi_ssid2 = ssid;
  config.wifi_password2 = password;
  config.Save();
}

void Network::ForgetBackup() {
  auto& config = DeviceConfig::Get();
  config.wifi_ssid2.clear();
  config.wifi_password2.clear();
  config.Save();
  if (using_backup_) {
    using_backup_ = false;
    disconnected_since_ = millis();
    WiFi.disconnect(false);
  }
}

std::vector<std::string> Network::SavedNetworks() const {
  auto& config = DeviceConfig::Get();
  return {config.wifi_ssid, config.wifi_ssid2};
}

void Network::Connect(const std::string& ssid, const std::string& password) {
  auto& config = DeviceConfig::Get();
  config.wifi_ssid = ssid;
  config.wifi_password = password;
  config.wifi_on = true;
  config.Save();
  connected_since_ = 0;
  using_backup_ = false;
  WiFi.disconnect(false);
  ApplyWifi();  // Starts connecting with the new credentials
}

void Network::ApplyBle() {
  const bool want = DeviceConfig::Get().ble_on;
  if (want == ble_advertising_) return;

  if (want) {
    if (!BLEDevice::getInitialized()) BLEDevice::init(DeviceName().c_str());
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->setName(DeviceName().c_str());
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();
  } else {
    BLEDevice::stopAdvertising();
  }
  ble_advertising_ = want;
}

void Network::ApplyTimezone() {
  const auto& tz = FindTimeZone(DeviceConfig::Get().timezone.c_str());
  configTzTime(tz.posix, "pool.ntp.org", "time.google.com");
}

Network::WifiState Network::State() const {
  auto& config = DeviceConfig::Get();
  if (!config.wifi_on) return WifiState::Off;
  if (WiFi.status() == WL_CONNECTED) return WifiState::Connected;
  if (config.wifi_ssid.empty()) return WifiState::Setup;
  return WifiState::Connecting;
}

std::string Network::WifiStatus() const {
  switch (State()) {
    case WifiState::Off: return "Off";
    case WifiState::Setup: return "Setup";
    case WifiState::Connecting: return "Connecting";
    case WifiState::Connected: return "Connected";
  }
  return "Off";
}

std::string Network::WifiSsid() const { return DeviceConfig::Get().wifi_ssid; }

std::string Network::WifiIp() const {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-";
}

int Network::WifiRssi() const { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

std::string Network::AccessPointIp() const { return WiFi.softAPIP().toString().c_str(); }

void Network::StartScan() {
  if (scanning_) return;
  scanning_ = true;
  xTaskCreatePinnedToCore(ScanTask, "wifi_scan", 4096, this, 1, nullptr, 0);
}

void Network::ScanTask(void* arg) {
  auto self = static_cast<Network*>(arg);
  std::vector<WifiNetwork> networks;
  Wireless::ScanWifi(networks);
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->scan_results_ = networks;
    self->scanned_at_ = millis();
  }
  self->scanning_ = false;
  vTaskDelete(nullptr);
}

uint32_t Network::ScanAgeMs() const { return scanned_at_ == 0 ? UINT32_MAX : millis() - scanned_at_; }

std::vector<WifiNetwork> Network::ScanResults() {
  std::lock_guard<std::mutex> lock(mutex_);
  return scan_results_;
}

bool Network::TimeValid() const {
  const time_t now = time(nullptr);
  return now > 1735689600;  // 2025-01-01
}

std::string Network::DeviceName() const { return Board::GetInstance().GetBoardType(); }

std::string Network::HostName() const {
  std::string name = DeviceName();
  for (auto& ch : name) {
    if (ch == '.') ch = '-';
  }
  return name;
}
