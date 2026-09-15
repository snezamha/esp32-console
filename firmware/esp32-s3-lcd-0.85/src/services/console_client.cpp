#include "console_client.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include <algorithm>

#include "../../version.h"
#include "../board/board.h"
#include "../common/settings.h"
#include "network.h"
#include "../runtime/project_runtime.h"
#include "sd_files.h"

// Default console address; `pnpm firmware:build` writes console_url.h from the CONSOLE_URL
// environment variable.
#if __has_include("../../console_url.h")
#include "../../console_url.h"
#endif
#ifndef CONSOLE_URL
#define CONSOLE_URL ""
#endif

// Root certificates bundled with the ESP-IDF libraries, used to verify HTTPS servers.
extern const uint8_t kCertBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kCertBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {

// The server holds a poll for up to 25 s; allow for slow networks on top.
constexpr int kPollWaitS = 25;
constexpr uint32_t kPollTimeoutMs = (kPollWaitS + 15) * 1000;
constexpr uint32_t kRequestTimeoutMs = 10000;
// Never start polls closer together than this, even when the server answers at once.
constexpr uint32_t kMinPollIntervalMs = 1000;
constexpr uint32_t kRetryMs = 3000;
constexpr uint32_t kMaxBackoffMs = 60000;
constexpr size_t kSeenCommands = 16;

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string UrlDecode(const std::string& value) {
  std::string out;
  for (size_t i = 0; i < value.size(); i++) {
    if (value[i] == '+') {
      out += ' ';
    } else if (value[i] == '%' && i + 2 < value.size()) {
      out += static_cast<char>(strtol(value.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += value[i];
    }
  }
  return out;
}

std::string HexDigest(const uint8_t* digest, size_t length) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(length * 2, '0');
  for (size_t i = 0; i < length; ++i) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 15];
  }
  return out;
}

std::string MacAddress() {
  const uint64_t mac = ESP.getEfuseMac();
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", uint8_t(mac), uint8_t(mac >> 8),
           uint8_t(mac >> 16), uint8_t(mac >> 24), uint8_t(mac >> 32), uint8_t(mac >> 40));
  return buf;
}

std::string NormalizeUrl(std::string url) {
  url.erase(std::remove_if(url.begin(), url.end(), [](char c) { return c == '"' || c == ' '; }),
            url.end());
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (!url.empty() && url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    url = "https://" + url;
  }
  return url;
}

void AttachCertificates(WiFiClientSecure& client, bool insecure) {
  if (insecure) {
    client.setInsecure();
  } else {
    client.setCACertBundle(kCertBundleStart, kCertBundleEnd - kCertBundleStart);
  }
}

}  // namespace

std::string ConsoleClient::FormValue(const std::string& form, const std::string& key) {
  size_t start = 0;
  while (start <= form.size()) {
    size_t end = form.find('&', start);
    if (end == std::string::npos) end = form.size();
    const std::string pair = form.substr(start, end - start);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && UrlDecode(pair.substr(0, eq)) == key) {
      return UrlDecode(pair.substr(eq + 1));
    }
    start = end + 1;
  }
  return "";
}

void ConsoleClient::Begin() {
  Settings s("console");
  server_ = NormalizeUrl(s.GetString("url", CONSOLE_URL));
  insecure_ = s.GetBool("insecure", false);
  token_ = s.GetString("token", "");
  rev_ = s.GetInt("rev", 0);
  NewSecret();
  esp_ota_img_states_t image_state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  ota_validation_pending_ = running && esp_ota_get_state_partition(running, &image_state) == ESP_OK && image_state == ESP_OTA_IMG_PENDING_VERIFY;
  SetState(server_.empty() ? State::Off : State::Offline);
}

void ConsoleClient::Save() {
  Settings s("console", true);
  s.SetString("url", server_);
  s.SetBool("insecure", insecure_);
  s.SetString("token", token_);
  s.SetInt("rev", rev_);
}

