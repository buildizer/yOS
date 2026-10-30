#pragma once
#include <lvgl.h>
#include <functional>

// Settings pages, one file each. Each pushes its page onto the Settings app's stack.
namespace settings {
void displayPage(lv_obj_t *appRoot);
void wallpaperPage(lv_obj_t *appRoot, std::function<void()> onChange);  // picker; onChange after a selection
const char *wallpaperSummary();  // value shown on the Wallpaper row
void wifiPage(lv_obj_t *appRoot);
void bluetoothPage(lv_obj_t *appRoot);
void pairingPromptBegin();  // at boot: shows the passkey whenever a phone pairs, over any screen
void remoteBegin();         // at boot: runs the phone remote's requests, shows file transfer progress
void aboutPage(lv_obj_t *appRoot);
void softwareUpdateSection(lv_obj_t *content, lv_obj_t *appRoot);  // About: GitHub updates and sign-in
lv_obj_t *segmented(lv_obj_t *parent, const char **opts);  // iOS-style segmented control
const char *wifiSummary();       // value shown on the Wi-Fi row
const char *bluetoothSummary();  // value shown on the Bluetooth row
}
