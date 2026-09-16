#include "project_api.h"

struct Station { const char *id, *name, *url; };
static const struct Station stations[] = {
  {"paradise", "Radio Paradise", "http://stream-uk1.radioparadise.com/mp3-128"},
  {"groove", "Groove Salad", "https://ice5.somafm.com/groovesalad-128-mp3"},
  {"agent", "Secret Agent", "https://ice5.somafm.com/secretagent-128-mp3"},
  {"drone", "Drone Zone", "https://ice5.somafm.com/dronezone-128-mp3"},
  {"farda", "Radio Farda", "https://stream.radiojar.com/cp13r2cpn3quv"},
  {"yar", "Radio Yar", "https://shoutcast.glwiz.com/RadioYAR.mp3"},
  {"navahang", "Navahang", "https://navairan.com/;stream.nsv"},
  {"bbc", "BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
  {"dlf", "Deutschlandfunk", "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3"},
  {"ndrinfo", "NDR Info", "https://icecast.ndr.de/ndr/ndrinfo/live/mp3/128/stream.mp3"},
  {"1live", "1LIVE", "https://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3"},
};
#define STATION_COUNT (sizeof(stations) / sizeof(stations[0]))

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
static int configured(const char *id) {
  for (int i = 0; i < (int)STATION_COUNT; ++i) if (same(id, stations[i].id)) return i;
  return 0;
}
static void copy(char *out, const char *in, int capacity) {
  int i = 0;
  while (in[i] && i + 1 < capacity) { out[i] = in[i]; ++i; }
  out[i] = 0;
}
static void append(char *buf, int *pos, int capacity, const char *s) {
  while (*s && *pos + 1 < capacity) buf[(*pos)++] = *s++;
  buf[*pos] = 0;
}
static void append_int(char *buf, int *pos, int capacity, int value) {
  char digits[8];
  int n = 0;
  do { digits[n++] = '0' + value % 10; value /= 10; } while (value && n < (int)sizeof(digits));
  for (int i = 0; i < n && *pos + 1 < capacity; ++i) buf[(*pos)++] = digits[n - i - 1];
  buf[*pos] = 0;
}

int app_main(int argc, char **argv) {
  if (argc != 1) return -1;
  struct ProjectFrame *f = (struct ProjectFrame *)argv[0];
  if (f->abi != DISPLAY_PROJECT_ABI) return -1;
  static int station = -1;
  static char last_config[20] = "";
  const char *config = f->data[0] ? f->data[0] : "";
  if (station < 0 || !same(config, last_config)) {
    copy(last_config, config, sizeof(last_config));
    station = configured(config);
    f->radio_start(stations[station].url);
  }

  // A tap (release before kTapMaxMs) changes station; anything held longer is the board's own
  // volume ramp instead (see Board::RampRadioVolume in board.cpp — kept in step with this value),
  // so it must not also advance the station once released. button_held_ms resets to 0 the instant
  // the button is up, so the peak while it was still down has to be tracked across frames.
  enum { kTapMaxMs = 350 };
  static int up_was_pressed = 0, down_was_pressed = 0;
  static uint32_t up_peak_ms = 0, down_peak_ms = 0;
  const int up_pressed = (f->buttons & 2) != 0;
  const int down_pressed = (f->buttons & 4) != 0;
  if (up_pressed) {
    if (f->button_held_ms[1] > up_peak_ms) up_peak_ms = f->button_held_ms[1];
  } else {
    if (up_was_pressed && up_peak_ms > 0 && up_peak_ms < kTapMaxMs) {
      station = (station + 1) % STATION_COUNT;
      f->radio_start(stations[station].url);
    }
    up_peak_ms = 0;
  }
  if (down_pressed) {
    if (f->button_held_ms[2] > down_peak_ms) down_peak_ms = f->button_held_ms[2];
  } else {
    if (down_was_pressed && down_peak_ms > 0 && down_peak_ms < kTapMaxMs) {
      station = (station + STATION_COUNT - 1) % STATION_COUNT;
      f->radio_start(stations[station].url);
    }
    down_peak_ms = 0;
  }
  up_was_pressed = up_pressed;
  down_was_pressed = down_pressed;

  char status[40], header[24], bottom[32];
  int kbps = 0;
  const int state = f->radio_status(status, sizeof(status), &kbps);
  const int center = f->width / 2;

  // "RADIO 3/11": station position is otherwise invisible while tapping through stations blind,
  // and every label() centers on the full width, so it rides on the same line as the title
  // rather than needing a dedicated corner.
  int hp = 0;
  append(header, &hp, sizeof(header), "RADIO ");
  append_int(header, &hp, sizeof(header), station + 1);
  append(header, &hp, sizeof(header), "/");
  append_int(header, &hp, sizeof(header), (int)STATION_COUNT);
  f->label(f->canvas, 8, header, f->muted, 1);
  f->line(f->canvas, 8, 23, f->width - 8, 23, 1, f->accent);
  f->label(f->canvas, 42, stations[station].name, f->text, 2);
  f->label(f->canvas, 64, status, state == 4 ? 0xf800 : f->accent, 1);
  if (state == 3) {
    for (int i = 0; i < 7; ++i) {
      int bar = 4 + ((f->frame_ms / 170 + i * 7) % (i + 5)) * 2;
      f->fill_rect(f->canvas, center - 25 + i * 8, 90 - bar, 4, bar, f->accent);
    }
  }
  // The +/- hint used to disappear the moment playback started (bitrate took its place); keep a
  // short reminder alongside the bitrate instead of losing it once someone's a few days in.
  int bp = 0;
  if (kbps > 0) {
    append_int(bottom, &bp, sizeof(bottom), kbps);
    append(bottom, &bp, sizeof(bottom), "k - +/-");
  } else {
    append(bottom, &bp, sizeof(bottom), "Tap +/-: station");
  }
  f->label(f->canvas, f->height - 9, bottom, f->muted, 1);
  return 0;
}
