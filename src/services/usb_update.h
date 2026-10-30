#pragma once
#include <stdint.h>

// Update.begin() for this firmware: keeps Update's 4 KB sector buffer in internal RAM. With the
// global "256 B and up goes to PSRAM" rule it would land in PSRAM, and flash can't be written from
// PSRAM directly: every write then crawls through a tiny bounce buffer (19 s instead of a few).
bool updateBeginFast(uint32_t size);
// Firmware updates over the USB cable that yOS receives itself (tools/usb_update.py), so the
// Software Update screen shows progress. The ROM-loader path (esptool) stays for recovery.
void usbUpdatePoll();  // call from loop(); returns at once unless an update starts
