#pragma once

#include <atomic>
#include <string>

// SD card file manager over the USB serial console, for a browser connected with Web Serial.
// Much faster than the console's relay: no polling, no TLS, full USB speed. Paths are
// percent-encoded so they may contain spaces. Every reply is one JSON line starting {"fs":
//   fs ls <path>                   {"fs":"entry",…} per entry, then {"fs":"ok","op":"ls",…}
//   fs get <path>                  {"fs":"begin","size":N}, {"fs":"data","b64":…}…, {"fs":"ok"}
//   fs put <path> <size> <sha256>  {"fs":"ready","block":B}; the host then sends raw blocks of at
//                                  most B bytes, each answered {"fs":"ack","done":N}; {"fs":"ok"}
//   fs rm <path> | fs mkdir <path> | fs mv <from> <to> | fs mount | fs unmount | fs info
//   fs format                      {"fs":"progress","done":N,"total":N}… then {"fs":"ok"}
// Failures answer {"fs":"error","op":…,"message":…}.
class SerialFs {
 public:
  // Serial RX queue the transfer blocks must fit in; set before Serial.begin().
  static constexpr size_t kRxBufferBytes = 4096;

  static SerialFs& Get() {
    static SerialFs self;
    return self;
  }
  void Handle(const std::string& line);
  // Reports the progress and result of a running format.
  void Loop(uint32_t now_ms);
  bool Busy() const { return formatting_; }

 private:
  static void FormatTask(void* arg);
  void List(const std::string& path);
  void Get(const std::string& path);
  void Put(const std::string& path, size_t size, const std::string& sha256);

  std::atomic<bool> formatting_{false}, format_done_{false}, format_ok_{false};
  uint32_t last_progress_ms_ = 0;
};
