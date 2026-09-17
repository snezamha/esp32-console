#include "app.h"

#include <esp_system.h>
#include <sys/time.h>

#include <algorithm>
#include <ctime>
#include <sstream>
#include <vector>

#include "../version.h"
#include "board/board.h"
#include "common/settings.h"
#include "hw_test.h"
#include "services/console_client.h"
#include "runtime/project_runtime.h"
#include "lang.h"
#include "services/device_config.h"
#include "services/heap_guard.h"
#include "services/led_feedback.h"
#include "led/led_pattern.h"
#include "services/network.h"
#include "services/serial_fs.h"
#include "services/time_zones.h"
#include "services/web_portal.h"
#include "ui/icons.h"

namespace {

constexpr uint8_t kButtonPower = 1 << 0;
constexpr uint8_t kButtonUp = 1 << 1;
constexpr uint8_t kButtonDown = 1 << 2;
constexpr uint32_t kMenuAutoCloseMs = 30000;
constexpr uint32_t kSplashMs = 1400;

constexpr int kSleepOptions[] = {10, 30, 60, 300, 0};
constexpr int kPowerOffOptions[] = {60, 300, 600, 0};
constexpr int kHoldOptions[] = {1500, 3000, 5000};
constexpr const char* kLedColorNames[] = {"White", "Red", "Green", "Blue", "Cyan", "Purple", "Orange"};
constexpr int kLedColorCount = sizeof(kLedColorNames) / sizeof(kLedColorNames[0]);

uint16_t StatusColor(TestStatus status, const Theme& theme) {
  switch (status) {
    case TestStatus::Ok: return theme.ok;
    case TestStatus::Fail: return theme.fail;
    case TestStatus::Running: return theme.running;
    case TestStatus::Info: return theme.info;
    default: return theme.muted;
  }
}

// Returns the option after `current` (wrapping), or the first one if `current` is unknown.
template <size_t N>
int NextOption(const int options[N], int current) {
  for (size_t i = 0; i < N; i++) {
    if (options[i] == current) return options[(i + 1) % N];
  }
  return options[0];
}

std::string Duration(int seconds) {
  if (seconds <= 0) return "Never";
  if (seconds < 60) return std::to_string(seconds) + "s";
  return std::to_string(seconds / 60) + "m";
}

std::string OnOff(bool on) { return on ? "On" : "Off"; }

std::string Bytes(uint64_t bytes) {
  char buf[16];
  if (bytes >= (1ULL << 30)) {
    snprintf(buf, sizeof(buf), "%.1fG", bytes / double(1ULL << 30));
  } else if (bytes >= (1ULL << 20)) {
    snprintf(buf, sizeof(buf), "%lluM", bytes >> 20);
  } else {
    snprintf(buf, sizeof(buf), "%lluK", bytes >> 10);
  }
  return buf;
}

std::string LocalTime() {
  if (!Network::GetInstance().TimeValid()) return "";
  time_t now = time(nullptr);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", localtime(&now));
  return buf;
}

std::string ClockText() {
  if (!Network::GetInstance().TimeValid()) return "-";
  time_t now = time(nullptr);
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", localtime(&now));
  return buf;
}

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

MenuItem Item(MenuItem::Kind kind, const std::string& label) {
  MenuItem item;
  item.kind = kind;
  item.label = label;
  return item;
}

MenuItem Submenu(const std::string& label, std::function<MenuItems()> build) {
  MenuItem item = Item(MenuItem::Kind::Submenu, label);
  item.build_submenu = build;
  return item;
}

MenuItem Toggle(const std::string& label, std::function<bool()> get, std::function<void()> flip) {
  MenuItem item = Item(MenuItem::Kind::Toggle, label);
  item.toggle = get;
  item.on_select = flip;
  return item;
}

MenuItem Back(const char* label = Lang::Strings::BACK) { return Item(MenuItem::Kind::Back, label); }

MenuItem WithIcon(MenuItem item, void (*icon)(Canvas&, int, int, uint16_t)) {
  item.icon = icon;
  return item;
}

MenuItem Info(const std::string& label, std::function<std::string()> value) {
  MenuItem item = Item(MenuItem::Kind::Info, label);
  item.value = value;
  return item;
}

}  // namespace

