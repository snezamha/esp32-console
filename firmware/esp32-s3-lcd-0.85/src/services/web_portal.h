#pragma once

#include <cstdint>

// Wi-Fi setup web page on port 80.
//   GET  /             setup page
//   GET  /api/status   {"device","state","ssid","ip","rssi","ap","ap_ip"}
//   GET  /api/scan     {"scanning",networks:[{"ssid","rssi","secure"}]}  (starts a scan if stale)
//   POST /api/wifi     form fields ssid, password; saves and connects
// While the setup access point is up, DNS answers every name with the board, so phones
// open the page automatically (captive portal).
class WebPortal {
 public:
  static WebPortal& GetInstance() {
    static WebPortal instance;
    return instance;
  }

  // Call from the main loop; starts and stops itself with Wi-Fi.
  void Loop(uint32_t now_ms);

 private:
  WebPortal() = default;
  void Start();
  void Stop();
  void UpdateDns();

  bool started_ = false;
  bool dns_started_ = false;
};
