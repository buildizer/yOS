#include "os/wallpaper.h"
#include <Preferences.h>
#include <SD.h>
#include "os/theme.h"
#include "services/storage.h"

namespace wallpaper {

static const char *DIR = "/wallpapers";

static String selected;
static bool selectedLoaded = false;

// Cached Home Screen image. `bakedDark` records which appearance the scrim was baked for.
static lv_img_dsc_t img;
static uint16_t *imgPx = nullptr;
static bool imgValid = false, bakedDark = false;
static String imgName;

bool validName(const String &name) {
  if (name.length() < 1 || name.length() > 32) return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  }
  return true;
}

const String &current() {
  if (!selectedLoaded) {
    Preferences nvs;
    nvs.begin("wallpaper", true);
    selected = nvs.getString("name", "");
    nvs.end();
    if (!validName(selected)) selected = "";
    selectedLoaded = true;
  }
  return selected;
}

void invalidate() { imgValid = false; }

void set(const String &name) {
  String n = validName(name) ? name : String("");
  current();
  selected = n;
  Preferences nvs;
  nvs.begin("wallpaper", false);
  nvs.putString("name", n);
  nvs.end();
  invalidate();
}

std::vector<Info> list() {
  std::vector<Info> out;
  if (!storageReady()) return out;
  storageWithLock([&] {
    File d = SD.open(DIR);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      String fn = String(f.name());
      size_t sz = f.size();
      bool dir = f.isDirectory();
      f.close();
      fn = fn.substring(fn.lastIndexOf('/') + 1);
      if (dir || !fn.endsWith(".rgb565") || sz != FULL_BYTES) continue;
      String base = fn.substring(0, fn.length() - 7);
      if (!validName(base)) continue;
      out.push_back({base, SD.exists(String(DIR) + "/" + base + ".thumb")});
    }
    d.close();
  });
  std::sort(out.begin(), out.end(), [](const Info &a, const Info &b) { return a.name < b.name; });
  return out;
}

static void makeDsc(lv_img_dsc_t &d, uint16_t *px, int w, int h) {
  memset(&d, 0, sizeof(d));
  d.header.w = w;
  d.header.h = h;
  d.header.cf = LV_IMG_CF_TRUE_COLOR;
  d.data_size = (uint32_t)w * h * 2;
  d.data = (const uint8_t *)px;
}

// Darken every pixel by `scrimPct` percent, once, at load time.
static void bakeScrim(uint16_t *px, size_t count, int scrimPct) {
  int k = 256 * (100 - scrimPct) / 100;
  for (size_t i = 0; i < count; i++) {
    uint16_t c = px[i];
    uint32_t r = ((c >> 11) * k) >> 8, g = (((c >> 5) & 0x3F) * k) >> 8, b = ((c & 0x1F) * k) >> 8;
    px[i] = (r << 11) | (g << 5) | b;
  }
}

const lv_img_dsc_t *image() {
  const String &name = current();
  if (name.isEmpty()) return nullptr;
  bool dark = theme::isDark();
  if (imgValid && imgName == name && bakedDark == dark) return &img;
  if (!storageReady()) return nullptr;

  if (!imgPx) imgPx = (uint16_t *)heap_caps_malloc(FULL_BYTES, MALLOC_CAP_SPIRAM);
  if (!imgPx) return nullptr;
  imgValid = false;
  bool ok = false;
  storageWithLock([&] {
    File f = SD.open(String(DIR) + "/" + name + ".rgb565", FILE_READ);
    if (!f) return;
    if (f.size() == FULL_BYTES) ok = f.read((uint8_t *)imgPx, FULL_BYTES) == (int)FULL_BYTES;
    f.close();
  });
  if (!ok) return nullptr;
  // The scrim is baked in here rather than blended over the image every frame: the panel's refresh
  // reads PSRAM too, and per-frame full-screen blending on top of the image fetch caused tearing.
  // Dark mode gets a 30% scrim so white home labels stay readable on light pictures.
  if (dark) bakeScrim(imgPx, FULL * FULL, 30);
  makeDsc(img, imgPx, FULL, FULL);
  lv_img_cache_invalidate_src(&img);
  imgName = name;
  bakedDark = dark;
  imgValid = true;
  return &img;
}

lv_img_dsc_t *loadThumb(const String &name) {
  if (!validName(name) || !storageReady()) return nullptr;
  uint16_t *px = (uint16_t *)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM);
  if (!px) return nullptr;
  bool ok = false;
  storageWithLock([&] {
    String base = String(DIR) + "/" + name;
    File t = SD.open(base + ".thumb", FILE_READ);
    if (t) {
      if (t.size() == THUMB_BYTES) ok = t.read((uint8_t *)px, THUMB_BYTES) == (int)THUMB_BYTES;
      t.close();
      if (ok) return;
    }
    // No thumbnail: nearest-neighbour subsample of the full image (every 5th row and column).
    File f = SD.open(base + ".rgb565", FILE_READ);
    if (!f) return;
    uint16_t *row = (uint16_t *)heap_caps_malloc(FULL * 2, MALLOC_CAP_SPIRAM);
    if (row && f.size() == FULL_BYTES) {
      ok = true;
      for (int y = 0; y < THUMB && ok; y++) {
        f.seek((size_t)(y * (FULL / THUMB) + 2) * FULL * 2);
        ok = f.read((uint8_t *)row, FULL * 2) == FULL * 2;
        for (int x = 0; x < THUMB; x++) px[y * THUMB + x] = row[x * (FULL / THUMB) + 2];
      }
    }
    if (row) heap_caps_free(row);
    f.close();
  });
  if (!ok) { heap_caps_free(px); return nullptr; }
  lv_img_dsc_t *d = new lv_img_dsc_t;
  makeDsc(*d, px, THUMB, THUMB);
  return d;
}

void freeThumb(lv_img_dsc_t *dsc) {
  if (!dsc) return;
  lv_img_cache_invalidate_src(dsc);
  heap_caps_free((void *)dsc->data);
  delete dsc;
}
}