void App::Start() {
  // Room for long JSON replies; never stall the main loop when no host is reading.
  Serial.setTxBufferSize(4096);
  // USB uploads from the SD file manager arrive in blocks that must fit this queue.
  Serial.setRxBufferSize(SerialFs::kRxBufferBytes);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  HeapGuard::Begin();
  HeapGuard::Phase("boot:start");
  DeviceConfig::Get().Load();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"start\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());

  auto& board = Board::GetInstance();
  board.Initialize();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"board\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
  HeapGuard::Phase("boot:board");
  HeapGuard::Check("boot:board");
  // Needs the SD card, so it must run after Initialize() creates it, and before the display's
  // first draw so a saved project is already loaded.
  ProjectRuntime::Get().Begin();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"project\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
  HeapGuard::Phase("boot:project");
  HeapGuard::Check("boot:project");

  auto display = board.GetDisplay();
  display->SetContentRenderer([this](Canvas& canvas, int x, int y, int w, int h, const Theme& theme) {
    if (menu_.IsOpen()) {
      menu_.Draw(canvas, x, y, w, h, theme);
    } else {
      DrawHome(canvas, x, y, w, h, theme);
    }
  });
  display->SetConnectivityProvider([]() {
    const auto& config = DeviceConfig::Get();
    auto& network = Network::GetInstance();
    ConnectivityStatus status;
    status.wifi_on = config.wifi_on;
    if (network.WifiStatus() == "Connected") {
      const int rssi = network.WifiRssi();
      status.wifi_bars = rssi > -60 ? 3 : (rssi > -72 ? 2 : 1);
    }
    status.ble_on = config.ble_on;
    return status;
  });
  display->SetOverlayRenderer([this](Canvas& canvas, int, int, int w, int h, const Theme& theme) {
    DrawOverlay(canvas, w, h, theme);
  });
  splash_until_ = millis() + kSplashMs;
  display->SetupUI();
  display->SetStatus("");
  display->UpdateStatusBar(true);
  display->Loop();  // Splash while the backlight fades in
  Serial.printf("{\"event\":\"boot\",\"stage\":\"ui\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
  HeapGuard::Phase("boot:ui");
  HeapGuard::Check("boot:ui");
  if (ProjectRuntime::Get().SafeMode()) display->ShowNotification("Project safe mode", 6000);

  menu_.on_change = [this]() {
    Board::GetInstance().SetMenuOpen(menu_.IsOpen());
    Board::GetInstance().GetDisplay()->Invalidate();
  };

  board.GetPowerButton().OnPressDown([this]() { OnButtonPressed(kButtonPower); });
  board.GetVolumeUpButton().OnPressDown([this]() { OnButtonPressed(kButtonUp); });
  board.GetVolumeDownButton().OnPressDown([this]() { OnButtonPressed(kButtonDown); });
  board.SetMenuKeyHandler([this](MenuKey key) { OnMenuKey(key); });
  // Holding Power on a hold-to-confirm row runs that action instead of powering off.
  board.SetPowerHoldInterceptor([this]() {
    const MenuItem* item = menu_.IsOpen() ? menu_.SelectedItem() : nullptr;
    return item && item->kind == MenuItem::Kind::Hold;
  });

  auto& tests = HwTest::GetInstance();
  tests.SetOnChange([display]() { display->Invalidate(); });
  tests.Begin();

  ApplySettings();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"settings\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
  HeapGuard::Phase("boot:settings");
  HeapGuard::Check("boot:settings");
  Network::GetInstance().Begin();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"network\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
  HeapGuard::Phase("boot:network");
  HeapGuard::Check("boot:network");

  auto& console = ConsoleClient::GetInstance();
  console.SetStateProvider([this]() { return ReportState(); });
  console.SetTelemetryProvider([this]() { return ReportTelemetry(); });
  console.SetApplyHandler([this](const ConsoleClient::Values& values) { ApplyRemoteSettings(values); });
  console.SetCommandHandler([this](const ConsoleClient::Command& command) { return HandleConsoleCommand(command); });
  console.SetOnChange([display]() { display->Invalidate(); });
  console.Begin();
  Serial.printf("{\"event\":\"boot\",\"stage\":\"ready\",\"heap\":%lu}\n", (unsigned long)ESP.getFreeHeap());
  HeapGuard::Phase("boot:ready");
  HeapGuard::Check("boot:ready");
}

void App::ApplySettings() {
  auto& board = Board::GetInstance();
  const auto& config = DeviceConfig::Get();
  board.GetDisplay()->SetRotate180(config.rotate);
  board.GetDisplay()->SetShowBatteryPercent(config.battery_percent);
  board.ApplyLed();
  board.ApplyPowerHold();
}

void App::OnButtonPressed(uint8_t bit) {
  last_input_ = millis();
  LedFeedback::GetInstance().OnButtonPress();
  if (DeviceConfig::Get().button_sound && !Board::GetInstance().IsSleeping()) {
    HwTest::GetInstance().Beep();
  }

  buttons_pressed_ |= bit;
  const bool all = buttons_pressed_ == (kButtonPower | kButtonUp | kButtonDown);
  std::string detail;
  if (all) {
    detail = "PWR + -";
  } else {
    if (buttons_pressed_ & kButtonPower) detail += "PWR ";
    if (buttons_pressed_ & kButtonUp) detail += "+ ";
    if (buttons_pressed_ & kButtonDown) detail += "-";
  }
  HwTest::GetInstance().SetResult(kTestButtons, all ? TestStatus::Ok : TestStatus::Info, detail);
}

void App::OnMenuKey(MenuKey key) {
  last_input_ = millis();
  switch (key) {
    case MenuKey::Toggle:
      if (menu_.IsOpen() && menu_.Title() == Lang::Strings::HW_CHECK) {
        menu_.Close();
      } else {
        menu_.Open(Lang::Strings::HW_CHECK, [this]() { return BuildHwCheckMenu(); });
      }
      break;
    case MenuKey::Up:
      menu_.Next();
      break;
    case MenuKey::Down:
      menu_.Previous();
      break;
    case MenuKey::Select:
      if (menu_.IsOpen()) {
        menu_.Select();
      } else {
        menu_.Open(Lang::Strings::MENU, [this]() { return BuildMainMenu(); });
      }
      break;
  }
  Board::GetInstance().GetDisplay()->Invalidate();
}

void App::Loop() {
  auto& board = Board::GetInstance();
  board.Loop();
  HandleSerial();

  const uint32_t now = millis();
  if (now - last_clock_tick_ >= 1000) {
    last_clock_tick_ = now;
    OnClockTick();
  }

  const bool busy = HwTest::GetInstance().IsBusy();
  LedFeedback::GetInstance().Loop(now, busy || ProjectRuntime::Get().OwnsLed());
  Network::GetInstance().Loop(now);
  WebPortal::GetInstance().Loop(now);
  ConsoleClient::GetInstance().Loop(now);
  SerialFs::Get().Loop(now);
  if (ProjectRuntime::Get().Busy() || ConsoleClient::GetInstance().OtaProgress() >= 0) {
    menu_.Close();
    board.WakeUp();
  }
  UpdatePowerHold(now);
  CheckHeap(now);

  if (splash_until_ != 0) {
    board.GetDisplay()->Invalidate();
    if (now >= splash_until_) splash_until_ = 0;
  }

  if (menu_.IsOpen()) {
    if (menu_.Animate(now)) board.GetDisplay()->Invalidate();

    // Live values: meters while a check runs, lists that fill in.
    if (busy && now - last_live_redraw_ >= 100) {
      last_live_redraw_ = now;
      board.GetDisplay()->Invalidate();
    }
    if (!menu_.IsEditing() && now - last_menu_refresh_ >= 1000) {
      last_menu_refresh_ = now;
      menu_.Refresh();
      board.GetDisplay()->Invalidate();
    }
    if (!busy && now - last_input_ >= kMenuAutoCloseMs) menu_.Close();
  }

  if (!menu_.IsOpen() && (ProjectRuntime::Get().Busy() || ProjectRuntime::Get().Id() != "none") && now - last_live_redraw_ >= 33) {
    last_live_redraw_ = now;
    board.GetDisplay()->Invalidate();
  }
  UpdateStatus();
  board.GetDisplay()->Loop();
  delay(5);
}

void App::UpdatePowerHold(uint32_t now) {
  (void)now;
  auto& board = Board::GetInstance();
  auto& power = board.GetPowerButton();
  const float previous = hold_progress_;

  if (!board.IsPowerHoldActive()) {
    hold_progress_ = 0;
    hold_done_ = false;
  } else {
    const uint32_t held = power.HeldMs();
    const MenuItem* item = menu_.IsOpen() ? menu_.SelectedItem() : nullptr;
    const bool confirm = item && item->kind == MenuItem::Kind::Hold && !menu_.IsEditing();
    const uint32_t duration =
        confirm ? item->hold_ms : static_cast<uint32_t>(DeviceConfig::Get().power_hold_ms);

    if (held < Board::kPowerHoldRingDelayMs) {
      hold_progress_ = 0;
    } else {
      hold_progress_ = std::min(1.0f, static_cast<float>(held) / duration);
      hold_label_ = confirm ? item->label : Lang::Strings::POWER_OFF;
    }
    if (confirm && held >= duration && !hold_done_) {
      hold_done_ = true;
      last_input_ = millis();
      if (item->on_select) item->on_select();
    }
  }

  if (hold_progress_ != previous) board.GetDisplay()->Invalidate();
}

void App::DrawOverlay(Canvas& c, int w, int h, const Theme& theme) {
  if (splash_until_ != 0) {
    // Splash: name and version with a loading bar.
    const float t = 1.0f - static_cast<float>(splash_until_ - std::min(millis(), splash_until_)) / kSplashMs;
    c.Fill(Color(0x000000));
    c.TextCentered(w / 2, h / 2 - 22, "ESP32", Color(0xFFFFFF), 3);
    c.TextCentered(w / 2, h / 2 + 4, "CONSOLE", Color(0x80A0FF));
    c.TextCentered(w / 2, h - 18, "v" FIRMWARE_VERSION, Color(0x707070));
    const int bw = 60, bx = (w - bw) / 2, by = h / 2 + 18;
    c.FillRoundRect(bx, by, bw, 3, 1, Color(0x303038));
    c.FillRoundRect(bx, by, std::max(3, static_cast<int>(bw * t)), 3, 1, Color(0x80A0FF));
    return;
  }

  const int update = ConsoleClient::GetInstance().OtaProgress();
  if (update >= 0) {
    const int cx = w / 2, cy = h / 2;
    c.FillRoundRect(cx - 54, cy - 35, 108, 70, 8, theme.background);
    c.RoundRect(cx - 54, cy - 35, 108, 70, 8, theme.muted);
    c.TextCentered(cx, cy - 23, "Firmware update", theme.text);
    const std::string percent = std::to_string(update) + "%";
    c.TextCentered(cx, cy - 5, percent.c_str(), theme.info, 2);
    c.FillRoundRect(cx - 42, cy + 20, 84, 5, 2, theme.selected);
    if (update > 0) c.FillRoundRect(cx - 42, cy + 20, std::max(2, 84 * update / 100), 5, 2, theme.info);
    return;
  }

  if (hold_progress_ <= 0) return;
  // Hold ring: fills clockwise from the top; releasing early cancels.
  const int cx = w / 2, cy = h / 2 + 4, r = 26;
  c.FillRoundRect(cx - 44, cy - 38, 88, 80, 8, theme.background);
  c.RoundRect(cx - 44, cy - 38, 88, 80, 8, theme.muted);
  c.Ring(cx, cy - 6, r, 5, theme.selected);
  const int sweep = static_cast<int>(hold_progress_ * 360);
  const uint16_t color = hold_label_ == Lang::Strings::POWER_OFF ? theme.fail : theme.info;
  if (sweep >= 359) {
    c.Ring(cx, cy - 6, r, 5, color);
  } else if (sweep > 0) {
    c.Arc(cx, cy - 6, r, 270, (270 + sweep) % 360, 5, color);
  }
  const std::string percent = std::to_string(static_cast<int>(hold_progress_ * 100)) + "%";
  c.TextCentered(cx, cy - 9, percent.c_str(), theme.text);
  c.TextMarquee(cx - 38, cy + 26, 76, hold_label_.c_str(), theme.text, 1, true);
}

void App::OnClockTick() {
  auto& board = Board::GetInstance();
  board.OnClockTick(menu_.IsOpen() || HwTest::GetInstance().IsBusy() || ProjectRuntime::Get().Busy() || ConsoleClient::GetInstance().OtaProgress() >= 0);

  // Redraw the home screen when its network details change.
  const std::string signature = HomeSignature();
  if (signature != home_signature_) {
    home_signature_ = signature;
    board.GetDisplay()->Invalidate();
    board.GetDisplay()->UpdateStatusBar();
  }
}

std::string App::HomeSignature() const {
  auto& network = Network::GetInstance();
  auto& console = ConsoleClient::GetInstance();
  return network.WifiStatus() + "|" + network.WifiSsid() + "|" + network.WifiIp() + "|" +
         (network.AccessPointActive() ? network.AccessPointIp() : "") + "|" + console.StateText() +
         "|" + console.Code() + "|" + console.Name() + "|" + console.Server() + "|" + console.LastError();
}

void App::DrawHome(Canvas& c, int x, int y, int w, int h, const Theme& theme) {
  auto& network = Network::GetInstance();
  const int cx = x + w / 2;
  auto& console = ConsoleClient::GetInstance();
  // A saved project must not obscure pairing or a failed pairing connection.
  const bool pairing = network.State() == Network::WifiState::Setup ||
      !console.Code().empty() ||
      (network.State() == Network::WifiState::Connected && !console.HasLink());
  if (!pairing && ProjectRuntime::Get().Draw(c, x, y, w, h, theme)) return;
  const int line = Canvas::LineHeight() + 2;
  std::vector<std::pair<std::string, uint16_t>> lines;

  switch (network.State()) {
    case Network::WifiState::Off:
      return;
    case Network::WifiState::Setup:
      lines = {{"Join Wi-Fi", theme.muted},
               {network.DeviceName(), theme.text},
               {"", 0},
               {"then open", theme.muted},
               {network.AccessPointIp(), theme.info}};
      break;
    case Network::WifiState::Connecting:
      lines = {{"Connecting to", theme.muted}, {network.WifiSsid(), theme.text}};
      if (network.AccessPointActive()) {
        lines.push_back({"", 0});
        lines.push_back({"Setup: join", theme.muted});
        lines.push_back({network.DeviceName(), theme.text});
        lines.push_back({network.AccessPointIp(), theme.info});
      }
      break;
    case Network::WifiState::Connected: {
      if (!console.Code().empty()) {
        // Verification code for Devices -> Add Device in the web console.
        const std::string& code = console.Code();
        const std::string spaced = code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
        const int big = Canvas::LineHeight(2);
        int ty = y + (h - (line * 3 + big + 4)) / 2;
        c.TextCentered(cx, ty, "Add device", theme.muted);
        ty += line + 2;
        c.TextCentered(cx, ty, spaced.c_str(), theme.info, 2);
        ty += big + 4;
        c.TextCentered(cx, ty, "code on", theme.muted);
        ty += line;
        c.TextCentered(cx, ty, console.ServerHost().c_str(), theme.text);
        return;
      }
      if (!console.HasLink()) {
        if (console.Server().empty()) {
          lines = {{"Console not set", theme.info}, {"Devices > Add", theme.muted},
                   {"Connect via USB", theme.text}};
        } else if (!console.LastError().empty()) {
          lines = {{"Pairing failed", theme.fail}, {console.LastError(), theme.text},
                   {console.ServerHost(), theme.muted}, {"Retrying...", theme.info}};
        } else {
          lines = {{"Getting code...", theme.info}, {console.ServerHost(), theme.text}};
        }
        break;
      }
      // Linked with no project: the default display stays empty (status bar only).
      return;
    }
  }

  int ty = y + (h - static_cast<int>(lines.size()) * line) / 2;
  for (const auto& [text, color] : lines) {
    if (!text.empty()) {
      c.TextMarquee(x + 2, ty, w - 4, text.c_str(), color, 1, true);
    }
    ty += line;
  }
}

void App::UpdateStatus() {
  auto display = Board::GetInstance().GetDisplay();
  if (menu_.IsOpen()) {
    const bool checking =
        HwTest::GetInstance().IsBusy() && menu_.Title() == Lang::Strings::HW_CHECK;
    display->SetStatus(checking ? Lang::Strings::CHECKING : menu_.Title().c_str());
    return;
  }
  const bool show_clock = DeviceConfig::Get().clock_on && Network::GetInstance().TimeValid();
  display->SetStatus(show_clock ? ClockText().c_str() : "");
}

// ---------------------------------------------------------------------------
// Menus

MenuItems App::BuildMainMenu() {
  return {
      WithIcon(Submenu("Display", [this]() { return BuildDisplayMenu(); }), Icons::Display),
      WithIcon(Submenu("Sound & LED", [this]() { return BuildSoundMenu(); }), Icons::Sound),
      WithIcon(Submenu("Connectivity", [this]() { return BuildConnectivityMenu(); }), Icons::Wifi),
      WithIcon(Submenu("Console", [this]() { return BuildConsoleMenu(); }), Icons::Cloud),
      WithIcon(Submenu("Device", [this]() { return BuildDeviceMenu(); }), Icons::Gear),
      WithIcon(Submenu(Lang::Strings::HW_CHECK, [this]() { return BuildHwCheckMenu(); }), Icons::Check),
      Back(Lang::Strings::CLOSE),
  };
}

MenuItems App::BuildDisplayMenu() {
  auto& board = Board::GetInstance();
  auto display = board.GetDisplay();
  auto& config = DeviceConfig::Get();
  MenuItems items;

  MenuItem theme = Item(MenuItem::Kind::Choice, "Theme");
  theme.value = [display]() {
    return std::string(std::string(display->GetTheme()) == "dark" ? "Dark" : "Light");
  };
  theme.on_select = [display]() {
    display->SetTheme(std::string(display->GetTheme()) == "dark" ? "light" : "dark");
  };
  items.push_back(theme);

  MenuItem brightness = Item(MenuItem::Kind::Adjust, "Brightness");
  brightness.value = []() {
    Settings settings("display");
    return std::to_string(settings.GetInt("brightness", Backlight::kDefaultBrightness)) + "%";
  };
  brightness.on_adjust = [&board](int step) {
    int current = Backlight::kDefaultBrightness;
    {
      Settings settings("display");
      current = settings.GetInt("brightness", Backlight::kDefaultBrightness);
    }
    board.GetBacklight()->SetBrightness(std::clamp(current + step * 10, 10, 100), true);
  };
  brightness.progress = []() {
    Settings settings("display");
    return (settings.GetInt("brightness", Backlight::kDefaultBrightness) - 10) * 100 / 90;
  };
  items.push_back(brightness);

  MenuItem sleep = Item(MenuItem::Kind::Choice, "Auto sleep");
  sleep.value = [&config]() { return Duration(config.sleep_seconds); };
  sleep.on_select = [&config]() {
    config.sleep_seconds = NextOption<5>(kSleepOptions, config.sleep_seconds);
    config.Save();
  };
  items.push_back(sleep);

  MenuItem power_off = Item(MenuItem::Kind::Choice, "Auto off");
  power_off.value = [&config]() { return Duration(config.power_off_seconds); };
  power_off.on_select = [&config]() {
    config.power_off_seconds = NextOption<4>(kPowerOffOptions, config.power_off_seconds);
    config.Save();
  };
  items.push_back(power_off);

  items.push_back(Toggle("Rotate 180", [&config]() { return config.rotate; }, [this, &config]() {
    config.rotate = !config.rotate;
    config.Save();
    ApplySettings();
  }));

  items.push_back(Back());
  return items;
}

MenuItems App::BuildSoundMenu() {
  auto& board = Board::GetInstance();
  auto& config = DeviceConfig::Get();
  MenuItems items;

  MenuItem volume = Item(MenuItem::Kind::Adjust, "Volume");
  volume.value = [&board]() { return std::to_string(board.GetAudioCodec()->output_volume()); };
  volume.on_adjust = [&board](int step) {
    auto codec = board.GetAudioCodec();
    codec->SetOutputVolume(std::clamp(codec->output_volume() + step * 10, 0, 100));
    HwTest::GetInstance().Beep(1000, 120);
  };
  volume.progress = [&board]() { return board.GetAudioCodec()->output_volume(); };
  items.push_back(volume);

  items.push_back(Toggle("Button sound", [&config]() { return config.button_sound; }, [&config]() {
    config.button_sound = !config.button_sound;
    config.Save();
  }));

  items.push_back(Toggle("LED ring", [&config]() { return config.led_on; }, [this, &config]() {
    config.led_on = !config.led_on;
    config.Save();
    ApplySettings();
  }));

  items.push_back(Toggle("LED feedback", [&config]() { return config.led_feedback; }, [this, &config]() {
    config.led_feedback = !config.led_feedback;
    config.Save();
    ApplySettings();
  }));

  MenuItem led_level = Item(MenuItem::Kind::Adjust, "LED bright");
  led_level.value = [&config]() { return std::to_string(config.led_brightness) + "/8"; };
  led_level.on_adjust = [this, &config](int step) {
    config.led_brightness = std::clamp(config.led_brightness + step, 1, 8);
    config.led_on = true;
    config.Save();
    ApplySettings();
  };
  led_level.progress = [&config]() { return (config.led_brightness - 1) * 100 / 7; };
  items.push_back(led_level);

  MenuItem led_color = Item(MenuItem::Kind::Choice, "LED color");
  led_color.value = [&config]() {
    return std::string(kLedColorNames[config.led_color % kLedColorCount]);
  };
  led_color.on_select = [this, &config]() {
    config.led_color = (config.led_color + 1) % kLedColorCount;
    config.led_pixels.clear();  // One color for the whole ring replaces a per-LED console design.
    config.led_on = true;
    config.Save();
    ApplySettings();
  };
  items.push_back(led_color);

  items.push_back(Back());
  return items;
}

MenuItems App::BuildConnectivityMenu() {
  auto& config = DeviceConfig::Get();
  auto& network = Network::GetInstance();
  MenuItems items;

  items.push_back(Toggle("Wi-Fi", [&config]() { return config.wifi_on; }, [&config, &network]() {
    config.wifi_on = !config.wifi_on;
    config.Save();
    network.Apply();
    Board::GetInstance().GetDisplay()->UpdateStatusBar();
  }));

  MenuItem status = Info("Status", [&network]() { return network.WifiStatus(); });
  status.color = [&network](const Theme& theme) {
    switch (network.State()) {
      case Network::WifiState::Connected: return theme.ok;
      case Network::WifiState::Connecting: return theme.running;
      case Network::WifiState::Setup: return theme.info;
      default: return theme.muted;
    }
  };
  items.push_back(status);
  items.push_back(Info("Network", [&config]() {
    return config.wifi_ssid.empty() ? std::string("-") : config.wifi_ssid;
  }));
  items.push_back(Info("IP", [&network]() {
    const std::string ip = network.State() == Network::WifiState::Connected
                               ? network.WifiIp()
                               : (network.AccessPointActive() ? network.AccessPointIp() : "-");
    return ip;
  }));

  MenuItem scan = Submenu("Scan", [this]() { return BuildNetworksMenu(); });
  scan.submenu_title = "Networks";
  items.push_back(scan);

  items.push_back(Toggle("Bluetooth", [&config]() { return config.ble_on; }, [&config, &network]() {
    config.ble_on = !config.ble_on;
    config.Save();
    network.Apply();
    Board::GetInstance().GetDisplay()->UpdateStatusBar();
  }));
  items.push_back(Info(network.DeviceName(), nullptr));

  items.push_back(Toggle("Clock", [&config]() { return config.clock_on; }, [&config]() {
    config.clock_on = !config.clock_on;
    config.Save();
  }));

  MenuItem tz = Submenu("Time zone", [this]() { return BuildTimeZoneMenu(); });
  tz.value = [&config]() { return std::string(FindTimeZone(config.timezone.c_str()).label); };
  items.push_back(tz);
  items.push_back(Info("Time", []() { return ClockText(); }));

  items.push_back(Back());
  return items;
}

MenuItems App::BuildConsoleMenu() {
  auto& console = ConsoleClient::GetInstance();
  MenuItems items;

  MenuItem status = Info("Status", [&console]() { return std::string(console.StateText()); });
  status.color = [&console](const Theme& theme) {
    switch (console.GetState()) {
      case ConsoleClient::State::Linked: return theme.ok;
      case ConsoleClient::State::Pairing: return theme.info;
      case ConsoleClient::State::Connecting: return theme.running;
      case ConsoleClient::State::Error: return theme.fail;
      default: return theme.muted;
    }
  };
  items.push_back(status);
  items.push_back(Info("Code", [&console]() {
    return console.Code().empty() ? std::string("-") : console.Code();
  }));
  items.push_back(Info("Name", [&console]() {
    return console.Name().empty() ? std::string("-") : console.Name();
  }));
  items.push_back(Info("Server", [&console]() {
    return console.Server().empty() ? std::string("-") : console.ServerHost();
  }));
  items.push_back(Info("Error", [&console]() {
    return console.LastError().empty() ? std::string("-") : console.LastError();
  }));
  if (console.OtaProgress() >= 0) {
    items.push_back(Info("Update", [&console]() { return std::to_string(console.OtaProgress()) + "%"; }));
  }

  if (console.Server().rfind("https://", 0) == 0) {
    items.push_back(Toggle("Skip cert check", [&console]() { return console.Insecure(); }, [&console]() {
      console.SetInsecure(!console.Insecure());
    }));
  }

  if (console.HasLink()) {
    items.push_back(Submenu("Unlink", [this, &console]() {
      return BuildConfirmMenu("Unlink", [this, &console]() {
        console.Unlink();
        menu_.Close();
      });
    }));
  }

  items.push_back(Back());
  return items;
}

MenuItems App::BuildNetworksMenu() {
  auto& network = Network::GetInstance();
  MenuItems items;
  const auto results = network.ScanResults();

  if (network.IsScanning() || results.empty()) {
    if (!network.IsScanning()) network.StartScan();
    items.push_back(Info(Lang::Strings::SCANNING, nullptr));
  } else {
    for (const auto& net : results) {
      const int rssi = net.rssi;
      MenuItem row = Info(net.ssid.empty() ? "(hidden)" : net.ssid,
                          [rssi]() { return std::to_string(rssi); });
      row.color = [rssi](const Theme& theme) {
        return rssi > -60 ? theme.ok : (rssi > -75 ? theme.running : theme.fail);
      };
      items.push_back(row);
    }
    MenuItem rescan = Item(MenuItem::Kind::Action, "Rescan");
    rescan.on_select = [&network]() { network.StartScan(); };
    items.push_back(rescan);
  }
  items.push_back(Back());
  return items;
}

MenuItems App::BuildTimeZoneMenu() {
  auto& config = DeviceConfig::Get();
  MenuItems items;
  for (const auto& zone : kTimeZones) {
    const std::string id = zone.id;
    MenuItem row = Item(MenuItem::Kind::Action, zone.label);
    row.color = [&config, id](const Theme& theme) {
      return FindTimeZone(config.timezone.c_str()).id == id ? theme.ok : theme.selected;
    };
    row.on_select = [&config, id]() {
      config.timezone = id;
      config.Save();
      Network::GetInstance().Apply();
    };
    items.push_back(row);
  }
  items.push_back(Back());
  return items;
}

MenuItems App::BuildDeviceMenu() {
  auto& board = Board::GetInstance();
  auto& config = DeviceConfig::Get();
  MenuItems items;

  items.push_back(Submenu("Battery", [this]() { return BuildBatteryMenu(); }));
  items.push_back(Submenu("SD card", [this]() { return BuildSdCardMenu(); }));

  MenuItem hold = Item(MenuItem::Kind::Choice, "Power hold");
  hold.value = [&config]() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1fs", config.power_hold_ms / 1000.0f);
    return std::string(buf);
  };
  hold.on_select = [this, &config]() {
    config.power_hold_ms = NextOption<3>(kHoldOptions, config.power_hold_ms);
    config.Save();
    ApplySettings();
  };
  items.push_back(hold);

  items.push_back(Submenu("About", [this]() { return BuildAboutMenu(); }));

  items.push_back(Submenu("Restart", [this]() {
    return BuildConfirmMenu("Restart", []() {
      Serial.flush();
      delay(100);
      ESP.restart();
    });
  }));

  items.push_back(Submenu("Power off", [this, &board]() {
    return BuildConfirmMenu("Power off", [&board]() { board.PowerOff(); });
  }));

  items.push_back(Submenu("Factory reset", []() {
    MenuItems confirm;
    confirm.push_back(Back(Lang::Strings::CANCEL));
    MenuItem reset = Item(MenuItem::Kind::Hold, "Hold to reset");
    reset.hold_ms = 2000;
    reset.on_select = []() {
      DeviceConfig::FactoryReset();
      delay(200);
      ESP.restart();
    };
    confirm.push_back(reset);
    return confirm;
  }));

  items.push_back(Back());
  return items;
}

