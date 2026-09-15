#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <FS.h>

#include "console_client.h"

// SD card file manager driven from the web console. The board starts every transfer itself
// (it cannot be reached directly), so file contents travel through the console server:
//   sd_mount                                   re-detect and mount the card
//   sd_unmount                                 close files and release the SDMMC bus
//   sd_list      path                      POST `d|f <tab> size <tab> mtime <tab> name` lines to src
//   sd_download  path, src                 POST the file's bytes to src
//   sd_upload    path, src, size, sha256   GET src into path (verified, then renamed into place)
//   sd_delete    path (folders recursively)
//   sd_mkdir     path
//   sd_rename    path, to
//   sd_format    erase the card with a new FAT file system
// One operation runs at a time, in a background task; its ack is returned from Loop().
class SdFiles {
 public:
  static SdFiles& Get() {
    static SdFiles self;
    return self;
  }
  // Returns an immediate "fail|…" ack, or "" when the operation started.
  std::string Start(const ConsoleClient::Command& command, const std::string& server, bool insecure,
                    const std::string& token);
  std::vector<std::string> Loop();
  bool Busy() const { return busy_; }

 private:
  static void Task(void* arg);
  std::string Run();
  std::string Post(const uint8_t* data, size_t size, fs::File* file);
  std::string Receive();

  std::atomic<bool> busy_{false}, done_{false};
  std::string id_, type_, path_, to_, url_, sha256_, token_, result_;
  size_t size_ = 0;
  bool insecure_ = false;
};
