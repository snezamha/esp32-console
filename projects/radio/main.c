#include "project_api.h"

struct Station { const char *id, *name, *menu_name, *lang, *url; };
static const struct Station stations[] = {
  {"farda", "Radio Farda", "Radio Farda", "FA", "https://stream.radiojar.com/cp13r2cpn3quv"},
  {"navahang", "Navahang", "Navahang", "FA", "https://navairan.com/;stream.nsv"},
  {"bbc", "BBC World Svc", "BBC World", "EN", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
  {"dlf", "Deutschlandfunk", "DLF", "DE", "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3"},
  {"ndrinfo", "NDR Info", "NDR Info", "DE", "https://icecast.ndr.de/ndr/ndrinfo/live/mp3/128/stream.mp3"},
  {"1live", "1LIVE", "1LIVE", "DE", "https://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3"},
  {"dlfkultur", "Deutschlandfunk Kultur", "DLF Kultur", "DE", "https://st02.sslstream.dlf.de/dlf/02/128/mp3/stream.mp3"},
  {"dlfnova", "Deutschlandfunk Nova", "DLF Nova", "DE", "https://st03.sslstream.dlf.de/dlf/03/128/mp3/stream.mp3"},
  {"1livediggi", "1LIVE DIGGI", "1LIVE DIGGI", "DE", "https://wdr-1live-diggi.icecastssl.wdr.de/wdr/1live/diggi/mp3/128/stream.mp3"},
  {"wdr3", "WDR 3", "WDR 3", "DE", "https://wdr-wdr3-live.icecastssl.wdr.de/wdr/wdr3/live/mp3/128/stream.mp3"},
  {"wdr4", "WDR 4", "WDR 4", "DE", "https://wdr-wdr4-live.icecastssl.wdr.de/wdr/wdr4/live/mp3/128/stream.mp3"},
  {"wdr5", "WDR 5", "WDR 5", "DE", "https://wdr-wdr5-live.icecastssl.wdr.de/wdr/wdr5/live/mp3/128/stream.mp3"},
};
#define STATION_COUNT (sizeof(stations) / sizeof(stations[0]))
#define PREVIOUS_STATION_COUNT 6
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
static const char *station_menu_name(int i) {
  return i < (int)STATION_COUNT ? stations[i].menu_name : station_name(i);
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

// Tiny 9x6 flag beside the station number; custom stations use their selected language.
static void draw_flag(struct ProjectFrame *f, int x, int y, const char *lang) {
  if (same(lang, "FA")) {
    f->fill_rect(f->canvas, x, y, 9, 2, 0x07e0);
    f->fill_rect(f->canvas, x, y + 2, 9, 2, 0xffff);
    f->fill_rect(f->canvas, x, y + 4, 9, 2, 0xf800);
    f->fill_rect(f->canvas, x + 4, y + 2, 1, 2, 0xf800);
  } else if (same(lang, "EN")) {
    f->fill_rect(f->canvas, x, y, 9, 6, 0x0010);
    f->line(f->canvas, x, y, x + 8, y + 5, 1, 0xffff);
    f->line(f->canvas, x + 8, y, x, y + 5, 1, 0xffff);
    f->fill_rect(f->canvas, x + 3, y, 3, 6, 0xffff);
    f->fill_rect(f->canvas, x, y + 2, 9, 2, 0xffff);
    f->fill_rect(f->canvas, x + 4, y, 1, 6, 0xf800);
    f->fill_rect(f->canvas, x, y + 3, 9, 1, 0xf800);
  } else if (same(lang, "DE")) {
    f->fill_rect(f->canvas, x, y, 9, 2, 0x0000);
    f->fill_rect(f->canvas, x, y + 2, 9, 2, 0xf800);
    f->fill_rect(f->canvas, x, y + 4, 9, 2, 0xffe0);
  } else if (same(lang, "FR")) {
    f->fill_rect(f->canvas, x, y, 3, 6, 0x001f);
    f->fill_rect(f->canvas, x + 3, y, 3, 6, 0xffff);
    f->fill_rect(f->canvas, x + 6, y, 3, 6, 0xf800);
  } else if (same(lang, "ES")) {
    f->fill_rect(f->canvas, x, y, 9, 2, 0xf800);
    f->fill_rect(f->canvas, x, y + 2, 9, 2, 0xffe0);
    f->fill_rect(f->canvas, x, y + 4, 9, 2, 0xf800);
  } else if (same(lang, "RU")) {
    f->fill_rect(f->canvas, x, y, 9, 2, 0xffff);
    f->fill_rect(f->canvas, x, y + 2, 9, 2, 0x001f);
    f->fill_rect(f->canvas, x, y + 4, 9, 2, 0xf800);
  } else if (same(lang, "TR")) {
    f->fill_rect(f->canvas, x, y, 9, 6, 0xf800);
    f->circle(f->canvas, x + 3, y + 3, 2, 0xffff);
    f->circle(f->canvas, x + 4, y + 2, 2, 0xf800);
    f->fill_rect(f->canvas, x + 7, y + 2, 1, 1, 0xffff);
  } else if (same(lang, "AR")) {
    f->fill_rect(f->canvas, x, y, 9, 6, 0x07e0);
    f->fill_rect(f->canvas, x + 2, y + 3, 5, 1, 0xffff);
  } else {
    f->fill_rect(f->canvas, x, y, 9, 6, f->muted);
  }
}

static void draw_tiny_text(struct ProjectFrame *f, int x, int y, const char *text, uint16_t color) {
  // Four-by-seven glyphs with one pixel of spacing. 10 = %, 11 = /.
  static const uint8_t glyphs[12][7] = {
    {15, 9, 9, 9, 9, 9, 15}, {2, 6, 2, 2, 2, 2, 7},
    {15, 1, 1, 15, 8, 8, 15}, {15, 1, 1, 7, 1, 1, 15},
    {9, 9, 9, 15, 1, 1, 1}, {15, 8, 8, 15, 1, 1, 15},
    {15, 8, 8, 15, 9, 9, 15}, {15, 1, 2, 2, 4, 4, 4},
    {15, 9, 9, 15, 9, 9, 15}, {15, 9, 9, 15, 1, 1, 15},
    {9, 9, 2, 2, 4, 9, 9}, {1, 1, 2, 2, 4, 8, 8},
  };
  for (int i = 0; text[i]; ++i) {
    const int glyph = text[i] == '%' ? 10 : text[i] == '/' ? 11 : text[i] - '0';
    if (glyph < 0 || glyph >= 12) continue;
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (glyphs[glyph][row] & (8 >> col))
          f->fill_rect(f->canvas, x + i * 5 + col, y + row, 1, 1, color);
      }
    }
  }
}

