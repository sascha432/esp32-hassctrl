#pragma once

// LVGL's memory (objects, styles, texts, timers) in PSRAM (docs/architecture.md section 11).
//
// LVGL allocates through these instead of malloc (main/CMakeLists.txt sets LV_MEM_CUSTOM_ALLOC and
// friends on the LVGL library, sdkconfig.main includes this header). Plain malloc puts everything
// below 16 KB into internal RAM first (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), which is nearly every
// allocation of LVGL: the UI filled the internal RAM that task stacks, DMA and Wi-Fi need. There is
// no pool of a fixed size: the UI can use the whole PSRAM, and internal RAM only when the PSRAM is
// full. The draw buffers are not allocated here; esp_lvgl_port keeps them in internal DMA RAM.

#include <stddef.h>

#include "esp_heap_caps.h"

#define HC_LV_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define HC_LV_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static inline void *hc_lv_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, HC_LV_PSRAM, HC_LV_INTERNAL);
}

static inline void *hc_lv_realloc(void *pointer, size_t size)
{
    return heap_caps_realloc_prefer(pointer, size, 2, HC_LV_PSRAM, HC_LV_INTERNAL);
}

static inline void hc_lv_free(void *pointer)
{
    heap_caps_free(pointer);
}
