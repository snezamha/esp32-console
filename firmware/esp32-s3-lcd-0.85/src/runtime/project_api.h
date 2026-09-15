#pragma once
#include <stdint.h>

// Stable C ABI for independently compiled Xtensa ELF display modules.
// Coordinates are relative to the content area. The host clips every primitive.
// ABI 3 appends SD card asset access. Fields are only appended, and the host sets `abi` to the
// version the module was built for, so ABI 2 modules keep working unchanged.
// `pnpm projects:build` defines DISPLAY_PROJECT_ABI=2 for projects without SD card assets.
#ifndef DISPLAY_PROJECT_ABI
#define DISPLAY_PROJECT_ABI 3
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
};
#ifdef __cplusplus
}
#endif