void ConsoleClient::NewSecret() {
  char buf[33];
  for (int i = 0; i < 16; i++) snprintf(buf + i * 2, 3, "%02x", uint8_t(esp_random()));
  secret_ = buf;
}

void ConsoleClient::Changed() {
  if (on_change_) on_change_();
}

// Restarts once the update's ack was actually delivered, so the console sees the result.
void ConsoleClient::NoteAcksDelivered(const std::vector<std::string>& delivered, uint32_t now_ms) {
  if (ota_pending_ack_id_.empty()) return;
  for (const auto& ack : delivered) {
    if (ack.rfind(ota_pending_ack_id_ + "|", 0) == 0) {
      ota_pending_ack_id_.clear();
      restart_at_ms_ = now_ms + 500;
      return;
    }
  }
}

void ConsoleClient::SetServer(const std::string& url) {
  const std::string normalized = NormalizeUrl(url);
  if (normalized != server_) {
    // A token belongs to one server.
    server_ = normalized;
    token_.clear();
    rev_ = 0;
    code_.clear();
    name_.clear();
    NewSecret();
  }
  Save();
  failures_ = 0;
  next_poll_ms_ = 0;
  SetState(server_.empty() ? State::Off : State::Offline);
}

void ConsoleClient::SetInsecure(bool insecure) {
  insecure_ = insecure;
  Save();
  failures_ = 0;
  next_poll_ms_ = 0;
}

void ConsoleClient::Unlink() {
  if (!token_.empty()) revoke_token_ = token_;
  token_.clear();
  rev_ = 0;
  name_.clear();
  code_.clear();
  NewSecret();
  Save();
  next_poll_ms_ = 0;
  if (state_ == State::Linked) SetState(State::Connecting);
}

void ConsoleClient::SetState(State state) {
  if (state == state_) return;
  state_ = state;
  // A code stays valid through short errors; only a link or switching off retires it.
  if (state == State::Linked || state == State::Off) code_.clear();
  Changed();
}

const char* ConsoleClient::StateText() const {
  switch (state_) {
    case State::Off: return "Off";
    case State::Offline: return "Offline";
    case State::Connecting: return "Connecting";
    case State::Pairing: return "Pairing";
    case State::Linked: return "Linked";
    case State::Error: return "Error";
  }
  return "Off";
}

std::string ConsoleClient::ServerHost() const {
  const size_t scheme = server_.find("://");
  return scheme == std::string::npos ? server_ : server_.substr(scheme + 3);
}

// ---------------------------------------------------------------------------
// Requests

void ConsoleClient::Loop(uint32_t now_ms) {
  // With bootloader rollback enabled, a newly installed image remains provisional until it has
  // kept the full application loop alive for 30 seconds. A crash before this point rolls back.
  if (ota_validation_pending_ && now_ms >= 30000) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      ota_validation_pending_ = false;
      Serial.println("{\"console\":\"firmware image validated\"}");
    }
  }
  for (const auto& project_ack : ProjectRuntime::Get().Loop()) { acks_.push_back(project_ack); Changed(); }
  for (const auto& file_ack : SdFiles::Get().Loop()) { acks_.push_back(file_ack); Changed(); }
  if (poll_.done) {
    poll_.done = false;
    const bool ok = poll_.code == 200;
    if (ok) {
      NoteAcksDelivered(poll_.acks, now_ms);
    } else {
      acks_.insert(acks_.begin(), poll_.acks.begin(), poll_.acks.end());
    }
    poll_.acks.clear();
    HandlePoll(poll_.code, poll_.response);
    poll_.busy = false;
  }
  if (ota_finished_) {
    ota_finished_ = false;
    FinishOta(now_ms);
  }
  if (ota_active_ && ota_progress_ != ota_shown_progress_) {
    ota_shown_progress_ = ota_progress_;
    Changed();
  }
  if (restart_at_ms_ && static_cast<int32_t>(now_ms - restart_at_ms_) >= 0) {
    Serial.println("{\"console\":\"restarting after update\"}");
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  if (server_.empty()) return;
  if (Network::GetInstance().State() != Network::WifiState::Connected) {
    if (state_ != State::Offline && state_ != State::Off) SetState(State::Offline);
    return;
  }
  if (state_ == State::Offline) {
    SetState(State::Connecting);
    next_poll_ms_ = now_ms;
  }

  // Only ever one network task in flight (this poll, or the OTA download): the Wi-Fi/lwIP stack
  // is not safe against two concurrent requests. A local change (settings, check results) goes
  // up on the poll after this one, not instantly.
  if (!poll_.busy && static_cast<int32_t>(now_ms - next_poll_ms_) >= 0 &&
      now_ms - last_poll_start_ms_ >= kMinPollIntervalMs && !ota_active_) {
    StartPoll(now_ms);
  }
}

