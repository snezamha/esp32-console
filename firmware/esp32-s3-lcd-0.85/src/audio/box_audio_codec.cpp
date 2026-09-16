#include "box_audio_codec.h"

#include <esp_log.h>

#include "../common/settings.h"

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(i2c_master_bus_handle_t i2c_bus, int sample_rate, gpio_num_t mclk,
                             gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                             gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr)
    : i2c_bus_(i2c_bus),
      sample_rate_(sample_rate),
      default_sample_rate_(sample_rate),
      mclk_(mclk),
      bclk_(bclk),
      ws_(ws),
      dout_(dout),
      din_(din),
      pa_pin_(pa_pin),
      es8311_addr_(es8311_addr),
      es7210_addr_(es7210_addr) {
  if (pa_pin_ != GPIO_NUM_NC) {
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ULL << pa_pin_;
    gpio_config(&io_conf);
    gpio_set_level(pa_pin_, 0);
  }
  Settings settings("audio");
  output_volume_ = settings.GetInt("output_volume", kDefaultVolume);
  if (output_volume_ <= 0) output_volume_ = 10;  // Never boot muted
}

bool BoxAudioCodec::Start() {
  if (started_) return true;

  es8311_found_ = i2c_master_probe(i2c_bus_, es8311_addr_, 100) == ESP_OK;
  es7210_found_ = i2c_master_probe(i2c_bus_, es7210_addr_, 100) == ESP_OK;
  ESP_LOGI(TAG, "ES8311 %s, ES7210 %s", es8311_found_ ? "found" : "missing",
           es7210_found_ ? "found" : "missing");

  // I2S first: the codecs lock onto MCLK during init.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = 240;
  chan_cfg.auto_clear_after_cb = true;
  if (i2s_new_channel(&chan_cfg, &tx_, &rx_) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(sample_rate_)),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = mclk_,
              .bclk = bclk_,
              .ws = ws_,
              .dout = dout_,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK) return false;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din = din_;
  if (i2s_channel_init_std_mode(rx_, &std_cfg) != ESP_OK) return false;
  i2s_channel_enable(tx_);
  i2s_channel_enable(rx_);

  if (es8311_found_) {
    es8311_ = new Es8311(i2c_bus_, es8311_addr_);
    es8311_found_ = es8311_->Init(sample_rate_);
    es8311_->SetVolume(output_volume_);
  }
  if (es7210_found_) {
    es7210_ = new Es7210(i2c_bus_, es7210_addr_);
    es7210_found_ = es7210_->Init(sample_rate_);
  }

  started_ = true;
  return es8311_found_ && es7210_found_;
}

void BoxAudioCodec::SetOutputVolume(int volume) {
  output_volume_ = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
  if (es8311_) es8311_->SetVolume(output_volume_);
  Settings settings("audio", true);
  settings.SetInt("output_volume", output_volume_);
}

void BoxAudioCodec::SetInputGain(float db) {
  if (es7210_) es7210_->SetGain(db);
}

void BoxAudioCodec::EnableOutput(bool enable) {
  if (pa_pin_ != GPIO_NUM_NC) gpio_set_level(pa_pin_, enable ? 1 : 0);
}

bool BoxAudioCodec::SetSampleRate(int sample_rate) {
  if (!started_ || !tx_ || !rx_ || sample_rate < 8000 || sample_rate > 48000) return false;
  if (sample_rate == sample_rate_) return true;
  EnableOutput(false);
  if (i2s_channel_disable(tx_) != ESP_OK || i2s_channel_disable(rx_) != ESP_OK) return false;
  i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(sample_rate));
  clock.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  const bool ok = i2s_channel_reconfig_std_clock(tx_, &clock) == ESP_OK &&
                  i2s_channel_reconfig_std_clock(rx_, &clock) == ESP_OK;
  i2s_channel_enable(rx_);
  i2s_channel_enable(tx_);
  if (ok) sample_rate_ = sample_rate;
  return ok;
}

int BoxAudioCodec::Write(const int16_t* samples, int frames, int timeout_ms) {
  if (!tx_) return 0;
  size_t written = 0;
  i2s_channel_write(tx_, samples, frames * 2 * sizeof(int16_t), &written, timeout_ms);
  return written / (2 * sizeof(int16_t));
}

int BoxAudioCodec::Read(int16_t* samples, int frames, int timeout_ms) {
  if (!rx_) return 0;
  size_t read = 0;
  i2s_channel_read(rx_, samples, frames * 2 * sizeof(int16_t), &read, timeout_ms);
  return read / (2 * sizeof(int16_t));
}
