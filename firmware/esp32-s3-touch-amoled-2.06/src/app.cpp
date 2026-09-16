#include "app.h"

#include <esp_system.h>

#include <algorithm>
#include <sstream>
#include <vector>

#include "../version.h"
#include "board/board.h"
#include "common/settings.h"
#include "services/console_client.h"
#include "services/device_config.h"
#include "services/network.h"
#include "services/web_portal.h"
#include "ui/ui.h"

namespace {

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> parts;
  std::istringstream stream(line);
  std::string part;
  while (stream >> part) parts.push_back(part);
  return parts;
}

std::string Rest(const std::string& line, size_t words) {
  size_t pos = 0;
  for (size_t i = 0; i < words; i++) {
    pos = line.find_first_not_of(' ', pos);
    pos = line.find(' ', pos);
    if (pos == std::string::npos) return "";
  }
  pos = line.find_first_not_of(' ', pos);
  return pos == std::string::npos ? "" : line.substr(pos);
}

enum class PmuKey { None, Short, Long };

// Reads and clears the power key IRQ latched by the AXP2101. Safe to call every loop tick.
PmuKey ReadPmuKey() {
  auto& power = Board::GetInstance().GetPower();
  power.getIrqStatus();
  PmuKey event = PmuKey::None;
  if (power.isPekeyLongPressIrq()) {
    event = PmuKey::Long;
  } else if (power.isPekeyShortPressIrq()) {
    event = PmuKey::Short;
  }
  power.clearIrqStatus();
  return event;
}

}  // namespace

void App::Start() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  DeviceConfig::Get().Load();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"start\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());

  auto& board = Board::GetInstance();
  board.Initialize();
  board.SetFactoryResetHandler([]() {
    DeviceConfig::FactoryReset();
    Serial.flush();
    delay(200);
    ESP.restart();
  });
  board.SetBootClickHandler([this]() {
    if (!screen_on_) WakeScreen();
  });
  Serial.printf("{\"event\":\"boot\",\"stage\":\"board\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());

  ui_init();
  {
    Settings settings("display");
    const int target = settings.GetInt("brightness", 200);
    auto display = board.GetDisplay();
    for (int i = 0; i <= target; i += 8) {
      display->setBrightness(i);
      delay(4);
    }
    display->setBrightness(target);
  }
  Serial.printf("{\"event\":\"boot\",\"stage\":\"ui\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());

  Network::GetInstance().Begin();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"network\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());

  auto& console = ConsoleClient::GetInstance();
  console.SetStateProvider([this]() { return ReportState(); });
  console.SetTelemetryProvider([this]() { return ReportTelemetry(); });
  console.SetApplyHandler([this](const ConsoleClient::Values& values) { ApplyRemoteSettings(values); });
  console.SetCommandHandler([this](const ConsoleClient::Command& command) { return HandleConsoleCommand(command); });
  console.Begin();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"ready\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
}

void App::Loop() {
  auto& board = Board::GetInstance();
  board.Loop();  // Ticks the boot button; its click handler wakes the screen.
  HandleSerial();

  const uint32_t now = millis();
  // Wi-Fi + console pairing always run, screen on or off, so the board stays visible to the
  // console regardless of what the display is doing.
  Network::GetInstance().Loop(now);
  WebPortal::GetInstance().Loop(now);
  ConsoleClient::GetInstance().Loop(now);

  const PmuKey key = ReadPmuKey();
  if (key == PmuKey::Long) {
    board.PowerOff();
    return;
  }

  if (!screen_on_) {
    if (key == PmuKey::Short) WakeScreen();
    delay(100);
    return;
  }

  if (key == PmuKey::Short) {
    SleepScreen();
    return;
  }

  ui_task();
  delay(1);
}

void App::WakeScreen() {
  auto display = Board::GetInstance().GetDisplay();
  display->displayOn();
  Settings settings("display");
  display->setBrightness(settings.GetInt("brightness", 200));
  screen_on_ = true;
  ui_wake_redraw();
}

void App::SleepScreen() {
  Board::GetInstance().GetDisplay()->displayOff();
  screen_on_ = false;
}

void App::HandleSerial() {
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (!serial_line_.empty()) HandleCommand(serial_line_);
      serial_line_.clear();
    } else if (serial_line_.size() < 1024) {
      serial_line_ += ch;
    }
  }
}

