#pragma once

#include <string>
#include <vector>

struct WifiNetwork {
  std::string ssid;
  int rssi;
  int channel;
  bool secure;
};

struct BleDevice {
  std::string address;
  std::string name;
  int rssi;
};

namespace Wireless {

// Station-mode scan, strongest first. Returns false if the radio failed.
bool ScanWifi(std::vector<WifiNetwork>& networks);
// Active BLE scan for `seconds`, strongest first. Returns false if the radio failed.
bool ScanBle(std::vector<BleDevice>& devices, int seconds = 3);

}  // namespace Wireless
