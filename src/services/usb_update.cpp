#include "services/usb_update.h"
#include <Arduino.h>
#include "os/ota_screen.h"
#include "services/firmware_update.h"

// Protocol (host = tools/usb_update.py):
//   host:   "YOSVER\n"                          device: "YOSPROTO 3\n" (older firmware stays silent)
//   host:   "YOSUPD <size> <md5> <baud> [<zsize>]\n"   device: "READY\n" or "FAIL <reason>", then switches to <baud>
//           With <zsize>, the chunks carry a compressed stream of the image (about a third smaller): zlib,
//           or from protocol 3 on also gzip (firmware.bin.gz as built). It is inflated with the
//           decompressor in the chip's ROM. <size> and <md5> are always of the uncompressed image.
//   host:   'C' seq:u32 len:u16 crc32:u32 data[len]   device: "OK <seq>" or "ERR <expected seq>"
//   host:   'C' with len 0 = end                device: "WRITING", then "DONE" (reboots) or "FAIL <reason>"
//   host:   "ABRT" at any point                  device: "FAIL cancelled", back to 115200
// Protocol 2 receives the whole image into PSRAM first and only then writes flash. Flash erases stall
// the CPU (and the UART), so writing while receiving made the host wait and resend; with nothing else
// going on, the host can keep 16 chunks in flight and the line runs at full speed (1 Mbaud; this board's CH340 loses data above that).
// Chunks still carry a CRC, because this board's CH340 USB bridge can drop bytes.
// The write phase goes through firmware_update.cpp (header check, internal-RAM buffers, writer task).

static const uint32_t CHUNK_MAX = 1024, BYTE_TIMEOUT_MS = 2000, IDLE_ABORT_MS = 3000;  // the host never pauses this long mid-transfer
static const size_t RX_BUFFER_UPDATE = 32768, RX_BUFFER_NORMAL = 4096;  // 16 chunks in flight, plus room for screen redraws

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
  fwupdate::abort();
  Serial.printf("FAIL %s\n", why);
  serialRestart(RX_BUFFER_NORMAL, 115200);
  otaScreenFail();
}

// Progress across both phases: receiving is the first half of the ring, writing the second.
static void showProgress(uint32_t done, uint32_t total, bool writing) {
  otaScreenProgress((writing ? 500 : 0) + (uint32_t)((uint64_t)done * 500 / max<uint32_t>(total, 1)), 1000);
}

// Hands the received image to the writer (which inflates it if compressed) and switches the boot slot.
static bool writeImage(const uint8_t *image, uint32_t len, uint32_t size, const char *md5, const char **why) {
  static String err;
  if (!fwupdate::begin("USB", size, err)) { *why = err.c_str(); return false; }
  for (uint32_t ofs = 0; ofs < len; ofs += 4096) {
    if (!fwupdate::write(image + ofs, min<uint32_t>(4096, len - ofs))) { *why = fwupdate::error().c_str(); return false; }
    showProgress(fwupdate::imageBytes(), size, true);
  }
  if (fwupdate::imageBytes() != size) { *why = "short"; return false; }
  if (!fwupdate::finish(md5)) { *why = fwupdate::error().c_str(); return false; }
  return true;
}

static void runUpdate(uint32_t size, const char *md5, uint32_t baud, uint32_t zsize) {
  uint32_t len = zsize ? zsize : size;  // bytes that travel
  uint8_t *image = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!image) { Serial.println("FAIL no memory"); return; }
  // The writer starts only once everything is here: flash erases would stall the UART mid-transfer.
  String err;
  if (!fwupdate::canBegin(err)) {
    free(image);
    Serial.printf("FAIL %s\n", err.c_str());
    return;
  }
  otaScreenBegin("Installing over USB");
  Serial.println("READY");
  serialRestart(RX_BUFFER_UPDATE, baud ? baud : 115200);
  drainInput();

  static uint8_t data[CHUNK_MAX];
  uint32_t expected = 0, received = 0, lastData = millis(), r0 = millis();
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
      Serial.printf("STATS receive %lu ms\n", (unsigned long)(millis() - r0));
      const char *why = nullptr;
      uint32_t w0 = millis();
      if (!writeImage(image, len, size, md5, &why)) return finishFailed(why, image);
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
      Serial.println("YOSPROTO 3");
      continue;
    }
    unsigned long size = 0, baud = 0, zsize = 0;
    char md5[40] = "";
    if (sscanf(line, "YOSUPD %lu %39s %lu %lu", &size, md5, &baud, &zsize) >= 2 && size > 0)
      runUpdate(size, md5, baud, zsize);
  }
}