void App::HandleCommand(const std::string& line) {
  auto& board = Board::GetInstance();
  auto& config = DeviceConfig::Get();
  auto& network = Network::GetInstance();
  const auto args = Split(line);
  if (args.empty()) return;
  const std::string& cmd = args[0];
  auto num = [&](size_t i, int fallback) {
    return i < args.size() ? atoi(args[i].c_str()) : fallback;
  };
  auto ok = [&]() { Serial.printf("{\"ok\":true,\"command\":\"%s\"}\n", cmd.c_str()); };

  if (!screen_on_) WakeScreen();

  if (cmd == "info") {
    PrintInfo();
  } else if (cmd == "wifi") {
    if (args.size() >= 2 && (args[1] == "off" || args[1] == "on")) {
      config.wifi_on = args[1] == "on";
      config.Save();
      network.Apply();
    } else if (args.size() >= 2) {
      network.Connect(args[1], args.size() >= 3 ? Rest(line, 2) : "");
    }
    Serial.printf("{\"wifi\":{\"on\":%s,\"ssid\":\"%s\",\"status\":\"%s\",\"ip\":\"%s\",\"error\":\"%s\"}}\n",
                  config.wifi_on ? "true" : "false", config.wifi_ssid.c_str(),
                  network.WifiStatus().c_str(), network.WifiIp().c_str(), network.LastError().c_str());
  } else if (cmd == "console") {
    auto& console = ConsoleClient::GetInstance();
    if (args.size() >= 2 && args[1] == "unlink") {
      console.Unlink();
    } else if (args.size() >= 2 && args[1] == "off") {
      console.SetServer("");
    } else if (args.size() >= 2 && args[1] == "insecure") {
      console.SetInsecure(args.size() < 3 || args[2] != "off");
    } else if (args.size() >= 2 && args[1] != "status") {
      console.SetServer(args[1]);
    }
    Serial.printf(
        "{\"console\":{\"server\":\"%s\",\"state\":\"%s\",\"code\":\"%s\",\"name\":\"%s\","
        "\"insecure\":%s}}\n",
        console.Server().c_str(), console.StateText(), console.Code().c_str(), console.Name().c_str(),
        console.Insecure() ? "true" : "false");
  } else if (cmd == "ble" && args.size() > 1) {
    config.ble_on = args[1] == "on";
    config.Save();
    network.Apply();
    ok();
  } else if (cmd == "notify" && args.size() > 1) {
    board.ShowNotification(Rest(line, 1));
    ok();
  } else if (cmd == "backlight" && args.size() > 1) {
    const int value = num(1, 200);
    board.GetDisplay()->setBrightness(value);
    Settings settings("display", true);
    settings.SetInt("brightness", value);
    ok();
  } else if (cmd == "sleep") {
    ok();
    SleepScreen();
  } else if (cmd == "factoryreset") {
    ok();
    FactoryReset();
  } else if (cmd == "reboot") {
    ok();
    Serial.flush();
    ESP.restart();
  } else if (cmd == "poweroff") {
    ok();
    board.PowerOff();
  } else if (cmd == "help") {
    Serial.println(
        "info | wifi <ssid> [password] | wifi on|off | "
        "console <url>|off|unlink|insecure [on|off]|status | ble on|off | "
        "notify <text> | backlight <0-255> | sleep | reboot | poweroff | factoryreset");
  } else {
    Serial.println("{\"ok\":false,\"error\":\"unknown command\"}");
  }
}

void App::FactoryReset() {
  DeviceConfig::FactoryReset();
  Serial.flush();
  delay(200);
  ESP.restart();
}

std::string App::ReportState() {
  const auto& config = DeviceConfig::Get();
  Settings settings("display");
  const int brightness = settings.GetInt("brightness", 200);

  auto b = [](bool v) { return v ? "1" : "0"; };
  char buf[256];
  snprintf(buf, sizeof(buf), "s.brightness=%d&s.ble_on=%s&s.tz=%s", brightness, b(config.ble_on),
           config.timezone.c_str());
  std::string body = buf;
  for (size_t pos = 0; (pos = body.find('/', pos)) != std::string::npos;) body.replace(pos, 1, "%2F");

  const auto networks = Network::GetInstance().SavedNetworks();
  body += "&wifi1=" + networks[0] + "&wifi2=" + networks[1];
  return body;
}

