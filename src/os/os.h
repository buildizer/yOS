#pragma once
#include <lvgl.h>
#include <vector>
#include "os/theme.h"

// An app: an icon on the home screen, an optional background service, and a UI built when opened.
class App {
public:
  virtual ~App() = default;
  virtual const char *id() const = 0;
  virtual const char *name() const = 0;
  virtual const char *icon() const = 0;          // ICON_* glyph
  virtual lv_color_t tint() const = 0;           // icon tile colour

  // Background service (optional)
  virtual bool hasService() const { return false; }
  virtual bool defaultAutostart() const { return false; }
  virtual void startService() {}
  virtual void stopService() {}
  virtual const char *serviceSummary() const { return nullptr; }  // what the service does, for App info

  // Foreground UI
  virtual void open(lv_obj_t *root) = 0;          // build the first page with ui::push(root, ...)
  virtual void close() {}                         // forget pointers into the UI; the OS deletes it
  virtual void tick() {}                          // ~4 Hz while open

  // Live icon: draw extra content on the home tile, refreshed every second.
  virtual bool liveIcon(lv_obj_t *tile) { return false; }  // return true if it replaced the glyph

  virtual uint64_t storageBytes() { return 0; }   // data kept on the SD card

  // App settings live in Settings › Apps › <app>, never inside the app (iOS convention).
  // appRoot is the Settings app's root, for pushing sub-pages.
  virtual void buildSettings(lv_obj_t *content, lv_obj_t *appRoot) {}

  virtual bool removable() const { return false; }  // installed (SD) apps can be removed

  bool running = false;
};

enum StatusLevel { STATUS_UNKNOWN, STATUS_OK, STATUS_WARN, STATUS_ALERT };
struct SystemStatus {
  StatusLevel level;
  char text[40];
};

namespace os {
void registerApp(App *app);
void unregisterApp(App *app);   // stops and removes it from the Home Screen (caller deletes it)
void rebuildHome();             // after the app list changes
const std::vector<App *> &apps();
App *find(const char *id);
App *current();

void begin();       // after hal::begin(): theme, autostart services, home screen
void loop();

void open(App *app);
void goHome();
void start(App *app);
void stop(App *app);
void showAppInfo(App *app);  // opens Settings › Apps › <app>
void appInfoPage(lv_obj_t *content, App *app);  // the shared App info page body

void setStatusProvider(SystemStatus (*fn)(), App *owner);
void refreshAppearance();    // re-evaluate light/dark (called when Settings change it)
}
