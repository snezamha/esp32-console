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
struct TapState {
  int was_pressed, second_press;
  uint32_t peak_ms, pressed_at_ms, last_release_ms;
};
static int double_tap(struct TapState *tap, int pressed, uint32_t held_ms, uint32_t now) {
  enum { kDoubleTapMs = 240, kHoldMs = 350 };
  if (pressed && !tap->was_pressed) {
    tap->second_press = tap->last_release_ms && now - tap->last_release_ms <= kDoubleTapMs;
    tap->peak_ms = 0;
    tap->pressed_at_ms = now;
  }
  if (pressed && held_ms > tap->peak_ms) tap->peak_ms = held_ms;
  int fired = 0;
  if (!pressed && tap->was_pressed) {
    if (tap->peak_ms > 0 && tap->peak_ms < kHoldMs && now - tap->pressed_at_ms < kHoldMs) {
      fired = tap->second_press;
      tap->last_release_ms = fired ? 0 : now;
    } else {
      tap->last_release_ms = 0;
    }
  }
  tap->was_pressed = pressed;
  return fired;
}

static void spinner(struct ProjectFrame *f, int x, int y) {
  static const int8_t offset[8][2] = {
    {0, -9}, {6, -6}, {9, 0}, {6, 6}, {0, 9}, {-6, 6}, {-9, 0}, {-6, -6}
  };
  const int head = (f->frame_ms / 110) % 8;
  for (int i = 0; i < 8; ++i) {
    const int age = (head + 8 - i) % 8;
    f->circle(f->canvas, x + offset[i][0], y + offset[i][1], age == 0 ? 2 : 1,
              age <= 2 ? f->accent : f->muted);
  }
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

  // Board::OnClick handles one tap as volume; the second release selects a station.
  // A hold is Board::RampRadioVolume, and must never also change stations on release.
  static struct TapState up_tap = {0}, down_tap = {0};
  static int combo_blocked = 0;
  static uint32_t last_frame_ms = 0;
  if (last_frame_ms && f->frame_ms - last_frame_ms > 300) {
    up_tap.was_pressed = down_tap.was_pressed = 0;
    up_tap.last_release_ms = down_tap.last_release_ms = 0;
  }
  last_frame_ms = f->frame_ms;
  const int up_pressed = (f->buttons & 2) != 0;
  const int down_pressed = (f->buttons & 4) != 0;
  if (up_pressed && down_pressed) combo_blocked = 1;
  if (combo_blocked) {
    up_tap.was_pressed = down_tap.was_pressed = 0;
    up_tap.last_release_ms = down_tap.last_release_ms = 0;
    if (!up_pressed && !down_pressed) combo_blocked = 0;
  } else {
    const int next = double_tap(&up_tap, up_pressed, f->button_held_ms[1], f->frame_ms);
    const int previous = double_tap(&down_tap, down_pressed, f->button_held_ms[2], f->frame_ms);
    if (next || previous) {
      station = (station + (next ? 1 : (int)STATION_COUNT - 1)) % STATION_COUNT;
      f->radio_start(stations[station].url);
    }
  }

  char status[40], header[24], info[32];
  int kbps = 0;
  const int state = f->radio_status(status, sizeof(status), &kbps);
  const int reconnecting = state == 4 && same(status, "Reconnecting");
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
  const int name_scale = f->text_width(stations[station].name, 2) <= f->width - 8 ? 2 : 1;
  f->label(f->canvas, name_scale == 2 ? 42 : 47, stations[station].name, f->text, name_scale);
  f->label(f->canvas, 62, status, state == 4 && !reconnecting ? 0xf800 : f->accent, 1);
  if (state == 3) {
    for (int i = 0; i < 7; ++i) {
      int bar = 4 + ((f->frame_ms / 170 + i * 7) % (i + 5)) * 2;
      f->fill_rect(f->canvas, center - 25 + i * 8, 86 - bar, 4, bar, f->accent);
    }
  } else if (state == 1 || state == 2 || reconnecting) {
    spinner(f, center, 80);
  } else if (state == 4) {
    f->ring(f->canvas, center, 80, 9, 2, 0xf800);
    f->label(f->canvas, 75, "!", 0xf800, 1);
  }
  int ip = 0;
  append(info, &ip, sizeof(info), "VOL ");
  append_int(info, &ip, sizeof(info), f->volume);
  append(info, &ip, sizeof(info), "%");
  if (kbps > 0) {
    append(info, &ip, sizeof(info), "  ");
    append_int(info, &ip, sizeof(info), kbps);
    append(info, &ip, sizeof(info), "k");
  }
  f->label(f->canvas, f->height - 22, info, f->text, 1);
  f->label(f->canvas, f->height - 9,
           (f->frame_ms / 4000) % 2 ? "Hold +/-: volume" : "2x +/-: station", f->muted, 1);
  return 0;
}