static void draw_percent(struct ProjectFrame *f, int value) {
  char text[8];
  int length = 0;
  append_int(text, &length, sizeof(text), value);
  append(text, &length, sizeof(text), "%");
  const int width = length * 5 - 1;
  int start_x = f->width - 8 - width / 2;
  if (start_x + width > f->width) start_x = f->width - width;
  draw_tiny_text(f, start_x, 97, text, f->text);
}

static void clear_bands(uint8_t *bands) {
  volatile uint8_t *out = bands;
  for (int i = 0; i < 8; ++i) out[i] = 0;
}

// The host's label() is always centered across the entire 128-pixel display. Keep text within
// the 96-pixel center column so it cannot run into the volume rail on the right.
static void fit_label(char *out, const char *text, int max_chars) {
  int length = 0;
  while (text[length] && length < max_chars) {
    out[length] = text[length];
    ++length;
  }
  if (text[length] && length >= 3) {
    out[length - 2] = '.';
    out[length - 1] = '.';
  }
  out[length] = 0;
}

// Persists the last played station index in the project's private SD area so the radio resumes
// where it left off after a reboot, instead of always restarting at the configured default.
// No-ops when the board has no SD card; the radio still works, it just won't remember.
static void save_last_station(struct ProjectFrame *f, int index) {
  if (!f->sd_mounted) return;
  char digits[8];
  int n = 0;
  append(digits, &n, sizeof(digits), "v2:");
  append_int(digits, &n, sizeof(digits), index);
  f->storage_write("station.idx", digits, n, 0);
}
static int load_last_station(struct ProjectFrame *f) {
  if (!f->sd_mounted) return -1;
  char digits[8];
  int32_t n = f->storage_read("station.idx", 0, digits, sizeof(digits));
  if (n <= 0) return -1;
  const int current_format = n >= 3 && digits[0] == 'v' && digits[1] == '2' && digits[2] == ':';
  int value = 0;
  for (int i = current_format ? 3 : 0; i < n; ++i) {
    if (digits[i] < '0' || digits[i] > '9') return -1;
    value = value * 10 + (digits[i] - '0');
  }
  if (!current_format && value >= PREVIOUS_STATION_COUNT &&
      value < PREVIOUS_STATION_COUNT + MAX_CUSTOM)
    value += (int)STATION_COUNT - PREVIOUS_STATION_COUNT;
  return value;
}

