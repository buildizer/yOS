// Finds, installs and removes script apps on internal flash (LittleFS) and the SD card.
#include "script/loader.h"
#include <LittleFS.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "os/hal.h"
#include "os/os.h"
#include "script/script_app.h"
#include "services/storage.h"

namespace script {

static const uint64_t INTERNAL_MARGIN = 256 * 1024;  // keep this much of the flash partition free
static const size_t FILE_MAX = 256 * 1024;
static const size_t APP_MAX = 1024 * 1024;
static bool internalReady = false;
static std::vector<ScriptApp *> installed;  // every ScriptApp ever created; guarded by hal::lock()

static bool ready(Location loc) { return loc == SDCARD ? storageReady() : internalReady; }

// Runs fn with the location's file system, holding the SD lock for the card.
static bool withFs(Location loc, std::function<void(fs::FS &)> fn) {
  if (!ready(loc)) return false;
  if (loc == SDCARD) storageWithLock([&] { fn(SD); });
  else fn(LittleFS);
  return true;
}

bool readFile(Location loc, const String &path, std::string &out, size_t maxBytes) {
  bool ok = false;
  withFs(loc, [&](fs::FS &fs) {
    File f = fs.open(path, FILE_READ);
    if (!f || f.isDirectory() || f.size() > maxBytes) return;
    out.resize(f.size());
    ok = f.read((uint8_t *)&out[0], out.size()) == out.size();
    f.close();
  });
  return ok;
}

bool writeFile(Location loc, const String &path, const std::string &data) {
  bool ok = false;
  withFs(loc, [&](fs::FS &fs) {
    File f = fs.open(path, FILE_WRITE);
    if (!f) return;
    ok = f.write((const uint8_t *)data.data(), data.size()) == data.size();
    f.close();
  });
  return ok;
}

static uint64_t treeBytes(fs::FS &fs, const String &dir) {
  uint64_t total = 0;
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) return 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    total += f.isDirectory() ? treeBytes(fs, f.path()) : f.size();
    f.close();
  }
  d.close();
  return total;
}

uint64_t folderBytes(Location loc, const String &dir) {
  uint64_t total = 0;
  withFs(loc, [&](fs::FS &fs) { total = treeBytes(fs, dir); });
  return total;
}

static void removeTree(fs::FS &fs, const String &dir) {
  std::vector<std::pair<String, bool>> entries;  // collect first: deleting while iterating confuses FAT
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    entries.push_back({f.path(), f.isDirectory()});
    f.close();
  }
  d.close();
  for (auto &e : entries) {
    if (e.second) removeTree(fs, e.first);
    else fs.remove(e.first);
  }
  fs.rmdir(dir);
}

void removeFolder(Location loc, const String &dir) {
  withFs(loc, [&](fs::FS &fs) { removeTree(fs, dir); });
}

void detachApp(ScriptApp *app) {
  if (app->removed) return;
  app->removed = true;  // first, so closing it (if open) skips re-measuring a folder about to go
  os::unregisterApp(app);
  os::rebuildHome();
}

static ScriptApp *findInstalled(const String &id) {
  for (ScriptApp *a : installed)
    if (!a->removed && a->manifest.id == id) return a;
  return nullptr;
}

// Registers every valid app folder in /apps on one location.
static void scan(Location loc) {
  const char *where = loc == SDCARD ? "SD card" : "internal flash";
  std::vector<String> folders;
  if (!withFs(loc, [&](fs::FS &fs) {
        File d = fs.open("/apps");
        if (!d || !d.isDirectory()) return;
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
          if (f.isDirectory()) folders.push_back(f.name());
          f.close();
        }
        d.close();
      }))
    return;
  for (const String &name : folders) {
    String dir = "/apps/" + name, err;
    std::string json;
    Manifest m;
    if (!readFile(loc, dir + "/app.json", json, 4096)) {
      Serial.printf("[apps] skipping %s on %s: no readable app.json\n", dir.c_str(), where);
      continue;
    }
    if (!parseManifest(json, m, err)) {
      Serial.printf("[apps] skipping %s on %s: %s\n", dir.c_str(), where, err.c_str());
      continue;
    }
    if (m.id != name) {
      Serial.printf("[apps] skipping %s on %s: app.json says id \"%s\"\n", dir.c_str(), where, m.id.c_str());
      continue;
    }
    bool hasMain = false;
    withFs(loc, [&](fs::FS &fs) { hasMain = fs.exists(dir + "/main.lua"); });
    if (!hasMain) {
      Serial.printf("[apps] skipping %s on %s: no main.lua\n", dir.c_str(), where);
      continue;
    }
    if (os::find(m.id.c_str())) {
      Serial.printf("[apps] skipping %s on %s: id \"%s\" is already taken\n", dir.c_str(), where, m.id.c_str());
      continue;
    }
    ScriptApp *app = new ScriptApp(m, loc, folderBytes(loc, dir));
    installed.push_back(app);
    os::registerApp(app);
    Serial.printf("[apps] %s (%s) from %s, %llu bytes\n", m.name.c_str(), m.id.c_str(), where, app->bytes);
  }
}

