#include "sd_files.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include <algorithm>

#include "../board/board.h"
#include "../runtime/project_runtime.h"
#include "../storage/sd_card.h"
#include "serial_fs.h"

extern const uint8_t kCertBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCertBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr size_t kMaxListing = 1000;
}  // namespace

// Absolute path without empty, "." or ".." segments.
bool SdFiles::ValidPath(const std::string& path) {
  if (path.empty() || path[0] != '/' || path.size() > 200) return false;
  if (path == "/") return true;
  if (path.back() == '/') return false;
  for (size_t start = 1;;) {
    const size_t end = path.find('/', start);
    const std::string segment = path.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == ".." ||
        std::any_of(segment.begin(), segment.end(), [](unsigned char c) { return c < 0x20 || c == '\\'; })) return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  }
}

bool SdFiles::RemoveTree(fs::FS& fs, const std::string& path) {
  File entry = fs.open(path.c_str());
  if (!entry) return false;
  if (!entry.isDirectory()) {
    entry.close();
    return fs.remove(path.c_str());
  }
  std::vector<std::string> children;
  for (File child = entry.openNextFile(); child; child = entry.openNextFile()) {
    children.push_back(path + "/" + child.name());
    child.close();
  }
  entry.close();
  bool ok = true;
  for (const auto& child : children) ok &= RemoveTree(fs, child);
  return fs.rmdir(path.c_str()) && ok;
}

namespace {
// Streams a file to HTTPClient while counting the bytes handed over.
class CountingStream : public Stream {
 public:
  CountingStream(fs::File* file, std::atomic<uint64_t>& count) : file_(*file), count_(count) {}
  int available() override { return file_.available(); }
  int read() override {
    const int value = file_.read();
    if (value >= 0) count_++;
    return value;
  }
  int peek() override { return file_.peek(); }
  size_t readBytes(uint8_t* buffer, size_t length) override {
    const size_t count = file_.read(buffer, length);
    count_ += count;
    return count;
  }
  size_t write(uint8_t) override { return 0; }

 private:
  fs::File& file_;
  std::atomic<uint64_t>& count_;
};

std::string Hex(const uint8_t* digest) {
  static const char hex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 15];
  }
  return out;
}
}  // namespace

std::string SdFiles::Start(const ConsoleClient::Command& command, const std::string& server, bool insecure,
                           const std::string& token) {
  if (busy_) return "fail|Another SD card operation is running";
  if (SerialFs::Get().Busy()) return "fail|The card is being formatted over USB";
  if (ProjectRuntime::Get().Busy()) return "fail|A project installation is running";
  const auto& arg = command.arg;
  type_ = command.type;
  path_ = type_ == "sd_format" || type_ == "sd_mount" || type_ == "sd_unmount"
              ? "/"
              : ConsoleClient::FormValue(arg, "path");
  to_ = ConsoleClient::FormValue(arg, "to");
  const auto src = ConsoleClient::FormValue(arg, "src");
  sha256_ = ConsoleClient::FormValue(arg, "sha256");
  size_ = strtoul(ConsoleClient::FormValue(arg, "size").c_str(), nullptr, 10);
  const bool known = type_ == "sd_mount" || type_ == "sd_unmount" || type_ == "sd_list" || type_ == "sd_download" || type_ == "sd_upload" || type_ == "sd_delete" ||
                     type_ == "sd_mkdir" || type_ == "sd_rename" || type_ == "sd_format";
  if (!known) return "fail|Unknown SD card operation";
  if (!ValidPath(path_) || (type_ == "sd_rename" && !ValidPath(to_))) return "fail|Invalid path";
  if ((type_ == "sd_delete" || type_ == "sd_rename") && path_ == "/") return "fail|The card root cannot be changed";
  if ((type_ == "sd_list" || type_ == "sd_download" || type_ == "sd_upload") && src.rfind("/api/device/files/", 0) != 0) return "fail|Invalid transfer";
  if (type_ == "sd_upload" && sha256_.size() != 64) return "fail|Invalid transfer";
  id_ = command.id;
  url_ = server + src;
  token_ = token;
  insecure_ = insecure;
  result_.clear();
  progress_done_ = 0;
  progress_total_ = 0;
  // Destructive operations must not race the running module's asset reads.
  if (type_ == "sd_mount" || type_ == "sd_unmount" || type_ == "sd_delete" || type_ == "sd_rename" || type_ == "sd_format") ProjectRuntime::Get().LockSd();
  done_ = false;
  busy_ = true;
  if (xTaskCreatePinnedToCore(Task, "sd_files", 12288, this, 1, nullptr, 0) != pdPASS) {
    busy_ = false;
    ProjectRuntime::Get().RemountSd();
    return "fail|No memory for the SD card operation";
  }
  return "";
}

