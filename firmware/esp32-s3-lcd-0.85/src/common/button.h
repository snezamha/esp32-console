#pragma once

#include <driver/gpio.h>

#include <functional>

// GPIO button with the same event model as espressif/button (iot_button):
// press down/up, single click, double click, multiple click and long press.
// Call Tick() from the main loop; callbacks run on the caller's task.
class Button {
 public:
  static constexpr uint16_t kDefaultLongPressMs = 1500;  // CONFIG_BUTTON_LONG_PRESS_TIME_MS
  static constexpr uint16_t kDefaultShortPressMs = 180;  // CONFIG_BUTTON_SHORT_PRESS_TIME_MS

  Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0,
         uint16_t short_press_time = 0);

  void OnPressDown(std::function<void()> callback) { on_press_down_ = callback; }
  void OnPressUp(std::function<void()> callback) { on_press_up_ = callback; }
  void OnLongPress(std::function<void()> callback) { on_long_press_ = callback; }
  void OnClick(std::function<void()> callback) { on_click_ = callback; }
  void OnDoubleClick(std::function<void()> callback) { on_double_click_ = callback; }
  void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3) {
    on_multiple_click_ = callback;
    multiple_click_count_ = click_count;
  }

  void Tick();
  bool IsPressed() const { return pressed_; }
  void SetLongPressTime(uint16_t ms) { long_press_time_ = ms ? ms : kDefaultLongPressMs; }
  void SetShortPressTime(uint16_t ms) { short_press_time_ = ms ? ms : kDefaultShortPressMs; }
  // A press held longer than this is not a click, even if released before the long press fires
  // (e.g. a hold-to-power-off that was abandoned). 0 = no limit.
  void SetMaxClickTime(uint16_t ms) { max_click_time_ = ms; }
  // Drops the current press: no click, double click or long press will follow from it.
  void Cancel() {
    long_press_fired_ = true;
    cancelled_ = true;
    click_count_ = 0;
  }
  bool IsCancelled() const { return cancelled_; }
  // How long the current press has lasted; 0 when released.
  uint32_t HeldMs() const;

 protected:
  gpio_num_t gpio_num_;
  bool active_high_;
  uint16_t long_press_time_;
  uint16_t short_press_time_;
  uint16_t max_click_time_ = 0;

  bool pressed_ = false;
  bool raw_ = false;
  uint32_t raw_changed_at_ = 0;
  uint32_t pressed_at_ = 0;
  uint32_t released_at_ = 0;
  bool long_press_fired_ = false;
  bool cancelled_ = false;
  uint8_t click_count_ = 0;
  uint8_t multiple_click_count_ = 3;

  std::function<void()> on_press_down_;
  std::function<void()> on_press_up_;
  std::function<void()> on_long_press_;
  std::function<void()> on_click_;
  std::function<void()> on_double_click_;
  std::function<void()> on_multiple_click_;
};
