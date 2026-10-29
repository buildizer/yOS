#pragma once
#include <Arduino.h>

// System settings, persisted in NVS.
enum Appearance : uint8_t { APPEARANCE_AUTO, APPEARANCE_LIGHT, APPEARANCE_DARK };

namespace prefs {
void load();
Appearance appearance();
void setAppearance(Appearance a);
uint8_t brightness();            // 10..255
void setBrightness(uint8_t b, bool persist = true);  // persist=false: apply only (e.g. while dragging)
uint8_t dimAfterMin();           // 0 = never dim
void setDimAfterMin(uint8_t m);
uint8_t rotation();              // 0..3 = 0, 90, 180, 270 degrees
void setRotation(uint8_t r);
bool rotationWidget();          // the Rotation widget on the Home Screen
void setRotationWidget(bool on);
bool use24h();
void setUse24h(bool on);
bool reduceMotion();
void setReduceMotion(bool on);
bool autostart(const char *appId, bool fallback);
void setAutostart(const char *appId, bool on);
}
