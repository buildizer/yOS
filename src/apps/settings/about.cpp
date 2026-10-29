// Settings › General › About
#include <esp_chip_info.h>
#include <WiFi.h>
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/ui.h"

#include "version.h"
#define YOS_VERSION YOS_FIRMWARE_VERSION

namespace settings {

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
      snprintf(buf, sizeof(buf), "%lu of %lu KB free", (unsigned long)(ESP.getFreeHeap() >> 10),
               (unsigned long)(ESP.getHeapSize() >> 10));
      ui::row(g, nullptr, p.gray, "RAM", buf);
      snprintf(buf, sizeof(buf), "%lu of %lu KB free", (unsigned long)(ESP.getFreePsram() >> 10),
               (unsigned long)(ESP.getPsramSize() >> 10));
      ui::row(g, nullptr, p.gray, "PSRAM", buf);
      g = ui::group(c);
      uint32_t up = millis() / 1000;
      snprintf(buf, sizeof(buf), "%lud %luh %lum", (unsigned long)(up / 86400), (unsigned long)(up % 86400 / 3600),
               (unsigned long)(up % 3600 / 60));
      ui::row(g, nullptr, p.gray, "Uptime", buf);
      ui::row(g, nullptr, p.gray, "Web Dashboard", WiFi.localIP().toString().c_str());

    });
}
}
