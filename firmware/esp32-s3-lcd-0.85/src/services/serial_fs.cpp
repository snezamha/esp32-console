#include "serial_fs.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "../board/board.h"
#include "../runtime/project_runtime.h"
#include "../storage/sd_card.h"
#include "sd_files.h"

namespace {
constexpr size_t kMaxListing = 2000;
constexpr size_t kGetChunkBytes = 2400;  // 3200 base64 characters per line, within the TX buffer
constexpr uint32_t kIoTimeoutMs = 3000;
constexpr size_t kMaxPutBytes = 512 * 1024 * 1024;

std::string Decode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '%' && i + 2 < text.size()) {
      out += static_cast<char>(strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string Json(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char escaped[8];
      snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

// Serial writes do not wait for the host (the console must never stall on an absent reader), so a
// line is only handed over once the TX buffer can take all of it.
bool WriteLine(std::string line) {
  line += '\n';
  const uint32_t start = millis();
  while (Serial.availableForWrite() < static_cast<int>(line.size())) {
    if (millis() - start > kIoTimeoutMs) return false;
    delay(1);
  }
  return Serial.write(reinterpret_cast<const uint8_t*>(line.data()), line.size()) == line.size();
}

void Ok(const std::string& op, const std::string& extra = "") {
  WriteLine("{\"fs\":\"ok\",\"op\":" + Json(op) + extra + "}");
}

void Error(const std::string& op, const std::string& message) {
  WriteLine("{\"fs\":\"error\",\"op\":" + Json(op) + ",\"message\":" + Json(message) + "}");
}

std::string Hex(const uint8_t* digest) {
  static const char hex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 15];
  }
  return out;
}

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < line.size()) {
    const size_t end = line.find(' ', start);
    if (end != start) parts.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}
}  // namespace

void SerialFs::Handle(const std::string& line) {
  const auto args = Split(line);
  const std::string op = args.size() > 1 ? args[1] : "";
  auto arg = [&](size_t i) { return i < args.size() ? Decode(args[i]) : std::string(); };
  auto& runtime = ProjectRuntime::Get();
  auto* sd = Board::GetInstance().GetSdCard();

  if (formatting_) return Error(op, "The card is being formatted");
  if (SdFiles::Get().Busy()) return Error(op, "The console is using the SD card. Try again when it finishes.");
  if (runtime.Busy()) return Error(op, "A project installation is using the SD card");

  if (op == "info") {
    Ok(op, ",\"mounted\":" + std::string(sd->mounted() ? "true" : "false") + ",\"total\":" + std::to_string(sd->total_bytes()) +
               ",\"used\":" + std::to_string(sd->used_bytes()) + ",\"message\":" + Json(sd->ProblemMessage()));
    return;
  }
  if (op == "mount") {
    runtime.LockSd();
    sd->Unmount();
    const bool mounted = sd->Mount();
    runtime.UnlockSd();
    return mounted ? Ok(op, ",\"total\":" + std::to_string(sd->total_bytes()) + ",\"used\":" + std::to_string(sd->used_bytes()))
                   : Error(op, sd->ProblemMessage());
  }
  if (op == "unmount") {
    runtime.LockSd();
    sd->Unmount();
    runtime.RefreshSd();
    return Ok(op);
  }
  if (op == "format") {
    runtime.LockSd();
    format_done_ = false;
    formatting_ = true;
    last_progress_ms_ = 0;
    if (xTaskCreatePinnedToCore(FormatTask, "serial_fs", 8192, this, 1, nullptr, 0) != pdPASS) {
      formatting_ = false;
      runtime.RemountSd();
      return Error(op, "No memory for formatting");
    }
    return;
  }

  const std::string path = arg(2);
  if (!SdFiles::ValidPath(path)) return Error(op, "Invalid path");
  if (!sd->Mount()) return Error(op, sd->ProblemMessage());
  auto& fs = sd->fs();

  if (op == "ls") return List(path);
  if (op == "get") return Get(path);
  if (op == "put") {
    const std::string size = arg(3), sha256 = arg(4);
    if (path == "/" || size.empty() || sha256.size() != 64) return Error(op, "Invalid upload");
    return Put(path, strtoull(size.c_str(), nullptr, 10), sha256);
  }
  if (op == "mkdir") {
    if (fs.exists(path.c_str())) return Error(op, "Already exists");
    const bool ok = fs.mkdir(path.c_str());
    runtime.RefreshSd();
    return ok ? Ok(op) : Error(op, "Could not create the folder");
  }
  if (op == "rm" || op == "mv") {
    const std::string to = arg(3);
    if (path == "/") return Error(op, "The card root cannot be changed");
    if (!fs.exists(path.c_str())) return Error(op, "Not found");
    if (op == "mv" && (!SdFiles::ValidPath(to) || to == "/")) return Error(op, "Invalid new name");
    if (op == "mv" && fs.exists(to.c_str())) return Error(op, "The new name already exists");
    // Closes the running module's asset files before the card changes under it.
    runtime.LockSd();
    const bool ok = op == "rm" ? SdFiles::RemoveTree(fs, path) : fs.rename(path.c_str(), to.c_str());
    runtime.RemountSd();
    return ok ? Ok(op) : Error(op, op == "rm" ? "Could not delete everything" : "Could not rename");
  }
  Error(op, "Unknown file operation");
}

