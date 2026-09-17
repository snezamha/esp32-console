#include "radio_stream.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <mp3dec.h>

#include "../board/board.h"
#include "../hw_test.h"
#include "heap_guard.h"
#include "network.h"
#include "radio_spectrum.h"

extern const uint8_t kCertBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCertBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
// Network jitter buffer. At 4096 (the old size) this held only ~0.25s of audio at 128kbps, so
// any Wi-Fi/TCP hiccup (retransmit, beacon-interval stall, a slow moment on the origin server)
// that outlasted that quarter-second starved the decoder directly — audible as a stutter, and
// independent of which core does the decoding. 16384 gives ~1s of cushion at 128kbps (less at
// higher bitrates, still several times the old margin) while staying a small, one-time heap
// allocation for the duration of playback.
constexpr size_t kInputSize = 16384;
constexpr int kMaxOutputSamples = 2304;
constexpr uint32_t kNoDataTimeoutMs = 12000;

void LogStage(const char* stage, int detail = 0) {
  char phase[32];
  snprintf(phase, sizeof(phase), "radio:%s", stage);
  HeapGuard::Phase(stage);  // Stage strings are literals from the call sites below.
  HeapGuard::Check(phase);
  Serial.printf("{\"radio\":\"%s\",\"detail\":%d,\"heap\":%u,\"largest\":%u,\"stack\":%u}\n",
                stage, detail, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}
}

RadioStream& RadioStream::Get() {
  static RadioStream stream;
  return stream;
}

int RadioStream::Start(const char* raw) {
  if (!raw) return -1;
  const std::string url(raw, strnlen(raw, 257));
  if (url.size() > 256 || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) return -1;
  std::lock_guard<std::mutex> lock(mutex_);
  if (url == url_ && task_running_) return 0;
  url_ = url;
  ++generation_;
  state_ = 1;
  bitrate_ = 0;
  message_ = "Connecting";
  spectrum_.fill(0);
  spectrum_at_ms_ = 0;
  if (!task_running_) {
    task_running_ = true;
    // Core 1, not 0: every other network task (console poll, mDNS, OTA, Wi-Fi/BLE scans) is
    // pinned to core 0 alongside the Wi-Fi driver and lwIP's own task. The MP3 decode + I2S
    // write loop below shares that core with all of them under the old pinning, and the I2S DMA
    // buffer only holds ~32-60 ms of audio — any of those tasks briefly holding core 0 is enough
    // to starve the decoder and produce an audible stutter. Core 1 only runs the Arduino
    // loop/display task, which is idle almost all the time, so decode+write no longer competes
    // for CPU with the network stack.
    if (xTaskCreatePinnedToCore(Task, "radio_stream", 12288, this, 2, nullptr, 1) != pdPASS) {
      task_running_ = false;
      state_ = 4;
      message_ = "No memory";
      return -1;
    }
  }
  return 0;
}

void RadioStream::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  url_.clear();
  ++generation_;
  auto* codec = Board::GetInstance().GetAudioCodec();
  if (codec && codec->started()) codec->EnableOutput(false);
  state_ = 0;
  bitrate_ = 0;
  message_.clear();
  spectrum_.fill(0);
  spectrum_at_ms_ = 0;
}

std::string RadioStream::CurrentUrl() {
  std::lock_guard<std::mutex> lock(mutex_);
  return url_;
}

bool RadioStream::WorkerRunning() {
  std::lock_guard<std::mutex> lock(mutex_);
  return task_running_;
}

int RadioStream::Spectrum(uint8_t* levels, uint32_t capacity) {
  if (!levels || capacity < spectrum_.size()) return -1;
  std::lock_guard<std::mutex> lock(mutex_);
  const bool fresh = state_ == 3 && spectrum_at_ms_ && millis() - spectrum_at_ms_ < 500;
  for (size_t i = 0; i < spectrum_.size(); ++i) levels[i] = fresh ? spectrum_[i] : 0;
  return fresh ? static_cast<int>(spectrum_.size()) : 0;
}