std::string App::ReportTelemetry() {
  int battery = -1;
  bool charging = false, discharging = false;
  Board::GetInstance().GetBatteryLevel(battery, charging, discharging);
  char buf[96];
  snprintf(buf, sizeof(buf), "battery=%d&charging=%s&heap=%lu&uptime=%lu", battery,
           charging ? "1" : "0", (unsigned long)ESP.getFreeHeap(), millis() / 1000);
  return std::string(buf) + "&reset_reason=" + std::to_string(esp_reset_reason());
}

void App::ApplyRemoteSettings(const ConsoleClient::Values& values) {
  auto& config = DeviceConfig::Get();
  bool network_changed = false;

  for (const auto& [key, value] : values) {
    const int n = atoi(value.c_str());
    const bool on = value == "1";
    if (key == "brightness") {
      Settings settings("display", true);
      settings.SetInt("brightness", std::clamp(n, 20, 255));
      Board::GetInstance().GetDisplay()->setBrightness(std::clamp(n, 20, 255));
    } else if (key == "ble_on") {
      network_changed |= config.ble_on != on;
      config.ble_on = on;
    } else if (key == "tz") {
      network_changed |= config.timezone != value;
      config.timezone = value;
    }
  }

  config.Save();
  if (network_changed) Network::GetInstance().Apply();
  Board::GetInstance().ShowNotification("Settings updated");
}

std::string App::HandleConsoleCommand(const ConsoleClient::Command& command) {
  auto& board = Board::GetInstance();
  auto& network = Network::GetInstance();

  if (command.type == "restart") {
    Serial.flush();
    delay(150);
    ESP.restart();
  } else if (command.type == "poweroff") {
    board.PowerOff();
  } else if (command.type == "identify") {
    board.ShowNotification("Identify", 4000);
  } else if (command.type == "notify") {
    board.ShowNotification(command.arg, 5000);
  } else if (command.type == "wifi_add") {
    const std::string ssid = ConsoleClient::FormValue(command.arg, "ssid");
    const std::string password = ConsoleClient::FormValue(command.arg, "password");
    if (ssid.empty()) return "fail|Missing network name";
    network.SetBackup(ssid, password);
    return "ok|Saved";
  } else if (command.type == "wifi_forget") {
    network.ForgetBackup();
    return "ok|Forgotten";
  }
  return "";
}

void App::PrintInfo() {
  auto& board = Board::GetInstance();
  auto& config = DeviceConfig::Get();
  auto& network = Network::GetInstance();
  const uint64_t mac = ESP.getEfuseMac();
  Serial.printf(
      "{\"firmware\":\"%s\",\"version\":\"%s\",\"board\":\"%s\",\"chip\":\"%s\","
      "\"revision\":%d,\"cores\":%d,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
      "\"flash\":%lu,\"psram\":%lu,\"free_heap\":%lu,"
      "\"wifi\":\"%s\",\"ip\":\"%s\",\"ble\":%s,\"tz\":\"%s\",\"uptime\":%lu,"
      "\"console\":\"%s\",\"min_heap\":%lu,\"max_block\":%lu}\n",
      FIRMWARE_NAME, FIRMWARE_VERSION, board.GetBoardType().c_str(), ESP.getChipModel(),
      ESP.getChipRevision(), ESP.getChipCores(), uint8_t(mac), uint8_t(mac >> 8),
      uint8_t(mac >> 16), uint8_t(mac >> 24), uint8_t(mac >> 32), uint8_t(mac >> 40),
      (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getPsramSize(),
      (unsigned long)ESP.getFreeHeap(), network.WifiStatus().c_str(), network.WifiIp().c_str(),
      config.ble_on ? "true" : "false", config.timezone.c_str(), millis() / 1000,
      ConsoleClient::GetInstance().StateText(), (unsigned long)ESP.getMinFreeHeap(),
      (unsigned long)ESP.getMaxAllocHeap());
}