MenuItems App::BuildBatteryMenu() {
  auto power = Board::GetInstance().GetPowerManager();
  auto& config = DeviceConfig::Get();
  MenuItems items;

  items.push_back(Info("Voltage", [power]() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.2fV", power->GetBatteryVoltage());
    return std::string(buf);
  }));
  MenuItem level =
      Info("Level", [power]() { return std::to_string(power->GetBatteryLevel()) + "%"; });
  level.color = [power](const Theme& theme) {
    const int l = power->GetBatteryLevel();
    return l >= 40 ? theme.ok : (l >= 20 ? theme.running : theme.fail);
  };
  items.push_back(level);
  items.push_back(Info("Power", [power]() {
    return std::string(power->IsCharging() ? "Charging" : "Not charging");
  }));
  items.push_back(Info("10m trend", [power]() {
    int change_mv = 0;
    uint32_t minutes = 0;
    if (!power->GetVoltageChange(change_mv, minutes)) return std::string("Collecting");
    char buf[24];
    snprintf(buf, sizeof(buf), "%+dmV / %lum", change_mv, static_cast<unsigned long>(minutes));
    return std::string(buf);
  }));

  items.push_back(Toggle("Show %", [&config]() { return config.battery_percent; }, [this, &config]() {
    config.battery_percent = !config.battery_percent;
    config.Save();
    ApplySettings();
  }));

  items.push_back(Back());
  return items;
}

