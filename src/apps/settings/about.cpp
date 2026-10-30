// Settings › General › About
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/ui.h"

#include "version.h"
#define YOS_VERSION YOS_FIRMWARE_VERSION

namespace settings {

// Memory rows, refreshed every 2 s while About is open. "Lowest since boot" is the figure to watch:
// Wi-Fi and TLS fail once internal RAM runs out, even while the current free amount looks fine.
static lv_obj_t *memRam, *memLow, *memBlock, *memPsram;

static void updateMemory() {
  if (!memRam) return;
  lv_label_set_text_fmt(memRam, "%u of %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
                        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) >> 10));
  lv_label_set_text_fmt(memLow, "%u KB", (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10));
  lv_label_set_text_fmt(memBlock, "%u KB", (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10));
  lv_label_set_text_fmt(memPsram, "%u of %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
                        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 10));
}

void aboutPage(lv_obj_t *appRoot) {
  ui::push(appRoot, "About", [appRoot](lv_obj_t *c) {
      const Palette &p = theme::p();
      char buf[40];
      softwareUpdateSection(c, appRoot);
      lv_obj_t *g = ui::group(c);
      ui::row(g, nullptr, p.gray, "Name", "yOS");
      ui::row(g, nullptr, p.gray, "Version", YOS_VERSION);
      ui::row(g, nullptr, p.gray, "Device", "Guition ESP32-4848S040");
      esp_chip_info_t chip;
      esp_chip_info(&chip);
      snprintf(buf, sizeof(buf), "ESP32-S3, %d cores, %lu MHz", chip.cores, (unsigned long)ESP.getCpuFreqMHz());
      ui::row(g, nullptr, p.gray, "Chip", buf);
      g = ui::group(c, "Memory");
      snprintf(buf, sizeof(buf), "%lu MB", (unsigned long)(ESP.getFlashChipSize() >> 20));
      ui::row(g, nullptr, p.gray, "Flash", buf);
      memRam = ui::row(g, nullptr, p.gray, "Internal RAM", "").value;
      memLow = ui::row(g, nullptr, p.gray, "Lowest Since Boot", "").value;
      memBlock = ui::row(g, nullptr, p.gray, "Largest Block", "").value;
      memPsram = ui::row(g, nullptr, p.gray, "PSRAM", "").value;
      updateMemory();
      lv_timer_t *tm = lv_timer_create([](lv_timer_t *) { updateMemory(); }, 2000, nullptr);
      // The rows all go together when the page closes; stop the timer with the first.
      lv_obj_add_event_cb(memRam, [](lv_event_t *e) {
        lv_timer_del((lv_timer_t *)lv_event_get_user_data(e));
        memRam = memLow = memBlock = memPsram = nullptr;
      }, LV_EVENT_DELETE, tm);
      g = ui::group(c);
      uint32_t up = millis() / 1000;
      snprintf(buf, sizeof(buf), "%lud %luh %lum", (unsigned long)(up / 86400), (unsigned long)(up % 86400 / 3600),
               (unsigned long)(up % 3600 / 60));
      ui::row(g, nullptr, p.gray, "Uptime", buf);
      ui::row(g, nullptr, p.gray, "Web Dashboard", WiFi.localIP().toString().c_str());

    });
}
}
