#pragma once

#include <FS.h>
#include <SD_MMC.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdmmc_types.h>
#include <esp_heap_caps.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <esp_vfs_fat.h>
#include <ff.h>
#include <sdmmc_cmd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

// TF card on the SDMMC host (4-bit), mounted at /sdcard.
class SdCard {
 public:
  // Why the last mount attempt failed, so a card that is present is never reported as missing.
  enum class Problem { None, NoCard, NoFilesystem, ExFat, Ntfs, Gpt, ReadError };

  SdCard(gpio_num_t clk, gpio_num_t cmd, gpio_num_t d0, gpio_num_t d1, gpio_num_t d2,
         gpio_num_t d3)
      : clk_(clk), cmd_(cmd), d0_(d0), d1_(d1), d2_(d2), d3_(d3) {}

  bool Mount() {
    if (mounted_) return true;
    if (!SD_MMC.setPins(clk_, cmd_, d0_, d1_, d2_, d3_)) return false;
    EnablePullups();
    // Arduino defaults to the SDMMC high-speed clock (40 MHz), while Waveshare's ESP-IDF BSP
    // initializes this board at the standard 20 MHz rate. Some cards fail negotiation at 40 MHz.
    mounted_ = SD_MMC.begin(kMountPoint, false /* 4-bit */, false /* never format implicitly */,
                            SDMMC_FREQ_DEFAULT, kMaxOpenFiles);
    // A marginal D1/D2/D3 line should not make the card unusable. The file manager works in
    // 1-bit mode too, so retry at a conservative clock before reporting that no card is present.
    if (!mounted_) {
      SD_MMC.setPins(clk_, cmd_, d0_);
      mounted_ = SD_MMC.begin(kMountPoint, true /* 1-bit */, false, kFallbackFrequencyKhz,
                              kMaxOpenFiles);
    }
    if (mounted_ && SD_MMC.cardType() == CARD_NONE) {
      SD_MMC.end();
      mounted_ = false;
    }
    problem_ = mounted_ ? Problem::None : Diagnose();
    return mounted_;
  }

  void Unmount() {
    if (!mounted_) return;
    SD_MMC.end();
    mounted_ = false;
  }

  // Erases the card with a fresh FAT file system, then mounts it. Formats through FatFs directly:
  // the SD_MMC/VFS path picks one-sector clusters and a PSRAM work buffer, which writes a large
  // card's FAT one sector at a time and takes hours on a 128 GB card.
  bool Format() {
    Unmount();
    const bool formatted = FormatWidth(false) || FormatWidth(true);
    if (!formatted) {
      problem_ = Diagnose();
      return false;
    }
    return Mount();
  }

  bool mounted() const { return mounted_; }
  Problem problem() const { return problem_; }

  // Fits the 128 px display and the hardware check.
  const char* ProblemLabel() const {
    switch (problem_) {
      case Problem::None: return mounted_ ? "Ready" : "Not mounted";
      case Problem::NoCard: return "No card";
      case Problem::NoFilesystem: return "No FAT fs";
      case Problem::ExFat: return "exFAT";
      case Problem::Ntfs: return "NTFS";
      case Problem::Gpt: return "GPT disk";
      case Problem::ReadError: return "Read error";
    }
    return "Error";
  }

  const char* ProblemMessage() const {
    switch (problem_) {
      case Problem::None: return mounted_ ? "SD card mounted" : "SD card is not mounted";
      case Problem::NoCard: return "No SD card detected. Insert a card and try again.";
      case Problem::NoFilesystem:
        return "SD card found, but it has no readable FAT file system (unformatted or damaged). Format it as FAT32.";
      case Problem::ExFat: return "SD card found, but it is formatted as exFAT, which is not supported. Format it as FAT32.";
      case Problem::Ntfs: return "SD card found, but it is formatted as NTFS, which is not supported. Format it as FAT32.";
      case Problem::Gpt:
        return "SD card found, but it uses a GPT partition table, which is not supported. Format it as FAT32 with an MBR partition table.";
      case Problem::ReadError: return "SD card found, but it could not be read. Reinsert it or try another card.";
    }
    return "Could not mount the SD card";
  }
  uint64_t total_bytes() const { return mounted_ ? SD_MMC.totalBytes() : 0; }
  uint64_t used_bytes() const { return mounted_ ? SD_MMC.usedBytes() : 0; }
  fs::FS& fs() { return SD_MMC; }

