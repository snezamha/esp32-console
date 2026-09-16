#pragma once

#include <cstddef>
#include <cstring>

// Named time zones with POSIX TZ rules, so daylight saving changes happen automatically.
struct TimeZoneInfo {
  const char* id;     // IANA name, stored in settings
  const char* label;  // Short name for the menu (≤ 9 characters)
  const char* posix;  // TZ rule for setenv("TZ")
};

inline constexpr TimeZoneInfo kTimeZones[] = {
    {"Europe/Berlin", "Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London", "London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris", "Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Istanbul", "Istanbul", "<+03>-3"},
    {"Europe/Moscow", "Moscow", "MSK-3"},
    {"Asia/Tehran", "Tehran", "<+0330>-3:30"},
    {"Asia/Dubai", "Dubai", "<+04>-4"},
    {"Asia/Kabul", "Kabul", "<+0430>-4:30"},
    {"Asia/Karachi", "Karachi", "PKT-5"},
    {"Asia/Kolkata", "Delhi", "IST-5:30"},
    {"Asia/Shanghai", "Shanghai", "CST-8"},
    {"Asia/Tokyo", "Tokyo", "JST-9"},
    {"Australia/Sydney", "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"UTC", "UTC", "UTC0"},
    {"America/Sao_Paulo", "Sao Paulo", "<-03>3"},
    {"America/New_York", "New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "LA", "PST8PDT,M3.2.0,M11.1.0"},
};

inline constexpr size_t kTimeZoneCount = sizeof(kTimeZones) / sizeof(kTimeZones[0]);

// Falls back to the first entry for unknown ids.
inline const TimeZoneInfo& FindTimeZone(const char* id) {
  for (const auto& tz : kTimeZones) {
    if (strcmp(tz.id, id) == 0) return tz;
  }
  return kTimeZones[0];
}
