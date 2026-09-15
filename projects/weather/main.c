#include "project_api.h"
static void detail(const char *text, int selected, char *out, int size) {
  int part = 0, used = 0;
  while (*text && part < selected) if (*text++ == '~') part++;
  while (*text && *text != '~' && used + 1 < size) out[used++] = *text++;
  out[used] = 0;
}
static int details(const char *text) { int count = *text ? 1 : 0; while (*text) if (*text++ == '~') count++; return count; }
int app_main(int argc, char **argv) {
  if (argc != 1) return -1;
  struct ProjectFrame *f = (struct ProjectFrame *)argv[0];
  if (f->abi != DISPLAY_PROJECT_ABI) return -1;
  f->label(f->canvas, 16, "Weather", f->muted, 1);
  if (!f->data[0][0]) {
    f->label(f->canvas, f->height / 2, "Loading weather", f->accent, 1);
  } else if (!f->data[1][0]) {
    f->label(f->canvas, f->height / 2, "Unavailable", f->muted, 1);
  } else {
    char line[48]; const int count = details(f->data[1]);
    detail(f->data[1], count ? (f->frame_ms / 3500) % count : 0, line, sizeof(line));
    f->label(f->canvas, f->height / 2 - 12, f->data[0], f->text, 2);
    f->label(f->canvas, f->height / 2 + 17, line, f->accent, 1);
    if (f->data_age_ms > 1200000) f->label(f->canvas, f->height - 12, "Last known", f->muted, 1);
  }
  return 0;
}
