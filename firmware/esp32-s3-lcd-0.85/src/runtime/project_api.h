#pragma once
#include <stdint.h>

// Stable C ABI for independently compiled Xtensa ELF display modules.
// Coordinates are relative to the content area. The host clips every primitive.
#define DISPLAY_PROJECT_ABI 1
#ifdef __cplusplus
extern "C" {
#endif
struct ProjectFrame {
  uint32_t abi;
  void *canvas;
  int width, height;
  int hour, minute, second, time_valid;
  uint32_t data_age_ms;
  const char *data[2];
  uint16_t text, muted, accent;
  void (*label)(void *, int, const char *, uint16_t, int);
  void (*line)(void *, int, int, int, int, int, uint16_t);
  void (*ring)(void *, int, int, int, int, uint16_t);
  void (*circle)(void *, int, int, int, uint16_t);
  float (*sine)(float);
  float (*cosine)(float);
};
#ifdef __cplusplus
}
#endif
