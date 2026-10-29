#pragma once
// Internal to src/script: the App wrapper around one installed Lua app, and the storage helpers
// it shares with the loader.
#include <Arduino.h>
#include <memory>
#include <string>
#include "os/os.h"

namespace script {

enum Location { INTERNAL, SDCARD };

struct Manifest {
  String id, name, version;
  const char *glyph;
  lv_color_t color;
};
// Parses app.json. False with a reason in err for a broken manifest.
bool parseManifest(const std::string &json, Manifest &m, String &err);

// File helpers for /apps on either location. SD access takes the SD lock; never call them while
// holding it. Paths are absolute within the location's file system.
bool readFile(Location loc, const String &path, std::string &out, size_t maxBytes);
bool writeFile(Location loc, const String &path, const std::string &data);
uint64_t folderBytes(Location loc, const String &dir);
void removeFolder(Location loc, const String &dir);

struct Session;

class ScriptApp : public App {
public:
  ScriptApp(const Manifest &m, Location loc, uint64_t bytes) : manifest(m), loc(loc), bytes(bytes) {}

  const char *id() const override { return manifest.id.c_str(); }
  const char *name() const override { return manifest.name.c_str(); }
  const char *icon() const override { return manifest.glyph; }
  lv_color_t tint() const override { return manifest.color; }
  void open(lv_obj_t *root) override;
  void close() override;
  void tick() override;
  uint64_t storageBytes() override { return bytes; }
  void buildSettings(lv_obj_t *content, lv_obj_t *appRoot) override;
  bool removable() const override { return true; }

  String folder() const { return "/apps/" + manifest.id; }

  Manifest manifest;
  Location loc;
  uint64_t bytes;
  // Removed apps are taken off the Home Screen but never deleted: a stale Settings row may still
  // point at one, and it stays safe to open (it just says it was removed).
  bool removed = false;

private:
  std::shared_ptr<Session> session;
};

// Takes the app off the Home Screen and marks it removed. Caller holds hal::lock().
void detachApp(ScriptApp *app);

}  // namespace script
