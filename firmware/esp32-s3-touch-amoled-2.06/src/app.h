#pragma once

#include <Arduino.h>

#include <string>

#include "services/console_client.h"

// Minimal skeleton: Wi-Fi + console pairing run in the background (so the console always sees
// the board) behind a static screen (board name + battery). PWR key: short press sleeps the
// screen, long press powers off. Boot button wakes it. Touch never sleeps the screen.
class App {
 public:
  static App& GetInstance() {
    static App instance;
    return instance;
  }

  void Start();
  void Loop();

 private:
  App() = default;

  void WakeScreen();
  void SleepScreen();

  void HandleSerial();
  void HandleCommand(const std::string& line);
  void PrintInfo();
  std::string ReportState();
  std::string ReportTelemetry();
  void ApplyRemoteSettings(const ConsoleClient::Values& values);
  std::string HandleConsoleCommand(const ConsoleClient::Command& command);
  void FactoryReset();

  bool screen_on_ = true;
  std::string serial_line_;
};
