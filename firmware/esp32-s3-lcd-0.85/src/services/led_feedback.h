#pragma once

#include <cstdint>

// Short LED ring cues on top of the user's LED setting:
//   button press   brief white glow
//   low battery    slow red pulse (on battery, level ≤ 20%)
//   charging       dim green dot circling the ring
// Enabled by DeviceConfig::led_feedback.
class LedFeedback {
 public:
  static LedFeedback& GetInstance() {
    static LedFeedback instance;
    return instance;
  }

  void OnButtonPress();
  // Call from the main loop. `paused` hands the ring to something else (e.g. the LED check).
  void Loop(uint32_t now_ms, bool paused);

 private:
  enum class Mode { Base, Glow, LowBattery, Charging };

  LedFeedback() = default;

  uint32_t glow_until_ = 0;
  uint32_t last_frame_ = 0;
  uint32_t last_battery_check_ = 0;
  bool charging_ = false;
  bool low_battery_ = false;
  Mode mode_ = Mode::Base;
};
