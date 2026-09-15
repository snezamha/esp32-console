# راهنمای Project Builder — ESP32-S3-LCD-0.85

Project Builder فایل `.ino` را به یک ELF مستقل تبدیل می‌کند. این فایل جای firmware اصلی را
نمی‌گیرد؛ فقط در ناحیه‌ی Project اجرا می‌شود. در ابتدای کد فقط هدر عمومی Builder را include کنید:

```c
#include "project_builder.h"

void setup(ProjectBoard *board) {
  // یک بار بعد از load شدن پروژه
}

void loop(ProjectBoard *board) {
  // در هر frame نمایشگر؛ این تابع باید سریع return کند
}

PROJECT_SKETCH()
```

## نمایشگر

مختصات نسبت به فضای پروژه و اندازه‌ی آن در `board->width` و `board->height` است. رنگ‌ها RGB565 هستند.

```c
uint16_t blue = project_rgb(20, 80, 255);
project_clear(board, project_rgb(0, 0, 0));
project_text(board, 12, "Hello", board->text, 1); // متن وسط‌چین
board->line(board->canvas, 4, 30, 120, 30, 2, blue);
board->circle(board->canvas, 64, 64, 10, blue);
board->ring(board->canvas, 64, 64, 24, 3, blue);
board->rect(board->canvas, 10, 90, 40, 20, blue);
board->fill_rect(board->canvas, 60, 90, 40, 20, blue);
```

زمان در `hour/minute/second/year/month/day/weekday` و زمان اجرای برد در `frame_ms` قرار دارد.

## LED ring

LEDها از صفر تا ۷ شماره‌گذاری شده‌اند. تغییر رنگ با `led_show()` اعمال می‌شود. با
`led_release()` کنترل دوباره به تنظیمات Console برمی‌گردد.

```c
board->led_fill(0, 0, 0);
board->led_set(0, 255, 0, 0);
board->led_set(1, 0, 255, 0);
board->led_show();
// board->led_release();
```

## دکمه‌ها و باتری

```c
if (project_button_pressed(board, PROJECT_BUTTON_POWER)) { /* ... */ }
if (project_button_pressed(board, PROJECT_BUTTON_PLUS))  { /* ... */ }
if (project_button_pressed(board, PROJECT_BUTTON_MINUS)) { /* ... */ }

uint32_t plus_held = board->button_held_ms[1];
int battery = board->battery_percent;       // -1 یعنی ناموجود
int charging = board->battery_charging;
```

## SD card

فایل‌های قابل‌نوشتن هر پروژه داخل `/projects-data/<project-id>` sandbox می‌شوند. نام فایل نسبی،
حداکثر ۹۶ کاراکتر و حداکثر چهار بخش است. هر read/write حداکثر ۱۰۲۴ بایت پردازش می‌کند و هنگام
استفاده‌ی File Manager مقدار `-1` برمی‌گرداند.

```c
const char message[] = "hello";
board->storage_write("settings.txt", message, sizeof(message) - 1, 0); // overwrite
board->storage_write("events.log", message, sizeof(message) - 1, 1);  // append

char data[64];
int n = board->storage_read("settings.txt", 0, data, sizeof(data));
int size = board->storage_size("settings.txt");
board->storage_mkdir("logs");
board->storage_remove("old.txt");
```

فهرست ریشه‌ی فضای پروژه:

```c
char name[64]; int is_dir; uint32_t size;
for (int i = 0; board->storage_list(i, name, sizeof(name), &is_dir, &size) > 0; ++i) {
  // name, is_dir, size
}
```

توابع `asset_size/asset_read/media_info/media_draw` برای assetهای read-only یک package کاتالوگ هستند.

## میکروفون و اسپیکر

صوت signed 16-bit، stereo و interleaved است. نرخ نمونه در `board->audio_sample_rate` قرار دارد.
هر فراخوانی حداکثر ۲۴۰ frame و با timeout صفر انجام می‌شود؛ نتیجه تعداد frameهای واقعی است.

```c
int available = board->audio_begin();
int16_t samples[240 * 2];

if (available & PROJECT_AUDIO_MICROPHONES) {
  int frames = board->mic_read(samples, 240);
}

if (available & PROJECT_AUDIO_SPEAKER) {
  board->speaker_enable(1);
  board->speaker_write(samples, 240);
  // board->speaker_enable(0);
}
```

## Wi-Fi و HTTP

وضعیت اتصال در `wifi_state`، `wifi_ssid`، `wifi_ip` و `wifi_rssi` است. اسکن و HTTP asynchronous هستند.

```c
// یک بار شروع کنید
board->wifi_scan_start();

// frameهای بعدی: -1 یعنی اسکن هنوز ادامه دارد
int count = board->wifi_scan_count();
char ssid[40]; int rssi, channel, secure;
if (count > 0) board->wifi_scan_result(0, ssid, sizeof(ssid), &rssi, &channel, &secure);
```

```c
static int requested;
if (!requested && board->wifi_state == PROJECT_WIFI_CONNECTED) {
  requested = board->http_get("https://example.com/data.json") == 0;
}
char response[512]; int http_status;
int n = board->http_result(response, sizeof(response), &http_status);
// -2: در حال دریافت، -1: خطا، >=0: تعداد بایت کپی‌شده؛ پاسخ کل حداکثر 8KB است.
```

## BLE

اسکن BLE در پس‌زمینه انجام می‌شود تا نمایشگر و Console متوقف نشوند.

```c
board->ble_scan_start();
int count = board->ble_scan_count(); // -1: هنوز در حال اسکن
char address[20], name[40]; int rssi;
if (count > 0) board->ble_scan_result(0, address, sizeof(address), name, sizeof(name), &rssi);
```

## قواعد پایداری

- `loop()` باید سریع return کند؛ حلقه‌ی بی‌نهایت و `delay` طولانی نگذارید.
- عملیات اسکن و HTTP را فقط یک بار شروع و در frameهای بعد poll کنید.
- نتیجه‌ی منفی یعنی قابلیت آماده نیست، SD در حال استفاده است یا عملیات شکست خورده است.
- پروژه به Arduino Core، GPIO خام، رمزهای Wi-Fi یا فایل‌های پروژه‌های دیگر دسترسی مستقیم ندارد.
- firmware نسخه‌ی 1.1.9 یا جدیدتر و Project ABI 4 برای این API لازم است.

