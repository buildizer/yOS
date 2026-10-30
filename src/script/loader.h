#pragma once
// Script (Lua) apps installed on internal flash or the SD card. See src/script/README.md.
//
// An app is a folder /apps/<id>/ holding app.json and main.lua (plus any other files), either on
// the internal LittleFS partition ("spiffs" in default_16MB.csv) or on the SD card. New installs go
// to internal flash while it keeps 256 KB free after the install, otherwise to the SD card.
//
// Threads: loadScriptApps() runs before os::begin(). The other functions are for the web server
// task (and the App Store's download task); they take hal::lock() themselves around anything that
// touches the app registry or LVGL, and never hold it while waiting for the SD card lock.
// scriptAppLook() touches nothing and is safe anywhere.
#include <Arduino.h>
#include <lvgl.h>
#include <string>
#include <vector>

void loadScriptApps();  // mounts internal flash, registers every installed app; called before os::begin()

struct ScriptAppInfo {
  String id, name, version;
  bool onSd;
  uint64_t bytes;
};
std::vector<ScriptAppInfo> scriptAppList();

struct ScriptAppFile {
  String name;       // plain file name (no folders)
  std::string data;
};
// Installs (or replaces) the app described by files["app.json"], then shows it on the Home Screen.
// On failure returns false with a human-readable reason in err.
bool scriptAppInstall(const std::vector<ScriptAppFile> &files, String &err, ScriptAppInfo *installed = nullptr);
bool scriptAppRemove(const char *id, String &err);

// Free space for new apps (bytes), for the web page.
uint64_t scriptInternalFree();

// The Home Screen glyph (ICON_*) and tile colour an app.json with this "icon" name and "#RRGGBB"
// "color" gets, so the App Store can draw apps that aren't installed yet the way they will look.
void scriptAppLook(const char *icon, const char *color, const char *&glyph, lv_color_t &tint);
