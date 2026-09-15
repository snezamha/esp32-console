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
#include "../board/board.h"
#include "../storage/sd_card.h"
#include "../common/settings.h"
#include "../display/lcd_display.h"
#include "../services/console_client.h"
#include "../services/network.h"
#include "../services/sd_files.h"
extern const uint8_t kCertBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCertBundleEnd[] asm("_binary_x509_crt_bundle_end");
namespace {
constexpr size_t kMaxPackage = 128 * 1024;
constexpr uint64_t kMaxAssets = 32ULL * 1024 * 1024 * 1024;
// Kept free on the card so the file system and other card users are never starved.
constexpr uint64_t kSdReserve = 1024 * 1024;
constexpr size_t kMaxAssetIndex = 64 * 1024;
constexpr int kMaxAssetFiles = 2000;
constexpr const char* kBoardId = "esp32-s3-lcd-0.85";
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
std::string Megabytes(uint64_t bytes) {
  char text[24]; snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0); return text;
}
std::string AssetDir(const std::string& id, const std::string& version) { return "/projects/" + id + "/" + version; }
// Relative path of up to four segments of [A-Za-z0-9._-]; no hidden names, "..", or leading "/".
bool ValidAssetName(const std::string& name) {
  if (name.empty() || name.size() > 96) return false;
  int segments = 0; size_t start = 0;
  for (;;) {
    const size_t end = name.find('/', start);
    const std::string segment = name.substr(start, end - start);
    if (segment.empty() || segment[0] == '.' || ++segments > 4) return false;
    if (!std::all_of(segment.begin(), segment.end(), [](unsigned char c) { return isalnum(c) || c == '.' || c == '_' || c == '-'; })) return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  }
}
bool Hex64(const std::string& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return isdigit(c) || (c >= 'a' && c <= 'f'); });
}
std::string HexDigest(const uint8_t* digest) {
  static const char hex[] = "0123456789abcdef"; std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) { out[i*2] = hex[digest[i] >> 4]; out[i*2+1] = hex[digest[i] & 15]; }
  return out;
}
void RemoveTree(fs::FS& fs, const std::string& path) {
  File dir = fs.open(path.c_str());
  if (!dir) return;
  if (!dir.isDirectory()) { dir.close(); fs.remove(path.c_str()); return; }
  std::vector<std::string> children;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) { children.push_back(path + "/" + entry.name()); entry.close(); }
  dir.close();
  for (const auto& child : children) RemoveTree(fs, child);
  fs.rmdir(path.c_str());
}

