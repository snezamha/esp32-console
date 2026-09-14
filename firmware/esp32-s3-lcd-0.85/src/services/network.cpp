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

// One attempt at a network before moving on (backup network, then the setup access point).
constexpr uint32_t kConnectTimeoutMs = 20000;
// Keep the setup access point this long after joining, so the page can show the result.
constexpr uint32_t kApLingerMs = 90000;
// While the setup access point is up and nobody uses it, retry the saved network this often.
constexpr uint32_t kSetupRetryMs = 120000;
// Give an attempt this long before trusting a "wrong password" report (the first handshake after
// a channel change sometimes times out on its own).
constexpr uint32_t kAuthFailGraceMs = 4000;

// NTP servers by address: a server given by name makes SNTP resolve it in the background, and
// that pending lookup can run SNTP code on whichever task next uses DNS (an HTTPS request), which
// trips lwIP's thread-safety check and resets the board.
constexpr const char* kNtpServer1 = "162.159.200.123";  // time.cloudflare.com
constexpr const char* kNtpServer2 = "216.239.35.0";     // time.google.com

// Last station disconnect reason (wifi_err_reason_t), set from the Wi-Fi event task.
std::atomic<int> g_disconnect_reason{0};

bool IsAuthFailure(int reason) {
  return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_MIC_FAILURE;
}

}  // namespace

void Network::Begin() {
  WiFi.persistent(false);
  // Reconnecting is done here, one step at a time; the driver's own retries would scan in the
  // background and pull the radio off the setup access point's channel.
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        g_disconnect_reason = info.wifi_sta_disconnected.reason;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  Apply();
}

void Network::Apply() {
  ApplyTimezone();
  ApplyWifi();
  ApplyBle();
}

void Network::ApplyWifi() {
  auto& config = DeviceConfig::Get();
  const uint32_t now = millis();

  if (!config.wifi_on) {
    if (phase_ == Phase::Off) return;
    if (mdns_started_) MDNS.end();
    mdns_started_ = false;
    if (ap_active_) CloseAccessPoint();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    phase_ = Phase::Off;
    return;
  }

  if (phase_ != Phase::Off) return;  // Already running; settings changes don't restart it.
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HostName().c_str());
  if (config.wifi_ssid.empty()) {
    OpenSetup(now);
  } else {
    TryNetwork(0, false, now);
  }
}

void Network::TryNetwork(int index, bool from_page, uint32_t now_ms) {
  const auto& config = DeviceConfig::Get();
  const std::string& ssid = index == 0 ? config.wifi_ssid : config.wifi_ssid2;
  const std::string& password = index == 0 ? config.wifi_password : config.wifi_password2;

  if (from_page) {
    attempt_id_++;
    error_.clear();
  }
  network_index_ = index;
  from_page_ = from_page;
  g_disconnect_reason = 0;

  WiFi.mode(ap_active_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect(false);
  WiFi.begin(ssid.c_str(), password.c_str());
  phase_ = Phase::Connecting;
  phase_since_ = now_ms;
}

void Network::AttemptFailed(uint32_t now_ms) {
  StopStation();
  const auto& config = DeviceConfig::Get();
  if (!from_page_ && network_index_ == 0 && !config.wifi_ssid2.empty()) {
    TryNetwork(1, false, now_ms);
    return;
  }
  OpenSetup(now_ms);
}

void Network::OpenSetup(uint32_t now_ms) {
  if (!ap_active_) {
    // Scan while no phone depends on the radio staying on one channel.
    ScanNow();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(DeviceName().c_str());
    ap_active_ = true;
    now_ms = millis();  // The scan took a few seconds.
  }
  phase_ = Phase::Setup;
  phase_since_ = now_ms;
}

void Network::StopStation() {
  WiFi.disconnect(false);
  // Leaving station mode stops any search that would pull the access point off its channel.
  WiFi.mode(ap_active_ ? WIFI_AP : WIFI_STA);
}

void Network::CloseAccessPoint() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(phase_ == Phase::Off ? WIFI_OFF : WIFI_STA);
  ap_active_ = false;
}

