#pragma once
#include <stdint.h>

// Stable C ABI for independently compiled Xtensa ELF display modules.
// Coordinates are relative to the content area. The host clips every primitive.
// ABI 3 appends SD card asset access. ABI 4 appends controlled board I/O. ABI 5 appends
// background MP3 radio playback. Fields are only
// appended, and the host sets `abi` to the version the module was built for, so older modules
// keep working unchanged.
// `pnpm projects:build` defines DISPLAY_PROJECT_ABI=2 for projects without SD card assets.
#ifndef DISPLAY_PROJECT_ABI
#define DISPLAY_PROJECT_ABI 5
#endif
#define DISPLAY_PROJECT_MIN_ABI 2
#ifdef __cplusplus
extern "C" {
#endif
struct ProjectFrame {
  uint32_t abi;
  void *canvas;
  int width, height;
  int hour, minute, second, year, month, day, weekday, time_valid;
  uint32_t data_age_ms;
  const char *data[2];
  uint32_t data_size[2];
  uint32_t frame_ms;
  uint16_t text, muted, accent;
  void (*label)(void *, int, const char *, uint16_t, int);
  void (*line)(void *, int, int, int, int, int, uint16_t);
  void (*ring)(void *, int, int, int, int, uint16_t);
  void (*circle)(void *, int, int, int, uint16_t);
  void (*rect)(void *, int, int, int, int, uint16_t);
  void (*fill_rect)(void *, int, int, int, int, uint16_t);
  int (*text_width)(const char *, int);
  float (*sine)(float);
  float (*cosine)(float);
  // ABI 3. Asset names are relative to the project's folder on the SD card, e.g. "images/logo.img".
  // Every call fails with -1 when the card or file is unavailable; modules must handle that.
  int32_t (*asset_size)(const char *name);
  int32_t (*asset_read)(const char *name, uint32_t offset, void *buffer, uint32_t size);
  // Media assets (.img/.vid, produced by `pnpm projects:build`): 16-byte header then RGB565 frames.
  int (*media_info)(const char *name, int *width, int *height, int *frames, int *fps);
  // Draws frame `frame % frames` with its top-left corner at x,y. Returns 0 on success.
  int (*media_draw)(void *, int x, int y, const char *name, uint32_t frame);
  // ABI 4: snapshots captured immediately before this frame is drawn.
  uint32_t buttons;       // PROJECT_BUTTON_* bit mask
  uint32_t button_held_ms[3]; // power, plus, minus
  int battery_percent;    // -1 when unavailable
  int battery_charging;
  int battery_discharging;
  int volume;
  int audio_sample_rate;
  int wifi_state;         // PROJECT_WIFI_*
  int wifi_rssi;
  const char *wifi_ssid;
  const char *wifi_ip;
  int ble_enabled;
  int sd_mounted;
  uint64_t sd_total_bytes;
  uint64_t sd_free_bytes;
  // LED changes are buffered until led_show(). led_release() restores Console settings.
  int (*led_set)(int index, uint8_t red, uint8_t green, uint8_t blue);
  int (*led_fill)(uint8_t red, uint8_t green, uint8_t blue);
  int (*led_show)(void);
  void (*led_release)(void);
  // Project-private SD area: /projects-data/<project-id>. Calls are rejected while SD is busy.
  int32_t (*storage_size)(const char *name);
  int32_t (*storage_read)(const char *name, uint32_t offset, void *buffer, uint32_t size);
  int32_t (*storage_write)(const char *name, const void *buffer, uint32_t size, int append);
  int (*storage_remove)(const char *name);
  int (*storage_mkdir)(const char *name);
  int (*storage_list)(int index, char *name, uint32_t capacity, int *is_dir, uint32_t *size);
  // Stereo interleaved signed 16-bit samples. At most 240 frames per non-blocking call.
  int (*audio_begin)(void); // bit 0 speaker, bit 1 microphones; 0 means unavailable
  int (*speaker_enable)(int enabled);
  int (*speaker_write)(const int16_t *samples, int frames);
  int (*mic_read)(int16_t *samples, int frames);
  // Scans start in a background task. Read results on later frames.
  int (*wifi_scan_start)(void);
  int (*wifi_scan_count)(void); // -1 while scanning
  int (*wifi_scan_result)(int index, char *ssid, uint32_t capacity, int *rssi, int *channel, int *secure);
  int (*ble_scan_start)(void);
  int (*ble_scan_count)(void); // -1 while scanning
  int (*ble_scan_result)(int index, char *address, uint32_t address_capacity,
                         char *name, uint32_t name_capacity, int *rssi);
  // One bounded (8 KB) asynchronous HTTP(S) GET at a time.
  int (*http_get)(const char *url);
  // -2 running, -1 failed, >=0 copied response bytes. `status` receives the HTTP status/error.
  int32_t (*http_result)(void *buffer, uint32_t capacity, int *status);
  // ABI 5: direct MP3 HTTP(S) streams, played by the host in a background task.
  int (*radio_start)(const char *url);
  void (*radio_stop)(void);
  // 0 stopped, 1 connecting, 2 buffering, 3 playing, 4 error.
  int (*radio_status)(char *text, uint32_t capacity, int *bitrate_kbps);
};
#ifdef __cplusplus
}
#endif
