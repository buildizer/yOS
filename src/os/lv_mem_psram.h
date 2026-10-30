#pragma once
// LVGL's heap (LV_MEM_CUSTOM_* in lv_conf.h). LVGL is C, so these keep C linkage.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *yos_lv_malloc(size_t size);
void *yos_lv_realloc(void *ptr, size_t size);
void yos_lv_free(void *ptr);

#ifdef __cplusplus
}
#endif
