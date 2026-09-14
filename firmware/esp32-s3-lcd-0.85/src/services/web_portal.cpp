#include "web_portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <string>

#include "network.h"
#include "portal_page.h"

namespace {

WebServer server(80);
DNSServer dns;

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

void SendPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", kPortalPage);
}

// Anything unknown (including OS captive-portal probes) lands on the setup page.
void RedirectToPage() {
  const String host = server.hostHeader();
  const String ap_ip = WiFi.softAPIP().toString();
  const String sta_ip = WiFi.localIP().toString();
  if (host == ap_ip || host == sta_ip || host.endsWith(".local")) {
    SendPage();
    return;
  }
  const bool ap = Network::GetInstance().AccessPointActive();
  server.sendHeader("Location", "http://" + (ap ? ap_ip : sta_ip) + "/", true);
  server.send(302, "text/plain", "");
}

void HandleStatus() {
  auto& network = Network::GetInstance();
  char buf[400];
  snprintf(buf, sizeof(buf),
           "{\"device\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"ap\":%s,\"ap_ip\":\"%s\",\"error\":\"%s\",\"attempt\":%d}",
           JsonEscape(network.DeviceName()).c_str(), network.WifiStatus().c_str(),
           JsonEscape(network.WifiSsid()).c_str(), network.WifiIp().c_str(), network.WifiRssi(),
           network.AccessPointActive() ? "true" : "false", network.AccessPointIp().c_str(),
           JsonEscape(network.LastError()).c_str(), network.AttemptId());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void HandleScan() {
  auto& network = Network::GetInstance();
  // Only on request: a scan moves the radio off the access point's channel, which briefly
  // disconnects the phone showing this page. Networks were scanned before the access point
  // started, so the page normally just reads those.
  const bool force = server.hasArg("force");
  if (force && !network.IsScanning()) network.StartScan();

  std::string json = std::string("{\"scanning\":") + (network.IsScanning() ? "true" : "false") +
                     ",\"networks\":[";
  const auto results = network.IsScanning() ? std::vector<WifiNetwork>{} : network.ScanResults();
  for (size_t i = 0; i < results.size(); i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + JsonEscape(results[i].ssid) + "\",\"rssi\":" +
            std::to_string(results[i].rssi) + ",\"secure\":" + (results[i].secure ? "true" : "false") +
            "}";
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json.c_str());
}

void HandleWifi() {
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "text/plain", "Network name must be 1-32 characters");
    return;
  }
  if (password.length() > 0 && (password.length() < 8 || password.length() > 63)) {
    server.send(400, "text/plain", "Password must be 8-63 characters");
    return;
  }
  // Reply first: connecting may move the radio to another channel.
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  Network::GetInstance().Connect(ssid.c_str(), password.c_str());
}

}  // namespace

void WebPortal::Start() {
  server.on("/", HTTP_GET, SendPage);
  server.on("/api/status", HTTP_GET, HandleStatus);
  server.on("/api/scan", HTTP_GET, HandleScan);
  server.on("/api/wifi", HTTP_POST, HandleWifi);
  server.onNotFound(RedirectToPage);
  server.begin();
  started_ = true;
}

void WebPortal::Stop() {
  server.stop();
  if (dns_started_) dns.stop();
  dns_started_ = false;
  started_ = false;
}

void WebPortal::UpdateDns() {
  const bool want = Network::GetInstance().AccessPointActive();
  if (want && !dns_started_) {
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns_started_ = dns.start(53, "*", WiFi.softAPIP());
  } else if (!want && dns_started_) {
    dns.stop();
    dns_started_ = false;
  }
}

void WebPortal::Loop(uint32_t now_ms) {
  (void)now_ms;
  const auto state = Network::GetInstance().State();
  const bool want = state != Network::WifiState::Off;

  if (want && !started_) Start();
  if (!want && started_) Stop();
  if (!started_) return;

  UpdateDns();
  server.handleClient();
}