void Network::StartMdns() {
  if (mdns_started_) MDNS.end();
  mdns_started_ = MDNS.begin(HostName().c_str());
  if (mdns_started_) MDNS.addService("http", "tcp", 80);
}

void Network::Loop(uint32_t now_ms) {
  const auto& config = DeviceConfig::Get();

  switch (phase_) {
    case Phase::Off:
      return;

    case Phase::Connecting: {
      if (WiFi.status() == WL_CONNECTED) {
        phase_ = Phase::Connected;
        connected_at_ = now_ms;
        error_.clear();
        StartMdns();  // Needs the station interface to be up
        return;
      }
      const int reason = g_disconnect_reason;
      const uint32_t elapsed = now_ms - phase_since_;
      if (IsAuthFailure(reason) && elapsed > kAuthFailGraceMs) {
        error_ = "Wrong password";
        AttemptFailed(now_ms);
      } else if (elapsed > kConnectTimeoutMs) {
        error_ = reason == WIFI_REASON_NO_AP_FOUND ? "Network not found" : "Could not connect";
        AttemptFailed(now_ms);
      }
      return;
    }

    case Phase::Connected:
      if (WiFi.status() != WL_CONNECTED) {
        if (mdns_started_) MDNS.end();
        mdns_started_ = false;
        TryNetwork(0, false, now_ms);
        return;
      }
      if (ap_active_ && now_ms - connected_at_ > kApLingerMs) CloseAccessPoint();
      return;

    case Phase::Setup:
      // Retry the saved network now and then, but never while a phone is on the setup page.
      if (!config.wifi_ssid.empty() && WiFi.softAPgetStationNum() == 0 &&
          now_ms - phase_since_ > kSetupRetryMs) {
        TryNetwork(0, false, now_ms);
      }
      return;
  }
}

void Network::Connect(const std::string& ssid, const std::string& password) {
  auto& config = DeviceConfig::Get();
  config.wifi_ssid = ssid;
  config.wifi_password = password;
  config.wifi_on = true;
  config.Save();
  if (phase_ == Phase::Off) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HostName().c_str());
  }
  TryNetwork(0, true, millis());
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
}

std::vector<std::string> Network::SavedNetworks() const {
  const auto& config = DeviceConfig::Get();
  return {config.wifi_ssid, config.wifi_ssid2};
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
  configTzTime(tz.posix, kNtpServer1, kNtpServer2);
}

Network::WifiState Network::State() const {
  switch (phase_) {
    case Phase::Off: return WifiState::Off;
    case Phase::Setup: return WifiState::Setup;
    case Phase::Connected:
      return WiFi.status() == WL_CONNECTED ? WifiState::Connected : WifiState::Connecting;
    case Phase::Connecting: return WifiState::Connecting;
  }
  return WifiState::Off;
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

std::string Network::WifiSsid() const {
  if (State() == WifiState::Connected) return WiFi.SSID().c_str();
  const auto& config = DeviceConfig::Get();
  return network_index_ == 1 ? config.wifi_ssid2 : config.wifi_ssid;
}

std::string Network::WifiIp() const {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-";
}

int Network::WifiRssi() const { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

std::string Network::AccessPointIp() const { return WiFi.softAPIP().toString().c_str(); }

void Network::ScanNow() {
  std::vector<WifiNetwork> networks;
  Wireless::ScanWifi(networks);
  std::lock_guard<std::mutex> lock(mutex_);
  scan_results_ = networks;
  scanned_at_ = millis();
}

void Network::StartScan() {
  if (scanning_) return;
  scanning_ = true;
  xTaskCreatePinnedToCore(ScanTask, "wifi_scan", 4096, this, 1, nullptr, 0);
}

void Network::ScanTask(void* arg) {
  auto self = static_cast<Network*>(arg);
  self->ScanNow();
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