  static constexpr const char* kMountPoint = "/sdcard";
  static constexpr uint8_t kMaxOpenFiles = 8;
  static constexpr int kFallbackFrequencyKhz = 10000;
  static constexpr size_t kFormatBufferBytes = 16 * 1024;

 private:
  // Runs after SD_MMC.begin() failed (which releases the host): probes the card directly and reads
  // its first sectors to tell a missing card from one whose file system FatFs cannot mount.
  Problem Diagnose() {
    Problem problem = Problem::NoCard;
    auto* sector = static_cast<uint8_t*>(heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (sector) {
      WithCard(true, [&](sdmmc_card_t* card) { problem = Classify(card, sector); });
    }
    free(sector);
    return problem;
  }

  // Initializes the host and card without the SD_MMC driver, runs `use`, and releases the host.
  template <typename Use>
  bool WithCard(bool one_bit, Use use) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = clk_;
    slot.cmd = cmd_;
    slot.d0 = d0_;
    if (one_bit) {
      host.flags = SDMMC_HOST_FLAG_1BIT;
      host.max_freq_khz = kFallbackFrequencyKhz;
      slot.width = 1;
    } else {
      host.flags = SDMMC_HOST_FLAG_4BIT;
      slot.width = 4;
      slot.d1 = d1_;
      slot.d2 = d2_;
      slot.d3 = d3_;
    }
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (sdmmc_host_init() != ESP_OK) return false;
    bool ready = false;
    auto* card = static_cast<sdmmc_card_t*>(calloc(1, sizeof(sdmmc_card_t)));
    if (card && sdmmc_host_init_slot(host.slot, &slot) == ESP_OK &&
        sdmmc_card_init(&host, card) == ESP_OK) {
      ready = true;
      use(card);
    }
    free(card);
    sdmmc_host_deinit();
    return ready;
  }

  bool FormatWidth(bool one_bit) {
    bool ok = false;
    WithCard(one_bit, [&](sdmmc_card_t* card) {
      BYTE pdrv = 0xFF;
      if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) return;
      // FatFs writes the FATs through this buffer; DMA memory lets it send many sectors per write.
      size_t size = kFormatBufferBytes;
      auto* work = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      if (!work) {
        size = FF_MAX_SS;
        work = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      }
      if (!work) return;
      ff_diskio_register_sdmmc(pdrv, card);
      const char drive[3] = {static_cast<char>('0' + pdrv), ':', '\0'};
      // FatFs chooses the cluster size for the card (32 KB on large cards); two FATs, MBR layout.
      const MKFS_PARM options = {FM_FAT | FM_FAT32, 2, 0, 0, 0};
      ok = f_mkfs(drive, &options, work, size) == FR_OK;
      ff_diskio_unregister(pdrv);
      free(work);
    });
    return ok;
  }

  static Problem Classify(sdmmc_card_t* card, uint8_t* sector) {
    if (sdmmc_read_sectors(card, sector, 0, 1) != ESP_OK) return Problem::ReadError;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return Problem::NoFilesystem;
    // The first sector may be a volume boot record instead of a partition table.
    if (const Problem vbr = VolumeProblem(sector); vbr != Problem::NoFilesystem) return vbr;
    const uint8_t type = sector[446 + 4];
    if (type == 0xEE) return Problem::Gpt;
    const uint32_t lba = sector[454] | (sector[455] << 8) | (sector[456] << 16) |
                         (static_cast<uint32_t>(sector[457]) << 24);
    if (type == 0 || lba == 0) return Problem::NoFilesystem;
    if (sdmmc_read_sectors(card, sector, lba, 1) != ESP_OK) return Problem::ReadError;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return Problem::NoFilesystem;
    return VolumeProblem(sector);
  }

  // A FAT volume FatFs still refused is damaged, so it is reported like a missing file system.
  static Problem VolumeProblem(const uint8_t* vbr) {
    if (memcmp(vbr + 3, "EXFAT   ", 8) == 0) return Problem::ExFat;
    if (memcmp(vbr + 3, "NTFS    ", 8) == 0) return Problem::Ntfs;
    return Problem::NoFilesystem;
  }

  void EnablePullups() const {
    gpio_pullup_en(cmd_);
    gpio_pullup_en(d0_);
    gpio_pullup_en(d1_);
    gpio_pullup_en(d2_);
    gpio_pullup_en(d3_);
  }

  gpio_num_t clk_, cmd_, d0_, d1_, d2_, d3_;
  bool mounted_ = false;
  Problem problem_ = Problem::None;
};
