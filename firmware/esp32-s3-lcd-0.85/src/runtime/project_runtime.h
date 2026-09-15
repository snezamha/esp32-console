#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "elf_loader/loader_config.h"
#include "elf_loader/esp_elf.h"
class Canvas;
struct Theme;

// Generic loader: it knows only the display ABI, never a project's implementation or id.
class ProjectRuntime {
 public:
  static ProjectRuntime& Get() { static ProjectRuntime self; return self; }
  void Begin();
  std::string Start(const std::string& command_id, const std::string& arg, const std::string& server, bool insecure, const std::string& token);
  std::vector<std::string> Loop();
  std::string Cancel(const std::string& stop_id, const std::string& target);
  std::string Report();
  bool Draw(Canvas&, int x, int y, int w, int h, const Theme&);
  void SetData(const std::string& data);
  const std::string& Id() const { return id_; }
  const std::string& Version() const { return version_; }
  const std::string& Sha256() const { return sha256_; }
  bool SafeMode() const { return safe_mode_; }
  bool Busy() const { return busy_ || testing_; }
  int Progress() const { return progress_; }
 private:
  static void DownloadTask(void* arg);
  void Log(const std::string& message, bool error = false);
  struct Metadata { std::string id, name, version, board; int abi = 0; };
  bool Inspect(const std::vector<uint8_t>& bytes, Metadata& metadata);
  bool Load(const std::vector<uint8_t>& bytes, esp_elf_t& elf, Metadata& metadata);
  bool Activate(const std::vector<uint8_t>& bytes, const Metadata& metadata);
  bool RestorePrevious(const std::string& reason);
  void RecordHealthyFrame();
  esp_elf_t elf_{};
  bool loaded_ = false, mounted_ = false, testing_ = false, ack_ready_ = false, safe_mode_ = false, healthy_recorded_ = false;
  bool ack_ok_ = false;
  struct Entry { int seq; bool error; std::string message; };
  std::vector<Entry> logs_;
  SemaphoreHandle_t log_mutex_ = nullptr;
  int next_log_ = 1;
  int slot_ = 0;
  std::string id_ = "none";
  std::string version_, sha256_;
  std::string data_[2];
  uint32_t data_at_ = 0;
  std::atomic<bool> busy_{false}, done_{false};
  std::atomic<int> progress_{0}, stage_{0};
  std::atomic<size_t> received_{0};
  std::atomic<bool> cancel_{false};
  std::string command_id_, target_id_, target_version_, url_, md5_, expected_sha256_, error_, token_, stop_id_;
  bool insecure_ = false;
  size_t expected_size_ = 0;
  uint32_t first_frame_at_ = 0;
  std::vector<uint8_t> downloaded_;
};
