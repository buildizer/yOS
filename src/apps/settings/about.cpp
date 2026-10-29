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

      // Wi-Fi updates need this password (tools/flash.sh asks for it once and keeps it in the Mac's Keychain).
      g = ui::group(c, "Software Update", "Needed to install updates over Wi-Fi. USB updates don't ask for it.");
      lv_obj_t *pwValue = ui::row(g, nullptr, p.gray, "Update Password", prefs::otaPassword().c_str(), [appRoot, c] {
        ui::textInput(appRoot, "Update Password", "At least 8 characters", prefs::otaPassword().c_str(), false,
                      [c](const String &text) {
          String pw = text;
          pw.trim();
          if (pw.length() < 8) {
            ui::alert("Password Too Short", "Use at least 8 characters.", {{"OK", ui::AlertAction::CANCEL, nullptr}});
            return;
          }
          prefs::setOtaPassword(pw);
          lv_obj_t *v = (lv_obj_t *)lv_obj_get_user_data(c);
          if (v) lv_label_set_text(v, pw.c_str());
        });
      }).value;
      lv_obj_set_user_data(c, pwValue);  // the page outlives the text entry, so the row can show the new value
    });
}
}
