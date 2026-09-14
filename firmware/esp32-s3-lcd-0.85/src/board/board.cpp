#include "board.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include <algorithm>

#include "../../config.h"
#include "../lang.h"
#include "../services/device_config.h"

Board::Board()
    : pwr_button_(PWR_BUTTON_GPIO),
      volume_up_button_(VOLUME_UP_BUTTON_GPIO),
      volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {}

void Board::Initialize() {
  InitializePowerManager();
  InitializeI2c();
  InitializeSpi();
  InitializeLcdDisplay();
  InitializeButtons();

  led_ring_ = new LedRing(BUILTIN_LED_GPIO, BUILTIN_LED_COUNT);
  audio_codec_ = new BoxAudioCodec(i2c_bus_, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
                                   AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
                                   AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
                                   AUDIO_CODEC_ES7210_ADDR);
  sd_card_ = new SdCard(SD_CARD_CLK_PIN, SD_CARD_CMD_PIN, SD_CARD_D0_PIN, SD_CARD_D1_PIN,
                        SD_CARD_D2_PIN, SD_CARD_D3_PIN);

  ApplyLed();
  ApplyPowerHold();
  GetBacklight()->RestoreBrightness();
}

void Board::InitializePowerManager() {
  power_manager_ = new PowerManager(BATTERY_CHARGING_PIN, BATTERY_ADC_PIN, BATTERY_EN_PIN);
  power_manager_->PowerON();
}

void Board::InitializeI2c() {
  i2c_master_bus_config_t i2c_bus_cfg = {
      .i2c_port = (i2c_port_t)0,
      .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
      .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags = {.enable_internal_pullup = 1},
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
}

void Board::InitializeSpi() {
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
  buscfg.miso_io_num = DISPLAY_MISO_PIN;
  buscfg.sclk_io_num = DISPLAY_CLK_PIN;
  buscfg.quadwp_io_num = GPIO_NUM_NC;
  buscfg.quadhd_io_num = GPIO_NUM_NC;
  buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

void Board::InitializeLcdDisplay() {
  esp_lcd_panel_io_handle_t panel_io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = DISPLAY_CS_PIN;
  io_config.dc_gpio_num = DISPLAY_DC_PIN;
  io_config.spi_mode = DISPLAY_SPI_MODE;
  io_config.pclk_hz = 40 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &panel_io));

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = DISPLAY_RST_PIN;
  panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
  panel_config.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
  esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
  esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

  display_ = new LcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                            DISPLAY_OFFSET_Y);
  display_->SetBatteryProvider([this]() {
    BatteryStatus status;
    status.available = GetBatteryLevel(status.level, status.charging, status.discharging);
    return status;
  });
  display_->SetVolumeProvider([this]() { return audio_codec_ ? audio_codec_->output_volume() : BoxAudioCodec::kDefaultVolume; });
}

void Board::InitializeButtons() {
  pwr_button_.SetMaxClickTime(kPowerHoldRingDelayMs);

  // The power button is still held when the board switches on: attach its
  // handlers only after that first release.
  pwr_button_.OnPressUp([this]() {
    if (pwr_button_armed_) return;
    pwr_button_armed_ = true;

    pwr_button_.OnClick([this]() {
      if (menu_key_handler_) menu_key_handler_(MenuKey::Select);
    });

    pwr_button_.OnLongPress([this]() {
      if (power_hold_interceptor_ && power_hold_interceptor_()) return;
      PowerOff();
    });

    pwr_button_.OnDoubleClick([this]() {
      static uint8_t brightness_last = 0;
      auto backlight = GetBacklight();
      if (backlight->brightness() == 0) {
        backlight->SetBrightness(brightness_last == 0 ? 50 : brightness_last, true);
      } else {
        brightness_last = backlight->brightness();
        backlight->SetBrightness(0);
      }
    });
  });

  volume_up_button_.OnClick([this]() {
    if (menu_open_) {
      if (menu_key_handler_) menu_key_handler_(MenuKey::Up);
      return;
    }
    const int volume = std::min(audio_codec_->output_volume() + 10, 100);
    audio_codec_->SetOutputVolume(volume);
    display_->ShowNotification(std::string(Lang::Strings::VOLUME) + std::to_string(volume / 10));
    Serial.printf("{\"event\":\"volume\",\"volume\":%d}\n", volume);
  });

  volume_up_button_.OnLongPress([this]() {
    if (menu_open_) return;
    audio_codec_->SetOutputVolume(100);
    display_->ShowNotification(Lang::Strings::MAX_VOLUME);
  });

  volume_down_button_.OnClick([this]() {
    if (menu_open_) {
      if (menu_key_handler_) menu_key_handler_(MenuKey::Down);
      return;
    }
    const int volume = std::max(audio_codec_->output_volume() - 10, 0);
    audio_codec_->SetOutputVolume(volume);
    display_->ShowNotification(std::string(Lang::Strings::VOLUME) + std::to_string(volume / 10));
    Serial.printf("{\"event\":\"volume\",\"volume\":%d}\n", volume);
  });

  volume_down_button_.OnLongPress([this]() {
    if (menu_open_) return;
    audio_codec_->SetOutputVolume(0);
    display_->ShowNotification(Lang::Strings::MUTED);
  });
}

