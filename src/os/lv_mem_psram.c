// LVGL objects, styles and labels live in PSRAM whatever their size; the 256-byte extmem threshold
// (main.cpp) would otherwise keep the many small ones in internal RAM, which Wi-Fi, TLS and DMA need.
// Internal RAM is only a fallback. The render buffers are allocated directly in hal.cpp and stay internal.
#include "os/lv_mem_psram.h"
#include <esp_heap_caps.h>

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void *yos_lv_malloc(size_t size) {
  void *p = heap_caps_malloc(size, PSRAM_CAPS);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

// LVGL frees itself on size 0, so size is never 0 here. A block that falls back to internal RAM
// moves to PSRAM on its next realloc.
void *yos_lv_realloc(void *ptr, size_t size) {
  void *p = heap_caps_realloc(ptr, size, PSRAM_CAPS);
  return p ? p : heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
}

void yos_lv_free(void *ptr) { heap_caps_free(ptr); }
