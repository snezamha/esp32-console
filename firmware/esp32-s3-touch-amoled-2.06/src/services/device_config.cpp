#include "device_config.h"

#include <nvs_flash.h>

#include "../common/settings.h"

void DeviceConfig::Load() {
  Settings s("ui");
  wifi_on = s.GetBool("wifi_on", wifi_on);
  wifi_ssid = s.GetString("wifi_ssid", "");
  wifi_password = s.GetString("wifi_pass", "");
  wifi_ssid2 = s.GetString("wifi_ssid2", "");
  wifi_password2 = s.GetString("wifi_pass2", "");
  timezone = s.GetString("tz", timezone.c_str());
}

void DeviceConfig::Save() {
  Settings s("ui", true);
  s.SetBool("wifi_on", wifi_on);
  s.SetString("wifi_ssid", wifi_ssid);
  s.SetString("wifi_pass", wifi_password);
  s.SetString("wifi_ssid2", wifi_ssid2);
  s.SetString("wifi_pass2", wifi_password2);
  s.SetString("tz", timezone);
}

void DeviceConfig::FactoryReset() {
  nvs_flash_erase();
  nvs_flash_init();
}
