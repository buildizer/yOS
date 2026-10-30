#include "services/usb_update.h"
#include <Arduino.h>
#include <Update.h>
#include "os/ota_screen.h"
#include <esp32s3/rom/miniz.h>

// Protocol (host = tools/usb_update.py):
//   host:   "YOSVER\n"                          device: "YOSPROTO 2\n" (older firmware stays silent)
//   host:   "YOSUPD <size> <md5> <baud> [<zsize>]\n"   device: "READY\n", then switches to <baud>
//           With <zsize>, the chunks carry a zlib stream of the image (about a third smaller); it is
//           inflated with the decompressor in the chip's ROM. <size> and <md5> are always of the
//           uncompressed image.
//   host:   'C' seq:u32 len:u16 crc32:u32 data[len]   device: "OK <seq>" or "ERR <expected seq>"
//   host:   'C' with len 0 = end                device: "WRITING", then "DONE" (reboots) or "FAIL <reason>"
//   host:   "ABRT" at any point                  device: "FAIL cancelled", back to 115200
// Protocol 2 receives the whole image into PSRAM first and only then writes flash. Flash erases stall
// the CPU (and the UART), so writing while receiving made the host wait and resend; with nothing else
// going on, the host can keep 16 chunks in flight and the line runs at full speed (1 Mbaud; this board's CH340 loses data above that).
// Chunks still carry a CRC, because this board's CH340 USB bridge can drop bytes.

static const uint32_t CHUNK_MAX = 1024, BYTE_TIMEOUT_MS = 2000, IDLE_ABORT_MS = 3000;  // the host never pauses this long mid-transfer
static const size_t RX_BUFFER_UPDATE = 32768, RX_BUFFER_NORMAL = 4096;  // 16 chunks in flight, plus room for screen redraws

// Streaming inflate state (PSRAM: the decompressor is ~11 KB and needs a 32 KB window).
struct Inflate {
  tinfl_decompressor d;
  uint8_t dict[TINFL_LZ_DICT_SIZE];
  size_t dictOfs;
};

// Feeds one compressed chunk; writes whatever it produces to Update. False on a corrupt stream.
bool updateBeginFast(uint32_t size) {
  heap_caps_malloc_extmem_enable(SPI_FLASH_SEC_SIZE + 1);  // this allocation only: keep it internal
  bool ok = Update.begin(size);
  heap_caps_malloc_extmem_enable(256);                    // back to main.cpp's rule
  return ok;
}

static uint32_t flashUs = 0;  // time inside Update.write, reported as STATS for tuning