std::string SdFiles::Report() const {
  if (!busy_) return "";
  uint64_t done = progress_done_, total = progress_total_;
  if (type_ == "sd_format") {
    const auto* sd = Board::GetInstance().GetSdCard();
    total = sd->format_expected();
    done = std::min(sd->format_written(), total);
  }
  return "&sd.job=" + id_ + "%7C" + std::to_string(done) + "%7C" + std::to_string(total);
}

std::vector<std::string> SdFiles::Loop() {
  if (!done_.exchange(false)) return {};
  busy_ = false;
  if (type_ == "sd_mount") ProjectRuntime::Get().UnlockSd();
  else if (type_ == "sd_unmount") ProjectRuntime::Get().RefreshSd();
  else if (type_ == "sd_delete" || type_ == "sd_rename" || type_ == "sd_format") ProjectRuntime::Get().RemountSd();
  else if (type_ != "sd_list" && type_ != "sd_download") ProjectRuntime::Get().RefreshSd();
  return {id_ + "|" + result_};
}

void SdFiles::Task(void* arg) {
  auto* self = static_cast<SdFiles*>(arg);
  self->result_ = self->Run();
  self->done_ = true;
  vTaskDelete(nullptr);
}

std::string SdFiles::Run() {
  auto* sd = Board::GetInstance().GetSdCard();
  if (type_ == "sd_mount") {
    sd->Unmount();
    return std::string(sd->Mount() ? "ok|" : "fail|") + sd->ProblemMessage();
  }
  if (type_ == "sd_unmount") {
    sd->Unmount();
    return "ok|SD card unmounted";
  }
  if (type_ == "sd_format") {
    return sd->Format() ? "ok|SD card formatted" : "fail|Formatting failed. Check that a card is inserted and not locked.";
  }
  if (!sd->Mount()) return std::string("fail|") + sd->ProblemMessage();
  auto& fs = sd->fs();

  if (type_ == "sd_list") {
    File dir = fs.open(path_.c_str());
    if (!dir || !dir.isDirectory()) return "fail|Folder not found";
    std::string body;
    size_t count = 0;
    bool truncated = false;
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (count == kMaxListing) {
        truncated = true;
        entry.close();
        break;
      }
      const bool folder = entry.isDirectory();
      body += std::string(folder ? "d" : "f") + "\t" + std::to_string(folder ? 0 : entry.size()) + "\t" +
              std::to_string(static_cast<long long>(entry.getLastWrite())) + "\t" + entry.name() + "\n";
      entry.close();
      count++;
    }
    dir.close();
    const auto error = Post(reinterpret_cast<const uint8_t*>(body.data()), body.size(), nullptr);
    if (!error.empty()) return "fail|" + error;
    return "ok|" + std::to_string(count) + (truncated ? " entries (first 1000 shown)" : " entries");
  }
  if (type_ == "sd_download") {
    File file = fs.open(path_.c_str(), FILE_READ);
    if (!file || file.isDirectory()) return "fail|File not found";
    if (file.size() > 4 * 1024 * 1024) return "fail|Files larger than 4 MB cannot be downloaded through the console";
    progress_total_ = file.size();
    const auto error = Post(nullptr, file.size(), &file);
    file.close();
    return error.empty() ? "ok|Sent to console" : "fail|" + error;
  }
  if (type_ == "sd_upload") {
    const auto error = Receive();
    return error.empty() ? "ok|Uploaded" : "fail|" + error;
  }
  if (type_ == "sd_delete") {
    if (!fs.exists(path_.c_str())) return "fail|Not found";
    return RemoveTree(fs, path_) ? "ok|Deleted" : "fail|Could not delete everything";
  }
  if (type_ == "sd_mkdir") {
    if (fs.exists(path_.c_str())) return "fail|Already exists";
    return fs.mkdir(path_.c_str()) ? "ok|Folder created" : "fail|Could not create the folder";
  }
  if (type_ == "sd_rename") {
    if (!fs.exists(path_.c_str())) return "fail|Not found";
    if (fs.exists(to_.c_str())) return "fail|The new name already exists";
    return fs.rename(path_.c_str(), to_.c_str()) ? "ok|Renamed" : "fail|Could not rename";
  }
  return "fail|Unknown SD card operation";
}

