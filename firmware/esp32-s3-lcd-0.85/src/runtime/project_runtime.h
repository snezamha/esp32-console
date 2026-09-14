#pragma once
#include <atomic>
#include <string>
#include <vector>
#include "elf_loader/loader_config.h"
#include "elf_loader/esp_elf.h"
class Canvas;
struct Theme;

// Generic loader: it knows only the display ABI, never a project's implementation or id.
class ProjectRuntime {
 public:
  static ProjectRuntime& Get() { static ProjectRuntime self; return self; }
  void Begin();
  std::string Start(const std::string& command_id, const std::string& arg, const std::string& server, bool insecure);
  std::string Loop(); // Completed command acknowledgement, including its id.
  bool Draw(Canvas&, int x, int y, int w, int h, const Theme&);
  void SetData(const std::string& data);
  const std::string& Id() const { return id_; }
  bool Busy() const { return busy_; }
  int Progress() const { return progress_; }
 private:
  static void DownloadTask(void* arg);
  bool Load(const std::vector<uint8_t>& bytes, esp_elf_t& elf);
  bool Activate(const std::vector<uint8_t>& bytes, const std::string& id);
  esp_elf_t elf_{};
  bool loaded_ = false, mounted_ = false;
  int slot_ = 0;
  std::string id_ = "none";
  std::string data_[2];
  uint32_t data_at_ = 0;
  std::atomic<bool> busy_{false}, done_{false};
  std::atomic<int> progress_{0};
  std::string command_id_, target_id_, url_, md5_, error_;
  bool insecure_ = false;
  size_t expected_size_ = 0;
  std::vector<uint8_t> downloaded_;
};
