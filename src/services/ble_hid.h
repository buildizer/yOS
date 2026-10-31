#pragma once
// Internal to the Bluetooth service: the HID keyboard / media / mouse's hooks into ble.cpp's GATT
// server. The public side (sending keys) is in ble.h.
#include <NimBLEDevice.h>

namespace ble {
namespace hid {
void init();                          // bleBegin(): lock and queue, nothing radio-related
void addTo(NimBLEServer *server);     // stack start: HID, battery and device information services
void detach();                        // stack stop, before deinit: no NimBLE calls after this returns
void peerSecured(uint16_t conn, bool encrypted);  // pairing or re-encryption finished
void peerGone();                      // the phone disconnected
}
}
