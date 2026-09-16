#include "heap_guard.h"

#if CONSOLE_HEAP_GUARD

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

namespace {

std::atomic<const char*> g_phase{"boot"};
std::atomic<bool> g_failed{false};

void Report(const char* where) {
  // heap_caps_check_integrity_all() has already printed the offending block address over the
  // console; this line says which activity was in flight when the damage became visible.
  Serial.printf("{\"heapguard\":\"corrupt\",\"where\":\"%s\",\"phase\":\"%s\",\"heap\":%u,\"task\":\"%s\"}\n",
                where, g_phase.load(),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                pcTaskGetName(nullptr));
  Serial.flush();
  delay(50);
  abort();
}

void WatchTask(void* arg) {
  const uint32_t interval_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  for (;;) {
    if (!heap_caps_check_integrity_all(true) && !g_failed.exchange(true)) Report("watchdog");
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
  }
}

}  // namespace

namespace HeapGuard {

void Begin(uint32_t interval_ms) {
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreatePinnedToCore(WatchTask, "heap_guard", 3072,
                          reinterpret_cast<void*>(static_cast<uintptr_t>(interval_ms)), 1, nullptr, 1);
}

void Phase(const char* tag) { g_phase.store(tag ? tag : "?"); }

bool Check(const char* where) {
  if (heap_caps_check_integrity_all(true)) return true;
  if (!g_failed.exchange(true)) Report(where);
  return false;
}

}  // namespace HeapGuard

#endif  // CONSOLE_HEAP_GUARD