MenuItems App::BuildSdCardMenu() {
  auto sd = Board::GetInstance().GetSdCard();
  MenuItems items;

  MenuItem status =
      Info("Status", [sd]() { return std::string(sd->ProblemLabel()); });
  status.color = [sd](const Theme& theme) { return sd->mounted() ? theme.ok : theme.fail; };
  items.push_back(status);
  items.push_back(Info("Total", [sd]() { return sd->mounted() ? Bytes(sd->total_bytes()) : "-"; }));
  items.push_back(Info("Used", [sd]() { return sd->mounted() ? Bytes(sd->used_bytes()) : "-"; }));

  MenuItem mount = Item(MenuItem::Kind::Action, "Mount");
  mount.on_select = []() { ProjectRuntime::Get().RemountSd(); };
  items.push_back(mount);

  items.push_back(Back());
  return items;
}

MenuItems App::BuildAboutMenu() {
  MenuItems items;
  items.push_back(Info("Firmware", []() { return std::string(FIRMWARE_VERSION); }));
  items.push_back(Info("Chip", []() { return std::string(ESP.getChipModel()); }));
  items.push_back(Info("MAC", []() {
    const uint64_t mac = ESP.getEfuseMac();
    char buf[12];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X", uint8_t(mac >> 24), uint8_t(mac >> 32),
             uint8_t(mac >> 40));
    return std::string(buf);
  }));
  items.push_back(Info("Flash", []() { return Bytes(ESP.getFlashChipSize()); }));
  items.push_back(Info("PSRAM", []() { return Bytes(ESP.getPsramSize()); }));
  items.push_back(Info("Free heap", []() { return Bytes(ESP.getFreeHeap()); }));
  items.push_back(Info("Uptime", []() {
    const unsigned long s = millis() / 1000;
    char buf[12];
    if (s >= 3600) {
      snprintf(buf, sizeof(buf), "%luh%02lum", s / 3600, s / 60 % 60);
    } else {
      snprintf(buf, sizeof(buf), "%lum%02lus", s / 60, s % 60);
    }
    return std::string(buf);
  }));
  items.push_back(Back());
  return items;
}

