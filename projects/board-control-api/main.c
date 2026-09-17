#include "project_api.h"
#include <stdint.h>

static int text_len(const char *text) { const volatile char *p = text; int n = 0; while (*p++) n++; return n; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *text, const char *prefix) { while (*prefix) if (*text++ != *prefix++) return 0; return 1; }
static void copy(char *out, const char *text) { while ((*out++ = *text++)) {} }
static int copy_url(char *out, const char *text) {
  int n = 0;
  while (text[n] && text[n] != '|') {
    if (n >= 240) { out[0] = 0; return -1; }
    out[n] = text[n]; n++;
  }
  out[n] = 0;
  return n;
}
static int number(const char *text) { int n = 0; while (*text >= '0' && *text <= '9') n = n * 10 + (*text++ - '0'); return n; }
static const char *find(const char *text, char c) { while (*text) { if (*text == c) return text; text++; } return 0; }

static char last_id[17];
static char last_settings_id[17];
static char stream_url[241];
static int beep_left, beep_phase, beep_half_period;
static int clip_mode;
static int music_active, http_fallback;
static uint32_t loading_started;
static int loading_seen;
static int action_state, action_kind;
static const char *action_result;

static void draw_loading(struct ProjectFrame *f) {
  static const int8_t x[8] = {0, 7, 10, 7, 0, -7, -10, -7};
  static const int8_t y[8] = {-10, -7, 0, 7, 10, 7, 0, -7};
  const int cx = f->width / 2, cy = f->height / 2;
  const int head = (f->frame_ms / 95) % 8;
  f->ring(f->canvas, cx, cy, 15, 1, f->muted);
  for (int i = 0; i < 8; ++i) {
    const int distance = (head + 8 - i) % 8;
    f->circle(f->canvas, cx + x[i], cy + y[i], distance < 2 ? 3 : 2,
              distance < 3 ? f->accent : f->muted);
  }
}

static void stop_audio(struct ProjectFrame *f) {
  const int was_beeping = beep_left > 0;
  beep_left = 0;
  clip_mode = 0;
  music_active = 0;
  http_fallback = 0;
  f->radio_stop();
  if (was_beeping) f->speaker_enable(0);
}

static void play_beep(struct ProjectFrame *f) {
  if (beep_left <= 0) return;
  int16_t samples[480];
  for (int chunk = 0; chunk < 6 && beep_left > 0; chunk++) {
    int frames = beep_left < 240 ? beep_left : 240;
    for (int i = 0; i < frames; i++) {
      int16_t sample = beep_phase < beep_half_period ? 6500 : -6500;
      samples[i * 2] = sample;
      samples[i * 2 + 1] = sample;
      if (++beep_phase >= beep_half_period * 2) beep_phase = 0;
    }
    int written = f->speaker_write(samples, frames);
    if (written <= 0) { action_state = 2; action_result = "Speaker write failed"; beep_left = 0; break; }
    beep_left -= written;
    if (written < frames) break;
  }
  if (beep_left <= 0) f->speaker_enable(0);
}

