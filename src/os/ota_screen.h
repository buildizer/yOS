#pragma once

// Full-screen "Updating yOS" overlay for over-the-air updates. Safe to call from loopTask:
// each call takes the LVGL lock and forces a refresh itself.
void otaScreenBegin(const char *via = "Installing over Wi-Fi");
void otaScreenProgress(unsigned int done, unsigned int total);
void otaScreenRestarting();  // call just before the reboot
void otaScreenFail();        // "Update failed. Try again.", removed after 3 s