MenuItems App::BuildHwCheckMenu() {
  auto& tests = HwTest::GetInstance();
  MenuItems items;

  for (int i = 0; i < kTestCount; i++) {
    const auto id = static_cast<TestId>(i);
    MenuItem row = Item(MenuItem::Kind::Action, HwTest::Name(id));
    row.color = [&tests, id](const Theme& theme) { return StatusColor(tests.Get(id).status, theme); };
    row.value = [&tests, id]() {
      const auto result = tests.Get(id);
      return result.status == TestStatus::Idle ? std::string("-") : result.detail;
    };
    if (id == kTestMic || id == kTestSpeaker) {
      row.level = [&tests, id]() {
        return tests.Get(id).status == TestStatus::Running ? tests.mic_level() : -1;
      };
    }
    row.on_select = [this, &tests, id]() {
      if (id == kTestButtons) {
        buttons_pressed_ = 0;
        tests.SetResult(kTestButtons, TestStatus::Info, "press all");
      } else {
        tests.Run(id);
      }
    };
    items.push_back(row);
  }

  MenuItem all = Item(MenuItem::Kind::Action, Lang::Strings::CHECK_ALL);
  all.on_select = [&tests]() { tests.RunAll(); };
  items.push_back(all);

  items.push_back(Back());
  return items;
}

