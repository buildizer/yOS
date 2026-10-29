#include "prefs.h"
#include <Preferences.h>

static Preferences nvs;
static Appearance appearanceVal = APPEARANCE_AUTO;
static uint8_t brightnessVal = 255;
static uint8_t dimAfterVal = 5;
static bool reduceMotionVal = false, use24hVal = true, rotationWidgetVal = true;
static uint8_t rotationVal = 0;

namespace prefs {
void load() {
  nvs.begin("yos", false);
  appearanceVal = Appearance(nvs.getUChar("appearance", APPEARANCE_AUTO));
  brightnessVal = nvs.getUChar("brightness", 255);
  dimAfterVal = nvs.getUChar("dimafter", 5);
  reduceMotionVal = nvs.getBool("reducemotion", false);
  use24hVal = nvs.getBool("use24h", true);
  rotationVal = nvs.getUChar("rotation", 0) & 3;
  rotationWidgetVal = nvs.getBool("rotwidget", true);
}
Appearance appearance() { return appearanceVal; }
void setAppearance(Appearance a) { appearanceVal = a; nvs.putUChar("appearance", a); }
uint8_t brightness() { return 255; }  // brightness control is disabled for now: always full (saved value kept)
void setBrightness(uint8_t b, bool persist) {
  brightnessVal = max<uint8_t>(b, 10);
  if (persist) nvs.putUChar("brightness", brightnessVal);  // flash writes stall the UI and the panel
}
uint8_t dimAfterMin() { return dimAfterVal; }
void setDimAfterMin(uint8_t m) { dimAfterVal = m; nvs.putUChar("dimafter", m); }
uint8_t rotation() { return rotationVal; }
void setRotation(uint8_t r) { rotationVal = r & 3; nvs.putUChar("rotation", rotationVal); }
bool rotationWidget() { return rotationWidgetVal; }
void setRotationWidget(bool on) { rotationWidgetVal = on; nvs.putBool("rotwidget", on); }
bool use24h() { return use24hVal; }
void setUse24h(bool on) { use24hVal = on; nvs.putBool("use24h", on); }
bool reduceMotion() { return reduceMotionVal; }
void setReduceMotion(bool on) { reduceMotionVal = on; nvs.putBool("reducemotion", on); }
bool autostart(const char *appId, bool fallback) { return nvs.getBool((String("as_") + appId).c_str(), fallback); }
void setAutostart(const char *appId, bool on) { nvs.putBool((String("as_") + appId).c_str(), on); }
}
