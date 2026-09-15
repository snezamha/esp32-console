#include "project_runtime.h"
#include "project_api.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <MD5Builder.h>
#include <mbedtls/sha256.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstring>
#include "../../config.h"
#include "../common/settings.h"
#include "../display/lcd_display.h"
#include "../services/console_client.h"
#include "../services/network.h"
extern const uint8_t kCertBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCertBundleEnd[] asm("_binary_x509_crt_bundle_end");
namespace {
constexpr size_t kMaxPackage = 128 * 1024;
constexpr const char* kBoardId = "esp32-s3-lcd-0.85";
constexpr int kProjectAbi = DISPLAY_PROJECT_ABI;
std::string Encode(const std::string& text) {
  const char* hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') result += c;
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
std::string Path(int slot) { return slot ? "/project-b.elf" : "/project-a.elf"; }
struct Drawing { Canvas* c; int x, y, w, h; };
void Label(void* p, int y, const char* text, uint16_t color, int scale) {
  auto& d = *static_cast<Drawing*>(p);
  d.c->TextMarquee(d.x + 2, d.y + y, d.w - 4, text, color, std::clamp(scale, 1, 3), true);
}
void Line(void* p, int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
  auto& d = *static_cast<Drawing*>(p);
  d.c->Line(d.x+x0, d.y+y0, d.x+x1, d.y+y1, std::clamp(thickness, 1, 8), color);
}
void Ring(void* p, int x, int y, int r, int thickness, uint16_t color) {
  auto& d = *static_cast<Drawing*>(p);
  d.c->Ring(d.x+x, d.y+y, std::clamp(r, 0, 128), std::clamp(thickness, 1, 8), color);
}
void Circle(void* p, int x, int y, int r, uint16_t color) {
  auto& d = *static_cast<Drawing*>(p);
  d.c->FillCircle(d.x+x, d.y+y, std::clamp(r, 0, 128), color);
}
void Rect(void* p, int x, int y, int w, int h, uint16_t color) {
  auto& d = *static_cast<Drawing*>(p);
  d.c->Rect(d.x+x, d.y+y, std::clamp(w, 0, d.w), std::clamp(h, 0, d.h), color);
}
void FillRect(void* p, int x, int y, int w, int h, uint16_t color) {
  auto& d = *static_cast<Drawing*>(p);
  d.c->FillRect(d.x+x, d.y+y, std::clamp(w, 0, d.w), std::clamp(h, 0, d.h), color);
}
int TextWidth(const char* text, int scale) { return Canvas::TextWidth(text, std::clamp(scale, 1, 3)); }

bool JsonString(const std::string& json, const char* key, std::string& value) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos || (p = json.find(':', p + needle.size())) == std::string::npos ||
      (p = json.find('"', p + 1)) == std::string::npos) return false;
  value.clear();
  for (++p; p < json.size(); ++p) {
    const char c = json[p];
    if (c == '"') return true;
    if (c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    value += c;
  }
  return false;
}
bool JsonInt(const std::string& json, const char* key, int& value) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos || (p = json.find(':', p + needle.size())) == std::string::npos) return false;
  char* end = nullptr; value = strtol(json.c_str() + p + 1, &end, 10);
  return end != json.c_str() + p + 1;
}
bool ValidId(const std::string& id) {
  if (id.empty() || id.size() > 48 || id == "none" || !isalnum(static_cast<unsigned char>(id[0]))) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) { return islower(c) || isdigit(c) || c == '-'; });
}
std::vector<std::string> Parts(const std::string& value) {
  std::vector<std::string> parts; size_t start = 0;
  for (;;) { const size_t end = value.find('|', start); parts.push_back(value.substr(start, end - start)); if (end == std::string::npos) return parts; start = end + 1; }
}
std::string ComputeSha256(const std::vector<uint8_t>& bytes) {
  uint8_t digest[32]; mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
  const bool failed = mbedtls_sha256_starts(&ctx, 0) != 0 || mbedtls_sha256_update(&ctx, bytes.data(), bytes.size()) != 0 || mbedtls_sha256_finish(&ctx, digest) != 0;
  mbedtls_sha256_free(&ctx);
  if (failed) return "";
  static const char hex[] = "0123456789abcdef"; std::string out(64, '0');
  for (size_t i = 0; i < sizeof(digest); ++i) { out[i*2] = hex[digest[i] >> 4]; out[i*2+1] = hex[digest[i] & 15]; }
  return out;
}
bool AbnormalReset(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}
}

