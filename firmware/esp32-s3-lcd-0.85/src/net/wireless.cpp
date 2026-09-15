#include "wireless.h"

#include <BLEDevice.h>
#include <BLEScan.h>
#include <WiFi.h>

#include <algorithm>

namespace Wireless {
namespace {
// Shuts BLE down again on every exit path unless it was already running before the scan.
struct Deinit {
  bool keep;
  ~Deinit() {
    if (!keep) BLEDevice::deinit(false);
  }
};
}  // namespace

bool ScanWifi(std::vector<WifiNetwork>& networks) {
  networks.clear();
  // Keep an existing station connection; only power the radio for the scan when it was off.
  const bool radio_was_off = WiFi.getMode() == WIFI_OFF;
  if (radio_was_off && !WiFi.mode(WIFI_STA)) return false;
  const int count = WiFi.scanNetworks();
  if (count < 0) {
    if (radio_was_off) WiFi.mode(WIFI_OFF);
    return false;
  }

  for (int i = 0; i < count; i++) {
    networks.push_back({WiFi.SSID(i).c_str(), WiFi.RSSI(i), static_cast<int>(WiFi.channel(i)),
                        WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  if (radio_was_off) WiFi.mode(WIFI_OFF);
  std::sort(networks.begin(), networks.end(),
            [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
  return true;
}

bool ScanBle(std::vector<BleDevice>& devices, int seconds) {
  devices.clear();
  // Started here only for the scan: leaving Bluedroid up afterwards would strand ~72 KB and break
  // the console's TLS handshake, which needs large contiguous blocks.
  const bool was_up = BLEDevice::getInitialized();
  if (!was_up && !BLEDevice::init("")) return false;
  Deinit guard{was_up};
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  BLEScanResults* results = scan->start(seconds, false);
  if (!results) return false;
  for (int i = 0; i < results->getCount(); i++) {
    BLEAdvertisedDevice device = results->getDevice(i);
    devices.push_back({device.getAddress().toString().c_str(),
                       device.haveName() ? device.getName().c_str() : "", device.getRSSI()});
  }
  scan->clearResults();
  std::sort(devices.begin(), devices.end(),
            [](const BleDevice& a, const BleDevice& b) { return a.rssi > b.rssi; });
  return true;
}

}  // namespace Wireless
