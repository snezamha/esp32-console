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

static void side_meters(struct ProjectFrame *f, int playing) {
  enum { kTop = 58, kSegments = 8, kStep = 4 };
  const int left = 8, right = f->width - 16;
  const int volume = f->volume < 0 ? 0 : f->volume > 100 ? 100 : f->volume;
  const int volume_segments = (volume * kSegments + 99) / 100;
  // These are visual playback bars; ProjectFrame does not expose audio amplitude.
  const int animated = playing ? 3 + (int)((f->frame_ms / 160 + (f->frame_ms / 390) % 5) % 6) : 0;
  f->rect(f->canvas, left - 2, kTop - 2, 12, 36, f->muted);
  f->rect(f->canvas, right - 2, kTop - 2, 12, 36, f->muted);
  for (int segment = 0; segment < kSegments; ++segment) {
    const int y = kTop + (kSegments - 1 - segment) * kStep;
    if (segment < animated) {
      const uint16_t color = segment < 3 ? 0x07e0 : segment < 6 ? 0xffe0 : 0xf800;
      f->fill_rect(f->canvas, left, y, 8, 3, color);
    }
    if (segment < volume_segments) {
      f->fill_rect(f->canvas, right, y, 8, 3, f->accent);
    }
  }
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
    copy(last_config, config, sizeof(last_config));
    copy(last_custom, custom_blob, sizeof(last_custom));
    parse_custom(custom_blob);
    station = configured(config);
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
    }
  }

  char status[40], header[24], info[32], name[CUSTOM_NAME_CAP + 8];
  int kbps = 0;
  const int state = f->radio_status(status, sizeof(status), &kbps);
  const int reconnecting = state == 4 && same(status, "Reconnecting");
  const int center = f->width / 2;

  // "RADIO 3/9": station position is otherwise invisible while tapping through stations blind,
  // and every label() centers on the full width, so it rides on the same line as the title
  // rather than needing a dedicated corner.
  int hp = 0;
  append(header, &hp, sizeof(header), "RADIO ");
  append_int(header, &hp, sizeof(header), station + 1);
  append(header, &hp, sizeof(header), "/");
  append_int(header, &hp, sizeof(header), total_stations());
  f->label(f->canvas, 5, header, f->muted, 1);
  f->line(f->canvas, 8, 20, f->width - 8, 20, 1, f->accent);
  // "FA Radio Farda": the language tag rides in front of the name rather than on its own line —
  // screen's too small to spare a row for it, and it stays right next to what it describes.
  int np = 0;
  append(name, &np, sizeof(name), station_lang(station));
  append(name, &np, sizeof(name), " ");
  append(name, &np, sizeof(name), station_name(station));
  const int name_scale = f->text_width(name, 2) <= f->width - 8 ? 2 : 1;
  f->label(f->canvas, name_scale == 2 ? 30 : 35, name, f->text, name_scale);
  f->label(f->canvas, 49, status, state == 4 && !reconnecting ? 0xf800 : f->accent, 1);
  side_meters(f, state == 3);
  if (state == 3) {
    f->ring(f->canvas, center, 74, 15, 2, f->muted);
    f->line(f->canvas, center - 4, 67, center + 6, 74, 2, f->accent);
    f->line(f->canvas, center + 6, 74, center - 4, 81, 2, f->accent);
    f->line(f->canvas, center - 4, 81, center - 4, 67, 2, f->accent);
  } else if (state == 1 || state == 2 || reconnecting) {
    loading_ring(f, center, 74);
  } else if (state == 4) {
    f->ring(f->canvas, center, 74, 15, 2, 0xf800);
    f->label(f->canvas, 70, "!", 0xf800, 1);
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
  f->label(f->canvas, f->height - 20, info, f->text, 1);
  f->label(f->canvas, f->height - 9,
           (f->frame_ms / 4000) % 2 ? "Hold +/-: volume" : "2x +/-: station", f->muted, 1);
  return 0;
}
