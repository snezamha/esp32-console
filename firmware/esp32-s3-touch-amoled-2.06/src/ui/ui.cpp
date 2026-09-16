#include "ui.h"

#include <lvgl.h>

#include "../../version.h"
#include "../board/board.h"

namespace {

constexpr uint16_t DRAW_BUFFER_LINES = 60;
uint32_t screen_width, screen_height, buf_size;
lv_display_t* disp;
lv_color_t* disp_draw_buf;
lv_color_t* disp_draw_buf2;

lv_obj_t* battery_lbl;
lv_obj_t* toast_lbl;
lv_timer_t* toast_timer;

int battery_level = -1;
bool battery_charging = false;
bool charge_visible = true;

uint32_t millis_cb() { return millis(); }

void disp_flush(lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
  const uint16_t width = area->x2 - area->x1 + 1;
  const uint16_t height = area->y2 - area->y1 + 1;
  Board::GetInstance().GetDisplay()->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, width, height);
  lv_disp_flush_ready(d);
}

// Touch only needs to exist so the panel doesn't stall on an unread interrupt; this UI has no
// interactive elements, so taps are read and discarded.
void touchpad_read(lv_indev_t*, lv_indev_data_t* data) {
  auto touch = Board::GetInstance().GetTouch();
  const int fingers = touch->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  data->state = fingers > 0 ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  if (fingers > 0) {
    data->point.x = touch->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    data->point.y = touch->IIC_Read_Device_Value(Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
  }
}

void update_battery_label() {
  if (!battery_lbl) return;
  if (battery_level < 0) {
    lv_label_set_text(battery_lbl, "--");
  } else if (battery_charging && charge_visible) {
    lv_label_set_text_fmt(battery_lbl, "%d%% " LV_SYMBOL_CHARGE, battery_level);
  } else {
    lv_label_set_text_fmt(battery_lbl, "%d%%", battery_level);
  }
}

void refresh_battery() {
  bool discharging = false;
  Board::GetInstance().GetBatteryLevel(battery_level, battery_charging, discharging);
  update_battery_label();
}

void hide_toast(lv_timer_t*) {
  lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
  toast_timer = nullptr;
}

}  // namespace

void ui_notify(const std::string& text, int duration_ms) {
  if (!toast_lbl) return;
  lv_label_set_text(toast_lbl, text.c_str());
  lv_obj_clear_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);
  if (toast_timer) lv_timer_del(toast_timer);
  toast_timer = lv_timer_create(hide_toast, duration_ms, NULL);
  lv_timer_set_repeat_count(toast_timer, 1);
}

void ui_init() {
  lv_init();
  lv_tick_set_cb(millis_cb);

  auto display = Board::GetInstance().GetDisplay();
  screen_width = display->width();
  screen_height = display->height();
  buf_size = screen_width * DRAW_BUFFER_LINES;

  disp_draw_buf = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  assert(disp_draw_buf && disp_draw_buf2);

  disp = lv_display_create(screen_width, screen_height);
  lv_display_set_flush_cb(disp, disp_flush);
  lv_display_set_buffers(disp, disp_draw_buf, disp_draw_buf2, buf_size * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchpad_read);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, FIRMWARE_NAME);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -12);

  lv_obj_t* subtitle = lv_label_create(scr);
  lv_label_set_text(subtitle, Board::GetInstance().GetBoardType().c_str());
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0xA0A0A0), 0);
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
  lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 16);

  battery_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(battery_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(battery_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(battery_lbl, LV_ALIGN_TOP_RIGHT, -16, 16);

  toast_lbl = lv_label_create(scr);
  lv_obj_set_style_text_color(toast_lbl, lv_color_white(), 0);
  lv_obj_set_style_bg_color(toast_lbl, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_90, 0);
  lv_obj_set_style_pad_all(toast_lbl, 10, 0);
  lv_obj_set_style_radius(toast_lbl, 10, 0);
  lv_obj_set_style_text_font(toast_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_add_flag(toast_lbl, LV_OBJ_FLAG_HIDDEN);

  refresh_battery();
}

void ui_task() {
  static uint32_t last_ui_ms = 0;
  static uint32_t last_battery_ms = 0;

  if (millis() - last_ui_ms < 33) return;
  last_ui_ms = millis();

  if (millis() - last_battery_ms >= 2000) {
    last_battery_ms = millis();
    refresh_battery();
  }

  static uint32_t last_charge_ms = 0;
  if (battery_charging && millis() - last_charge_ms >= 500) {
    last_charge_ms = millis();
    charge_visible = !charge_visible;
    update_battery_label();
  }

  lv_timer_handler();
}

void ui_wake_redraw() {
  refresh_battery();
  lv_obj_invalidate(lv_screen_active());
}
