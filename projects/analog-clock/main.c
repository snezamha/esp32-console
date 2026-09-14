#include "project_api.h"
static void hand(struct ProjectFrame *f, int cx, int cy, float units, float length, int thickness, uint16_t color) {
  const float angle = units * .104719755f - 1.570796327f;
  f->line(f->canvas, cx, cy, cx + f->cosine(angle) * length, cy + f->sine(angle) * length, thickness, color);
}
int app_main(int argc, char **argv) {
  if (argc != 1) return -1;
  struct ProjectFrame *f = (struct ProjectFrame *)argv[0];
  if (f->abi != DISPLAY_PROJECT_ABI) return -1;
  if (!f->time_valid) { f->label(f->canvas, f->height / 2, "Syncing time...", f->muted, 1); return 0; }
  const int cx = f->width / 2, cy = f->height / 2;
  const int r = (f->width < f->height ? f->width : f->height) / 2 - 7;
  f->ring(f->canvas, cx, cy, r, 1, f->muted);
  for (int i = 0; i < 12; ++i) {
    const float a = i * .523598776f - 1.570796327f;
    f->line(f->canvas, cx + f->cosine(a) * (r - 4), cy + f->sine(a) * (r - 4), cx + f->cosine(a) * (r - 1), cy + f->sine(a) * (r - 1), 1, f->text);
  }
  hand(f, cx, cy, (f->hour % 12) * 5 + f->minute * .083333333f, r * .5f, 3, f->text);
  hand(f, cx, cy, f->minute + f->second * .016666667f, r * .75f, 2, f->text);
  hand(f, cx, cy, f->second, r * .82f, 1, f->accent);
  f->circle(f->canvas, cx, cy, 3, f->accent);
  return 0;
}
