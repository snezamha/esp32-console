#include "led_feedback.h"

#include <Arduino.h>

#include <cmath>

#include "../board/board.h"
#include "device_config.h"

namespace {
constexpr uint32_t kGlowMs = 140;
constexpr uint32_t kFrameMs = 33;
}  // namespace

void LedFeedback::OnButtonPress() {
  if (DeviceConfig::Get().led_feedback) glow_until_ = millis() + kGlowMs;
}

void LedFeedback::Loop(uint32_t now_ms, bool paused) {
  auto& board = Board::GetInstance();
  auto ring = board.GetLedRing();

  if (now_ms - last_battery_check_ >= 1000) {
    last_battery_check_ = now_ms;
    auto power = board.GetPowerManager();
    charging_ = power->IsCharging();
    low_battery_ = power->IsDischarging() && power->GetBatteryLevel() <= 20;
  }

  Mode mode = Mode::Base;
  if (DeviceConfig::Get().led_feedback && !paused) {
    if (now_ms < glow_until_) {
      mode = Mode::Glow;
    } else if (low_battery_) {
      mode = Mode::LowBattery;
    } else if (charging_) {
      mode = Mode::Charging;
    }
  }

  if (mode == Mode::Base) {
    // Restore the user's LED setting once when a cue ends.
    if (mode_ != Mode::Base && !paused) board.ApplyLed();
    else if (!paused) board.AnimateLed(now_ms);
    mode_ = mode;
    return;
  }

  const bool changed = mode != mode_;
  mode_ = mode;
  if (!changed && mode == Mode::Glow) return;  // Static color
  if (!changed && now_ms - last_frame_ < kFrameMs) return;
  last_frame_ = now_ms;

  switch (mode) {
    case Mode::Glow:
      ring->SetAll({20, 20, 20});
      break;
    case Mode::LowBattery: {
      const float wave = (std::sin(now_ms / 2000.0f * 2.0f * M_PI) + 1.0f) / 2.0f;
      ring->SetAll({static_cast<uint8_t>(2 + wave * 30), 0, 0});
      break;
    }
    case Mode::Charging: {
      const float pos = std::fmod(now_ms / 180.0f, static_cast<float>(ring->count()));
      for (int i = 0; i < ring->count(); i++) {
        float d = std::fabs(i - pos);
        d = std::min(d, ring->count() - d);
        const uint8_t g = static_cast<uint8_t>(std::max(0.0f, 1.0f - d / 1.5f) * 14);
        ring->SetPixel(i, {0, g, 0});
      }
      break;
    }
    default:
      break;
  }
  ring->Show();
}
