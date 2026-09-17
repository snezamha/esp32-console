#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "elf_loader/loader_config.h"
#include "elf_loader/esp_elf.h"
class Canvas;
class HTTPClient;
class NetworkClient;
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
  bool RadioMenuSupported() const { return loaded_ && id_ == "radio" && abi_ >= 7 && !busy_ && !testing_; }
  bool RadioMenuOpen() const { return RadioMenuSupported() && radio_menu_open_; }
  bool RadioMenuRecentlyClosed() const;
  bool OwnsLed() const;
  bool SdIoAvailable() const { return !sd_locked_; }
  int Progress() const { return progress_; }
  // `mounted|total bytes|free bytes` of the SD card, as last checked.
  std::string SdReport() const;
  // Re-mounts the card after closing the running module's open asset files.
  bool RemountSd();
  // Closes asset files and pauses the module's SD access while the file manager changes the card.
  void LockSd();
  // Resumes project SD access after a file-manager mount attempt has finished.
  void UnlockSd();
  void RefreshSd();
 private:
  static void DownloadTask(void* arg);
  void LaunchDownload();
  void Log(const std::string& message, bool error = false);
  struct Metadata { std::string id, name, version, board; int abi = 0; };
  bool Inspect(const std::vector<uint8_t>& bytes, Metadata& metadata);
  bool Load(const std::vector<uint8_t>& bytes, esp_elf_t& elf, Metadata& metadata);
  bool Activate(const std::vector<uint8_t>& bytes, const Metadata& metadata);
  bool RestorePrevious(const std::string& reason);
  void RecordHealthyFrame();
  // SD card assets live in /projects/<id>/<version>; `.complete` holds the verified index SHA-256.
  bool AssetsReady(const std::string& id, const std::string& version, const std::string& digest);
  void UseAssets(const std::string& digest);
  void CleanupAssets();
  std::string Fetch(HTTPClient& http, NetworkClient& client, const std::string& url, size_t expected, uint32_t limit_ms, const std::function<bool(const uint8_t*, size_t)>& sink);
  std::string InstallAssets(HTTPClient& http, NetworkClient& client);
  esp_elf_t elf_{};
  bool loaded_ = false, mounted_ = false, testing_ = false, ack_ready_ = false, safe_mode_ = false, healthy_recorded_ = false;
  bool ack_ok_ = false;
  bool sd_required_ = false, sd_ready_ = false, sd_locked_ = false;
  int abi_ = 0;
  bool radio_menu_open_ = false;
  uint32_t radio_menu_closed_at_ms_ = 0;
  uint32_t sd_checked_at_ = 0;
  uint64_t sd_total_ = 0, sd_free_ = 0;
  std::string assets_digest_;
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
  std::atomic<uint64_t> received_{0};
  std::atomic<bool> cancel_{false};
  std::string command_id_, target_id_, target_version_, url_, md5_, expected_sha256_, error_, token_, stop_id_;
  std::string assets_url_, assets_sha256_;
  uint64_t assets_bytes_ = 0;
  std::atomic<uint64_t> total_size_{0};
  bool insecure_ = false;
  size_t expected_size_ = 0;
  uint32_t first_frame_at_ = 0;
  std::vector<uint8_t> downloaded_;
  std::string radio_url_before_install_;
  bool waiting_for_radio_ = false;
  uint32_t radio_stop_started_ms_ = 0;
  uint32_t radio_stopped_at_ms_ = 0;
  uint32_t resume_radio_at_ms_ = 0;
  uint32_t download_finished_at_ms_ = 0;
};
