// yOS: an app launcher for the Guition ESP32-4848S040.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "apps/apps.h"
#include "apps/settings/settings.h"
#include "os/hal.h"
#include "os/boot_screen.h"
#include "os/ota_screen.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/ui.h"
#include "services/monitor.h"
#include "services/storage.h"
#include "services/web.h"
#include "services/usb_update.h"
#include "services/wifi_manager.h"
#include "services/github_update.h"
#include "services/firmware_update.h"
#include "services/ble.h"
#include "script/loader.h"

// A freshly installed image stays "pending" until its UI has run for a while (fwupdate::confirmLater()).
// If it crashes or resets before then, the bootloader goes back to the previous version on its own.
extern "C" bool verifyRollbackLater() { return true; }

// Over-the-air updates (tools/flash.sh ota). A full-screen overlay shows progress so nobody unplugs mid-update.
static void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(prefs::otaPassword().c_str());
  ArduinoOTA.setMdnsEnabled(false);  // the Wi-Fi manager owns mDNS
  ArduinoOTA.onStart([] { otaScreenBegin("Installing over Wi-Fi"); });
  ArduinoOTA.onProgress(otaScreenProgress);
  ArduinoOTA.onEnd(otaScreenRestarting);
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error %d\n", e);
    otaScreenFail();
  });
  ArduinoOTA.begin();
}

void setup() {
  // Ordinary allocations of 256 bytes and up go to PSRAM (8 MB). Internal RAM (~90 KB free) is kept for
  // what can only live there: Wi-Fi/lwIP packet buffers, DMA, task stacks and the LVGL render buffers.
  // At the default 4 KB threshold, JSON parsing and UI objects starved the network stack (ENOMEM).
  heap_caps_malloc_extmem_enable(256);
  Serial.setRxBufferSize(4096);  // USB updates enlarge it while they run (usb_update.cpp)
  Serial.begin(115200);
  prefs::load();
  hal::begin();
  hal::setRotation(prefs::rotation());

  bootScreenStart();
  lv_timer_handler();     // draw the first frame, then light the panel
  hal::backlightOnNow();
  ghupdate::begin();
  wifimgr::begin();
  String first = wifimgr::firstSavedSsid();
  String shown;
  bootScreenStage(1, first.length() ? ("Connecting to " + first + "\xE2\x80\xA6").c_str()
                                    : "No Wi-Fi yet. Join a network in Settings.");
  uint32_t t0 = millis();
  while (first.length() && wifimgr::state() != wifimgr::CONNECTED && wifimgr::state() != wifimgr::OFF && millis() - t0 < 20000) {
    String now = wifimgr::connectingSsid();
    if (now.length() && now != shown) {
      shown = now;
      bootScreenStage(1, ("Connecting to " + now + "\xE2\x80\xA6").c_str());
    }
    lv_timer_handler();
    delay(10);
  }
  Serial.printf("WiFi: %s\n", wifimgr::state() == wifimgr::CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");  // mDNS is (re)started by the Wi-Fi manager

  bootScreenStage(2, "Mounting the SD card\xE2\x80\xA6");
  bool sdOk = storageStart();
  bootScreenStage(3, "Starting apps\xE2\x80\xA6");
  if (sdOk) {
    static Event saved[MAX_EVENTS];
    monitorRestoreEvents(saved, storageLoadEvents(saved, MAX_EVENTS));
  }

  bootScreenEnd();
  os::registerApp(createNetwatchApp());
  os::registerApp(createClockApp());
  os::registerApp(createWeatherApp());
  os::registerApp(createFilesApp());
  os::registerApp(createStoreApp());
  os::registerApp(createSettingsApp());
  loadScriptApps();
  os::begin();
  fwupdate::bootCheck();     // "update failed, rolled back" once, if the last install didn't come up
  fwupdate::confirmLater();
  bleBegin();
  settings::pairingPromptBegin();
  webStart();
  startOta();

  // The UI gets its own high-priority task on core 1 so web requests never delay a frame;
  // the network services run on core 0.
  xTaskCreatePinnedToCore([](void *) {
    for (;;) {
      hal::lock();
      os::loop();
      hal::unlock();
      vTaskDelay(1);
    }
  }, "ui", 12288, nullptr, 3, nullptr, 1);
  Serial.println("yOS ready");
}

void loop() {
  // A GitHub install owns the flash and the update screen; USB and Wi-Fi pushes wait until it's done.
  if (!ghupdate::installing()) usbUpdatePoll();
  fwupdate::loop();
  ghupdate::loop();
  static uint32_t otaPwSeen = 0;  // a new password from Settings applies here, between OTA sessions
  if (prefs::otaPasswordVersion() != otaPwSeen) {
    otaPwSeen = prefs::otaPasswordVersion();
    ArduinoOTA.setPassword(prefs::otaPassword().c_str());
  }
  if (!ghupdate::installing()) ArduinoOTA.handle();
  webLoop();
  delay(2);
}
