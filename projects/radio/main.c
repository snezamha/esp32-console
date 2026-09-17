#include "project_api.h"

struct Station { const char *id, *name, *lang, *url; };
static const struct Station stations[] = {
  {"farda", "Radio Farda", "FA", "https://stream.radiojar.com/cp13r2cpn3quv"},
  {"navahang", "Navahang", "FA", "https://navairan.com/;stream.nsv"},
  {"bbc", "BBC World Service", "EN", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
  {"dlf", "Deutschlandfunk", "DE", "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3"},
  {"ndrinfo", "NDR Info", "DE", "https://icecast.ndr.de/ndr/ndrinfo/live/mp3/128/stream.mp3"},
  {"1live", "1LIVE", "DE", "https://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3"},
};
#define STATION_COUNT (sizeof(stations) / sizeof(stations[0]))
#define MAX_CUSTOM 3
#define CUSTOM_URL_CAP 128
#define CUSTOM_NAME_CAP 24
#define CUSTOM_LANG_CAP 4

// User-supplied stations (project settings custom1..3: URL, name, language), appended after the
// built-in list. Names are ASCII-only (enforced server-side in project-config.ts) since the
// device's bitmap font only has printable ASCII glyphs.
static char custom_urls[MAX_CUSTOM][CUSTOM_URL_CAP];
static char custom_names[MAX_CUSTOM][CUSTOM_NAME_CAP];
static char custom_langs[MAX_CUSTOM][CUSTOM_LANG_CAP];
static int custom_count = 0;

static int total_stations(void) { return (int)STATION_COUNT + custom_count; }
static const char *station_name(int i) {
  if (i < (int)STATION_COUNT) return stations[i].name;
  const char *custom = custom_names[i - STATION_COUNT];
  return custom[0] ? custom : "Custom station";
}
static const char *station_lang(int i) {
  return i < (int)STATION_COUNT ? stations[i].lang : custom_langs[i - STATION_COUNT];
}
static const char *station_url(int i) {
  return i < (int)STATION_COUNT ? stations[i].url : custom_urls[i - STATION_COUNT];
}

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
// same() compares whole strings (both hit '\0' together); a prefix check on an unterminated
// span (a slice of the pipe-joined custom URL blob) needs its own bounds-aware loop.
static int starts_with(const char *text, int text_len, const char *prefix) {
  int i = 0;
  for (; prefix[i]; ++i) {
    if (i >= text_len || text[i] != prefix[i]) return 0;
  }
  return 1;
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

// data[1] is 3 slots of "url|name|lang", pipe-joined (project_data in the console's sync
// route) — 9 fields total. A slot whose url doesn't look like a stream URL is dropped
// entirely (radio_start() would reject it anyway; this just avoids wasting a station on a
// blank or malformed one), even if its name/lang were filled in.
static void take_field(const char **p, char *out, int capacity) {
  const char *start = *p;
  while (**p && **p != '|') ++(*p);
  int len = (int)(*p - start);
  if (len > capacity - 1) len = capacity - 1;
  int i = 0;
  for (; i < len; ++i) out[i] = start[i];
  out[i] = 0;
  if (**p == '|') ++(*p);
}
static void parse_custom(const char *blob) {
  custom_count = 0;
  const char *p = blob;
  for (int slot = 0; slot < MAX_CUSTOM; ++slot) {
    char url[CUSTOM_URL_CAP], name[CUSTOM_NAME_CAP], lang[CUSTOM_LANG_CAP];
    take_field(&p, url, sizeof(url));
    take_field(&p, name, sizeof(name));
    take_field(&p, lang, sizeof(lang));
    if (starts_with(url, CUSTOM_URL_CAP, "http://") ||
        starts_with(url, CUSTOM_URL_CAP, "https://")) {
      copy(custom_urls[custom_count], url, CUSTOM_URL_CAP);
      copy(custom_names[custom_count], name, CUSTOM_NAME_CAP);
      copy(custom_langs[custom_count], lang[0] ? lang : "EN", CUSTOM_LANG_CAP);
      ++custom_count;
    }
  }
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

static void loading_ring(struct ProjectFrame *f, int x, int y) {
  // The power-hold overlay uses a muted track and a bright clockwise arc.
  // Stream buffering has no measurable percentage, so the arc rotates instead of filling.
  const int radius = 15, start = (f->frame_ms / 7) % 360;
  const float radians = 0.017453293f;
  f->ring(f->canvas, x, y, radius, 4, f->muted);
  for (int degree = 0; degree < 120; degree += 10) {
    const float a = (start + degree - 90) * radians;
    const float b = (start + degree + 10 - 90) * radians;
    f->line(f->canvas,
            x + (int)(radius * f->cosine(a)), y + (int)(radius * f->sine(a)),
            x + (int)(radius * f->cosine(b)), y + (int)(radius * f->sine(b)),
            4, f->accent);
  }
}

// Small color chip next to the header so a station's language is recognizable at a glance
// while tapping through blind, without needing a second color slot inside label().
static uint16_t lang_color(const char *lang) {
  if (same(lang, "FA")) return 0x07ff;
  if (same(lang, "EN")) return 0x07e0;
  if (same(lang, "DE")) return 0xffe0;
  if (same(lang, "AR")) return 0xf81f;
  if (same(lang, "TR")) return 0xfd20;
  if (same(lang, "FR")) return 0x001f;
  if (same(lang, "ES")) return 0xfea0;
  if (same(lang, "RU")) return 0xf800;
  return 0xffff;
}

static void clear_bands(uint8_t *bands) {
  volatile uint8_t *out = bands;
  for (int i = 0; i < 8; ++i) out[i] = 0;
}

// Persists the last played station index in the project's private SD area so the radio resumes
// where it left off after a reboot, instead of always restarting at the configured default.
// No-ops when the board has no SD card; the radio still works, it just won't remember.
static void save_last_station(struct ProjectFrame *f, int index) {
  if (!f->sd_mounted) return;
  char digits[8];
  int n = 0;
  append_int(digits, &n, sizeof(digits), index);
  f->storage_write("station.idx", digits, n, 0);
}
static int load_last_station(struct ProjectFrame *f) {
  if (!f->sd_mounted) return -1;
  char digits[8];
  int32_t n = f->storage_read("station.idx", 0, digits, sizeof(digits));
  if (n <= 0) return -1;
  int value = 0;
  for (int i = 0; i < n; ++i) {
    if (digits[i] < '0' || digits[i] > '9') return -1;
    value = value * 10 + (digits[i] - '0');
  }
  return value;
}

int app_main(int argc, char **argv) {
  if (argc != 1) return -1;
  struct ProjectFrame *f = (struct ProjectFrame *)argv[0];
  if (f->abi != DISPLAY_PROJECT_ABI) return -1;
  static int station = -1;
  static char last_config[20] = "";
  static char last_custom[3 * (CUSTOM_URL_CAP + CUSTOM_NAME_CAP + CUSTOM_LANG_CAP)] = "";
  const char *config = f->data[0] ? f->data[0] : "";
  const char *custom_blob = f->data[1] ? f->data[1] : "";
  if (station < 0 || !same(config, last_config) || !same(custom_blob, last_custom)) {
    const int first_boot = station < 0;
    copy(last_config, config, sizeof(last_config));
    copy(last_custom, custom_blob, sizeof(last_custom));
    parse_custom(custom_blob);
    const int restored = first_boot ? load_last_station(f) : -1;
    station = (restored >= 0 && restored < total_stations()) ? restored : configured(config);
    f->radio_start(station_url(station));
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
      station = (station + (next ? 1 : total_stations() - 1)) % total_stations();
      f->radio_start(station_url(station));
      save_last_station(f, station);
    }
  }

  char status[40], counter[8], line2[48], info[24];
  int kbps = 0;
  const int state = f->radio_status(status, sizeof(status), &kbps);
  const int reconnecting = state == 4 && same(status, "Reconnecting");
  const int center = f->width / 2;
  static uint8_t displayed_bands[8] = {0};

  // Just the position ("3/9"), not the whole "RADIO 3/9" — the icon and name already say
  // it's a radio, and one screen this small can't spare width on a word that adds no data.
  int cp = 0;
  append_int(counter, &cp, sizeof(counter), station + 1);
  append(counter, &cp, sizeof(counter), "/");
  append_int(counter, &cp, sizeof(counter), total_stations());
  f->label(f->canvas, 6, counter, f->muted, 1);
  // Language chip in the header corner replaces the old "FA "/"DE " text prefix on the name —
  // one glance at the color says the language, so the name line stays short enough to run large.
  f->fill_rect(f->canvas, f->width - 12, 6, 6, 6, lang_color(station_lang(station)));
  f->line(f->canvas, 8, 20, f->width - 8, 20, 1, f->accent);
  const char *name = station_name(station);
  const int name_scale = f->text_width(name, 2) <= f->width - 8 ? 2 : 1;
  f->label(f->canvas, name_scale == 2 ? 34 : 38, name, f->text, name_scale);
  if (state == 3) {
    uint8_t measured[8];
    const int fresh = f->radio_spectrum(measured, 8);
    const int bar_width = 6, gap = 4, left = center - (8 * bar_width + 7 * gap) / 2;
    for (int i = 0; i < 8; ++i) {
      const int target = fresh == 8 ? measured[i] : 0;
      int current = displayed_bands[i];
      if (target > current) {
        int step = (target - current + 1) / 2;
        current += step > 0 ? step : 1;
      } else if (target < current) {
        int step = (current - target + 5) / 6;
        current -= step > 0 ? step : 1;
      }
      displayed_bands[i] = (uint8_t)current;
      const int height = current * 30 / 100;
      if (height > 0) f->fill_rect(f->canvas, left + i * (bar_width + gap), 90 - height,
                                   bar_width, height, f->accent);
    }
  } else if (state == 1 || state == 2 || reconnecting) {
    clear_bands(displayed_bands);
    loading_ring(f, center, 74);
  } else if (state == 4) {
    clear_bands(displayed_bands);
    f->ring(f->canvas, center, 74, 15, 2, 0xf800);
    f->label(f->canvas, 70, "!", 0xf800, 1);
  } else {
    clear_bands(displayed_bands);
  }
  // Status and bitrate share one line instead of two — "Playing  128k" reads as a single fact.
  int lp = 0;
  append(line2, &lp, sizeof(line2), status);
  if (kbps > 0) {
    append(line2, &lp, sizeof(line2), "  ");
    append_int(line2, &lp, sizeof(line2), kbps);
    append(line2, &lp, sizeof(line2), "k");
  }
  f->label(f->canvas, 98, line2, state == 4 && !reconnecting ? 0xf800 : f->accent, 1);
  // One rotating bottom line instead of two fixed rows: volume, then each gesture hint in turn.
  const int cycle = (int)(f->frame_ms / 3000) % 3;
  if (cycle == 0) {
    int ip = 0;
    append(info, &ip, sizeof(info), "VOL ");
    append_int(info, &ip, sizeof(info), f->volume);
    append(info, &ip, sizeof(info), "%");
    f->label(f->canvas, f->height - 12, info, f->text, 1);
  } else if (cycle == 1) {
    f->label(f->canvas, f->height - 12, "2x +/-: station", f->muted, 1);
  } else {
    f->label(f->canvas, f->height - 12, "Hold +/-: volume", f->muted, 1);
  }
  return 0;
}
