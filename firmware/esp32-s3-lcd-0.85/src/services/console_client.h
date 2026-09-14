#pragma once

#include <freertos/FreeRTOS.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Link to the ESP32 Console web app (POST <server>/api/device/sync).
//
// While Wi-Fi is connected the board keeps a long poll open: the server answers as soon as
// there is something to deliver, or after 25 s.
//   unlinked  the server hands out a 6-digit code; the screen shows it until someone enters
//             it under Devices → Add Device, then the board receives and stores its token
//   linked    the board applies settings changed in the console and runs its commands
// Changes on the board itself (settings, check results, update progress) go up right away in a
// second, short request. Removing the device in the console (or Unlink on the board) drops the
// token, and the next check-in shows a new code.
//
// Requests run on background tasks; results are handled in Loop() on the main task.
class ConsoleClient {
 public:
  enum class State { Off, Offline, Connecting, Pairing, Linked, Error };
  using Values = std::vector<std::pair<std::string, std::string>>;
  struct Command {
    std::string id;
    std::string type;
    std::string arg;  // URL-decoded
  };

  static ConsoleClient& GetInstance() {
    static ConsoleClient instance;
    return instance;
  }

  void Begin();
  void Loop(uint32_t now_ms);

  // `url` like https://console.example.com; empty turns the link off.
  void SetServer(const std::string& url);
  // Skip HTTPS certificate checks (self-signed servers).
  void SetInsecure(bool insecure);
  bool Insecure() const { return insecure_; }
  // Forgets the token (telling the server when online) and starts pairing again.
  void Unlink();

  State GetState() const { return state_; }
  // "Off", "Offline", "Connecting", "Pairing", "Linked", "Error"
  const char* StateText() const;
  const std::string& Code() const { return code_; }
  const std::string& Server() const { return server_; }
  // Server without scheme, for the small screen.
  std::string ServerHost() const;
  const std::string& Name() const { return name_; }
  const std::string& LastError() const { return error_; }

  // Over-the-air update progress 0–100, or -1 when no update is running.
  int OtaProgress() const { return ota_active_ ? ota_progress_.load() : -1; }

  // Form fields describing the board: settings (s.*), check results (t.*), networks. A change
  // in this text is uploaded right away.
  void SetStateProvider(std::function<std::string()> provider) { state_provider_ = provider; }
  // Form fields that change all the time (battery, memory, uptime); sent with every request.
  void SetTelemetryProvider(std::function<std::string()> provider) { telemetry_provider_ = provider; }
  // Called with the `set.<key>` values received from the console.
  void SetApplyHandler(std::function<void(const Values&)> handler) { apply_handler_ = handler; }
  // Runs a command; return "ok|<result>" or "fail|<error>" to confirm it, or "" for none.
  // Over-the-air updates are handled here and never reach the handler.
  void SetCommandHandler(std::function<std::string(const Command&)> handler) { command_handler_ = handler; }
  // Called when state, code, name or update progress change (for redrawing).
  void SetOnChange(std::function<void()> callback) { on_change_ = callback; }

  // Value of `key` in an `a=1&b=2` form string, URL-decoded.
  static std::string FormValue(const std::string& form, const std::string& key);

 private:
  struct Slot {
    const char* name;
    std::atomic<bool> busy{false};
    std::atomic<bool> done{false};
    std::string url;
    std::string body;
    uint32_t timeout_ms = 0;
    bool tls = false;
    bool insecure = false;
    int code = 0;
    std::string response;
    // Acks and state text carried by this request, restored if it fails.
    std::vector<std::string> acks;
    std::string state;
  };

  ConsoleClient() = default;
  static void RequestTask(void* arg);
  static void OtaTask(void* arg);
  bool Start(Slot& slot, const std::string& body, uint32_t timeout_ms);
  std::string CommonFields();
  void StartPoll(uint32_t now_ms);
  void StartReport(uint32_t now_ms);
  void HandlePoll(int http_code, const std::string& body);
  void HandleCommand(const Command& command);
  void StartOta(const Command& command);
  void FinishOta(uint32_t now_ms);
  void SetState(State state);
  void Save();
  void NewSecret();
  void Changed();
  void NoteAcksDelivered(const std::vector<std::string>& delivered, uint32_t now_ms);

  std::string server_;
  bool insecure_ = false;
  std::string token_;
  int rev_ = 0;
  std::string secret_;
  // Token to revoke on the server once online, after Unlink() on the board.
  std::string revoke_token_;

  State state_ = State::Off;
  std::string code_;
  std::string name_;
  std::string error_;
  uint32_t next_poll_ms_ = 0;
  uint32_t last_poll_start_ms_ = 0;
  uint32_t next_report_ms_ = 0;
  int failures_ = 0;

  Slot poll_{"console_poll"};
  Slot report_{"console_report"};
  std::vector<std::string> acks_;
  std::string reported_state_;
  uint32_t last_state_check_ms_ = 0;
  std::vector<std::string> seen_commands_;

  // Over-the-air update (runs on its own task).
  std::atomic<bool> ota_active_{false};
  std::atomic<bool> ota_finished_{false};
  std::atomic<int> ota_progress_{0};
  int ota_shown_progress_ = -1;
  std::string ota_url_;
  std::string ota_md5_;
  std::string ota_version_;
  std::string ota_command_id_;
  size_t ota_size_ = 0;
  std::string ota_error_;
  bool ota_ok_ = false;
  // Reported state of the last update: "downloading", "done", "failed" or empty.
  std::string ota_state_;
  uint32_t restart_at_ms_ = 0;
  int ota_reported_progress_ = -1;
  // Set while the "done"/"failed" ack has not been delivered yet.
  std::string ota_pending_ack_id_;

  std::function<std::string()> state_provider_;
  std::function<std::string()> telemetry_provider_;
  std::function<void(const Values&)> apply_handler_;
  std::function<std::string(const Command&)> command_handler_;
  std::function<void()> on_change_;
};
