#pragma once

// Single public include for Project Builder sketches. Keep application code on this
// wrapper instead of firmware internals so the host can evolve without breaking sketches.
#include "project_api.h"

#define PROJECT_BOARD_ID "esp32-s3-lcd-0.85"
#define PROJECT_DISPLAY_WIDTH 128
#define PROJECT_DISPLAY_HEIGHT 128
#define PROJECT_LED_COUNT 8
#define PROJECT_FLASH_BYTES (8u * 1024u * 1024u)
#define PROJECT_PSRAM_BYTES (8u * 1024u * 1024u)

#define PROJECT_BUTTON_POWER (1u << 0)
#define PROJECT_BUTTON_PLUS (1u << 1)
#define PROJECT_BUTTON_MINUS (1u << 2)
#define PROJECT_WIFI_OFF 0
#define PROJECT_WIFI_SETUP 1
#define PROJECT_WIFI_CONNECTING 2
#define PROJECT_WIFI_CONNECTED 3
#define PROJECT_AUDIO_SPEAKER 1
#define PROJECT_AUDIO_MICROPHONES 2

typedef struct ProjectFrame ProjectBoard;

static inline uint16_t project_rgb(uint8_t red, uint8_t green, uint8_t blue) {
  return (uint16_t)(((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) | (blue >> 3));
}

static inline void project_clear(ProjectBoard *board, uint16_t color) {
  board->fill_rect(board->canvas, 0, 0, board->width, board->height, color);
}

static inline void project_text(ProjectBoard *board, int y, const char *text,
                                uint16_t color, int scale) {
  board->label(board->canvas, y, text, color, scale);
}

static inline int project_button_pressed(ProjectBoard *board, uint32_t button) {
  return (board->buttons & button) != 0;
}

// setup() runs once after the ELF is loaded. loop() runs once for every display frame.
// Both receive only the stable board API; direct access to firmware globals is intentionally absent.
#define PROJECT_SKETCH()                                                                    \
  __attribute__((visibility("default"))) int app_main(int argc, char **argv) {             \
    if (argc != 1 || !argv || !argv[0]) return -1;                                          \
    ProjectBoard *board = (ProjectBoard *)argv[0];                                          \
    if (board->abi < DISPLAY_PROJECT_MIN_ABI || board->abi > DISPLAY_PROJECT_ABI) return -2; \
    static uint8_t initialized = 0;                                                         \
    if (!initialized) { setup(board); initialized = 1; }                                    \
    loop(board);                                                                            \
    return 0;                                                                               \
  }