static bool validFileName(const String &n) {
  if (n.isEmpty() || n.length() > 48 || n[0] == '.') return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') return false;
  return true;
}

}  // namespace script

using namespace script;

void loadScriptApps() {
  // Internal app storage is the "spiffs" data partition, formatted as LittleFS on first use.
  internalReady = LittleFS.begin(true, "/littlefs", 10, "spiffs");
  if (internalReady) {
    LittleFS.mkdir("/apps");
    Serial.printf("[apps] internal flash: %u KB used of %u KB\n", (unsigned)(LittleFS.usedBytes() / 1024),
                  (unsigned)(LittleFS.totalBytes() / 1024));
  } else {
    Serial.println("[apps] internal flash could not be mounted");
  }
  scan(INTERNAL);
  scan(SDCARD);
}

uint64_t scriptInternalFree() {
  if (!internalReady) return 0;
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return used < total ? total - used : 0;
}

std::vector<ScriptAppInfo> scriptAppList() {
  std::vector<ScriptAppInfo> out;
  hal::lock();
  for (ScriptApp *a : installed)
    if (!a->removed) out.push_back({a->manifest.id, a->manifest.name, a->manifest.version, a->loc == SDCARD, a->bytes});
  hal::unlock();
  return out;
}

bool scriptAppRemove(const char *id, String &err) {
  hal::lock();
  ScriptApp *app = findInstalled(id);
  Location loc = INTERNAL;
  String dir;
  if (app) {
    loc = app->loc;
    dir = app->folder();
    detachApp(app);
  }
  hal::unlock();
  if (!app) {
    err = "No such app";
    return false;
  }
  removeFolder(loc, dir);
  return true;
}

bool scriptAppInstall(const std::vector<ScriptAppFile> &files, String &err, ScriptAppInfo *result) {
  const ScriptAppFile *manifestFile = nullptr;
  bool hasMain = false;
  uint64_t need = 2 * 4096, total = 0;  // the folder itself plus LittleFS metadata
  for (const ScriptAppFile &f : files) {
    if (!validFileName(f.name)) {
      err = "Bad file name \"" + f.name + "\" (letters, digits, . - _ only)";
      return false;
    }
    if (f.data.size() > FILE_MAX) {
      err = f.name + " is larger than 256 KB";
      return false;
    }
    if (f.name == "app.json") manifestFile = &f;
    if (f.name == "main.lua") hasMain = true;
    total += f.data.size();
    need += (f.data.size() + 4095) / 4096 * 4096 + 4096;
  }
  if (!manifestFile || !hasMain) {
    err = "An app needs app.json and main.lua";
    return false;
  }
  if (total > APP_MAX) {
    err = "An app may be at most 1 MB";
    return false;
  }
  Manifest m;
  if (!parseManifest(manifestFile->data, m, err)) return false;

  // Replacing an installed app (an update) frees its space first.
  hal::lock();
  App *taken = os::find(m.id.c_str());
  ScriptApp *old = findInstalled(m.id);
  hal::unlock();
  if (taken && !old) {
    err = "The id \"" + m.id + "\" belongs to a built-in app";
    return false;
  }
  uint64_t internalFree = scriptInternalFree() + (old && old->loc == INTERNAL ? old->bytes : 0);
  Location loc;
  if (internalReady && internalFree >= need + INTERNAL_MARGIN) loc = INTERNAL;
  else if (storageReady()) loc = SDCARD;
  else {
    err = "Not enough space on this device, and no SD card is inserted";
    return false;
  }
  if (old) {
    hal::lock();
    detachApp(old);
    hal::unlock();
    removeFolder(old->loc, old->folder());
  }

  String dir = "/apps/" + m.id;
  bool ok = withFs(loc, [&](fs::FS &fs) {
    fs.mkdir("/apps");
    fs.mkdir(dir);
  });
  for (const ScriptAppFile &f : files) ok = ok && writeFile(loc, dir + "/" + f.name, f.data);
  if (!ok) {
    removeFolder(loc, dir);
    err = String("Could not write to ") + (loc == SDCARD ? "the SD card" : "internal flash");
    return false;
  }

  ScriptApp *app = new ScriptApp(m, loc, folderBytes(loc, dir));
  hal::lock();
  installed.push_back(app);
  os::registerApp(app);
  os::rebuildHome();
  hal::unlock();
  Serial.printf("[apps] installed %s (%s) on %s\n", m.name.c_str(), m.id.c_str(), loc == SDCARD ? "SD card" : "internal flash");
  if (result) *result = {m.id, m.name, m.version, loc == SDCARD, app->bytes};
  return true;
}

void scriptAppLook(const char *icon, const char *color, const char *&glyph, lv_color_t &tint) {
  // Same rules as an installed app's manifest, so a store listing and the installed tile agree.
  JsonDocument doc;
  doc["id"] = "look";
  doc["name"] = "look";
  doc["icon"] = icon && *icon ? icon : "extension";
  if (color && *color) doc["color"] = color;
  std::string json;
  serializeJson(doc, json);
  Manifest m;
  String err;
  parseManifest(json, m, err);
  glyph = m.glyph;
  tint = m.color;
}