MenuItems App::BuildConfirmMenu(const char* action_label, std::function<void()> action) {
  MenuItems items;
  items.push_back(Back(Lang::Strings::CANCEL));
  MenuItem confirm = Item(MenuItem::Kind::Action, action_label);
  confirm.color = [](const Theme& theme) { return theme.fail; };
  confirm.on_select = action;
  items.push_back(confirm);
  return items;
}

// ---------------------------------------------------------------------------
// Serial

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
  auto& tests = HwTest::GetInstance();
  auto& config = DeviceConfig::Get();
  auto& network = Network::GetInstance();
  auto display = board.GetDisplay();
  const auto args = Split(line);
  if (args.empty()) return;
  const std::string& cmd = args[0];
  if (cmd == "fs") {
    SerialFs::Get().Handle(line);
    return;
  }
  auto num = [&](size_t i, int fallback) {
    return i < args.size() ? atoi(args[i].c_str()) : fallback;
  };
  auto ok = [&]() { Serial.printf("{\"ok\":true,\"command\":\"%s\"}\n", cmd.c_str()); };

  last_input_ = millis();
  board.WakeUp();

  if (cmd == "info") {
    PrintInfo();
  } else if (cmd == "menu") {
    if (args.size() > 1 && args[1] == "close") {
      menu_.Close();
    } else if (args.size() > 1 && args[1] == "check") {
      menu_.Open(Lang::Strings::HW_CHECK, [this]() { return BuildHwCheckMenu(); });
    } else {
      menu_.Open(Lang::Strings::MENU, [this]() { return BuildMainMenu(); });
    }
    ok();
  } else if (cmd == "key" && args.size() > 1) {
    // Simulated buttons for remote control: next, prev, select
    if (args[1] == "next") OnMenuKey(MenuKey::Up);
    if (args[1] == "prev") OnMenuKey(MenuKey::Down);
    if (args[1] == "select") OnMenuKey(MenuKey::Select);
    Serial.printf("{\"menu\":\"%s\",\"open\":%s,\"editing\":%s}\n", menu_.Title().c_str(),
                  menu_.IsOpen() ? "true" : "false", menu_.IsEditing() ? "true" : "false");
  } else if (cmd == "test") {
    TestId id;
    if (args.size() < 2 || args[1] == "all") {
      tests.RunAll();
      ok();
    } else if (HwTest::FromKey(args[1], id)) {
      tests.Run(id);
      ok();
    } else {
      Serial.println("{\"ok\":false,\"error\":\"unknown test\"}");
    }
  } else if (cmd == "results") {
    for (int i = 0; i < kTestCount; i++) {
      const auto id = static_cast<TestId>(i);
      const auto r = tests.Get(id);
      tests.SetResult(id, r.status, r.detail);  // Re-prints the line
    }
  } else if (cmd == "tone") {
    tests.PlayTone(num(1, 1000), num(2, 1000));
    ok();
  } else if (cmd == "mic") {
    tests.Record(num(1, 1000));
    ok();
  } else if (cmd == "volume" && args.size() > 1) {
    board.GetAudioCodec()->SetOutputVolume(num(1, 70));
    display->ShowNotification(std::string(Lang::Strings::VOLUME) +
                              std::to_string(board.GetAudioCodec()->output_volume() / 10));
    ok();
  } else if (cmd == "gain" && args.size() > 1) {
    board.GetAudioCodec()->SetInputGain(static_cast<float>(num(1, 30)));
    ok();
  } else if (cmd == "led") {
    auto ring = board.GetLedRing();
    if (args.size() >= 6 && args[1] == "pixel") {
      ring->SetPixel(num(2, 0), {(uint8_t)num(3, 0), (uint8_t)num(4, 0), (uint8_t)num(5, 0)});
    } else if (args.size() >= 4) {
      ring->SetAll({(uint8_t)num(1, 0), (uint8_t)num(2, 0), (uint8_t)num(3, 0)});
    } else {
      ring->Clear();
    }
    ring->Show();
    ok();
  } else if (cmd == "wifi") {
    // Plain `wifi` only reports; it must not disturb a connection in progress.
    if (args.size() >= 2 && (args[1] == "off" || args[1] == "on")) {
      config.wifi_on = args[1] == "on";
      config.Save();
      network.Apply();
    } else if (args.size() >= 2) {
      // wifi <ssid> [password]; the SSID cannot contain spaces here (use the setup page).
      network.Connect(args[1], args.size() >= 3 ? Rest(line, 2) : "");
    }
    display->UpdateStatusBar();
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
    display->UpdateStatusBar();
    ok();
  } else if (cmd == "tz" && args.size() > 1) {
    config.timezone = FindTimeZone(args[1].c_str()).id;  // IANA id, e.g. Europe/Berlin
    config.Save();
    network.Apply();
    ok();
  } else if (cmd == "time" && args.size() > 1) {
    struct timeval tv = {.tv_sec = static_cast<time_t>(atoll(args[1].c_str())), .tv_usec = 0};
    settimeofday(&tv, nullptr);
    ok();
  } else if (cmd == "status" && args.size() > 1) {
    display->SetStatus(Rest(line, 1).c_str());
    ok();
  } else if (cmd == "notify" && args.size() > 1) {
    display->ShowNotification(Rest(line, 1));
    ok();
  } else if (cmd == "theme" && args.size() > 1) {
    display->SetTheme(args[1].c_str());
    ok();
  } else if (cmd == "backlight" && args.size() > 1) {
    board.GetBacklight()->SetBrightness(num(1, Backlight::kDefaultBrightness), true);
    ok();
  } else if (cmd == "factoryreset") {
    ok();
    DeviceConfig::FactoryReset();
    Serial.flush();
    delay(200);
    ESP.restart();
  } else if (cmd == "reboot") {
    ok();
    Serial.flush();
    ESP.restart();
  } else if (cmd == "poweroff") {
    ok();
    board.PowerOff();
  } else if (cmd == "help") {
    Serial.println(
        "info | menu [open|check|close] | key next|prev|select | "
        "test [all|battery|memory|buttons|led|sd|codec|mic|speaker|wifi|ble] | results | "
        "tone <hz> <ms> | mic <ms> | volume <0-100> | gain <0-37> | "
        "led <r> <g> <b> | led pixel <i> <r> <g> <b> | led off | "
        "wifi <ssid> [password] | wifi on|off | console <url>|off|unlink|insecure [on|off]|status | ble on|off | tz <Europe/Berlin|...> | time <epoch> | "
        "status <text> | notify <text> | theme <light|dark> | backlight <0-100> | "
        "fs ls|get|put|rm|mkdir|mv|mount|unmount|format|info … | "
        "reboot | poweroff | factoryreset");
  } else {
    Serial.println("{\"ok\":false,\"error\":\"unknown command\"}");
  }
}

