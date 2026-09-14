#pragma once

#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

#include <cmath>

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
    int adc_raw = 0;
    int voltage_mv = 0;
    if (adc_handle_ == nullptr || !do_calibration_) return 0.0f;
    if (adc_oneshot_read(adc_handle_, adc_channel_, &adc_raw) != ESP_OK) return 0.0f;
    if (adc_cali_raw_to_voltage(adc_cali_handle_, adc_raw, &voltage_mv) != ESP_OK) return 0.0f;
    return (voltage_mv / 1000.0f) * 3.0f;  // 1:3 divider
  }

  // Stepped level (1/20/40/60/80/100) with 0.1 V hysteresis.
  int GetBatteryLevel() {
    if (adc_handle_ == nullptr || !do_calibration_) return 100;

    const float voltage = GetBatteryVoltage();
    if (std::fabs(voltage - last_voltage_) >= kVoltageThreshold) {
      last_voltage_ = voltage;
      if (voltage < 3.52f) {
        last_battery_level_ = 1;
      } else if (voltage < 3.64f) {
        last_battery_level_ = 20;
      } else if (voltage < 3.76f) {
        last_battery_level_ = 40;
      } else if (voltage < 3.88f) {
        last_battery_level_ = 60;
      } else if (voltage < 4.0f) {
        last_battery_level_ = 80;
      } else {
        last_battery_level_ = 100;
      }
    }
    return last_battery_level_;
  }

  bool IsCharging() {
    return charging_pin_ != GPIO_NUM_NC && gpio_get_level(charging_pin_) == 0;
  }

  bool IsDischarging() {
    return charging_pin_ == GPIO_NUM_NC || gpio_get_level(charging_pin_) == 1;
  }

  bool IsChargingDone() { return GetBatteryLevel() == 100; }

  void PowerOff() {
    if (bat_power_pin_ != GPIO_NUM_NC) gpio_set_level(bat_power_pin_, 0);
  }

  void PowerON() {
    if (bat_power_pin_ != GPIO_NUM_NC) gpio_set_level(bat_power_pin_, 1);
  }

 private:
  static constexpr float kVoltageThreshold = 0.1f;

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
  float last_voltage_ = 0.0f;
  int last_battery_level_ = 0;
};