// Sends either a memory buffer or a whole file to the console.
std::string SdFiles::Post(const uint8_t* data, size_t size, fs::File* file) {
  HTTPClient http;
  NetworkClient plain;
  NetworkClientSecure secure;
  const bool tls = url_.rfind("https://", 0) == 0;
  if (tls) {
    if (insecure_) secure.setInsecure();
    else secure.setCACertBundle(kCertBundleStart, kCertBundleEnd - kCertBundleStart);
  }
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  if (!(tls ? http.begin(secure, url_.c_str()) : http.begin(plain, url_.c_str()))) return "Could not reach the console";
  http.addHeader("Authorization", ("Bearer " + token_).c_str());
  http.addHeader("Content-Type", "application/octet-stream");
  int code;
  if (file) {
    CountingStream stream(file, progress_done_);
    code = http.sendRequest("POST", &stream, size);
  } else {
    code = http.POST(const_cast<uint8_t*>(data), size);
  }
  http.end();
  return code == 200 ? "" : "Console rejected the transfer (HTTP " + std::to_string(code) + ")";
}

// Downloads the console's copy into `<path>.part`, checks size and SHA-256, then replaces path.
std::string SdFiles::Receive() {
  auto* sd = Board::GetInstance().GetSdCard();
  auto& fs = sd->fs();
  const uint64_t free = sd->total_bytes() > sd->used_bytes() ? sd->total_bytes() - sd->used_bytes() : 0;
  if (free < size_ + 64 * 1024) return "Not enough free space on the SD card";
  progress_total_ = size_;
  HTTPClient http;
  NetworkClient plain;
  NetworkClientSecure secure;
  const bool tls = url_.rfind("https://", 0) == 0;
  if (tls) {
    if (insecure_) secure.setInsecure();
    else secure.setCACertBundle(kCertBundleStart, kCertBundleEnd - kCertBundleStart);
  }
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!(tls ? http.begin(secure, url_.c_str()) : http.begin(plain, url_.c_str()))) return "Could not reach the console";
  http.addHeader("Authorization", ("Bearer " + token_).c_str());
  const int code = http.GET();
  if (code != 200 || http.getSize() != static_cast<int>(size_)) {
    http.end();
    return "Console download failed (HTTP " + std::to_string(code) + ")";
  }
  const std::string part = path_ + ".part";
  fs.remove(part.c_str());
  File out = fs.open(part.c_str(), FILE_WRITE);
  if (!out) {
    http.end();
    return "Could not create the file; check that its folder exists";
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  auto* stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t total = 0;
  uint32_t last_data = millis();
  std::string error;
  while (total < size_) {
    const size_t available = stream->available();
    if (!available) {
      if (!http.connected() || millis() - last_data > 15000) { error = "Download interrupted"; break; }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const size_t count = stream->readBytes(buffer, std::min({available, sizeof(buffer), size_ - total}));
    if (out.write(buffer, count) != count) { error = "SD card write failed"; break; }
    mbedtls_sha256_update(&ctx, buffer, count);
    total += count;
    progress_done_ = total;
    last_data = millis();
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  out.close();
  http.end();
  if (error.empty() && Hex(digest) != sha256_) error = "Checksum mismatch";
  if (error.empty()) {
    fs.remove(path_.c_str());
    if (!fs.rename(part.c_str(), path_.c_str())) error = "Could not finish the file";
  }
  if (!error.empty()) fs.remove(part.c_str());
  return error;
}
