#pragma once

#include <FS.h>
#include <SD_MMC.h>
#include <driver/gpio.h>
#include <esp_vfs_fat.h>

#include <cstdint>

// TF card on the SDMMC host (4-bit), mounted at /sdcard.
class SdCard {
 public:
  SdCard(gpio_num_t clk, gpio_num_t cmd, gpio_num_t d0, gpio_num_t d1, gpio_num_t d2,
         gpio_num_t d3)
      : clk_(clk), cmd_(cmd), d0_(d0), d1_(d1), d2_(d2), d3_(d3) {}

  bool Mount() {
    if (mounted_) return true;
    if (!SD_MMC.setPins(clk_, cmd_, d0_, d1_, d2_, d3_)) return false;
    mounted_ = SD_MMC.begin(kMountPoint, false /* 4-bit */);
    return mounted_;
  }

  void Unmount() {
    if (!mounted_) return;
    SD_MMC.end();
    mounted_ = false;
  }

  // Erases the card with a fresh FAT file system. An unreadable card is formatted while mounting.
  bool Format() {
    if (!Mount()) {
      if (!SD_MMC.setPins(clk_, cmd_, d0_, d1_, d2_, d3_)) return false;
      mounted_ = SD_MMC.begin(kMountPoint, false, true /* format_if_mount_failed */);
      if (!mounted_) return false;
    }
    esp_vfs_fat_mount_config_t config{};
    config.max_files = 5;
    config.allocation_unit_size = 16 * 1024;
    return esp_vfs_fat_sdcard_format_cfg(kMountPoint, CardAccess::Card(SD_MMC), &config) == ESP_OK;
  }

  bool mounted() const { return mounted_; }
  uint64_t total_bytes() const { return mounted_ ? SD_MMC.totalBytes() : 0; }
  uint64_t used_bytes() const { return mounted_ ? SD_MMC.usedBytes() : 0; }
  fs::FS& fs() { return SD_MMC; }

  static constexpr const char* kMountPoint = "/sdcard";

 private:
  // SDMMCFS keeps its card handle protected; formatting needs it. Adds no members.
  struct CardAccess : fs::SDMMCFS {
    static sdmmc_card_t* Card(fs::SDMMCFS& fs) { return static_cast<CardAccess&>(fs)._card; }
  };
  gpio_num_t clk_, cmd_, d0_, d1_, d2_, d3_;
  bool mounted_ = false;
};
