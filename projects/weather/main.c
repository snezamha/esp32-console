#include "project_api.h"
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
    f->label(f->canvas, f->height / 2 - 10, f->data[0], f->text, 2);
    f->label(f->canvas, f->height / 2 + 16, f->data[1], f->accent, 1);
    if (f->data_age_ms > 1200000) f->label(f->canvas, f->height - 12, "Last known", f->muted, 1);
  }
  return 0;
}