void SerialFs::List(const std::string& path) {
  auto* sd = Board::GetInstance().GetSdCard();
  File dir = sd->fs().open(path.c_str());
  if (!dir || !dir.isDirectory()) return Error("ls", "Folder not found");
  size_t count = 0;
  bool truncated = false;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (count == kMaxListing) {
      truncated = true;
      entry.close();
      break;
    }
    const bool folder = entry.isDirectory();
    const bool sent = WriteLine("{\"fs\":\"entry\",\"dir\":" + std::string(folder ? "true" : "false") +
                                ",\"size\":" + std::to_string(folder ? 0 : entry.size()) +
                                ",\"mtime\":" + std::to_string(static_cast<long long>(entry.getLastWrite())) +
                                ",\"name\":" + Json(entry.name()) + "}");
    entry.close();
    if (!sent) {
      dir.close();
      return;
    }
    count++;
  }
  dir.close();
  Ok("ls", ",\"count\":" + std::to_string(count) + ",\"truncated\":" + (truncated ? "true" : "false") +
               ",\"total\":" + std::to_string(sd->total_bytes()) + ",\"used\":" + std::to_string(sd->used_bytes()));
}

void SerialFs::Get(const std::string& path) {
  File file = Board::GetInstance().GetSdCard()->fs().open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) return Error("get", "File not found");
  const size_t size = file.size();
  if (!WriteLine("{\"fs\":\"begin\",\"op\":\"get\",\"size\":" + std::to_string(size) + "}")) return;
  std::unique_ptr<uint8_t[]> raw(new uint8_t[kGetChunkBytes]);
  std::unique_ptr<unsigned char[]> text(new unsigned char[kGetChunkBytes * 4 / 3 + 8]);
  size_t sent = 0;
  while (sent < size) {
    const size_t count = file.read(raw.get(), std::min(kGetChunkBytes, size - sent));
    if (!count) break;
    size_t length = 0;
    mbedtls_base64_encode(text.get(), kGetChunkBytes * 4 / 3 + 8, &length, raw.get(), count);
    if (!WriteLine("{\"fs\":\"data\",\"b64\":\"" + std::string(reinterpret_cast<char*>(text.get()), length) + "\"}")) {
      file.close();
      return;
    }
    sent += count;
  }
  file.close();
  sent == size ? Ok("get", ",\"size\":" + std::to_string(size)) : Error("get", "SD card read failed");
}

// Receives raw bytes into `<path>.part` in acknowledged blocks that fit the serial RX queue (it
// drops what does not fit), verifies the SHA-256, then replaces path.
void SerialFs::Put(const std::string& path, size_t size, const std::string& sha256) {
  auto* sd = Board::GetInstance().GetSdCard();
  auto& fs = sd->fs();
  const uint64_t available = sd->total_bytes() > sd->used_bytes() ? sd->total_bytes() - sd->used_bytes() : 0;
  if (size > kMaxPutBytes) return Error("put", "Files over 512 MB cannot be uploaded over USB");
  if (available < size + 64 * 1024) return Error("put", "Not enough free space on the SD card");
  const std::string part = path + ".part";
  fs.remove(part.c_str());
  File out = fs.open(part.c_str(), FILE_WRITE);
  if (!out) return Error("put", "Could not create the file; check that its folder exists");

  const size_t block = kRxBufferBytes - 256;
  auto* buffer = static_cast<uint8_t*>(heap_caps_malloc_prefer(block, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT));
  if (!buffer) {
    out.close();
    fs.remove(part.c_str());
    return Error("put", "No memory for the upload");
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  std::string error;
  size_t done = 0;
  if (!WriteLine("{\"fs\":\"ready\",\"op\":\"put\",\"block\":" + std::to_string(block) + "}")) error = "Host stopped reading";
  while (error.empty() && done < size) {
    const size_t want = std::min(block, size - done);
    size_t got = 0;
    uint32_t last_data = millis();
    while (got < want) {
      const size_t count = Serial.read(buffer + got, want - got);
      if (count) {
        got += count;
        last_data = millis();
      } else if (millis() - last_data > kIoTimeoutMs) {
        break;
      } else {
        delay(1);
      }
    }
    if (got < want) { error = "Upload interrupted"; break; }
    if (out.write(buffer, got) != got) { error = "SD card write failed"; break; }
    mbedtls_sha256_update(&ctx, buffer, got);
    done += got;
    if (!WriteLine("{\"fs\":\"ack\",\"done\":" + std::to_string(done) + "}")) error = "Host stopped reading";
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  free(buffer);
  out.close();
  if (error.empty() && Hex(digest) != sha256) error = "Checksum mismatch";
  if (error.empty()) {
    fs.remove(path.c_str());
    if (!fs.rename(part.c_str(), path.c_str())) error = "Could not finish the file";
  }
  if (!error.empty()) fs.remove(part.c_str());
  ProjectRuntime::Get().RefreshSd();
  error.empty() ? Ok("put", ",\"size\":" + std::to_string(size)) : Error("put", error);
}

void SerialFs::FormatTask(void* arg) {
  auto* self = static_cast<SerialFs*>(arg);
  self->format_ok_ = Board::GetInstance().GetSdCard()->Format();
  self->format_done_ = true;
  vTaskDelete(nullptr);
}

void SerialFs::Loop(uint32_t now_ms) {
  if (!formatting_) return;
  auto* sd = Board::GetInstance().GetSdCard();
  if (format_done_) {
    formatting_ = false;
    ProjectRuntime::Get().RemountSd();
    format_ok_ ? Ok("format", ",\"total\":" + std::to_string(sd->total_bytes())) : Error("format", sd->ProblemMessage());
    return;
  }
  if (now_ms - last_progress_ms_ < 300) return;
  last_progress_ms_ = now_ms;
  const uint64_t total = sd->format_expected();
  WriteLine("{\"fs\":\"progress\",\"op\":\"format\",\"done\":" + std::to_string(std::min(sd->format_written(), total)) +
            ",\"total\":" + std::to_string(total) + "}");
}
