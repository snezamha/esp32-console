#pragma once
#define CONFIG_ELF_LOADER 1
#define CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR 1
#define ELF_LOADER_VER_MAJOR 1
#define ELF_LOADER_VER_MINOR 3
#define ELF_LOADER_VER_PATCH 3

// Small display modules execute from internal IRAM, without PSRAM cache aliases.
