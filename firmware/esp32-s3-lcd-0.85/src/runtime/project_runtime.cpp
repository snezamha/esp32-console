#include "project_runtime.h"
#include "project_api.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <MD5Builder.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include "../common/settings.h"
#include "../display/lcd_display.h"
#include "../services/console_client.h"
#include "../services/network.h"
extern const uint8_t kCertBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCertBundleEnd[] asm("_binary_x509_crt_bundle_end");
namespace {
constexpr size_t kMaxPackage = 128 * 1024;
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
}

bool ProjectRuntime::Load(const std::vector<uint8_t>& bytes, esp_elf_t& elf) {
  if (bytes.size() < sizeof(elf32_hdr_t) || bytes.size() > kMaxPackage) return false;
  const auto* hdr = reinterpret_cast<const elf32_hdr_t*>(bytes.data());
  if (memcmp(bytes.data(), "\x7f" "ELF\x01\x01", 6) || hdr->machine != 94 || hdr->shentsize != sizeof(elf32_shdr_t) ||
      !hdr->shnum || hdr->shstrndx >= hdr->shnum || hdr->shoff > bytes.size() ||
      hdr->shnum * sizeof(elf32_shdr_t) > bytes.size() - hdr->shoff) return false;
  const auto* sections = reinterpret_cast<const elf32_shdr_t*>(bytes.data() + hdr->shoff);
  for (int i = 0; i < hdr->shnum; ++i) {
    if (sections[i].link >= hdr->shnum || sections[i].size > kMaxPackage ||
        (sections[i].type != SHT_NOBITS && (sections[i].offset > bytes.size() || sections[i].size > bytes.size() - sections[i].offset))) return false;
  }
  esp_elf_init(&elf);
  if (esp_elf_relocate(&elf, bytes.data())) { esp_elf_deinit(&elf); return false; }
  return true;
}

void ProjectRuntime::Begin() {
  mounted_ = LittleFS.begin(false);
  if (!mounted_) { // Only initialize a blank partition; preserve an existing filesystem.
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    uint8_t first[16];
    if (part && esp_partition_read(part, 0, first, sizeof(first)) == ESP_OK &&
        std::all_of(first, first + sizeof(first), [](uint8_t b) { return b == 0xff; })) mounted_ = LittleFS.begin(true);
  }
  if (!mounted_) return;
  Settings s("project");
  const std::string active = s.GetString("active", "0|none");
  slot_ = active[0] == '1' ? 1 : 0;
  const std::string saved = active.size() > 2 ? active.substr(2) : "none";
  if (saved == "none") return;
  auto file = LittleFS.open(Path(slot_).c_str(), "r");
  if (!file || file.size() > kMaxPackage) return;
  std::vector<uint8_t> bytes(file.size());
  if (file.read(bytes.data(), bytes.size()) == bytes.size() && Load(bytes, elf_)) { loaded_ = true; id_ = saved; }
}

bool ProjectRuntime::Activate(const std::vector<uint8_t>& bytes, const std::string& id) {
  esp_elf_t candidate{};
  if (!mounted_ || !Load(bytes, candidate)) return false;
  const int next_slot = 1 - slot_;
  auto file = LittleFS.open(Path(next_slot).c_str(), "w");
  if (!file || file.write(bytes.data(), bytes.size()) != bytes.size()) { esp_elf_deinit(&candidate); return false; }
  file.flush(); file.close();
  // Publish a verified, fully written slot; a failed download leaves the running slot intact.
  { Settings s("project", true); s.SetString("active", std::to_string(next_slot) + "|" + id); }
  if (loaded_) esp_elf_deinit(&elf_);
  elf_ = candidate; loaded_ = true; slot_ = next_slot; id_ = id;
  data_[0].clear(); data_[1].clear(); data_at_ = 0;
  return true;
}

std::string ProjectRuntime::Start(const std::string& command_id, const std::string& arg, const std::string& server, bool insecure) {
  if (busy_) return "fail|Another project is loading";
  const auto id = ConsoleClient::FormValue(arg, "id");
  if (id == "none") {
    { Settings s("project", true); s.SetString("active", "0|none"); }
    if (loaded_) esp_elf_deinit(&elf_);
    loaded_ = false; id_ = "none";
    return "ok|Default display restored";
  }
  if (!mounted_) return "fail|Project flash storage unavailable";
  const auto path = ConsoleClient::FormValue(arg, "path");
  const auto abi = ConsoleClient::FormValue(arg, "abi");
  expected_size_ = strtoul(ConsoleClient::FormValue(arg, "size").c_str(), nullptr, 10);
  md5_ = ConsoleClient::FormValue(arg, "md5");
  if (id.empty() || path.rfind("/projects/", 0) != 0 || abi != "1" || !expected_size_ || expected_size_ > kMaxPackage || md5_.size() != 32) return "fail|Invalid project package";
  command_id_ = command_id; target_id_ = id; url_ = server + path; insecure_ = insecure;
  error_.clear(); downloaded_.clear(); progress_ = 0; done_ = false; busy_ = true;
  if (xTaskCreatePinnedToCore(DownloadTask, "project_load", 8192, this, 1, nullptr, 0) != pdPASS) {
    busy_ = false; return "fail|No memory for project download";
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
    http.setTimeout(15000);
    const bool begun = tls ? http.begin(secure, self->url_.c_str()) : http.begin(plain, self->url_.c_str());
    const int code = begun ? http.GET() : -1;
    if (code != 200 || http.getSize() != static_cast<int>(self->expected_size_)) self->error_ = "Project download failed or size mismatch";
    else {
      self->downloaded_.resize(self->expected_size_);
      auto stream = http.getStreamPtr(); size_t total = 0; uint32_t last_data = millis();
      while (total < self->expected_size_) {
        const size_t available = stream->available();
        if (!available) {
          if (!http.connected() || millis() - last_data > 15000) break;
          vTaskDelay(pdMS_TO_TICKS(5)); continue;
        }
        const size_t count = stream->readBytes(self->downloaded_.data() + total, std::min(available, self->expected_size_ - total));
        total += count; if (count) last_data = millis();
        self->progress_ = total * 100 / self->expected_size_;
      }
      if (total != self->expected_size_) self->error_ = "Project download interrupted";
      else {
        MD5Builder md5; md5.begin(); md5.add(self->downloaded_.data(), self->downloaded_.size()); md5.calculate();
        if (md5.toString() != self->md5_.c_str()) self->error_ = "Project checksum mismatch";
      }
    }
    http.end();
  }
  self->done_ = true;
  vTaskDelete(nullptr);
}

std::string ProjectRuntime::Loop() {
  if (!done_.exchange(false)) return "";
  if (error_.empty() && !Activate(downloaded_, target_id_)) error_ = "Invalid ELF or failed flash write";
  downloaded_.clear(); busy_ = false;
  return command_id_ + (error_.empty() ? "|ok|Project installed" : "|fail|" + error_);
}

void ProjectRuntime::SetData(const std::string& data) {
  const auto divider = data.find('|');
  data_[0] = data.substr(0, divider); data_[1] = divider == std::string::npos ? "" : data.substr(divider + 1);
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
    Network::GetInstance().TimeValid(), data_at_ ? millis()-data_at_ : 0,
    {data_[0].c_str(), data_[1].c_str()}, theme.text, theme.muted, theme.info, Label, Line, Ring, Circle, sinf, cosf};
  const auto saved = c.GetClip(); c.IntersectClip(x,y,w,h);
  char* argv[] = {reinterpret_cast<char*>(&frame)};
  esp_elf_request(&elf_, 0, 1, argv); c.RestoreClip(saved);
  return true;
}