void Board::Loop() {
  const bool was[3] = {pwr_button_.IsPressed(), volume_up_button_.IsPressed(),
                       volume_down_button_.IsPressed()};
  pwr_button_.Tick();
  volume_up_button_.Tick();
  volume_down_button_.Tick();
  const bool now[3] = {pwr_button_.IsPressed(), volume_up_button_.IsPressed(),
                       volume_down_button_.IsPressed()};
  static constexpr const char* kNames[3] = {"power", "up", "down"};
  Button* buttons[3] = {&pwr_button_, &volume_up_button_, &volume_down_button_};
  for (int i = 0; i < 3; i++) {
    if (!was[i] && now[i]) {
      // The press that wakes the screen does nothing else.
      if (sleeping_) buttons[i]->Cancel();
      WakeUp();
    }
    if (was[i] != now[i]) {
      Serial.printf("{\"event\":\"button\",\"button\":\"%s\",\"action\":\"%s\",\"menu\":%s}\n",
                    kNames[i], now[i] ? "down" : "up", menu_open_ ? "true" : "false");
    }
  }

  // Vol+ and Vol- together: toggle the menu once per press, without volume changes.
  const bool up = volume_up_button_.IsPressed();
  const bool down = volume_down_button_.IsPressed();
  if (up && down && !volume_combo_latched_) {
    volume_combo_latched_ = true;
    if (menu_key_handler_) menu_key_handler_(MenuKey::Toggle);
  } else if (!up && !down) {
    volume_combo_latched_ = false;
  }
  if (volume_combo_latched_) {
    volume_up_button_.Cancel();
    volume_down_button_.Cancel();
  }

  if (pwr_button_.IsPressed() || volume_up_button_.IsPressed() ||
      volume_down_button_.IsPressed()) {
    idle_seconds_ = 0;
  }
}

void Board::OnClockTick(bool busy) {
  const auto& config = DeviceConfig::Get();
  idle_seconds_ = busy ? 0 : idle_seconds_ + 1;

  if (!sleeping_ && config.sleep_seconds > 0 && idle_seconds_ >= config.sleep_seconds) {
    sleeping_ = true;
    GetBacklight()->SetBrightness(10);
  }
  if (config.power_off_seconds > 0 && idle_seconds_ >= config.power_off_seconds &&
      power_manager_->IsDischarging()) {
    PowerOff();
  }
  display_->UpdateStatusBar();
}

void Board::WakeUp() {
  idle_seconds_ = 0;
  if (sleeping_) {
    sleeping_ = false;
    GetBacklight()->RestoreBrightness();
  }
}

void Board::ApplyLed() {
  static constexpr RgbColor kColors[] = {{255, 255, 255}, {255, 0, 0},   {0, 255, 0},  {0, 0, 255},
                                         {0, 255, 255},   {160, 0, 255}, {255, 90, 0}};
  const auto& config = DeviceConfig::Get();
  if (!config.led_on) {
    led_ring_->Clear();
  } else {
    const RgbColor base = kColors[config.led_color % (sizeof(kColors) / sizeof(kColors[0]))];
    const int level = std::clamp(config.led_brightness, 1, 8);
    const int scale = (1 << level) - 1;  // 1 … 255
    led_ring_->SetAll({static_cast<uint8_t>(base.red * scale / 255),
                       static_cast<uint8_t>(base.green * scale / 255),
                       static_cast<uint8_t>(base.blue * scale / 255)});
  }
  led_ring_->Show();
}

void Board::ApplyPowerHold() { pwr_button_.SetLongPressTime(DeviceConfig::Get().power_hold_ms); }

Backlight* Board::GetBacklight() {
  static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
  return &backlight;
}

bool Board::GetBatteryLevel(int& level, bool& charging, bool& discharging) {
  charging = power_manager_->IsCharging();
  discharging = power_manager_->IsDischarging();
  level = power_manager_->GetBatteryLevel();
  return true;
}

void Board::PowerOff() {
  power_manager_->PowerOff();
  delay(300);
  // Still running: the board is powered over USB.
  display_->ShowNotification(Lang::Strings::USB_POWERED);
}
