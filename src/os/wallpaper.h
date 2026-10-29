#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include <vector>

// User wallpapers on the SD card, shared by the Home Screen, the Settings page and the web dashboard.
//   /wallpapers/<name>.rgb565  raw 480x480 RGB565 little-endian (460800 bytes), required
//   /wallpapers/<name>.thumb   raw 96x96 RGB565 little-endian (18432 bytes), optional
// <name> is [A-Za-z0-9_-]{1,32}. The selection is stored in NVS (namespace "wallpaper", key "name");
// an empty name means the built-in gradient.
namespace wallpaper {
static const int FULL = 480, THUMB = 96;
static const size_t FULL_BYTES = FULL * FULL * 2, THUMB_BYTES = THUMB * THUMB * 2;

struct Info {
  String name;
  bool thumb;  // has a .thumb file
};

bool validName(const String &name);
const String &current();            // "" = built-in gradient
void set(const String &name);       // persists the choice and drops the cached image (call os::rebuildHome() after)
void invalidate();                  // the file for the current selection changed on disk
std::vector<Info> list();           // takes the SD lock; empty if the SD card isn't ready

// Home Screen image for the current selection, or nullptr (use the gradient). Loaded into PSRAM on first
// use and kept until the selection or the appearance changes. UI task only (holds hal::lock()).
const lv_img_dsc_t *image();

// Thumbnail for the picker, in PSRAM. Uses the .thumb file, or subsamples the full image if there is none.
// Free with freeThumb() when the page goes away. nullptr if the files can't be read.
lv_img_dsc_t *loadThumb(const String &name);
void freeThumb(lv_img_dsc_t *dsc);
}