static bool inflateChunk(Inflate *z, const uint8_t *in, size_t len, bool last, uint32_t &written) {
  for (;;) {
    size_t inBytes = len, outBytes = TINFL_LZ_DICT_SIZE - z->dictOfs;
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
    tinfl_status st = tinfl_decompress(&z->d, in, &inBytes, z->dict, z->dict + z->dictOfs, &outBytes, flags);
    if (outBytes) {
      uint32_t t = micros();
      bool ok = Update.write(z->dict + z->dictOfs, outBytes) == outBytes;
      flashUs += micros() - t;
      if (!ok) return false;
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

// The UART driver can only change its receive buffer by restarting.
static void serialRestart(size_t rxBuffer, uint32_t baud) {
  Serial.flush();
  Serial.end();
  Serial.setRxBufferSize(rxBuffer);
  Serial.begin(baud);
}

static void finishFailed(const char *why, uint8_t *image) {
  free(image);
  Update.abort();
  Serial.printf("FAIL %s\n", why);
  serialRestart(RX_BUFFER_NORMAL, 115200);
  otaScreenFail();
}

// Progress across both phases: receiving is the first half of the ring, writing the second.
static void showProgress(uint32_t done, uint32_t total, bool writing) {
  otaScreenProgress((writing ? 500 : 0) + (uint32_t)((uint64_t)done * 500 / max<uint32_t>(total, 1)), 1000);
}

// Writes the received image (inflating it if compressed) and switches the boot slot.
static bool writeImage(const uint8_t *image, uint32_t len, bool compressed, uint32_t size, const char **why) {
  uint32_t written = 0;
  if (!compressed) {
    for (uint32_t ofs = 0; ofs < len; ofs += 4096) {
      uint32_t n = min<uint32_t>(4096, len - ofs);
      if (Update.write((uint8_t *)image + ofs, n) != n) { *why = Update.errorString(); return false; }
      written += n;
      showProgress(written, size, true);
    }
  } else {
    Inflate *z = (Inflate *)heap_caps_malloc(sizeof(Inflate), MALLOC_CAP_SPIRAM);
    if (!z) { *why = "no memory"; return false; }
    tinfl_init(&z->d);
    z->dictOfs = 0;
    bool ok = true;
    for (uint32_t ofs = 0; ok && ofs < len; ofs += 4096) {
      uint32_t n = min<uint32_t>(4096, len - ofs);
      ok = inflateChunk(z, image + ofs, n, ofs + n >= len, written);
      showProgress(written, size, true);
    }
    free(z);
    if (!ok) { *why = "corrupt stream"; return false; }
  }
  if (written != size) { *why = "short"; return false; }
  uint32_t t = millis();
  if (!Update.end(true)) { *why = Update.errorString(); return false; }
  Serial.printf("STATS flash %lu ms, verify %lu ms\n", (unsigned long)(flashUs / 1000), (unsigned long)(millis() - t));
  return true;
}

static void runUpdate(uint32_t size, const char *md5, uint32_t baud, uint32_t zsize) {
  uint32_t len = zsize ? zsize : size;  // bytes that travel
  uint8_t *image = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!image) { Serial.println("FAIL no memory"); return; }
  if (!updateBeginFast(size)) {
    free(image);
    Serial.printf("FAIL %s\n", Update.errorString());
    return;
  }
  if (strlen(md5) == 32) Update.setMD5(md5);
  otaScreenBegin("Installing over USB");
  Serial.println("READY");
  serialRestart(RX_BUFFER_UPDATE, baud ? baud : 115200);
  drainInput();

  static uint8_t data[CHUNK_MAX];
  uint32_t expected = 0, received = 0, lastData = millis();
  for (;;) {
    // Find the next chunk marker; anything else on the line is noise.
    int c = Serial.read();
    if (c < 0) {
      if (millis() - lastData > IDLE_ABORT_MS) return finishFailed("timeout", image);
      delay(1);
      continue;
    }
    if (c == 'A') {
      uint8_t rest[3];
      if (readExact(rest, 3, 200) && memcmp(rest, "BRT", 3) == 0) return finishFailed("cancelled", image);
      continue;
    }
    if (c != 'C') continue;
    uint8_t hdr[10];
    if (!readExact(hdr, sizeof(hdr), BYTE_TIMEOUT_MS)) { drainInput(); Serial.printf("ERR %lu\n", (unsigned long)expected); continue; }
    uint32_t seq, crc;
    uint16_t n;
    memcpy(&seq, hdr, 4);
    memcpy(&n, hdr + 4, 2);
    memcpy(&crc, hdr + 6, 4);
    lastData = millis();
    if (n > CHUNK_MAX) { drainInput(); Serial.printf("ERR %lu\n", (unsigned long)expected); continue; }
    if (n == 0) {  // end of image: everything is in PSRAM, now write flash
      if (seq != expected || received != len) { Serial.printf("ERR %lu\n", (unsigned long)expected); continue; }
      Serial.println("WRITING");
      Serial.flush();
      const char *why = nullptr;
      flashUs = 0;
      uint32_t w0 = millis();
      if (!writeImage(image, len, zsize != 0, size, &why)) return finishFailed(why, image);
      free(image);
      Serial.printf("STATS write phase %lu ms\n", (unsigned long)(millis() - w0));
      Serial.println("DONE");
      Serial.flush();
      otaScreenRestarting();
      delay(800);
      ESP.restart();
    }
    if (!readExact(data, n, BYTE_TIMEOUT_MS) || crc32(data, n) != crc) {
      drainInput();
      Serial.printf("ERR %lu\n", (unsigned long)expected);
      continue;
    }
    if (seq < expected) {  // a resend of something we have: acknowledge again, don't store twice
      Serial.printf("OK %lu\n", (unsigned long)seq);
      continue;
    }
    if (seq != expected || (uint64_t)seq * CHUNK_MAX + n > len) {
      Serial.printf("ERR %lu\n", (unsigned long)expected);
      continue;
    }
    memcpy(image + seq * CHUNK_MAX, data, n);
    received += n;
    expected++;
    if ((expected & 31) == 0 || received == len) showProgress(received, len, false);
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
    if (strcmp(line, "YOSVER") == 0) {
      Serial.println("YOSPROTO 2");
      continue;
    }
    unsigned long size = 0, baud = 0, zsize = 0;
    char md5[40] = "";
    if (sscanf(line, "YOSUPD %lu %39s %lu %lu", &size, md5, &baud, &zsize) >= 2 && size > 0)
      runUpdate(size, md5, baud, zsize);
  }
}
