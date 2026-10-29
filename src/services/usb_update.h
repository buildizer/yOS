#pragma once
// Firmware updates over the USB cable that yOS receives itself (tools/usb_update.py), so the
// Software Update screen shows progress. The ROM-loader path (esptool) stays for recovery.
void usbUpdatePoll();  // call from loop(); returns at once unless an update starts
