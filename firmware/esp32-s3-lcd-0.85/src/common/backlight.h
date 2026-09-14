#pragma once

#include <driver/gpio.h>
#include <esp_timer.h>

#include <cstdint>

class Backlight {
 public:
  static constexpr int kDefaultBrightness = 100;

  Backlight();
  virtual ~Backlight();

  // Loads brightness from settings ("display"/"brightness", default kDefaultBrightness).
  void RestoreBrightness();
  // Fades 1% every 5 ms towards `brightness`; `permanent` saves it to settings.
  void SetBrightness(uint8_t brightness, bool permanent = false);
  uint8_t brightness() const { return brightness_; }

 protected:
  void OnTransitionTimer();
  virtual void SetBrightnessImpl(uint8_t brightness) = 0;

  esp_timer_handle_t transition_timer_ = nullptr;
  uint8_t brightness_ = 0;
  uint8_t target_brightness_ = 0;
  int8_t step_ = 1;
};

class PwmBacklight : public Backlight {
 public:
  PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
  ~PwmBacklight();

  void SetBrightnessImpl(uint8_t brightness) override;
};
