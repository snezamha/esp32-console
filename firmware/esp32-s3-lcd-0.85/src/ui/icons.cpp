#include "icons.h"

#include "../display/canvas.h"

namespace Icons {

void Display(Canvas& c, int x, int y, uint16_t color) {
  c.Rect(x, y + 1, 9, 6, color);
  c.FillRect(x + 4, y + 7, 1, 1, color);
  c.FillRect(x + 2, y + 8, 5, 1, color);
}

void Sound(Canvas& c, int x, int y, uint16_t color) {
  c.FillRect(x, y + 3, 2, 3, color);
  c.FillTriangle(x + 2, y + 4, x + 5, y + 1, x + 5, y + 7, color);
  c.Arc(x + 5, y + 4, 2, 300, 60, 1, color);
  c.Arc(x + 5, y + 4, 4, 300, 60, 1, color);
}

void Wifi(Canvas& c, int x, int y, uint16_t color) {
  const int cx = x + 4, cy = y + 8;
  c.FillRect(cx, cy - 1, 1, 1, color);
  c.Arc(cx, cy, 3, 225, 315, 1, color);
  c.Arc(cx, cy, 6, 225, 315, 1, color);
}

void Gear(Canvas& c, int x, int y, uint16_t color) {
  const int cx = x + 4, cy = y + 4;
  c.Ring(cx, cy, 2, 2, color);
  c.FillRect(cx, y, 1, 2, color);
  c.FillRect(cx, y + 7, 1, 2, color);
  c.FillRect(x, cy, 2, 1, color);
  c.FillRect(x + 7, cy, 2, 1, color);
  c.FillRect(x + 1, y + 1, 1, 1, color);
  c.FillRect(x + 7, y + 1, 1, 1, color);
  c.FillRect(x + 1, y + 7, 1, 1, color);
  c.FillRect(x + 7, y + 7, 1, 1, color);
}

void Check(Canvas& c, int x, int y, uint16_t color) {
  c.Line(x, y + 5, x + 3, y + 8, 1, color);
  c.Line(x + 3, y + 8, x + 8, y + 1, 1, color);
  c.Line(x + 1, y + 5, x + 3, y + 7, 1, color);
  c.Line(x + 3, y + 7, x + 8, y + 2, 1, color);
}

// Cloud: the remote console.
void Cloud(Canvas& c, int x, int y, uint16_t color) {
  c.Arc(x + 3, y + 5, 2, 90, 270, 1, color);
  c.Arc(x + 5, y + 4, 3, 180, 360, 1, color);
  c.Arc(x + 7, y + 6, 1, 270, 90, 1, color);
  c.FillRect(x + 3, y + 7, 5, 1, color);
}

}  // namespace Icons
