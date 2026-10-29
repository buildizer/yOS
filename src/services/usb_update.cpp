#include "services/usb_update.h"
#include <Arduino.h>
#include <Update.h>
#include "os/ota_screen.h"
#include <esp32s3/rom/miniz.h>

// Protocol (host = tools/usb_update.py):
//   host:   "YOSUPD <size> <md5> <baud> [<zsize>]\n"   device: "READY\n", then switches to <baud>
//           With <zsize>, the chunks carry a zlib stream of the image (about a third smaller); it is
//           inflated on the fly with the decompressor in the chip's ROM. <size> and <md5> are always
//           of the uncompressed image.
//   host:   'C' seq:u32 len:u16 crc32:u32 data[len]   device: "OK <seq>" or "ERR <expected seq>"
//   host:   'C' with len 0 = end                device: "DONE" (then reboots) or "FAIL <reason>"
// Chunks are stop-and-wait with a CRC, because this board's CH340 USB bridge drops bytes.

static const uint32_t CHUNK_MAX = 1024, BYTE_TIMEOUT_MS = 2000, IDLE_ABORT_MS = 15000;

// Streaming inflate state (PSRAM: the decompressor is ~11 KB and needs a 32 KB window).
struct Inflate {
  tinfl_decompressor d;
  uint8_t dict[TINFL_LZ_DICT_SIZE];
  size_t dictOfs;
};

// Feeds one compressed chunk; writes whatever it produces to Update. False on a corrupt stream.
static bool inflateChunk(Inflate *z, const uint8_t *in, size_t len, bool last, uint32_t &written) {
  for (;;) {
    size_t inBytes = len, outBytes = TINFL_LZ_DICT_SIZE - z->dictOfs;
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
    tinfl_status st = tinfl_decompress(&z->d, in, &inBytes, z->dict, z->dict + z->dictOfs, &outBytes, flags);
    if (outBytes) {
      if (Update.write(z->dict + z->dictOfs, outBytes) != outBytes) return false;
      written += outBytes;
      z->dictOfs = (z->dictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
    }
    in += inBytes;
    len -= inBytes;
    if (st < 0) return false;
    if (st == TINFL_STATUS_DONE) return true;
    if (len == 0 && st == TINFL_STATUS_NEEDS_MORE_INPUT) return true;
    // TINFL_STATUS_HAS_MORE_OUTPUT: the window wrapped; loop to drain it.
  }
}

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
  }
  return ~c;
}

static bool readExact(uint8_t *buf, size_t n, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  size_t got = 0;
  while (got < n) {
    int avail = Serial.available();
    if (avail > 0) {
      got += Serial.readBytes(buf + got, min<size_t>(avail, n - got));
      t0 = millis();
    } else if (millis() - t0 > timeoutMs) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

static void drainInput() {
  uint32_t t0 = millis();
  while (millis() - t0 < 30) {
    while (Serial.available()) { Serial.read(); t0 = millis(); }
    delay(1);
  }
}

static void finishFailed(const char *why) {
  Update.abort();
  Serial.printf("FAIL %s\n", why);
  Serial.flush();
  Serial.updateBaudRate(115200);
  otaScreenFail();
}

static void runUpdate(uint32_t size, const char *md5, uint32_t baud, uint32_t zsize) {
  if (!Update.begin(size)) {
    Serial.printf("FAIL %s\n", Update.errorString());
    return;
  }
  if (strlen(md5) == 32) Update.setMD5(md5);
  Inflate *z = nullptr;
  if (zsize) {
    z = (Inflate *)heap_caps_malloc(sizeof(Inflate), MALLOC_CAP_SPIRAM);
    if (!z) { Update.abort(); Serial.println("FAIL no memory"); return; }
    tinfl_init(&z->d);
    z->dictOfs = 0;
  }
  uint32_t received = 0;  // compressed bytes taken in
  otaScreenBegin("Installing over USB");
  Serial.println("READY");
  Serial.flush();
  delay(20);
  if (baud) Serial.updateBaudRate(baud);
  drainInput();

  static uint8_t data[CHUNK_MAX];
  uint32_t expected = 0, written = 0, lastData = millis();
  for (;;) {
    // Find the next chunk marker; anything else on the line is noise.
    int c = Serial.read();
    if (c < 0) {
      if (millis() - lastData > IDLE_ABORT_MS) { free(z); return finishFailed("timeout"); }
      delay(1);
      continue;
    }
    if (c != 'C') continue;
    uint8_t hdr[10];
    if (!readExact(hdr, sizeof(hdr), BYTE_TIMEOUT_MS)) { drainInput(); Serial.printf("ERR %lu\n", (unsigned long)expected); continue; }
    uint32_t seq, crc;
    uint16_t len;
    memcpy(&seq, hdr, 4);
    memcpy(&len, hdr + 4, 2);
    memcpy(&crc, hdr + 6, 4);
    lastData = millis();
    if (len > CHUNK_MAX) { drainInput(); Serial.printf("ERR %lu\n", (unsigned long)expected); continue; }
    if (len == 0) {  // end of image
      free(z);
      z = nullptr;
      if (written != size || !Update.end(true)) return finishFailed(written != size ? "short" : Update.errorString());
      Serial.println("DONE");
      Serial.flush();
      otaScreenRestarting();
      delay(800);
      ESP.restart();
    }
    if (!readExact(data, len, BYTE_TIMEOUT_MS) || crc32(data, len) != crc) {
      drainInput();
      Serial.printf("ERR %lu\n", (unsigned long)expected);
      continue;
    }
    if (seq + 1 == expected) {  // our OK was lost: acknowledge again, don't write twice
      Serial.printf("OK %lu\n", (unsigned long)seq);
      continue;
    }
    if (seq != expected) { Serial.printf("ERR %lu\n", (unsigned long)expected); continue; }
    if (z) {
      received += len;
      if (!inflateChunk(z, data, len, received >= zsize, written)) { free(z); return finishFailed("corrupt stream"); }
    } else {
      if (Update.write(data, len) != len) return finishFailed(Update.errorString());
      written += len;
    }
    expected++;
    otaScreenProgress(written, size);
    Serial.printf("OK %lu\n", (unsigned long)seq);
  }
}

void usbUpdatePoll() {
  static char line[96];
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (n < sizeof(line) - 1) line[n++] = c;
      continue;
    }
    line[n] = 0;
    n = 0;
    unsigned long size = 0, baud = 0, zsize = 0;
    char md5[40] = "";
    if (sscanf(line, "YOSUPD %lu %39s %lu %lu", &size, md5, &baud, &zsize) >= 2 && size > 0)
      runUpdate(size, md5, baud, zsize);
  }
}