// Asset access for the running module. Called only from Draw (inside esp_elf_request).
std::string g_asset_dir;
struct OpenAsset { std::string name; File file; };
OpenAsset g_assets[4];
int g_asset_next = 0;
void CloseAssets() { for (auto& asset : g_assets) { if (asset.file) asset.file.close(); asset.name.clear(); } }
File* Asset(const char* raw) {
  if (g_asset_dir.empty() || !raw) return nullptr;
  const std::string name(raw, strnlen(raw, 97));
  if (!ValidAssetName(name)) return nullptr;
  for (auto& asset : g_assets) if (asset.file && asset.name == name) return &asset.file;
  auto* sd = Board::GetInstance().GetSdCard();
  if (!sd->mounted()) return nullptr;
  File file = sd->fs().open((g_asset_dir + "/" + name).c_str(), FILE_READ);
  if (!file || file.isDirectory()) return nullptr;
  auto& slot = g_assets[g_asset_next++ % 4];
  if (slot.file) slot.file.close();
  slot.name = name; slot.file = file;
  return &slot.file;
}
int32_t AssetSize(const char* name) {
  File* file = Asset(name);
  return file && file->size() <= INT32_MAX ? static_cast<int32_t>(file->size()) : -1;
}
int32_t AssetRead(const char* name, uint32_t offset, void* buffer, uint32_t size) {
  File* file = Asset(name);
  if (!file || !buffer || offset > file->size() || !file->seek(offset)) return -1;
  return static_cast<int32_t>(file->read(static_cast<uint8_t*>(buffer), std::min<size_t>(size, file->size() - offset)));
}
// .img/.vid header, little-endian: "EVM1", width, height, frames, fps (uint16), reserved (uint32).
struct Media { File* file; int width, height, frames, fps; };
bool OpenMedia(const char* name, Media& media) {
  media.file = Asset(name);
  uint8_t header[16];
  if (!media.file || !media.file->seek(0) || media.file->read(header, 16) != 16 || memcmp(header, "EVM1", 4)) return false;
  media.width = header[4] | header[5] << 8; media.height = header[6] | header[7] << 8;
  media.frames = header[8] | header[9] << 8; media.fps = header[10] | header[11] << 8;
  return media.width > 0 && media.width <= 1024 && media.height > 0 && media.height <= 1024 && media.frames > 0 &&
      media.file->size() >= 16 + static_cast<uint64_t>(media.frames) * media.width * media.height * 2;
}
int MediaInfo(const char* name, int* width, int* height, int* frames, int* fps) {
  Media media;
  if (!OpenMedia(name, media)) return -1;
  if (width) *width = media.width;
  if (height) *height = media.height;
  if (frames) *frames = media.frames;
  if (fps) *fps = media.fps;
  return 0;
}
int MediaDraw(void* p, int x, int y, const char* name, uint32_t frame) {
  auto& d = *static_cast<Drawing*>(p);
  Media media;
  if (!OpenMedia(name, media)) return -1;
  const auto clip = d.c->GetClip();
  const int left = d.x + x, top = d.y + y;
  const int first = std::max(0, clip.y0 - top), last = std::min(media.height, clip.y1 - top);
  if (first >= last || left >= clip.x1 || left + media.width <= clip.x0) return 0;
  // Seek straight to the first visible row of the frame and read visible rows in bounded chunks.
  const size_t row_bytes = static_cast<size_t>(media.width) * 2;
  const uint64_t offset = 16 + static_cast<uint64_t>(frame % media.frames) * row_bytes * media.height + static_cast<uint64_t>(first) * row_bytes;
  if (!media.file->seek(offset)) return -1;
  const int chunk = std::max<int>(1, 32768 / row_bytes);
  static std::vector<uint8_t> rows;
  rows.resize(row_bytes * std::min(chunk, last - first));
  for (int row = first; row < last;) {
    const int count = std::min(chunk, last - row);
    if (media.file->read(rows.data(), count * row_bytes) != count * row_bytes) return -1;
    for (int i = 0; i < count; ++i) d.c->BlitRow(left, top + row + i, media.width, rows.data() + i * row_bytes);
    row += count;
  }
  return 0;
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
      metadata.board == kBoardId && metadata.abi >= DISPLAY_PROJECT_MIN_ABI && metadata.abi <= DISPLAY_PROJECT_ABI;
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
  std::string body = "&p.status=" + Encode(command_id_ + "|" + stages[std::clamp(stage_.load(), 0, 8)] + "|" + std::to_string(progress_.load()) + "|" + std::to_string(received_.load()) + "|" + std::to_string(total_size_.load()));
  if (log_mutex_ && xSemaphoreTake(log_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (const auto& log : logs_) body += "&p.log=" + Encode(command_id_ + "|" + std::to_string(log.seq) + "|" + (log.error ? "error" : "info") + "|" + log.message);
    xSemaphoreGive(log_mutex_);
  }
  return body;
}

std::string ProjectRuntime::SdReport() const {
  return std::string(Board::GetInstance().GetSdCard()->mounted() ? "1" : "0") + "|" + std::to_string(sd_total_) + "|" + std::to_string(sd_free_);
}

void ProjectRuntime::LockSd() {
  CloseAssets();
  sd_locked_ = true;
}

bool ProjectRuntime::RemountSd() {
  sd_locked_ = false;
  auto* sd = Board::GetInstance().GetSdCard();
  CloseAssets(); sd->Unmount();
  const bool mounted = sd->Mount();
  if (mounted) RefreshSd(); else sd_total_ = sd_free_ = 0;
  sd_ready_ = !sd_required_ || AssetsReady(id_, version_, assets_digest_);
  sd_checked_at_ = millis();
  return mounted;
}

void ProjectRuntime::RefreshSd() {
  auto* sd = Board::GetInstance().GetSdCard();
  sd_total_ = sd->total_bytes();
  const uint64_t used = sd->used_bytes();
  sd_free_ = sd_total_ > used ? sd_total_ - used : 0;
}

bool ProjectRuntime::AssetsReady(const std::string& id, const std::string& version, const std::string& digest) {
  auto* sd = Board::GetInstance().GetSdCard();
  if (!sd->mounted()) return false;
  File marker = sd->fs().open((AssetDir(id, version) + "/.complete").c_str(), FILE_READ);
  if (!marker) return false;
  char text[65] = {};
  const bool ok = marker.read(reinterpret_cast<uint8_t*>(text), 64) == 64 && digest == text;
  marker.close();
  return ok;
}

// Points the asset API at the loaded module's folder. An empty digest means the module has no SD files.
void ProjectRuntime::UseAssets(const std::string& digest) {
  CloseAssets();
  assets_digest_ = digest;
  sd_required_ = loaded_ && !digest.empty();
  g_asset_dir = sd_required_ ? AssetDir(id_, version_) : "";
  sd_ready_ = !sd_required_ || AssetsReady(id_, version_, digest);
  sd_checked_at_ = millis();
}

// Keeps only the folders of the active and previous project versions.
void ProjectRuntime::CleanupAssets() {
  auto* sd = Board::GetInstance().GetSdCard();
  if (!sd->mounted()) return;
  std::vector<std::string> keep;
  { Settings s("project", true);
    for (const auto& value : {s.GetString("active", ""), s.GetString("previous", "")}) {
      const auto parts = Parts(value);
      if (parts.size() >= 3 && parts[1] != "none") keep.push_back(AssetDir(parts[1], parts[2]));
    }
  }
  auto& fs = sd->fs();
  File root = fs.open("/projects");
  if (!root || !root.isDirectory()) return;
  std::vector<std::string> stale;
  for (File project = root.openNextFile(); project; project = root.openNextFile()) {
    const std::string project_dir = std::string("/projects/") + project.name();
    if (project.isDirectory()) {
      for (File version = project.openNextFile(); version; version = project.openNextFile()) {
        const std::string dir = project_dir + "/" + version.name();
        if (std::find(keep.begin(), keep.end(), dir) == keep.end()) stale.push_back(dir);
        version.close();
      }
    }
    project.close();
  }
  root.close();
  for (const auto& dir : stale) { RemoveTree(fs, dir); Log("Removed unused SD card files " + dir + "."); }
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
  if (Board::GetInstance().GetSdCard()->Mount()) RefreshSd();
  if (!mounted_) return;
  const auto saved = Parts(active);
  slot_ = !saved.empty() && saved[0] == "1" ? 1 : 0;
  if (saved.size() < 2 || saved[1] == "none") return;
  auto file = LittleFS.open(Path(slot_).c_str(), "r");
  if (!file || file.size() > kMaxPackage) return;
  std::vector<uint8_t> bytes(file.size());
  Metadata metadata;
  if (file.read(bytes.data(), bytes.size()) == bytes.size() && Load(bytes, elf_, metadata) && metadata.id == saved[1]) {
    loaded_ = true; id_ = metadata.id; version_ = metadata.version; sha256_ = ComputeSha256(bytes); abi_ = metadata.abi;
    const std::string digest = saved.size() > 4 && Hex64(saved[4]) ? saved[4] : "";
    UseAssets(digest);
    if (!sd_ready_) Log("Project files are missing from the SD card. Insert the card used during installation.", true);
    Settings s("project", true);
    s.SetString("active", std::to_string(slot_) + "|" + id_ + "|" + version_ + "|" + sha256_ + "|" + digest);
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
    s.SetString("active", std::to_string(next_slot) + "|" + metadata.id + "|" + metadata.version + "|" + expected_sha256_ + "|" + assets_sha256_);
  }
  if (loaded_) esp_elf_deinit(&elf_);
  elf_ = candidate; loaded_ = true; slot_ = next_slot; id_ = metadata.id; version_ = metadata.version; sha256_ = expected_sha256_; abi_ = metadata.abi;
  UseAssets(assets_sha256_);
  data_[0].clear(); data_[1].clear(); data_at_ = 0;
  testing_ = true; healthy_recorded_ = false; first_frame_at_ = 0; stage_ = 5; Log("Testing the first project frame before confirming installation.");
  return true;
}

bool ProjectRuntime::RestorePrevious(const std::string& reason) {
  if (loaded_) esp_elf_deinit(&elf_);
  loaded_ = false; id_ = "none"; version_.clear(); sha256_.clear(); testing_ = false;
  UseAssets("");
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
        loaded_ = true; slot_ = previous_slot; id_ = metadata.id; version_ = metadata.version; sha256_ = ComputeSha256(bytes); abi_ = metadata.abi;
        UseAssets(parts.size() > 4 && Hex64(parts[4]) ? parts[4] : "");
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
  if (SdFiles::Get().Busy()) return "fail|An SD card operation is running";
  const auto id = ConsoleClient::FormValue(arg, "id");
  if (id == "none") {
    { Settings s("project", true); s.SetString("active", "0|none"); }
    if (loaded_) esp_elf_deinit(&elf_);
    loaded_ = false; id_ = "none"; version_.clear(); sha256_.clear(); safe_mode_ = false;
    UseAssets("");
    return "ok|Default display restored";
  }
  if (!mounted_) return "fail|Project flash storage unavailable";
  const auto path = ConsoleClient::FormValue(arg, "path");
  const int abi = atoi(ConsoleClient::FormValue(arg, "abi").c_str());
  target_version_ = ConsoleClient::FormValue(arg, "version");
  expected_size_ = strtoul(ConsoleClient::FormValue(arg, "size").c_str(), nullptr, 10);
  md5_ = ConsoleClient::FormValue(arg, "md5");
  expected_sha256_ = ConsoleClient::FormValue(arg, "sha256");
  if (id.empty() || target_version_.empty() || (path.rfind("/projects/", 0) != 0 && path.rfind("/api/devices/", 0) != 0) || abi < DISPLAY_PROJECT_MIN_ABI || abi > DISPLAY_PROJECT_ABI || !expected_size_ || expected_size_ > kMaxPackage || md5_.size() != 32 || expected_sha256_.size() != 64) return "fail|Invalid project package";
  const auto assets = ConsoleClient::FormValue(arg, "assets");
  assets_sha256_ = ConsoleClient::FormValue(arg, "assets_sha256");
  assets_bytes_ = strtoull(ConsoleClient::FormValue(arg, "sd").c_str(), nullptr, 10);
  if (!assets.empty() && (assets.rfind("/projects/", 0) != 0 || assets.find("..") != std::string::npos || abi < 3 || !Hex64(assets_sha256_) || !assets_bytes_ || assets_bytes_ > kMaxAssets)) return "fail|Invalid project package";
  if (assets.empty()) { assets_sha256_.clear(); assets_bytes_ = 0; }
  else {
    // The card may have been inserted, swapped or removed since boot: always check it again.
    if (!RemountSd()) return "fail|This project needs an SD card. Insert a FAT32 microSD card and retry.";
    if (!AssetsReady(id, target_version_, assets_sha256_) && sd_free_ < assets_bytes_ + kSdReserve)
      return "fail|Not enough SD card space: this project needs " + Megabytes(assets_bytes_ + kSdReserve) + ", the card has " + Megabytes(sd_free_) + " free.";
  }
  command_id_ = command_id; target_id_ = id; url_ = server + path; insecure_ = insecure; token_ = token;
  assets_url_ = assets.empty() ? "" : server + assets;
  total_size_ = expected_size_ + assets_bytes_;
  if (log_mutex_ && xSemaphoreTake(log_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) { logs_.clear(); next_log_ = 1; xSemaphoreGive(log_mutex_); }
  cancel_ = false; stop_id_.clear(); ack_ready_ = false; received_ = 0; stage_ = 1;
  { Settings s("project", true); s.SetString("pending", command_id); s.SetString("phase", "download"); }
  Log("Opening project download connection; expected " + std::to_string(expected_size_) + " bytes.");
  if (assets_bytes_) Log("SD card ready: " + Megabytes(sd_free_) + " free; project files need " + Megabytes(assets_bytes_) + ".");
  error_.clear(); downloaded_.clear(); progress_ = 0; done_ = false; busy_ = true;
  if (xTaskCreatePinnedToCore(DownloadTask, "project_load", 12288, this, 1, nullptr, 0) != pdPASS) {
    busy_ = false; stage_ = 7;
    { Settings s("project", true); s.SetString("pending", ""); }
    return "fail|No memory for project download";
  }
  return "";
}

// One GET into `sink`. `expected` 0 accepts any declared Content-Length; `limit_ms` 0 means no total time limit.
std::string ProjectRuntime::Fetch(HTTPClient& http, NetworkClient& client, const std::string& url, size_t expected, uint32_t limit_ms, const std::function<bool(const uint8_t*, size_t)>& sink) {
  if (!http.begin(client, url.c_str())) return "Could not open " + url + ".";
  if (url.find("/api/devices/") != std::string::npos) http.addHeader("Authorization", ("Bearer " + token_).c_str());
  const int code = http.GET();
  const int length = http.getSize();
  if (code != 200 || length <= 0 || (expected && length != static_cast<int>(expected))) {
    http.end();
    return "HTTP " + std::to_string(code) + ", expected " + std::to_string(expected) + " bytes, got " + std::to_string(length) + ".";
  }
  auto* stream = http.getStreamPtr();
  const size_t size = static_cast<size_t>(length);
  uint8_t buffer[4096];
  size_t total = 0; uint32_t last_data = millis(); const uint32_t started = millis();
  std::string error;
  while (total < size && !cancel_) {
    if (limit_ms && millis() - started >= limit_ms) { error = "timed out"; break; }
    const size_t available = stream->available();
    if (!available) {
      if (!http.connected()) { error = "connection closed"; break; }
      if (millis() - last_data > 15000) { error = "no data for 15 s"; break; }
      vTaskDelay(pdMS_TO_TICKS(5)); continue;
    }
    const size_t count = stream->readBytes(buffer, std::min({available, sizeof(buffer), size - total}));
    if (!count) continue;
    if (!sink(buffer, count)) { error = "write failed"; break; }
    total += count; received_ += count; last_data = millis();
    progress_ = total_size_ ? std::min<uint64_t>(99, received_ * 100 / total_size_) : 0;
  }
  http.end();
  if (cancel_) return "Cancelled by user";
  if (error.empty() && total != size) error = "incomplete";
  return error.empty() ? "" : "Download of " + url.substr(url.rfind('/') + 1) + " failed after " + std::to_string(total) + " bytes: " + error + ".";
}

std::string ProjectRuntime::InstallAssets(HTTPClient& http, NetworkClient& client) {
  auto* sd = Board::GetInstance().GetSdCard();
  if (!sd->mounted()) return "SD card is no longer available.";
  auto& fs = sd->fs();
  const std::string dir = AssetDir(target_id_, target_version_);
  if (AssetsReady(target_id_, target_version_, assets_sha256_)) {
    received_ += assets_bytes_;
    Log("Project files already verified on the SD card; skipping their download.");
    return "";
  }
  std::string index;
  stage_ = 2; Log("Downloading the SD card file list.");
  const auto saved = received_.load();
  auto error = Fetch(http, client, assets_url_, 0, 30000, [&](const uint8_t* data, size_t size) {
    if (index.size() + size > kMaxAssetIndex) return false;
    index.append(reinterpret_cast<const char*>(data), size); return true;
  });
  received_ = saved;
  if (!error.empty()) return error;
  if (ComputeSha256(std::vector<uint8_t>(index.begin(), index.end())) != assets_sha256_) return "SD card file list SHA-256 mismatch.";
  struct Item { std::string name, sha256; uint64_t size; };
  std::vector<Item> items; uint64_t total = 0;
  for (size_t start = 0; start < index.size();) {
    size_t end = index.find('\n', start); if (end == std::string::npos) end = index.size();
    const auto parts = Parts(index.substr(start, end - start));
    start = end + 1;
    if (parts.size() == 1 && parts[0].empty()) continue;
    if (parts.size() != 3 || !ValidAssetName(parts[0]) || !Hex64(parts[2]) || items.size() >= kMaxAssetFiles) return "Invalid SD card file list.";
    items.push_back({parts[0], parts[2], strtoull(parts[1].c_str(), nullptr, 10)});
    total += items.back().size;
  }
  if (total != assets_bytes_) return "SD card file list does not match the package size.";
  Log("Installing " + std::to_string(items.size()) + " files (" + Megabytes(total) + ") into " + dir + " on the SD card.");
  fs.mkdir("/projects"); fs.mkdir(("/projects/" + target_id_).c_str()); fs.mkdir(dir.c_str());
  fs.remove((dir + "/.complete").c_str());
  const std::string base = assets_url_.substr(0, assets_url_.rfind('/') + 1) + "assets/";
  std::vector<uint8_t> buffer(4096);
  for (const auto& item : items) {
    const std::string path = dir + "/" + item.name;
    for (size_t slash = item.name.find('/'); slash != std::string::npos; slash = item.name.find('/', slash + 1)) fs.mkdir((dir + "/" + item.name.substr(0, slash)).c_str());
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    // Reuse a file left by an interrupted installation when its contents already verify.
    if (File existing = fs.open(path.c_str(), FILE_READ)) {
      bool same = !existing.isDirectory() && existing.size() == item.size;
      if (same) {
        mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
        const auto before = received_.load();
        for (size_t count; !cancel_ && (count = existing.read(buffer.data(), buffer.size())) > 0;) {
          mbedtls_sha256_update(&ctx, buffer.data(), count); received_ += count;
          progress_ = std::min<uint64_t>(99, received_ * 100 / total_size_);
        }
        mbedtls_sha256_finish(&ctx, digest); mbedtls_sha256_free(&ctx);
        same = !cancel_ && HexDigest(digest) == item.sha256;
        if (!same) received_ = before;
      }
      existing.close();
      if (cancel_) return "Cancelled by user";
      if (same) continue;
    }
    const std::string part = path + ".part";
    fs.remove(part.c_str());
    File out = fs.open(part.c_str(), FILE_WRITE);
    if (!out) return "Could not create " + path + " on the SD card.";
    mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
    error = item.size ? Fetch(http, client, base + item.name, item.size, 0, [&](const uint8_t* data, size_t size) {
      mbedtls_sha256_update(&ctx, data, size);
      return out.write(data, size) == size;
    }) : "";
    mbedtls_sha256_finish(&ctx, digest); mbedtls_sha256_free(&ctx);
    out.close();
    if (error.empty() && HexDigest(digest) != item.sha256) error = "SHA-256 mismatch for " + item.name + ".";
    if (error.empty()) { fs.remove(path.c_str()); if (!fs.rename(part.c_str(), path.c_str())) error = "Could not finish " + path + " on the SD card."; }
    if (!error.empty()) { fs.remove(part.c_str()); return error; }
  }
  File marker = fs.open((dir + "/.complete").c_str(), FILE_WRITE);
  if (!marker || marker.write(reinterpret_cast<const uint8_t*>(assets_sha256_.data()), 64) != 64) return "Could not write the SD card installation marker.";
  marker.close();
  Log("All SD card files verified.");
  return "";
}

void ProjectRuntime::DownloadTask(void* arg) {
  auto self = static_cast<ProjectRuntime*>(arg);
  {
    HTTPClient http; NetworkClient plain; NetworkClientSecure secure;
    const bool tls = self->url_.rfind("https://", 0) == 0;
    if (tls) {
      if (self->insecure_) secure.setInsecure();
      else secure.setCACertBundle(kCertBundleStart, kCertBundleEnd - kCertBundleStart);
    }
    NetworkClient& client = tls ? secure : plain;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.setReuse(true);
    self->stage_ = 2; self->Log("Receiving project file.");
    self->downloaded_.clear(); self->downloaded_.reserve(self->expected_size_);
    self->error_ = self->Fetch(http, client, self->url_, self->expected_size_, 45000, [self](const uint8_t* data, size_t size) {
      self->downloaded_.insert(self->downloaded_.end(), data, data + size); return true;
    });
    if (self->error_.empty()) {
      self->stage_ = 3; self->Log("File received. Checking checksum.");
      MD5Builder md5; md5.begin();
      for (size_t offset = 0; offset < self->downloaded_.size(); offset += 4096) md5.add(self->downloaded_.data() + offset, std::min<size_t>(4096, self->downloaded_.size() - offset));
      md5.calculate();
      if (md5.toString() != self->md5_.c_str()) self->error_ = "Project checksum mismatch";
      else if (ComputeSha256(self->downloaded_) != self->expected_sha256_) self->error_ = "Project SHA-256 mismatch";
    }
    // Files go onto the card before activation, so the first frame already sees them.
    if (self->error_.empty() && !self->assets_url_.empty()) self->error_ = self->InstallAssets(http, client);
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
  if (error_.empty() && !Activate(downloaded_, {target_id_, "", target_version_, kBoardId, DISPLAY_PROJECT_ABI})) error_ = "Project identity, ABI, ELF or flash validation failed";
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
  if (sd_required_ && sd_locked_) {
    c.TextMarquee(x+2, y+h/2-4, w-4, "SD card busy", theme.muted, 1, true);
    return true;
  }
  if (sd_required_ && !sd_ready_ && testing_) { RestorePrevious("Project files could not be verified on the SD card."); return loaded_; }
  if (sd_required_ && !sd_ready_) {
    // Retry every 10 s while the card or its files are missing instead of running the module without them.
    if (millis() - sd_checked_at_ > 10000) {
      auto* sd = Board::GetInstance().GetSdCard();
      if (!sd->mounted()) sd->Mount();
      sd_ready_ = AssetsReady(id_, version_, assets_digest_);
      sd_checked_at_ = millis();
      if (sd_ready_) { RefreshSd(); CloseAssets(); Log("SD card files found; project resumed."); }
    }
    if (!sd_ready_) {
      c.TextMarquee(x+2, y+h/2-12, w-4, "Insert SD card", theme.info, 1, true);
      c.TextMarquee(x+2, y+h/2+2, w-4, "Project files are on the card", theme.muted, 1, true);
      return true;
    }
  }
  Drawing drawing{&c, x, y, w, h};
  const time_t now = time(nullptr); const tm local = *localtime(&now);
  ProjectFrame frame{static_cast<uint32_t>(abi_), &drawing, w, h, local.tm_hour, local.tm_min, local.tm_sec,
    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_wday,
    Network::GetInstance().TimeValid(), data_at_ ? millis()-data_at_ : 0,
    {data_[0].c_str(), data_[1].c_str()}, {static_cast<uint32_t>(data_[0].size()), static_cast<uint32_t>(data_[1].size())}, millis(),
    theme.text, theme.muted, theme.info, Label, Line, Ring, Circle, Rect, FillRect, TextWidth, sinf, cosf,
    AssetSize, AssetRead, MediaInfo, MediaDraw};
  const auto saved = c.GetClip(); c.IntersectClip(x,y,w,h);
  char* argv[] = {reinterpret_cast<char*>(&frame)};
  const int result = esp_elf_request(&elf_, 0, 1, argv); c.RestoreClip(saved);
  if (testing_) {
    if (result != 0) { RestorePrevious("Project rejected its first display frame."); return loaded_; }
    testing_ = false; stage_ = 6; progress_ = 100; ack_ok_ = true; ack_ready_ = true;
    first_frame_at_ = millis();
    { Settings s("project", true); s.SetBool("trial", false); s.SetString("pending", ""); }
    Log("Project activated successfully. First display frame completed.");
    if (sd_required_) { CleanupAssets(); RefreshSd(); }
  }
  RecordHealthyFrame();
  return true;
}