namespace {
// Appends "&t.<key>=<status>|<detail>" for every non-idle check, matching HwTest's TestStatus.
void AppendTestResults(std::string& body) {
  static constexpr const char* kStatusKeys[] = {"idle", "running", "ok", "fail", "info"};
  auto& tests = HwTest::GetInstance();
  for (int i = 0; i < kTestCount; i++) {
    const auto id = static_cast<TestId>(i);
    const auto result = tests.Get(id);
    if (result.status == TestStatus::Idle) continue;
    std::string detail = result.detail;
    // '|' and '&' would break the mini-format the console parses; strip them, they never occur
    // in real check output.
    detail.erase(std::remove_if(detail.begin(), detail.end(), [](char c) { return c == '|' || c == '&'; }),
                 detail.end());
    body += std::string("&t.") + HwTest::Key(id) + "=" + kStatusKeys[static_cast<int>(result.status)] + "|" + detail;
  }
}
}  // namespace

std::string App::ReportState() {
  auto& board = Board::GetInstance();
  const auto& config = DeviceConfig::Get();
  int brightness = Backlight::kDefaultBrightness;
  {
    Settings settings("display");
    brightness = settings.GetInt("brightness", Backlight::kDefaultBrightness);
  }

  auto b = [](bool v) { return v ? "1" : "0"; };
  char buf[512];
  snprintf(buf, sizeof(buf),
           "s.volume=%d&s.brightness=%d&s.theme=%s&s.button_sound=%s"
           "&s.led_on=%s&s.led_feedback=%s&s.led_brightness=%d&s.led_color=%d&s.sleep_s=%d"
           "&s.power_off_s=%d&s.rotate=%s&s.ble_on=%s&s.clock_on=%s&s.tz=%s&s.bat_pct=%s",
           board.GetAudioCodec()->output_volume(), brightness, board.GetDisplay()->GetTheme(),
           b(config.button_sound), b(config.led_on), b(config.led_feedback), config.led_brightness,
           config.led_color, config.sleep_seconds, config.power_off_seconds, b(config.rotate),
           b(config.ble_on), b(config.clock_on), config.timezone.c_str(), b(config.battery_percent));
  // Time zone ids only contain letters and '/', '_': safe in a form body except '/'.
  std::string body = buf;
  body += "&s.project=" + ProjectRuntime::Get().Id() + "&project_api=5";
  body += "&project_version=" + ProjectRuntime::Get().Version();
  body += "&project_sha256=" + ProjectRuntime::Get().Sha256();
  body += "&project_safe=" + std::string(ProjectRuntime::Get().SafeMode() ? "1" : "0");
  body += "&sd=" + ProjectRuntime::Get().SdReport();
  body += "&s.led_mode=" + config.led_mode + "&s.led_speed=" + std::to_string(config.led_speed);
  body += "&s.led_pixels=" + config.led_pixels;  // [0-9a-f:,] only: safe in a form body
  for (size_t pos = 0; (pos = body.find('/', pos)) != std::string::npos;) body.replace(pos, 1, "%2F");

  const auto networks = Network::GetInstance().SavedNetworks();
  body += "&wifi1=" + networks[0] + "&wifi2=" + networks[1];
  AppendTestResults(body);
  return body;
}

std::string App::ReportTelemetry() {
  auto& board = Board::GetInstance();
  int battery = -1;
  bool charging = false, discharging = false;
  if (!board.GetBatteryLevel(battery, charging, discharging)) battery = -1;
  const int battery_mv = static_cast<int>(std::lround(board.GetPowerManager()->GetBatteryVoltage() * 1000.0f));
  char buf[120];
  snprintf(buf, sizeof(buf), "battery=%d&battery_mv=%d&charging=%s&heap=%lu&uptime=%lu", battery,
           battery_mv, charging ? "1" : "0", (unsigned long)ESP.getFreeHeap(), millis() / 1000);
  return std::string(buf) + "&reset_reason=" + std::to_string(esp_reset_reason());
}

