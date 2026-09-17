#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

// Battery rail, charge detection and battery level.
class PowerManager {
 public:
  PowerManager(gpio_num_t charging_pin, gpio_num_t bat_adc_pin, gpio_num_t bat_power_pin)
      : charging_pin_(charging_pin), bat_adc_pin_(bat_adc_pin), bat_power_pin_(bat_power_pin) {
    if (charging_pin_ != GPIO_NUM_NC) {
      gpio_config_t io_conf = {};
      io_conf.intr_type = GPIO_INTR_DISABLE;
      io_conf.mode = GPIO_MODE_INPUT;
      io_conf.pin_bit_mask = 1ULL << charging_pin_;
      io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
      gpio_config(&io_conf);
    }

    if (bat_power_pin_ != GPIO_NUM_NC) {
      gpio_config_t io_conf = {};
      io_conf.intr_type = GPIO_INTR_DISABLE;
      io_conf.mode = GPIO_MODE_OUTPUT;
      io_conf.pin_bit_mask = 1ULL << bat_power_pin_;
      io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
      gpio_config(&io_conf);
    }

    if (bat_adc_pin_ != GPIO_NUM_NC) {
      adc_oneshot_unit_init_cfg_t init_config = {};
      init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
      if (bat_adc_pin_ >= GPIO_NUM_1 && bat_adc_pin_ <= GPIO_NUM_10) {
        init_config.unit_id = ADC_UNIT_1;
        adc_channel_ = (adc_channel_t)((int)bat_adc_pin_ - 1);
      } else if (bat_adc_pin_ >= GPIO_NUM_11 && bat_adc_pin_ <= GPIO_NUM_20) {
        init_config.unit_id = ADC_UNIT_2;
        adc_channel_ = (adc_channel_t)((int)bat_adc_pin_ - 11);
      } else {
        return;
      }
      ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));
      adc_oneshot_chan_cfg_t config = {};
      config.bitwidth = ADC_BITWIDTH_DEFAULT;
      config.atten = ADC_ATTEN_DB_12;
      ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_channel_, &config));
      do_calibration_ =
          CalibrationInit(init_config.unit_id, adc_channel_, config.atten, &adc_cali_handle_);
    }
  }

  ~PowerManager() {
    if (adc_handle_) adc_oneshot_del_unit(adc_handle_);
  }

  float GetBatteryVoltage() {
    UpdateReading();
    return reading_valid_ ? filtered_voltage_mv_ / 1000.0f : 0.0f;
  }

  // Estimated Li-ion state of charge. Voltage is useful for trends, but this is not a fuel gauge:
  // temperature, load and cell chemistry still affect the estimate.
  int GetBatteryLevel() {
    UpdateReading();
    return reading_valid_ ? battery_level_ : -1;
  }

  // Change within the current ten-minute charging or non-charging period.
  bool GetVoltageChange(int& millivolts, uint32_t& minutes) {
    UpdateReading();
    std::lock_guard<std::mutex> lock(reading_mutex_);
    if (!reading_valid_ || trend_count_ < 2) return false;
    const int oldest = (trend_next_ + trend_history_.size() - trend_count_) % trend_history_.size();
    const auto& start = trend_history_[oldest];
    if (millis() - start.at < 5 * 60 * 1000) return false;
    millivolts = static_cast<int>(std::lround(filtered_voltage_mv_ - start.mv));
    minutes = (millis() - start.at) / 60000;
    return true;
  }

  bool IsCharging() {
    return charging_pin_ != GPIO_NUM_NC && gpio_get_level(charging_pin_) == 0;
  }

  bool IsDischarging() {
    return charging_pin_ == GPIO_NUM_NC || gpio_get_level(charging_pin_) == 1;
  }

  bool IsChargingDone() { return GetBatteryLevel() >= 99; }

  void PowerOff() {
    if (bat_power_pin_ != GPIO_NUM_NC) gpio_set_level(bat_power_pin_, 0);
  }

  void PowerON() {
    if (bat_power_pin_ != GPIO_NUM_NC) gpio_set_level(bat_power_pin_, 1);
  }

 private:
  static constexpr uint32_t kSampleIntervalMs = 1000;
  static constexpr int kSampleCount = 15;
  static constexpr float kFilterAlpha = 0.18f;
  static constexpr float kDividerRatio = 3.0f;

  struct CurvePoint { int millivolts, percent; };
  // Top anchor measured on this board, not the textbook 4200mV: its charge IC terminates around
  // 4.08V at rest (confirmed with the charger unplugged, no load), so 100% was previously
  // unreachable and every reading capped near 92%.
  static constexpr CurvePoint kDischargeCurve[] = {
      {3300, 0}, {3500, 5}, {3600, 10}, {3680, 20}, {3740, 30}, {3770, 40},
      {3790, 50}, {3820, 60}, {3870, 70}, {3950, 80}, {4050, 90}, {4080, 100},
  };

  static int VoltageToPercent(float millivolts) {
    if (millivolts <= kDischargeCurve[0].millivolts) return 0;
    for (size_t i = 1; i < std::size(kDischargeCurve); ++i) {
      if (millivolts <= kDischargeCurve[i].millivolts) {
        const auto& low = kDischargeCurve[i - 1]; const auto& high = kDischargeCurve[i];
        const float position = (millivolts - low.millivolts) / (high.millivolts - low.millivolts);
        return std::clamp(static_cast<int>(std::lround(low.percent + position * (high.percent - low.percent))), 0, 100);
      }
    }
    return 100;
  }

  void UpdateReading() {
    std::lock_guard<std::mutex> lock(reading_mutex_);
    const uint32_t now = millis();
    if (reading_valid_ && now - sampled_at_ < kSampleIntervalMs) return;
    sampled_at_ = now;
    if (adc_handle_ == nullptr || !do_calibration_) return;

    std::array<int, kSampleCount> raw{};
    int count = 0;
    for (int i = 0; i < kSampleCount; ++i) {
      int value = 0;
      if (adc_oneshot_read(adc_handle_, adc_channel_, &value) == ESP_OK) raw[count++] = value;
    }
    if (count < 7) return;
    std::sort(raw.begin(), raw.begin() + count);
    const int trim = count / 5;  // Drop the highest and lowest 20% before averaging.
    int64_t sum = 0;
    for (int i = trim; i < count - trim; ++i) sum += raw[i];
    const int average_raw = static_cast<int>(sum / (count - 2 * trim));
    int adc_mv = 0;
    if (adc_cali_raw_to_voltage(adc_cali_handle_, average_raw, &adc_mv) != ESP_OK) return;
    const float measured_mv = adc_mv * kDividerRatio;
    if (measured_mv < 2500.0f || measured_mv > 4600.0f) return;

    const float previous_mv = filtered_voltage_mv_;
    filtered_voltage_mv_ = reading_valid_ ? previous_mv + kFilterAlpha * (measured_mv - previous_mv) : measured_mv;
    int estimate = VoltageToPercent(filtered_voltage_mv_);
    if (reading_valid_ && IsDischarging() && estimate > battery_level_ && measured_mv - previous_mv < 200.0f) {
      estimate = battery_level_;  // Ignore ordinary load recovery while no charger is present.
    }
    if (!reading_valid_ || std::abs(estimate - battery_level_) >= 2) battery_level_ = estimate;
    const bool charging = IsCharging();
    if (!reading_valid_ || charging != trend_charging_) {
      trend_count_ = 0;
      trend_next_ = 0;
      trend_charging_ = charging;
    }
    if (trend_count_ == 0 || now - trend_last_sample_ms_ >= 60 * 1000) {
      trend_history_[trend_next_] = {now, filtered_voltage_mv_};
      trend_next_ = (trend_next_ + 1) % trend_history_.size();
      trend_count_ = std::min(trend_count_ + 1, static_cast<int>(trend_history_.size()));
      trend_last_sample_ms_ = now;
    }
    reading_valid_ = true;
  }

  bool CalibrationInit(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten,
                       adc_cali_handle_t* out_handle) {
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, out_handle);
    if (ret != ESP_OK) {
      ESP_LOGW("PowerManager", "ADC calibration unavailable (%d)", ret);
    }
    return ret == ESP_OK;
  }

  gpio_num_t charging_pin_ = GPIO_NUM_NC;
  gpio_num_t bat_adc_pin_ = GPIO_NUM_NC;
  gpio_num_t bat_power_pin_ = GPIO_NUM_NC;
  adc_oneshot_unit_handle_t adc_handle_ = nullptr;
  adc_cali_handle_t adc_cali_handle_ = nullptr;
  adc_channel_t adc_channel_ = ADC_CHANNEL_0;
  bool do_calibration_ = false;
  std::mutex reading_mutex_;
  uint32_t sampled_at_ = 0;
  float filtered_voltage_mv_ = 0.0f;
  int battery_level_ = -1;
  bool reading_valid_ = false;
  struct TrendSample { uint32_t at; float mv; };
  std::array<TrendSample, 11> trend_history_{};
  int trend_next_ = 0;
  int trend_count_ = 0;
  uint32_t trend_last_sample_ms_ = 0;
  bool trend_charging_ = false;
};
