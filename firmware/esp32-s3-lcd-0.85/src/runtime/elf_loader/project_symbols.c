#include "loader_config.h"
#include "esp_elf.h"
// The project ABI supplies all functions as pointers; modules have no unresolved imports.
uintptr_t elf_find_sym_default(const char *name) { return esp_elf_find_symbol(name); }
