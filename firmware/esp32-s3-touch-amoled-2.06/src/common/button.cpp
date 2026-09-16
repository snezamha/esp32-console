#include "button.h"

#include <Arduino.h>

namespace {
constexpr uint32_t kDebounceMs = 10;
}

Button::Button(gpio_num_t gpio_num, bool active_high, uint16_t long_press_time,
               uint16_t short_press_time)
    : gpio_num_(gpio_num),
      active_high_(active_high),
      long_press_time_(long_press_time ? long_press_time : kDefaultLongPressMs),
      short_press_time_(short_press_time ? short_press_time : kDefaultShortPressMs) {
  if (gpio_num_ == GPIO_NUM_NC) return;
  pinMode(gpio_num_, active_high_ ? INPUT_PULLDOWN : INPUT_PULLUP);
  raw_ = pressed_ = digitalRead(gpio_num_) == (active_high_ ? HIGH : LOW);
  // A press already in progress at boot never counts as a click or long press.
  long_press_fired_ = pressed_;
}

uint32_t Button::HeldMs() const { return pressed_ ? millis() - pressed_at_ : 0; }

void Button::Tick() {
  if (gpio_num_ == GPIO_NUM_NC) return;
  const uint32_t now = millis();
  const bool raw = digitalRead(gpio_num_) == (active_high_ ? HIGH : LOW);

  if (raw != raw_) {
    raw_ = raw;
    raw_changed_at_ = now;
  }

  if (raw_ != pressed_ && now - raw_changed_at_ >= kDebounceMs) {
    pressed_ = raw_;
    if (pressed_) {
      pressed_at_ = now;
      long_press_fired_ = false;
      cancelled_ = false;
      if (on_press_down_) on_press_down_();
    } else {
      released_at_ = now;
      if (on_press_up_) on_press_up_();
      const bool too_long = max_click_time_ && released_at_ - pressed_at_ > max_click_time_;
      if (long_press_fired_ || too_long) {
        click_count_ = 0;
      } else {
        click_count_++;
        if (click_count_ == 2 && on_double_click_) on_double_click_();
        if (click_count_ == multiple_click_count_ && on_multiple_click_) on_multiple_click_();
      }
    }
  }

  if (pressed_ && !long_press_fired_ && now - pressed_at_ >= long_press_time_) {
    long_press_fired_ = true;
    click_count_ = 0;
    if (on_long_press_) on_long_press_();
  }

  // Click sequence ends when no new press arrives within the short press window.
  if (!pressed_ && click_count_ > 0 && now - released_at_ >= short_press_time_) {
    if (click_count_ == 1 && on_click_) on_click_();
    click_count_ = 0;
  }
}
