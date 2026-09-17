#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

// Owns the network/MP3 worker; project modules only request a station and read its state.
class RadioStream {
 public:
  static RadioStream& Get();
  int Start(const char* url);
  void Stop();
  std::string CurrentUrl();
  bool WorkerRunning();
  int Spectrum(uint8_t* levels, uint32_t capacity);
  // 0 stopped, 1 connecting, 2 buffering, 3 playing, 4 error.
  int Status(char* text, uint32_t capacity, int* bitrate_kbps);
  bool Active();

 private:
  static void Task(void* arg);
  void Run(const std::string& url, uint32_t generation);
  void SetStatus(uint32_t generation, int state, const char* message, int bitrate = 0);
  void UpdateSpectrum(const int16_t* pcm, int frames, int channels, int sample_rate, uint32_t generation);
  std::mutex mutex_;
  std::string url_;
  std::string message_;
  std::atomic<uint32_t> generation_{0};
  bool task_running_ = false;
  int state_ = 0;
  int bitrate_ = 0;
  std::array<uint8_t, 8> spectrum_{};
  uint32_t spectrum_at_ms_ = 0;
  uint32_t last_spectrum_compute_ms_ = 0;
};