void RadioStream::UpdateSpectrum(const int16_t* pcm, int frames, int channels, int sample_rate, uint32_t generation) {
  const uint32_t now = millis();
  if (now - last_spectrum_compute_ms_ < 75) return;
  last_spectrum_compute_ms_ = now;
  uint8_t levels[radio_spectrum::kBands];
  radio_spectrum::Analyze(pcm, frames, channels, sample_rate,
                          Board::GetInstance().GetAudioCodec()->output_volume(), levels);
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != generation_) return;
  for (size_t i = 0; i < spectrum_.size(); ++i) spectrum_[i] = levels[i];
  spectrum_at_ms_ = now;
}

int RadioStream::Status(char* text, uint32_t capacity, int* bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (text && capacity) {
    const size_t count = std::min<size_t>(capacity - 1, message_.size());
    memcpy(text, message_.data(), count);
    text[count] = 0;
  }
  if (bitrate_kbps) *bitrate_kbps = bitrate_;
  return state_;
}

bool RadioStream::Active() {
  std::lock_guard<std::mutex> lock(mutex_);
  return !url_.empty();
}

void RadioStream::SetStatus(uint32_t generation, int state, const char* message, int bitrate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != generation_) return;
  state_ = state;
  bitrate_ = bitrate;
  message_ = message;
}

void RadioStream::Task(void* arg) {
  auto* self = static_cast<RadioStream*>(arg);
  uint32_t handled = 0;
  for (;;) {
    std::string url;
    uint32_t generation;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      generation = self->generation_;
      url = self->url_;
    }
    if (url.empty()) {
      // Give the stack and decoder buffers back before a firmware download starts.
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (self->url_.empty()) {
        self->task_running_ = false;
        break;
      }
      continue;  // A new station was selected while the old one was closing.
    }
    if (generation != handled) {
      handled = generation;
      self->SetStatus(generation, 1, "Connecting");
    }
    self->Run(url, generation);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec->started()) {
      codec->EnableOutput(false);
      codec->SetSampleRate(codec->default_sample_rate());
    }
    for (int i = 0; i < 20 && generation == self->generation_; ++i) vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelete(nullptr);
}

