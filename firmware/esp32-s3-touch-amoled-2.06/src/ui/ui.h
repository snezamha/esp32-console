#pragma once

#include <cstdint>
#include <string>

// Minimal touch UI: board name centered, battery top right. No pages, no swipe.
void ui_init();
// Pumps LVGL; call every loop iteration while the screen is on.
void ui_task();
void ui_wake_redraw();

void ui_notify(const std::string& text, int duration_ms = 3000);
