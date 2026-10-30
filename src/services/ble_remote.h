#pragma once
// Internal to the Bluetooth service: the phone remote's hooks into ble.cpp's GATT server.
// The public side (UI requests, transfer progress) is in ble.h.
#include <NimBLEDevice.h>

namespace ble {
// Anything a phone can use to control the board must only open on an encrypted, authenticated
// (passkey) link: create it with these. Read-only info may stay plain READ.
const uint32_t SECURE_READ = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN;
const uint32_t SECURE_WRITE = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;

namespace remote {
void init();                        // bleBegin(): locks and queues, nothing radio-related
void addTo(NimBLEService *svc);     // stack start: the command and data characteristics
void detach();                      // stack stop, before deinit: no NimBLE calls after this returns
void peerGone();                    // the phone disconnected: drop any transfer
}
}