bool ProjectRuntime::Inspect(const std::vector<uint8_t>& bytes, Metadata& metadata) {
  if (bytes.size() < sizeof(elf32_hdr_t) || bytes.size() > kMaxPackage) return false;
  const auto* hdr = reinterpret_cast<const elf32_hdr_t*>(bytes.data());
  if (memcmp(bytes.data(), "\x7f" "ELF\x01\x01", 6) || hdr->type != 3 || hdr->machine != 94 || hdr->shentsize != sizeof(elf32_shdr_t) ||
      !hdr->shnum || hdr->shnum > 128 || hdr->shstrndx >= hdr->shnum || hdr->shoff > bytes.size() ||
      hdr->shnum * sizeof(elf32_shdr_t) > bytes.size() - hdr->shoff) return false;
  const auto* sections = reinterpret_cast<const elf32_shdr_t*>(bytes.data() + hdr->shoff);
  for (int i = 0; i < hdr->shnum; ++i) {
    if (sections[i].link >= hdr->shnum || sections[i].size > kMaxPackage ||
        (sections[i].type != SHT_NOBITS && (sections[i].offset > bytes.size() || sections[i].size > bytes.size() - sections[i].offset))) return false;
  }
  const auto& names = sections[hdr->shstrndx];
  bool entry_valid = false, metadata_valid = false;
  auto mapped = [&](uint32_t address, uint32_t length) {
    for (int j = 0; j < hdr->shnum; ++j) {
      const auto& section = sections[j];
      if (section.name >= names.size) continue;
      const char* name = reinterpret_cast<const char*>(bytes.data() + names.offset + section.name);
      if ((section.flags & SHF_ALLOC) && (!strcmp(name, ".text") || !strcmp(name, ".data") || !strcmp(name, ".rodata") || !strcmp(name, ".data.rel.ro") || !strcmp(name, ".bss")) &&
          address >= section.addr && length <= section.size && address - section.addr <= section.size - length) return true;
    }
    return false;
  };
  if (names.type != SHT_STRTAB) return false;
  for (int i = 0; i < hdr->shnum; ++i) {
    const auto& section = sections[i];
    if (section.name >= names.size || !memchr(bytes.data() + names.offset + section.name, 0, names.size - section.name)) return false;
  }
  for (int i = 0; i < hdr->shnum; ++i) {
    const auto& section = sections[i];
    const char* name = reinterpret_cast<const char*>(bytes.data() + names.offset + section.name);
    if (!strcmp(name, ".project")) {
      if (metadata_valid || section.type == SHT_NOBITS || !section.size || section.size > 4096) return false;
      const std::string json(reinterpret_cast<const char*>(bytes.data() + section.offset), section.size);
      metadata_valid = JsonString(json, "id", metadata.id) && JsonString(json, "name", metadata.name) &&
          JsonString(json, "version", metadata.version) && JsonString(json, "board", metadata.board) && JsonInt(json, "abi", metadata.abi);
    }
    if (!strcmp(name, ".text") && (section.flags & SHF_EXECINSTR) && hdr->entry >= section.addr && hdr->entry - section.addr < section.size) entry_valid = true;
    if (section.type == 11) {
      if (section.size % 16 || sections[section.link].type != SHT_STRTAB) return false;
      const auto& strings = sections[section.link];
      for (size_t n = 16; n < section.size; n += 16) {
        uint32_t symbol_name; uint16_t index;
        memcpy(&symbol_name, bytes.data() + section.offset + n, 4);
        memcpy(&index, bytes.data() + section.offset + n + 14, 2);
        if (!index || symbol_name >= strings.size || !memchr(bytes.data() + strings.offset + symbol_name, 0, strings.size - symbol_name)) return false;
      }
    }
    if (section.type == SHT_RELA) {
      if (section.size % 12 || sections[section.link].type != 11) return false;
      for (size_t n = 0; n < section.size; n += 12) {
        uint32_t address, info;
        memcpy(&address, bytes.data() + section.offset + n, 4);
        memcpy(&info, bytes.data() + section.offset + n + 4, 4);
        if ((info & 255) != 5 || (info >> 8) >= sections[section.link].size / 16 || address % 4 || !mapped(address, 4)) return false;
        bool target_valid = false;
        for (int j = 0; j < hdr->shnum; ++j) {
          const auto& target = sections[j];
          if (target.type != SHT_NOBITS && address >= target.addr && target.size >= 4 && address - target.addr <= target.size - 4 && (target.flags & SHF_ALLOC)) {
            uint32_t value; memcpy(&value, bytes.data() + target.offset + address - target.addr, 4);
            target_valid = mapped(value, 1); break;
          }
        }
        if (!target_valid) return false;
      }
    }
    if (section.type == SHT_REL) return false;
  }
  return entry_valid && metadata_valid && ValidId(metadata.id) && !metadata.name.empty() && metadata.name.size() <= 80 &&
      metadata.board == kBoardId && metadata.abi == kProjectAbi;
}