std::string ConsoleClient::CommonFields() {
  auto& network = Network::GetInstance();
  // Runs on every poll for as long as the board is on; a plain "+" chain regrows the string
  // several times per call, which fragments the heap over many hours. One reservation avoids that.
  std::string body;
  body.reserve(256);
  body = "mac=" + UrlEncode(MacAddress()) +
                     "&board=" + UrlEncode(Board::GetInstance().GetBoardType()) +
                     "&fw=" + UrlEncode(FIRMWARE_VERSION) + "&ip=" + UrlEncode(network.WifiIp()) +
                     "&rssi=" + std::to_string(network.WifiRssi());
  if (telemetry_provider_) body += "&" + telemetry_provider_();
  if (!ota_state_.empty()) {
    body += "&ota=" + ota_state_ + UrlEncode("|" + std::to_string(ota_progress_.load()) + "|" + ota_error_);
    ota_reported_progress_ = ota_progress_;
  }
  body += ProjectRuntime::Get().Report();
  return body;
}

bool ConsoleClient::Start(Slot& slot, const std::string& body, uint32_t timeout_ms) {
  slot.url = server_ + "/api/device/sync";
  slot.body = body;
  slot.timeout_ms = timeout_ms;
  slot.tls = server_.rfind("https://", 0) == 0;
  slot.insecure = insecure_;
  slot.busy = true;
  if (xTaskCreatePinnedToCore(RequestTask, slot.name, 8192, &slot, 1, nullptr, 0) != pdPASS) {
    slot.busy = false;
    acks_.insert(acks_.begin(), slot.acks.begin(), slot.acks.end());
    slot.acks.clear();
    return false;
  }
  return true;
}

void ConsoleClient::StartPoll(uint32_t now_ms) {
  last_poll_start_ms_ = now_ms;
  std::string body = CommonFields();
  bool wait = false;

  if (!revoke_token_.empty()) {
    body += "&token=" + UrlEncode(revoke_token_) + "&unlink=1";
  } else if (!token_.empty()) {
    body += "&token=" + UrlEncode(token_) + "&rev=" + std::to_string(rev_);
    const std::string state = state_provider_ ? state_provider_() : "";
    if (!state.empty()) body += "&" + state;
    for (const auto& ack : acks_) body += "&ack=" + UrlEncode(ack);
    poll_.acks = acks_;
    acks_.clear();
    // Hold the poll only when nothing new rides on it, so local changes are never delayed.
    wait = state_ == State::Linked && state == reported_state_ && poll_.acks.empty();
    reported_state_ = state;
  } else {
    body += "&secret=" + secret_;
    if (!code_.empty()) body += "&code=" + code_;
    wait = !code_.empty();
  }
  if (wait) body += "&wait=" + std::to_string(kPollWaitS);

  if (!Start(poll_, body, wait ? kPollTimeoutMs : kRequestTimeoutMs)) next_poll_ms_ = now_ms + kRetryMs;
}

