// ESP32 Console firmware for Waveshare ESP32-S3-LCD-0.85
//
// Buttons   Power   click: open menu / select · double click: backlight on/off · hold: power off
//           Vol+    click: +10 · hold: max volume      (menu: next item, or +1 while editing)
//           Vol-    click: -10 · hold: mute            (menu: previous item, or -1 while editing)
//           Vol+ and Vol- together: open / close the hardware check
// Menu      Display · Sound & LED · Connectivity · Device · HW check
// Serial    USB CDC 115200, send `help`. Replies are JSON lines.

#include "src/app.h"

void setup() { App::GetInstance().Start(); }

void loop() { App::GetInstance().Loop(); }