void RadioStream::Run(const std::string& url, uint32_t generation) {
  LogStage("start", url.rfind("https://", 0) == 0);
  if (Network::GetInstance().State() != Network::WifiState::Connected) {
    SetStatus(generation, 4, "Wi-Fi offline");
    return;
  }
  if (HwTest::GetInstance().IsBusy()) {
    SetStatus(generation, 4, "Audio busy");
    return;
  }
  // ~HTTPClient() calls _client->stop() through a raw pointer, so the clients must outlive it.
  NetworkClient plain;
  NetworkClientSecure secure;
  HTTPClient http;
  const bool tls = url.rfind("https://", 0) == 0;
  if (tls) secure.setCACertBundle(kCertBundleStart, kCertBundleEnd - kCertBundleStart);
  NetworkClient& client = tls ? static_cast<NetworkClient&>(secure) : plain;
  http.useHTTP10(true);  // A raw MP3 body, with no chunked transfer framing.
  // Some broadcasters (BBC) reject generic HTTP client user agents with 400 Bad Request and only
  // serve the stream to player-like ones.
  http.setUserAgent("VLC/3.0");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(8000);
  http.setTimeout(1500);
  if (!http.begin(client, url.c_str())) {
    SetStatus(generation, 4, "Invalid stream");
    return;
  }
  LogStage("http_begin");
  const int code = http.GET();
  LogStage("http_get", code);
  if (generation != generation_) {
    http.end();
    return;
  }
  if (code != 200) {
    SetStatus(generation, 4, code < 0 ? "Connection failed" : "Station unavailable");
    http.end();
    return;
  }
  SetStatus(generation, 2, "Buffering");
  auto* codec = Board::GetInstance().GetAudioCodec();
  if (!codec->started()) codec->Start();
  LogStage("codec_start", codec->es8311_found());
  if (generation != generation_) {
    http.end();
    return;
  }
  if (!codec->es8311_found()) {
    SetStatus(generation, 4, "Speaker unavailable");
    http.end();
    return;
  }
  HMP3Decoder decoder = MP3InitDecoder();
  LogStage("decoder_init", decoder != nullptr);
  if (!decoder) {
    SetStatus(generation, 4, "Decoder unavailable");
    http.end();
    return;
  }
  std::unique_ptr<uint8_t[]> input(new (std::nothrow) uint8_t[kInputSize]);
  std::unique_ptr<int16_t[]> output(new (std::nothrow) int16_t[kMaxOutputSamples]);
  std::unique_ptr<int16_t[]> stereo(new (std::nothrow) int16_t[kMaxOutputSamples]);
  if (!input || !output || !stereo) {
    SetStatus(generation, 4, "No memory");
    MP3FreeDecoder(decoder);
    http.end();
    return;
  }
  LogStage("buffers_ready");
  auto* stream = http.getStreamPtr();
  size_t buffered = 0;
  uint32_t last_data = millis();
  bool played = false;
  uint32_t frames_decoded = 0;
  while (generation == generation_ && http.connected() && !HwTest::GetInstance().IsBusy()) {
    const size_t available = stream->available();
    if (available && buffered < kInputSize) {
      const size_t read = stream->readBytes(input.get() + buffered, std::min<size_t>(available, kInputSize - buffered));
      buffered += read;
      if (read) last_data = millis();
    }
    if (buffered < 2048 && millis() - last_data < kNoDataTimeoutMs) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!buffered || millis() - last_data >= kNoDataTimeoutMs) break;
    const int offset = MP3FindSyncWord(input.get(), buffered);
    if (offset < 0) {
      buffered = 0;
      continue;
    }
    if (offset) {
      memmove(input.get(), input.get() + offset, buffered - offset);
      buffered -= offset;
    }
    if (buffered < 2048 && available) continue;
    unsigned char* cursor = input.get();
    int remaining = buffered;
    const int error = MP3Decode(decoder, &cursor, &remaining, output.get(), 0);
    if (!played && error == 0) LogStage("first_decode");
    if (error == ERR_MP3_INDATA_UNDERFLOW) {
      if (buffered == kInputSize) { memmove(input.get(), input.get() + 1, --buffered); }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const size_t consumed = error ? 1 : buffered - static_cast<size_t>(remaining);
    if (consumed > buffered) break;
    buffered -= consumed;
    memmove(input.get(), input.get() + consumed, buffered);
    if (error) continue;
    MP3FrameInfo info{};
    MP3GetLastFrameInfo(decoder, &info);
    if (generation != generation_) break;
    if ((info.nChans != 1 && info.nChans != 2) || info.outputSamps < 1 ||
        info.outputSamps > kMaxOutputSamples || info.samprate < 8000 || info.samprate > 48000) continue;
    UpdateSpectrum(output.get(), info.outputSamps / info.nChans, info.nChans, info.samprate, generation);
    if (codec->sample_rate() != info.samprate && !codec->SetSampleRate(info.samprate)) {
      SetStatus(generation, 4, "Audio clock failed");
      break;
    }
    const int frames = info.outputSamps / info.nChans;
    const int16_t* samples = output.get();
    if (info.nChans == 1) {
      for (int i = 0; i < frames; ++i) stereo[i * 2] = stereo[i * 2 + 1] = output[i];
      samples = stereo.get();
    }
    if (!played) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation != generation_) break;
      codec->EnableOutput(true);
      played = true;
      LogStage("first_audio");
    }
    SetStatus(generation, 3, "Playing", info.bitrate / 1000);
    // The MP3 buffers are the largest writes this task makes; verify the heap every ~100 frames
    // (roughly every 2.5 s of audio) so an overflow here is caught before lwIP trips over it.
    if (++frames_decoded % 100 == 0) HeapGuard::Check("radio:decode_loop");
    int written = 0;
    while (written < frames && generation == generation_) {
      const int count = codec->Write(samples + written * 2, frames - written, 300);
      if (count <= 0) break;
      written += count;
    }
  }
  MP3FreeDecoder(decoder);
  http.end();
  LogStage("stream_end", played);
  if (generation == generation_) SetStatus(generation, 4, played ? "Reconnecting" : "Stream unavailable");
}
