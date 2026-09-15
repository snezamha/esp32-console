#include "device_config.h"

#include <nvs_flash.h>

#include "../common/settings.h"

void DeviceConfig::Load() {
  Settings s("ui");
  sleep_seconds = s.GetInt("sleep_s", sleep_seconds);
  power_off_seconds = s.GetInt("power_off_s", power_off_seconds);
  rotate = s.GetBool("rotate", rotate);
  button_sound = s.GetBool("btn_sound", button_sound);
  led_on = s.GetBool("led_on", led_on);
  led_feedback = s.GetBool("led_fb", led_feedback);
  led_brightness = s.GetInt("led_level", led_brightness);
  led_color = s.GetInt("led_color", led_color);
  led_mode = s.GetString("led_mode", led_mode.c_str());
  led_speed = s.GetInt("led_speed", led_speed);
  led_pixels = s.GetString("led_px", "");
  wifi_on = s.GetBool("wifi_on", wifi_on);
  wifi_ssid = s.GetString("wifi_ssid", "");
  wifi_password = s.GetString("wifi_pass", "");
  wifi_ssid2 = s.GetString("wifi_ssid2", "");
  wifi_password2 = s.GetString("wifi_pass2", "");
  ble_on = s.GetBool("ble_on", ble_on);
  clock_on = s.GetBool("clock_on", clock_on);
  timezone = s.GetString("tz", timezone.c_str());
  battery_percent = s.GetBool("bat_pct", battery_percent);
  power_hold_ms = s.GetInt("hold_ms", power_hold_ms);
}

void DeviceConfig::Save() {
  Settings s("ui", true);
  s.SetInt("sleep_s", sleep_seconds);
  s.SetInt("power_off_s", power_off_seconds);
  s.SetBool("rotate", rotate);
  s.SetBool("btn_sound", button_sound);
  s.SetBool("led_on", led_on);
  s.SetBool("led_fb", led_feedback);
  s.SetInt("led_level", led_brightness);
  s.SetInt("led_color", led_color);
  s.SetString("led_mode", led_mode);
  s.SetInt("led_speed", led_speed);
  s.SetString("led_px", led_pixels);
  s.SetBool("wifi_on", wifi_on);
  s.SetString("wifi_ssid", wifi_ssid);
  s.SetString("wifi_pass", wifi_password);
  s.SetString("wifi_ssid2", wifi_ssid2);
  s.SetString("wifi_pass2", wifi_password2);
  s.SetBool("ble_on", ble_on);
  s.SetBool("clock_on", clock_on);
  s.SetString("tz", timezone);
  s.SetBool("bat_pct", battery_percent);
  s.SetInt("hold_ms", power_hold_ms);
}

void DeviceConfig::FactoryReset() {
  nvs_flash_erase();
  nvs_flash_init();
}
