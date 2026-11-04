#pragma once

// HC_PSRAM_BSS puts a static object into PSRAM instead of the internal .bss
// (docs/architecture.md section 11), in code that the PC builds compile too; firmware-only code
// writes EXT_RAM_BSS_ATTR itself. On the PC it is nothing.
//
// The section is never loaded from the image: ESP-IDF zeroes it before the C++ constructors run.
// Only an object that starts as zeros, or whose constructor runs at startup, may live there. One
// that the compiler could fill in at build time with non-zero values would start as zeros.

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HC_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define HC_PSRAM_BSS
#endif
