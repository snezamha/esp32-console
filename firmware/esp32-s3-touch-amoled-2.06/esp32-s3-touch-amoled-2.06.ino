// ESP32 Console firmware for Waveshare ESP32-S3-Touch-AMOLED-2.06 (minimal skeleton)
//
// Wi-Fi provisioning + console pairing shown on the AMOLED panel. Boot button: hold 3s for a
// factory reset. No touch, audio, LED ring or SD card support yet.
// Serial    USB CDC 115200, send `help`. Replies are JSON lines.

#include "src/app.h"

void setup() { App::GetInstance().Start(); }

void loop() { App::GetInstance().Loop(); }