int app_main(int argc, char **argv) {
  if (argc != 1) return -1;
  struct ProjectFrame *f = (struct ProjectFrame *)argv[0];
  if (f->abi != DISPLAY_PROJECT_ABI) return -1;

  // data[0] is an action id; data[1] is an audio action or "settings|". A changed
  // settings revision may be appended as "|@s<revision>" to an existing audio action.
  // Repeated sync replies must never restart a stream or tone.
  if (f->data[0] && f->data[0][0] && f->data[1]) {
    const char *first = find(f->data[1], '|');
    const char *extra = first ? find(first + 1, '|') : 0;
    if (extra && starts(extra + 1, "@s") && text_len(extra + 1) <= 16 &&
        !same(extra + 1, last_settings_id)) {
      copy(last_settings_id, extra + 1);
      loading_started = f->frame_ms;
      loading_seen = 1;
    }
  }
  if (f->data[0] && f->data[0][0] && !same(f->data[0], last_id)) {
    const char *payload = f->data[1];
    if (text_len(f->data[0]) <= 16 && payload) {
      copy(last_id, f->data[0]);
      action_state = 0; action_kind = 0; action_result = 0;
      loading_started = f->frame_ms;
      loading_seen = 1;
      if (starts(payload, "beep|")) {
        action_kind = 1;
        const int hz = number(payload + 5);
        const char *comma = find(payload + 5, ',');
        const int ms = comma ? number(comma + 1) : 0;
        if (hz >= 100 && hz <= 4000 && ms >= 20 && ms <= 3000) {
          stop_audio(f);
          beep_half_period = (f->audio_sample_rate > 0 ? f->audio_sample_rate : 24000) / (hz * 2);
          if (beep_half_period < 1) beep_half_period = 1;
          beep_left = (f->audio_sample_rate > 0 ? f->audio_sample_rate : 24000) * ms / 1000;
          beep_phase = 0;
          if (!((f->audio_begin() & 1) && f->speaker_enable(1) == 0)) { beep_left = 0; action_state = 2; action_result = "Speaker unavailable"; }
        } else { action_state = 2; action_result = "Invalid beep"; }
      } else if (starts(payload, "music|") || starts(payload, "clip|")) {
        action_kind = 2;
        const int clip = starts(payload, "clip|");
        const char *url = payload + (clip ? 5 : 6);
        char candidate[241];
        if (copy_url(candidate, url) > 0 &&
            (starts(candidate, "https://") || starts(candidate, "http://"))) {
          stop_audio(f);
          copy(stream_url, candidate);
          clip_mode = clip;
          music_active = 1;
          if (f->radio_start(stream_url) != 0) { clip_mode = 0; music_active = 0; action_state = 2; action_result = "Could not start stream"; }
        } else { action_state = 2; action_result = "Invalid music URL"; }
      } else if (starts(payload, "pause|") || starts(payload, "stop|")) {
        stop_audio(f);
        action_state = 1; action_result = "Audio stopped";
      } else if (same(payload, "settings|")) {
        // The settings arrived in this same sync response; keep the spinner briefly visible.
        action_state = 1; action_result = "Settings applied";
      } else { action_state = 2; action_result = "Unknown action"; }
    }
  }
  play_beep(f);
  char status[40];
  int stream_state = music_active ? f->radio_status(status, sizeof(status), 0) : 0;
  // The public Navahang Shoutcast stream also serves MP3 over HTTP. If an ESP32's TLS
  // handshake fails, retry this one known public stream without TLS instead of staying silent.
  if (music_active && !clip_mode && !http_fallback &&
      same(stream_url, "https://navairan.com/;stream.nsv")) {
    if (stream_state == 4 && (same(status, "Connection failed") || same(status, "Station unavailable") ||
                              same(status, "Stream unavailable"))) {
      http_fallback = 1;
      copy(stream_url, "http://navairan.com/;stream.nsv");
      if (f->radio_start(stream_url) != 0) music_active = 0;
      else stream_state = 1;
    }
  }
  if (clip_mode && stream_state == 4) stop_audio(f);
  if (!action_state && action_kind == 1 && beep_left <= 0) { action_state = 1; action_result = "Beep played"; }
  if (!action_state && action_kind == 2 && stream_state == 3) { action_state = 1; action_result = "Music is playing"; }
  if (!action_state && action_kind == 2 && (!music_active || stream_state == 4)) { action_state = 2; action_result = "Music stream failed"; }
  f->control_action_state = action_state;
  f->control_action_result = action_result;
  if (beep_left > 0 || (loading_seen && f->frame_ms - loading_started < 800) ||
      (music_active && (stream_state == 1 || stream_state == 2))) draw_loading(f);
  return 0;
}
