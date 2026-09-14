Espressif esp-iot-solution ELF loader, pinned commit `6958385313b0e4fc1f3de259b1d677fca7d7d236` (Apache-2.0).

https://github.com/espressif/esp-iot-solution/tree/6958385313b0e4fc1f3de259b1d677fca7d7d236/components/elf_loader

Arduino adaptation: explicit loader_config.h, flattened arch source, ABI-only symbol resolver. Project modules receive host functions through a versioned API; no libc or IDF symbols are exported.

Integration fixes: relative private-header includes, null released allocation pointers on load failures, propagate unsupported relocation errors.

Execution on Arduino ESP32-S3: code uses internal MALLOC_CAP_EXEC IRAM and data uses internal 8-bit RAM. No PSRAM instruction alias or cache flush is needed. Executable buffers are rounded to 4 bytes and populated through aligned word stores, including a padded final word, because byte stores into IRAM can raise LoadStoreError.

The .text virtual range retains its original section size. Only its allocation is rounded to 4 bytes. Rounding the virtual range incorrectly remapped the first bytes of an immediately adjacent .rodata section into IRAM (e.g. Weather .text ends at 0x1fa), causing a first-frame LoadStoreError when reading labels.