bool ProjectRuntime::Load(const std::vector<uint8_t>& bytes, esp_elf_t& elf, Metadata& metadata) {
  if (!Inspect(bytes, metadata)) return false;
  esp_elf_init(&elf);
  if (esp_elf_relocate(&elf, bytes.data())) { esp_elf_deinit(&elf); return false; }
  return true;
}

void ProjectRuntime::Log(const std::string& message, bool error) {
  if (log_mutex_ && xSemaphoreTake(log_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    logs_.push_back({next_log_++, error, message});
    if (logs_.size() > 40) logs_.erase(logs_.begin());
    xSemaphoreGive(log_mutex_);
  }
  Serial.printf("[project] %s%s\n", error ? "ERROR: " : "", message.c_str());
}

std::string ProjectRuntime::Report() {
  if (command_id_.empty()) return "";
  static const char* stages[] = {"connecting", "connecting", "downloading", "verifying", "writing", "activating", "done", "failed", "cancelled"};
  std::string body = "&p.status=" + Encode(command_id_ + "|" + stages[std::clamp(stage_.load(), 0, 8)] + "|" + std::to_string(progress_.load()) + "|" + std::to_string(received_.load()) + "|" + std::to_string(expected_size_));
  if (log_mutex_ && xSemaphoreTake(log_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (const auto& log : logs_) body += "&p.log=" + Encode(command_id_ + "|" + std::to_string(log.seq) + "|" + (log.error ? "error" : "info") + "|" + log.message);
    xSemaphoreGive(log_mutex_);
  }
  return body;
}

void ProjectRuntime::Begin() {
  log_mutex_ = xSemaphoreCreateMutex();
  bool interrupted = false;
  std::string active;
  {
    Settings s("project", true);
    command_id_ = s.GetString("pending", "");
    interrupted = !command_id_.empty();
    if (s.GetBool("trial", false)) {
      s.SetString("active", s.GetString("previous", "0|none"));
      s.SetBool("trial", false);
    }
    if (interrupted) {
      error_ = "Board restarted during project installation (reset " + std::to_string(esp_reset_reason()) + ", stage " + s.GetString("phase", "unknown") + "). Previous project restored.";
      stage_ = 7; ack_ready_ = true; ack_ok_ = false;
      s.SetString("pending", "");
      Log(error_, true);
    }
    active = s.GetString("active", "0|none");
    const auto saved = Parts(active);
    const bool has_project = saved.size() > 1 && saved[1] != "none";
    pinMode(VOLUME_UP_BUTTON_GPIO, INPUT_PULLUP);
    const bool manual_safe_mode = digitalRead(VOLUME_UP_BUTTON_GPIO) == LOW;
    int crashes = s.GetInt("crashes", 0);
    if (has_project && AbnormalReset(esp_reset_reason())) crashes++;
    else if (!AbnormalReset(esp_reset_reason())) crashes = 0;
    if (has_project && (manual_safe_mode || crashes >= 3)) {
      s.SetString("previous", active);
      s.SetString("active", "0|none");
      s.SetBool("trial", false);
      s.SetInt("crashes", 0);
      active = "0|none";
      safe_mode_ = true;
      Log(manual_safe_mode ? "Safe mode: Vol+ was held during boot; project disabled." : "Safe mode: project disabled after repeated watchdog/panic resets.", true);
    } else {
      s.SetInt("crashes", crashes);
    }
  }
  mounted_ = LittleFS.begin(false);
  if (!mounted_) {
    // This partition contains only replaceable project slots. Reformatting it is a safe recovery:
    // the base firmware and settings are elsewhere and the console can reinstall the package.
    mounted_ = LittleFS.begin(true);
    if (mounted_) {
      Settings s("project", true); s.SetString("active", "0|none"); s.SetBool("trial", false);
      active = "0|none"; safe_mode_ = true;
      Log("Project storage was repaired; default display restored.", true);
    }
  }
  if (!mounted_) return;
  const auto saved = Parts(active);
  slot_ = !saved.empty() && saved[0] == "1" ? 1 : 0;
  if (saved.size() < 2 || saved[1] == "none") return;
  auto file = LittleFS.open(Path(slot_).c_str(), "r");
  if (!file || file.size() > kMaxPackage) return;
  std::vector<uint8_t> bytes(file.size());
  Metadata metadata;
  if (file.read(bytes.data(), bytes.size()) == bytes.size() && Load(bytes, elf_, metadata) && metadata.id == saved[1]) {
    loaded_ = true; id_ = metadata.id; version_ = metadata.version; sha256_ = ComputeSha256(bytes);
    Settings s("project", true);
    s.SetString("active", std::to_string(slot_) + "|" + id_ + "|" + version_ + "|" + sha256_);
  } else {
    Settings s("project", true); s.SetString("active", "0|none");
    safe_mode_ = true; Log("Saved project failed validation and was disabled.", true);
  }
}

bool ProjectRuntime::Activate(const std::vector<uint8_t>& bytes, const Metadata& expected) {
  esp_elf_t candidate{};
  Metadata metadata;
  { Settings s("project", true); s.SetString("phase", "ELF relocation"); }
  stage_ = 3; Log("Validating and relocating the project ELF into internal executable memory.");
  if (!mounted_ || !Load(bytes, candidate, metadata) || metadata.id != expected.id || metadata.version != expected.version) return false;
  char addresses[96]; snprintf(addresses, sizeof(addresses), "ELF loaded: entry %p, code %p, data %p.", reinterpret_cast<void*>(candidate.entry), candidate.ptext, candidate.pdata);
  Log(addresses);
  { Settings s("project", true); s.SetString("phase", "flash write"); }
  const int next_slot = 1 - slot_;
  stage_ = 4; Log("Writing verified file to inactive project flash slot.");
  auto file = LittleFS.open(Path(next_slot).c_str(), "w");
  if (!file || file.write(bytes.data(), bytes.size()) != bytes.size()) { esp_elf_deinit(&candidate); return false; }
  file.flush(); file.close();
  // Publish a verified, fully written slot; a failed download leaves the running slot intact.
  { Settings s("project", true);
    s.SetString("previous", s.GetString("active", "0|none"));
    s.SetBool("trial", true);
    s.SetString("phase", "first display frame");
    s.SetString("active", std::to_string(next_slot) + "|" + metadata.id + "|" + metadata.version + "|" + expected_sha256_);
  }
  if (loaded_) esp_elf_deinit(&elf_);
  elf_ = candidate; loaded_ = true; slot_ = next_slot; id_ = metadata.id; version_ = metadata.version; sha256_ = expected_sha256_;
  data_[0].clear(); data_[1].clear(); data_at_ = 0;
  testing_ = true; healthy_recorded_ = false; first_frame_at_ = 0; stage_ = 5; Log("Testing the first project frame before confirming installation.");
  return true;
}

bool ProjectRuntime::RestorePrevious(const std::string& reason) {
  if (loaded_) esp_elf_deinit(&elf_);
  loaded_ = false; id_ = "none"; version_.clear(); sha256_.clear(); testing_ = false;
  Settings s("project", true);
  const std::string previous = s.GetString("previous", "0|none");
  const auto parts = Parts(previous);
  s.SetBool("trial", false); s.SetString("pending", ""); s.SetString("active", previous);
  if (parts.size() >= 2 && parts[1] != "none") {
    const int previous_slot = parts[0] == "1" ? 1 : 0;
    auto file = LittleFS.open(Path(previous_slot).c_str(), "r");
    if (file && file.size() <= kMaxPackage) {
      std::vector<uint8_t> bytes(file.size()); Metadata metadata;
      if (file.read(bytes.data(), bytes.size()) == bytes.size() && Load(bytes, elf_, metadata) && metadata.id == parts[1]) {
        loaded_ = true; slot_ = previous_slot; id_ = metadata.id; version_ = metadata.version; sha256_ = ComputeSha256(bytes);
      }
    }
  }
  if (!loaded_) s.SetString("active", "0|none");
  error_ = reason + (loaded_ ? " Previous project restored." : " Default display restored.");
  stage_ = 7; ack_ok_ = false; ack_ready_ = true; Log(error_, true);
  return loaded_;
}

void ProjectRuntime::RecordHealthyFrame() {
  if (!loaded_ || healthy_recorded_ || !first_frame_at_ || millis() - first_frame_at_ < 30000) return;
  Settings s("project", true); s.SetInt("crashes", 0); healthy_recorded_ = true;
}

std::string ProjectRuntime::Start(const std::string& command_id, const std::string& arg, const std::string& server, bool insecure, const std::string& token) {
  if (Busy()) return "fail|Another project is loading";
  const auto id = ConsoleClient::FormValue(arg, "id");
  if (id == "none") {
    { Settings s("project", true); s.SetString("active", "0|none"); }
    if (loaded_) esp_elf_deinit(&elf_);
    loaded_ = false; id_ = "none"; version_.clear(); sha256_.clear(); safe_mode_ = false;
    return "ok|Default display restored";
  }
  if (!mounted_) return "fail|Project flash storage unavailable";
  const auto path = ConsoleClient::FormValue(arg, "path");
  const auto abi = ConsoleClient::FormValue(arg, "abi");
  target_version_ = ConsoleClient::FormValue(arg, "version");
  expected_size_ = strtoul(ConsoleClient::FormValue(arg, "size").c_str(), nullptr, 10);
  md5_ = ConsoleClient::FormValue(arg, "md5");
  expected_sha256_ = ConsoleClient::FormValue(arg, "sha256");
  if (id.empty() || target_version_.empty() || (path.rfind("/projects/", 0) != 0 && path.rfind("/api/devices/", 0) != 0) || abi != std::to_string(kProjectAbi) || !expected_size_ || expected_size_ > kMaxPackage || md5_.size() != 32 || expected_sha256_.size() != 64) return "fail|Invalid project package";
  command_id_ = command_id; target_id_ = id; url_ = server + path; insecure_ = insecure; token_ = token;
  if (log_mutex_ && xSemaphoreTake(log_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) { logs_.clear(); next_log_ = 1; xSemaphoreGive(log_mutex_); }
  cancel_ = false; stop_id_.clear(); ack_ready_ = false; received_ = 0; stage_ = 1;
  { Settings s("project", true); s.SetString("pending", command_id); s.SetString("phase", "download"); }
  Log("Opening project download connection; expected " + std::to_string(expected_size_) + " bytes.");
  error_.clear(); downloaded_.clear(); progress_ = 0; done_ = false; busy_ = true;
  if (xTaskCreatePinnedToCore(DownloadTask, "project_load", 8192, this, 1, nullptr, 0) != pdPASS) {
    busy_ = false; stage_ = 7;
    { Settings s("project", true); s.SetString("pending", ""); }
    return "fail|No memory for project download";
  }
  return "";
}

void ProjectRuntime::DownloadTask(void* arg) {
  auto self = static_cast<ProjectRuntime*>(arg);
  {
    HTTPClient http; WiFiClient plain; WiFiClientSecure secure;
    const bool tls = self->url_.rfind("https://", 0) == 0;
    if (tls) {
      if (self->insecure_) secure.setInsecure();
      else secure.setCACertBundle(kCertBundleStart, kCertBundleEnd - kCertBundleStart);
    }
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    const bool begun = tls ? http.begin(secure, self->url_.c_str()) : http.begin(plain, self->url_.c_str());
    if (begun && self->url_.find("/api/devices/") != std::string::npos) http.addHeader("Authorization", ("Bearer " + self->token_).c_str());
    const int code = begun ? http.GET() : -1;
    self->Log("Download server returned HTTP " + std::to_string(code) + "; content length " + std::to_string(http.getSize()) + ".");
    if (code != 200 || http.getSize() != static_cast<int>(self->expected_size_)) self->error_ = "Project download failed: HTTP " + std::to_string(code) + ", expected " + std::to_string(self->expected_size_) + " bytes, got " + std::to_string(http.getSize()) + ".";
    else {
      self->stage_ = 2; self->Log("Receiving project file.");
      self->downloaded_.resize(self->expected_size_);
      auto stream = http.getStreamPtr(); size_t total = 0; uint32_t last_data = millis(); const uint32_t started = millis();
      while (total < self->expected_size_ && !self->cancel_ && millis() - started < 45000) {
        const size_t available = stream->available();
        if (!available) {
          if (!http.connected() || millis() - last_data > 15000) break;
          vTaskDelay(pdMS_TO_TICKS(5)); continue;
        }
        const size_t count = stream->readBytes(self->downloaded_.data() + total, std::min(available, self->expected_size_ - total));
        total += count; self->received_ = total; if (count) last_data = millis();
        self->progress_ = total * 100 / self->expected_size_;
      }
      if (self->cancel_) self->error_ = "Cancelled by user";
      else if (total != self->expected_size_) self->error_ = "Project download interrupted or timed out after " + std::to_string(total) + " bytes.";
      else {
        self->stage_ = 3; self->Log("File received. Checking checksum.");
        MD5Builder md5; md5.begin();
        for (size_t offset = 0; offset < self->downloaded_.size(); offset += 4096) md5.add(self->downloaded_.data() + offset, std::min<size_t>(4096, self->downloaded_.size() - offset));
        md5.calculate();
        if (md5.toString() != self->md5_.c_str()) self->error_ = "Project checksum mismatch";
        else if (ComputeSha256(self->downloaded_) != self->expected_sha256_) self->error_ = "Project SHA-256 mismatch";
      }
    }
    http.end();
  }
  self->done_ = true;
  vTaskDelete(nullptr);
}

std::string ProjectRuntime::Cancel(const std::string& stop_id, const std::string& target) {
  if (target != command_id_ || !busy_) return "ok|Transfer is already stopped";
  stop_id_ = stop_id; cancel_ = true; Log("Stop requested. Cancelling the file transfer.");
  return "";
}

std::vector<std::string> ProjectRuntime::Loop() {
  std::vector<std::string> acks;
  if (ack_ready_) {
    ack_ready_ = false;
    acks.push_back(command_id_ + (ack_ok_ ? "|ok|Project installed and first frame verified" : "|fail|" + error_));
  }
  if (!done_.exchange(false)) return acks;
  if (cancel_) error_ = "Cancelled by user";
  if (error_.empty() && !Activate(downloaded_, {target_id_, "", target_version_, kBoardId, kProjectAbi})) error_ = "Project identity, ABI, ELF or flash validation failed";
  downloaded_.clear(); busy_ = false;
  if (!error_.empty()) {
    stage_ = cancel_ ? 8 : 7; Log(error_, !cancel_);
    { Settings s("project", true); s.SetString("pending", ""); }
    acks.push_back(command_id_ + "|fail|" + error_);
  }
  if (!stop_id_.empty()) { acks.push_back(stop_id_ + "|ok|Transfer stopped"); stop_id_.clear(); }
  return acks;
}

void ProjectRuntime::SetData(const std::string& data) {
  const std::string bounded = data.substr(0, 512);
  const auto divider = bounded.find('|');
  data_[0] = bounded.substr(0, divider); data_[1] = divider == std::string::npos ? "" : bounded.substr(divider + 1);
  data_at_ = millis();
}

bool ProjectRuntime::Draw(Canvas& c, int x, int y, int w, int h, const Theme& theme) {
  if (busy_) {
    c.TextMarquee(x+2, y+h/2-12, w-4, "Loading project...", theme.info, 1, true);
    c.FillRoundRect(x+20, y+h/2+6, w-40, 4, 2, theme.selected);
    c.FillRoundRect(x+20, y+h/2+6, std::max(2, (w-40)*progress_.load()/100), 4, 2, theme.info);
    return true;
  }
  if (!loaded_) return false;
  Drawing drawing{&c, x, y, w, h};
  const time_t now = time(nullptr); const tm local = *localtime(&now);
  ProjectFrame frame{DISPLAY_PROJECT_ABI, &drawing, w, h, local.tm_hour, local.tm_min, local.tm_sec,
    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_wday,
    Network::GetInstance().TimeValid(), data_at_ ? millis()-data_at_ : 0,
    {data_[0].c_str(), data_[1].c_str()}, {static_cast<uint32_t>(data_[0].size()), static_cast<uint32_t>(data_[1].size())}, millis(),
    theme.text, theme.muted, theme.info, Label, Line, Ring, Circle, Rect, FillRect, TextWidth, sinf, cosf};
  const auto saved = c.GetClip(); c.IntersectClip(x,y,w,h);
  char* argv[] = {reinterpret_cast<char*>(&frame)};
  const int result = esp_elf_request(&elf_, 0, 1, argv); c.RestoreClip(saved);
  if (testing_) {
    if (result != 0) { RestorePrevious("Project rejected its first display frame."); return loaded_; }
    testing_ = false; stage_ = 6; progress_ = 100; ack_ok_ = true; ack_ready_ = true;
    first_frame_at_ = millis();
    { Settings s("project", true); s.SetBool("trial", false); s.SetString("pending", ""); }
    Log("Project activated successfully. First display frame completed.");
  }
  RecordHealthyFrame();
  return true;
}