void ConsoleClient::RequestTask(void* arg) {
  auto slot = static_cast<Slot*>(arg);
  int code = -1;
  std::string body;

  // Scoped so HTTPClient/WiFiClientSecure/WiFiClient are destroyed (freeing the TLS session's
  // buffers) before this function returns. vTaskDelete(nullptr) below ends the task without
  // unwinding the stack — anything still in scope at that point would never be destructed and
  // its memory never reclaimed, leaking a full TLS session (tens of KB) on every single request.
  {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    if (slot->tls) AttachCertificates(secure, slot->insecure);

    http.setTimeout(slot->timeout_ms);
    http.setConnectTimeout(10000);
    const bool begun = slot->tls ? http.begin(secure, slot->url.c_str()) : http.begin(plain, slot->url.c_str());
    if (begun) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      code = http.POST(slot->body.c_str());
      if (code > 0) body = http.getString().c_str();
      http.end();
    }
  }

  slot->code = code;
  slot->response = body;
  slot->done = true;
  vTaskDelete(nullptr);
}

void ConsoleClient::HandlePoll(int http_code, const std::string& body) {
  std::string status, token, code, name;
  int rev = rev_;
  bool has_rev = false;
  Values settings;
  std::vector<Command> commands;
  std::string project_data;

  size_t start = 0;
  while (start < body.size()) {
    size_t end = body.find('\n', start);
    if (end == std::string::npos) end = body.size();
    const std::string line = body.substr(start, end - start);
    start = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "status") {
      status = value;
    } else if (key == "token") {
      token = value;
    } else if (key == "code") {
      code = value;
    } else if (key == "name") {
      name = value;
    } else if (key == "rev") {
      rev = atoi(value.c_str());
      has_rev = true;
    } else if (key == "project_data") {
      project_data = UrlDecode(value);
    } else if (key.rfind("set.", 0) == 0) {
      settings.push_back({key.substr(4), value});
    } else if (key.rfind("cmd.", 0) == 0) {
      // cmd.<id>.<type>=<url-encoded arg>
      const size_t dot = key.find('.', 4);
      if (dot != std::string::npos) {
        commands.push_back({key.substr(4, dot - 4), key.substr(dot + 1), UrlDecode(value)});
      }
    }
  }

  const uint32_t now = millis();
  if (http_code != 200 || status.empty()) {
    failures_++;
    error_ = http_code > 0 ? "HTTP " + std::to_string(http_code) : "No connection";
    Serial.printf("{\"console\":\"error\",\"detail\":\"%s\"}\n", error_.c_str());
    next_poll_ms_ = now + std::min<uint32_t>(kMaxBackoffMs, kRetryMs << std::min(failures_, 4));
    reported_state_.clear();  // Upload everything again once the server is back.
    SetState(State::Error);
    return;
  }
  failures_ = 0;
  error_.clear();
  next_poll_ms_ = now;

  // Revocation after Unlink() on the board: done once the server answered.
  if (!revoke_token_.empty()) {
    revoke_token_.clear();
    SetState(State::Connecting);
    return;
  }

  if (status == "pending") {
    const bool changed = code != code_ || state_ != State::Pairing;
    code_ = code;
    SetState(State::Pairing);
    if (changed) {
      Serial.printf("{\"console\":\"pairing\",\"code\":\"%s\"}\n", code_.c_str());
      Changed();
    }
  } else if (status == "linked" && !token.empty()) {
    if (token != token_) {
      token_ = token;
      rev_ = has_rev ? rev : 0;
      reported_state_.clear();
      Save();
      Board::GetInstance().GetDisplay()->ShowNotification("Device linked");
      Serial.println("{\"console\":\"linked\"}");
    }
    if (!settings.empty() && apply_handler_) apply_handler_(settings);
    if (has_rev && rev != rev_) {
      rev_ = rev;
      Save();
    }
    for (const auto& command : commands) HandleCommand(command);
    if (!project_data.empty()) ProjectRuntime::Get().SetData(project_data);
    const bool name_changed = name != name_;
    name_ = name;
    SetState(State::Linked);
    if (name_changed) Changed();
  } else if (status == "unlinked") {
    if (!token_.empty()) {
      token_.clear();
      rev_ = 0;
      name_.clear();
      Save();
      Board::GetInstance().GetDisplay()->ShowNotification("Device removed");
      Serial.println("{\"console\":\"unlinked\"}");
    }
    code_.clear();
    NewSecret();
    SetState(State::Connecting);
  } else {
    error_ = "Bad reply";
    SetState(State::Error);
  }
}

