#pragma once
#include <Arduino.h>

// Writes a new yOS image into the spare app slot, for every update route (GitHub, USB, Wi-Fi push).
// Callers feed the bytes as they arrive: a plain image, or a gzip or zlib stream of one (told apart by
// the first bytes), inflated here with the decompressor in the chip's ROM. The image is checked
// (ESP image, ESP32-S3, project "yOS") before anything is written. Flash is written by a task of its
// own on core 0, from 16 KB buffers in internal RAM (flash can't be written from PSRAM directly), and
// erased 64 KB at a time just ahead of the writes, so only what the image needs is erased.
// One update at a time: begin() refuses while another one runs.
namespace fwupdate {

bool canBegin(String &err);  // whether begin() would be allowed now (nothing claimed)
// sizeHint: the image size when known (0 = not known); used only to erase ahead.
bool begin(const char *via, uint32_t sizeHint, String &err);
bool write(const uint8_t *data, size_t len);  // false: error() says why; call abort()
// Writes the rest, checks the image (and its MD5 when given), notes the version as pending and
// switches the boot slot. Prints a timing line. Restarting is up to the caller.
bool finish(const char *md5Hex = nullptr);
void abort();
const String &error();
bool busy();
uint32_t imageBytes();    // image bytes produced so far (after inflating)
String imageVersion();    // from the image's app description, once its header has arrived

// Boot side. bootCheck() once in setup(), after the UI exists: when the last install didn't come up
// (the bootloader went back to this version), it tells the person once. confirmLater() arms a UI
// timer that keeps this version ~30 s after boot, so an image whose UI hangs is never confirmed.
// loop() rolls a version that still isn't confirmed after 2 minutes back to the previous one.
void bootCheck();
void confirmLater();
void loop();

}  // namespace fwupdate
