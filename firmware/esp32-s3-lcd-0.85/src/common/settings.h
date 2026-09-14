#pragma once

#include <Preferences.h>

#include <string>

// NVS-backed key/value settings, one namespace per instance.
class Settings {
 public:
  Settings(const char* ns, bool read_write = false) : read_write_(read_write) {
    prefs_.begin(ns, !read_write);
  }
  ~Settings() { prefs_.end(); }

  int GetInt(const char* key, int default_value = 0) {
    return prefs_.isKey(key) ? prefs_.getInt(key, default_value) : default_value;
  }
  void SetInt(const char* key, int value) {
    if (read_write_) prefs_.putInt(key, value);
  }

  bool GetBool(const char* key, bool default_value = false) {
    return prefs_.isKey(key) ? prefs_.getBool(key, default_value) : default_value;
  }
  void SetBool(const char* key, bool value) {
    if (read_write_) prefs_.putBool(key, value);
  }

  std::string GetString(const char* key, const char* default_value = "") {
    return prefs_.isKey(key) ? std::string(prefs_.getString(key, default_value).c_str())
                             : std::string(default_value);
  }
  void SetString(const char* key, const std::string& value) {
    if (read_write_) prefs_.putString(key, value.c_str());
  }

 private:
  Preferences prefs_;
  bool read_write_;
};