void ConsoleClient::HandleCommand(const Command& command) {
  // Commands are delivered once; still, never run one twice if a reply is repeated.
  if (std::find(seen_commands_.begin(), seen_commands_.end(), command.id) != seen_commands_.end()) return;
  seen_commands_.push_back(command.id);
  if (seen_commands_.size() > kSeenCommands) seen_commands_.erase(seen_commands_.begin());

  Serial.printf("{\"console\":\"command\",\"type\":\"%s\"}\n", command.type.c_str());
  if (command.type == "project_stop") {
    const auto result = ProjectRuntime::Get().Cancel(command.id, command.arg);
    if (!result.empty()) acks_.push_back(command.id + "|" + result);
    Changed(); return;
  }
  if (command.type == "project_install") {
    const auto result = ota_active_ ? "fail|Firmware update is running" : ProjectRuntime::Get().Start(command.id, command.arg, server_, insecure_, token_);
    if (!result.empty()) acks_.push_back(command.id + "|" + result);
    Changed(); return;
  }
  if (command.type.rfind("sd_", 0) == 0) {
    const auto result = ota_active_ ? "fail|Firmware update is running" : SdFiles::Get().Start(command, server_, insecure_, token_);
    if (!result.empty()) acks_.push_back(command.id + "|" + result);
    Changed(); return;
  }
  if (command.type == "ota") {
    StartOta(command);
    return;
  }
  const std::string ack = command_handler_ ? command_handler_(command) : "fail|unsupported";
  if (!ack.empty()) acks_.push_back(command.id + "|" + ack);
}

// ---------------------------------------------------------------------------
// Over-the-air update

void ConsoleClient::StartOta(const Command& command) {
  if (ota_active_ || ProjectRuntime::Get().Busy() || SdFiles::Get().Busy()) {
    acks_.push_back(command.id + "|fail|Another update is running");
    return;
  }
  ota_url_ = server_ + FormValue(command.arg, "path");
  ota_md5_ = FormValue(command.arg, "md5");
  ota_sha256_ = FormValue(command.arg, "sha256");
  ota_version_ = FormValue(command.arg, "version");
  ota_size_ = strtoul(FormValue(command.arg, "size").c_str(), nullptr, 10);
  ota_command_id_ = command.id;
  if (ota_md5_.size() != 32 || ota_sha256_.size() != 64 || !ota_size_) {
    acks_.push_back(command.id + "|fail|Invalid update identity");
    return;
  }
  ota_error_.clear();
  ota_ok_ = false;
  ota_progress_ = 0;
  ota_shown_progress_ = -1;
  ota_reported_progress_ = -1;
  ota_state_ = "downloading";
  ota_active_ = true;
  Changed();
  if (xTaskCreatePinnedToCore(OtaTask, "console_ota", 8192, this, 1, nullptr, 0) != pdPASS) {
    ota_error_ = "No memory for update task";
    ota_finished_ = true;
  }
}