void App::ApplyRemoteSettings(const ConsoleClient::Values& values) {
  auto& board = Board::GetInstance();
  auto& config = DeviceConfig::Get();
  auto display = board.GetDisplay();
  bool network_changed = false;

  for (const auto& [key, value] : values) {
    const int n = atoi(value.c_str());
    const bool on = value == "1";
    if (key == "volume") {
      board.GetAudioCodec()->SetOutputVolume(std::clamp(n, 0, 100));
    } else if (key == "brightness") {
      Settings settings("display");
      if (settings.GetInt("brightness", Backlight::kDefaultBrightness) != n) {
        board.GetBacklight()->SetBrightness(std::clamp(n, 10, 100), true);
      }
    } else if (key == "theme") {
      if (value != display->GetTheme()) display->SetTheme(value.c_str());
    } else if (key == "button_sound") {
      config.button_sound = on;
    } else if (key == "led_on") {
      config.led_on = on;
    } else if (key == "led_feedback") {
      config.led_feedback = on;
    } else if (key == "led_brightness") {
      config.led_brightness = std::clamp(n, 1, 8);
    } else if (key == "led_color") {
      config.led_color = std::clamp(n, 0, kLedColorCount - 1);
    } else if (key == "led_mode") {
      if (LedPattern::ValidMode(value)) config.led_mode = value;
    } else if (key == "led_speed") {
      config.led_speed = std::clamp(n, 1, 10);
    } else if (key == "led_pixels") {
      config.led_pixels.clear();
      for (char ch : value.substr(0, 160)) if (isxdigit(static_cast<unsigned char>(ch)) || ch == ':' || ch == ',') config.led_pixels += tolower(ch);
    } else if (key == "sleep_s") {
      config.sleep_seconds = std::max(0, n);
    } else if (key == "power_off_s") {
      config.power_off_seconds = std::max(0, n);
    } else if (key == "rotate") {
      config.rotate = on;
    } else if (key == "ble_on") {
      network_changed |= config.ble_on != on;
      config.ble_on = on;
    } else if (key == "clock_on") {
      config.clock_on = on;
    } else if (key == "tz") {
      const std::string tz = FindTimeZone(value.c_str()).id;
      network_changed |= config.timezone != tz;
      config.timezone = tz;
    } else if (key == "bat_pct") {
      config.battery_percent = on;
    }
  }

  config.Save();
  ApplySettings();
  if (network_changed) Network::GetInstance().Apply();
  display->UpdateStatusBar();
  display->ShowNotification("Settings updated");
  display->Invalidate();
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
    Identify();
  } else if (command.type == "notify") {
    board.GetDisplay()->ShowNotification(command.arg, 5000);
  } else if (command.type == "test") {
    auto& tests = HwTest::GetInstance();
    TestId id;
    if (command.arg == "all") {
      tests.RunAll();
    } else if (HwTest::FromKey(command.arg, id)) {
      tests.Run(id);
    } else {
      return "fail|Unknown test";
    }
    return "ok|started";
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

void App::Identify() {
  auto& board = Board::GetInstance();
  auto ring = board.GetLedRing();
  auto& tests = HwTest::GetInstance();
  board.GetDisplay()->ShowNotification("Identify", 4000);
  for (int i = 0; i < 3; i++) {
    ring->SetAll({255, 255, 255});
    ring->Show();
    tests.Beep(1800, 80);
    delay(150);
    ring->Clear();
    ring->Show();
    delay(150);
  }
  ApplySettings();  // Restores the configured LED state
}

namespace {
// Why the board last started; the ROM's own boot line is lost when USB re-enumerates on reset.
const char* ResetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "other";
  }
}

// TLS handshakes alone need on the order of 40 KB; below this the console link and web portal
// silently fail (and stay failing until something frees memory some other way).
constexpr uint32_t kLowHeapBytes = 32 * 1024;
// How long it has to stay that low before restarting — a brief dip during a burst of activity
// (e.g. several checks at once) should not trigger this.
constexpr uint32_t kLowHeapGraceMs = 5 * 60 * 1000;
}  // namespace

void App::CheckHeap(uint32_t now) {
  if (ConsoleClient::GetInstance().OtaProgress() >= 0 || ProjectRuntime::Get().Busy()) {
    low_heap_since_ = 0;  // Never interrupt an update.
    return;
  }
  if (ESP.getFreeHeap() >= kLowHeapBytes) {
    low_heap_since_ = 0;
    return;
  }
  if (low_heap_since_ == 0) {
    low_heap_since_ = now;
    return;
  }
  if (now - low_heap_since_ < kLowHeapGraceMs) return;

  Serial.printf("{\"event\":\"restart\",\"reason\":\"low heap\",\"free\":%lu}\n",
                (unsigned long)ESP.getFreeHeap());
  Serial.flush();
  delay(100);
  ESP.restart();
}

void App::PrintInfo() {
  auto& board = Board::GetInstance();
  auto& config = DeviceConfig::Get();
  auto& network = Network::GetInstance();
  const uint64_t mac = ESP.getEfuseMac();
  Serial.printf(
      "{\"firmware\":\"%s\",\"version\":\"%s\",\"board\":\"%s\",\"chip\":\"%s\","
      "\"revision\":%d,\"cores\":%d,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
      "\"flash\":%lu,\"psram\":%lu,\"free_heap\":%lu,\"theme\":\"%s\",\"brightness\":%u,"
      "\"volume\":%d,\"sleep_s\":%d,\"power_off_s\":%d,\"rotate\":%s,\"led_on\":%s,"
      "\"wifi\":\"%s\",\"ip\":\"%s\",\"ble\":%s,\"tz\":\"%s\",\"time\":\"%s\",\"uptime\":%lu,"
      "\"console\":\"%s\",\"reset\":\"%s\",\"min_heap\":%lu,\"max_block\":%lu}\n",
      FIRMWARE_NAME, FIRMWARE_VERSION, board.GetBoardType().c_str(), ESP.getChipModel(),
      ESP.getChipRevision(), ESP.getChipCores(), uint8_t(mac), uint8_t(mac >> 8),
      uint8_t(mac >> 16), uint8_t(mac >> 24), uint8_t(mac >> 32), uint8_t(mac >> 40),
      (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getPsramSize(),
      (unsigned long)ESP.getFreeHeap(), board.GetDisplay()->GetTheme(),
      board.GetBacklight()->brightness(), board.GetAudioCodec()->output_volume(),
      config.sleep_seconds, config.power_off_seconds, config.rotate ? "true" : "false",
      config.led_on ? "true" : "false", network.WifiStatus().c_str(), network.WifiIp().c_str(),
      config.ble_on ? "true" : "false", config.timezone.c_str(), LocalTime().c_str(), millis() / 1000,
      ConsoleClient::GetInstance().StateText(), ResetReasonText(),
      (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
}