static void draw_station_menu(struct ProjectFrame *f, int selected) {
  const int count = total_stations();
  char counter[8] = "";
  int cp = 0;
  append_int(counter, &cp, sizeof(counter), selected + 1);
  append(counter, &cp, sizeof(counter), "/");
  append_int(counter, &cp, sizeof(counter), count);
  f->label(f->canvas, 3, "STATIONS", f->text, 1);
  draw_tiny_text(f, (f->width - (cp * 5 - 1)) / 2, 13, counter, f->muted);
  for (int row = -2; row <= 2; ++row) {
    const int index = (selected + row + count) % count;
    const int y = 59 + row * 17;
    if (row == 0) f->rect(f->canvas, 15, y - 2, f->width - 30, 12, f->accent);
    draw_flag(f, 3, y + 1, station_lang(index));
    char name[15];
    fit_label(name, station_menu_name(index), 14);
    f->label(f->canvas, y, name, row == 0 ? f->accent : f->text, 1);
    char number[5] = "";
    int np = 0;
    append_int(number, &np, sizeof(number), index + 1);
    draw_tiny_text(f, f->width - 4 - (np * 5 - 1), y, number, f->muted);
  }
  f->label(f->canvas, 103, "PWR play  2x back", f->muted, 1);
}

int app_main(int argc, char **argv) {
  if (argc != 1) return -1;
  struct ProjectFrame *f = (struct ProjectFrame *)argv[0];
  if (f->abi != DISPLAY_PROJECT_ABI) return -1;
  static int station = -1;
  static int menu_open = 0, menu_station = 0;
  static uint32_t menu_input_at_ms = 0;
  static struct TapState power_tap = {0};
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
    menu_open = 0;
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
    power_tap.was_pressed = power_tap.last_release_ms = 0;
  }
  last_frame_ms = f->frame_ms;
  const int power_pressed = (f->buttons & 1) != 0;
  const int up_pressed = (f->buttons & 2) != 0;
  const int down_pressed = (f->buttons & 4) != 0;
  const int power_double = double_tap(&power_tap, power_pressed, f->button_held_ms[0], f->frame_ms);
  if (power_double) {
    menu_open = !menu_open;
    menu_station = station;
    menu_input_at_ms = f->frame_ms;
    up_tap.was_pressed = down_tap.was_pressed = 0;
    up_tap.last_release_ms = down_tap.last_release_ms = 0;
  }
  if (menu_open) {
    static int menu_up_was = 0, menu_down_was = 0;
    static uint32_t up_repeat_at = 0, down_repeat_at = 0;
    if (up_pressed && !down_pressed &&
        (!menu_up_was || (f->button_held_ms[1] >= 450 && f->frame_ms >= up_repeat_at))) {
      menu_station = (menu_station + 1) % total_stations();
      menu_input_at_ms = f->frame_ms;
      up_repeat_at = f->frame_ms + (menu_up_was ? 150 : 450);
    }
    if (down_pressed && !up_pressed &&
        (!menu_down_was || (f->button_held_ms[2] >= 450 && f->frame_ms >= down_repeat_at))) {
      menu_station = (menu_station + total_stations() - 1) % total_stations();
      menu_input_at_ms = f->frame_ms;
      down_repeat_at = f->frame_ms + (menu_down_was ? 150 : 450);
    }
    menu_up_was = up_pressed;
    menu_down_was = down_pressed;
    if (f->frame_ms - menu_input_at_ms > 20000) {
      menu_open = 0;
      power_tap.last_release_ms = 0;
    }
    if (!power_pressed && power_tap.last_release_ms &&
        f->frame_ms - power_tap.last_release_ms > 240) {
      power_tap.last_release_ms = 0;
      menu_open = 0;
      if (station != menu_station) {
        station = menu_station;
        f->radio_start(station_url(station));
        save_last_station(f, station);
      }
    }
    if (menu_open) {
      f->radio_menu_open = 1;
      draw_station_menu(f, menu_station);
      return 0;
    }
  }
  f->radio_menu_open = 0;
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

  char status[40], counter[8], line2[48];
  int kbps = 0;
  const int state = f->radio_status(status, sizeof(status), &kbps);
  const int reconnecting = state == 4 && same(status, "Reconnecting");
  const int center = f->width / 2;
  static uint8_t displayed_bands[8] = {0};

  // Each element has its own space: station index/flag, name, status, spectrum, and volume.
  int cp = 0;
  append_int(counter, &cp, sizeof(counter), station + 1);
  append(counter, &cp, sizeof(counter), "/");
  append_int(counter, &cp, sizeof(counter), total_stations());
  const int counter_width = cp * 5 - 1;
  const int header_x = (f->width - counter_width - 4 - 9) / 2;
  draw_tiny_text(f, header_x, 5, counter, f->muted);
  draw_flag(f, header_x + counter_width + 4, 6, station_lang(station));
  const char *name = station_name(station);
  char short_name[17];
  fit_label(short_name, name, 16);
  const int name_scale = f->text_width(short_name, 2) <= 96 ? 2 : 1;
  const int name_y = name_scale == 2 ? 25 : 29;
  f->label(f->canvas, name_y, short_name, f->text, name_scale);

  int lp = 0;
  append(line2, &lp, sizeof(line2), status);
  if (kbps > 0) {
    append(line2, &lp, sizeof(line2), " ");
    append_int(line2, &lp, sizeof(line2), kbps);
    append(line2, &lp, sizeof(line2), "k");
  }
  char short_status[17];
  fit_label(short_status, line2, 16);
  f->label(f->canvas, 48, short_status, state == 4 && !reconnecting ? 0xf800 : f->accent, 1);

  if (state == 3) {
    uint8_t measured[8];
    const int fresh = f->radio_spectrum(measured, 8);
    const int bar_width = 5, gap = 4, left = center - (8 * bar_width + 7 * gap) / 2;
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
      const int height = current * 28 / 100;
      if (height > 0) f->fill_rect(f->canvas, left + i * (bar_width + gap), 91 - height,
                                   bar_width, height, f->accent);
    }
  } else if (state == 1 || state == 2 || reconnecting) {
    clear_bands(displayed_bands);
    loading_ring(f, center, 75);
  } else if (state == 4) {
    clear_bands(displayed_bands);
    f->ring(f->canvas, center, 75, 15, 2, 0xf800);
    f->label(f->canvas, 71, "!", 0xf800, 1);
  } else {
    clear_bands(displayed_bands);
  }

  // One clean volume track at the right, with its percentage directly underneath.
  const int volume = f->volume < 0 ? 0 : f->volume > 100 ? 100 : f->volume;
  f->fill_rect(f->canvas, f->width - 10, 29, 4, 62, f->muted);
  const int volume_height = volume * 62 / 100;
  if (volume_height > 0) f->fill_rect(f->canvas, f->width - 10, 91 - volume_height, 4, volume_height, f->accent);
  draw_percent(f, volume);
  return 0;
}