void ConsoleClient::OtaTask(void* arg) {
  auto self = static_cast<ConsoleClient*>(arg);
  bool ok = false;
  std::string error;

  // Scoped for the same reason as RequestTask: vTaskDelete(nullptr) below does not run
  // destructors, so HTTPClient/WiFiClientSecure/WiFiClient must go out of scope (freeing the
  // TLS session) before it, or every update leaks one.
  {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    const bool tls = self->ota_url_.rfind("https://", 0) == 0;
    if (tls) AttachCertificates(secure, self->insecure_);

    http.setTimeout(20000);
    const bool begun = tls ? http.begin(secure, self->ota_url_.c_str()) : http.begin(plain, self->ota_url_.c_str());
    const int code = begun ? http.GET() : -1;
    const int length = code == 200 ? http.getSize() : 0;

    if (code != 200) {
      error = code > 0 ? "Download failed: HTTP " + std::to_string(code) : "Download failed: no connection";
    } else if (length <= 0 || (self->ota_size_ && static_cast<size_t>(length) != self->ota_size_)) {
      error = "Unexpected image size";
    } else if (!Update.begin(length, U_FLASH)) {
      error = std::string("Not enough space: ") + Update.errorString();
    } else {
      if (!self->ota_md5_.empty()) Update.setMD5(self->ota_md5_.c_str());
      NetworkClient* stream = http.getStreamPtr();
      uint8_t buffer[4096];
      uint8_t digest[32];
      mbedtls_sha256_context sha;
      mbedtls_sha256_init(&sha);
      bool sha_ok = mbedtls_sha256_starts(&sha, 0) == 0;
      size_t written = 0;
      uint32_t last_data = millis();
      while (written < static_cast<size_t>(length)) {
        const size_t available = stream->available();
        if (!available) {
          if (!http.connected() || millis() - last_data > 20000) break;
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        const int read = stream->readBytes(buffer, std::min(available, sizeof(buffer)));
        if (read <= 0) continue;
        if (sha_ok) sha_ok = mbedtls_sha256_update(&sha, buffer, read) == 0;
        if (Update.write(buffer, read) != static_cast<size_t>(read)) break;
        written += read;
        last_data = millis();
        self->ota_progress_ = static_cast<int>(written * 100 / length);
      }
      if (sha_ok) sha_ok = mbedtls_sha256_finish(&sha, digest) == 0;
      mbedtls_sha256_free(&sha);
      if (written != static_cast<size_t>(length)) {
        error = Update.hasError() ? Update.errorString() : "Download interrupted";
        Update.abort();
      } else if (!sha_ok || HexDigest(digest, sizeof(digest)) != self->ota_sha256_) {
        error = "Firmware SHA-256 mismatch";
        Update.abort();
      } else if (!Update.end()) {
        error = std::string("Verification failed: ") + Update.errorString();
      } else {
        ok = true;
      }
    }
    http.end();
  }

  self->ota_error_ = error;
  self->ota_ok_ = ok;
  self->ota_finished_ = true;
  vTaskDelete(nullptr);
}

void ConsoleClient::FinishOta(uint32_t now_ms) {
  ota_active_ = false;
  if (ota_ok_) {
    ota_progress_ = 100;
    ota_state_ = "done";
    acks_.push_back(ota_command_id_ + "|ok|Installed v" + ota_version_);
    Board::GetInstance().GetDisplay()->ShowNotification("Update installed", 5000);
    ota_pending_ack_id_ = ota_command_id_;
    // Restart once the ack reaches the server, or after 10 s regardless.
    restart_at_ms_ = now_ms + 10000;
  } else {
    ota_state_ = "failed";
    acks_.push_back(ota_command_id_ + "|fail|" + ota_error_);
    Board::GetInstance().GetDisplay()->ShowNotification("Update failed", 5000);
  }
  Serial.printf("{\"console\":\"ota\",\"ok\":%s,\"detail\":\"%s\"}\n", ota_ok_ ? "true" : "false",
                ota_error_.c_str());
  next_poll_ms_ = now_ms;
  Changed();
}
