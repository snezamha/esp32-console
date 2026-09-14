#pragma once

#include <cstdint>

class Canvas;

// 9×9 line icons for menu rows. (x, y) is the top-left corner.
namespace Icons {

void Display(Canvas& c, int x, int y, uint16_t color);
void Sound(Canvas& c, int x, int y, uint16_t color);
void Wifi(Canvas& c, int x, int y, uint16_t color);
void Gear(Canvas& c, int x, int y, uint16_t color);
void Check(Canvas& c, int x, int y, uint16_t color);
void Cloud(Canvas& c, int x, int y, uint16_t color);

}  // namespace Icons
