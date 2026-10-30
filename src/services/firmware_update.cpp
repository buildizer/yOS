#include "services/firmware_update.h"
#include <Preferences.h>
#include <MD5Builder.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_format.h>
#include <esp_rom_crc.h>
#include <esp32s3/rom/miniz.h>
#include <lvgl.h>
#include "version.h"
#include "os/ui.h"

namespace fwupdate {
namespace {

const uint32_t BLOCK = 64 * 1024;             // erase unit (one block erase is much faster than 16 sectors)
const size_t BUF_SIZE = 16 * 1024, NBUF = 2;  // flash writes, internal RAM; while one is written the next fills
const uint32_t ERASE_AHEAD = 2 * BLOCK;       // erased ahead of the writes when the size isn't known
const size_t HEAD = 16;                       // written last, so a half-written slot never looks bootable
const size_t CHECK_BYTES = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
const char *NVS_NS = "yos_ota";

enum Format : uint8_t { UNKNOWN, RAW, ZLIB, GZIP };

// Streaming inflate state (PSRAM: the decompressor is ~11 KB and needs a 32 KB window).
struct Inflate {
  tinfl_decompressor d;
  uint8_t dict[TINFL_LZ_DICT_SIZE];
  size_t dictOfs;
};

struct Block {
  uint8_t *buf;
  uint32_t ofs, len;
};

volatile bool active = false;
String err, via, version;
const esp_partition_t *slot = nullptr;
Format fmt = UNKNOWN;
Inflate *z = nullptr;
bool zDone = false;
uint8_t gzHead[512];  // gzip header, kept until it is complete (a stored file name makes it longer)
size_t gzHeadLen = 0;
bool gzHeadDone = false;
uint8_t gzTail[8];  // the last 8 bytes that arrived: for gzip, CRC-32 and size of the image
size_t gzTailLen = 0;
uint32_t crc = 0;
MD5Builder md5;
bool checked = false;
uint8_t head[HEAD];

uint8_t *bufs[NBUF] = {};
size_t bufSize = 0;
uint8_t *cur = nullptr;  // being filled
uint32_t curLen = 0, produced = 0, sizeHint = 0;
QueueHandle_t full = nullptr, empty = nullptr;
SemaphoreHandle_t writerDone = nullptr;
TaskHandle_t writer = nullptr;
volatile bool writerFailed = false;

// Timing, reported by finish() (and by abort(), to see where a failed attempt spent its time).
uint32_t startMs, inBytes, inflateUs, waitUs;
volatile uint32_t eraseUs, writeUs, erasedTo;

bool setErr(const String &why) {
  if (err.isEmpty()) err = why;
  return false;
}

bool eraseNext() {
  uint32_t n = min(BLOCK - erasedTo % BLOCK, (uint32_t)slot->size - erasedTo);
  uint32_t t = micros();
  bool ok = esp_partition_erase_range(slot, erasedTo, n) == ESP_OK;
  eraseUs += micros() - t;
  erasedTo += n;
  return ok;
}

// Takes filled buffers, erases what they need (and a little ahead while idle) and writes them.
void writerTask(void *) {
  Block b;
  for (;;) {
    bool idleErase = !writerFailed && erasedTo < slot->size &&
                     erasedTo < (sizeHint ? sizeHint : produced + ERASE_AHEAD);
    if (xQueueReceive(full, &b, idleErase ? 0 : portMAX_DELAY) != pdTRUE) {
      if (!eraseNext()) writerFailed = true;
      continue;
    }
    if (!b.buf) break;  // finish() or abort()
    if (!writerFailed) {
      while (erasedTo < b.ofs + b.len && !writerFailed)
        if (!eraseNext()) writerFailed = true;
      uint32_t skip = b.ofs == 0 ? HEAD : 0;
      uint32_t t = micros();
      if (!writerFailed && esp_partition_write(slot, b.ofs + skip, b.buf + skip, b.len - skip) != ESP_OK)
        writerFailed = true;
      writeUs += micros() - t;
    }
    xQueueSend(empty, &b.buf, portMAX_DELAY);
  }
  xSemaphoreGive(writerDone);
  vTaskDelete(nullptr);
}

// The first bytes of the image: an ESP32-S3 app image built as yOS.
bool checkHeader(const uint8_t *p) {
  esp_image_header_t h;
  esp_app_desc_t d;
  memcpy(&h, p, sizeof(h));
  memcpy(&d, p + sizeof(h) + sizeof(esp_image_segment_header_t), sizeof(d));
  if (h.magic != ESP_IMAGE_HEADER_MAGIC) return setErr("This isn't a firmware image.");
  if (h.chip_id != ESP_CHIP_ID_ESP32S3) return setErr("This firmware is for a different chip.");
  if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) return setErr("This firmware has no app description.");
  d.project_name[sizeof(d.project_name) - 1] = 0;
  d.version[sizeof(d.version) - 1] = 0;
  if (strcmp(d.project_name, "yOS") != 0)
    return setErr(String("This firmware isn't yOS (it is \"") + d.project_name + "\").");
  version = d.version;
  Serial.printf("[fwupdate] image: yOS %s via %s\n", version.c_str(), via.c_str());
  return true;
}

bool sendCurrent() {
  if (!checked) {
    if (curLen < CHECK_BYTES) return setErr("The firmware image is too short.");
    if (!checkHeader(cur)) return false;
    checked = true;
    memcpy(head, cur, HEAD);
  }
  if (writerFailed) return setErr("Writing to flash failed.");
  Block b = {cur, produced - curLen, curLen};
  xQueueSend(full, &b, portMAX_DELAY);
  uint32_t t = micros();
  xQueueReceive(empty, &cur, portMAX_DELAY);
  waitUs += micros() - t;
  curLen = 0;
  return true;
}

// Image bytes (after inflating) into the current buffer.
bool put(const uint8_t *p, size_t n) {
  if (produced + n > slot->size) return setErr("The firmware is too big for this device.");
  for (size_t at = 0; at < n; at += 32768) md5.add((uint8_t *)p + at, min<size_t>(n - at, 32768));
  if (fmt == GZIP) crc = esp_rom_crc32_le(crc, p, n);
  while (n) {
    size_t k = min(n, bufSize - curLen);
    memcpy(cur + curLen, p, k);
    curLen += k;
    produced += k;
    p += k;
    n -= k;
    if (curLen == bufSize && !sendCurrent()) return false;
  }
  return true;
}

bool inflate(const uint8_t *in, size_t len) {
  // Runs until the input is used up and the window holds nothing more: tinfl can stop with all input
  // taken but output still pending (HAS_MORE_OUTPUT), which must be drained now, not with the next call.
  while (!zDone) {
    size_t inBytes = len, outBytes = TINFL_LZ_DICT_SIZE - z->dictOfs;
    int flags = TINFL_FLAG_HAS_MORE_INPUT | (fmt == ZLIB ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
    uint32_t t = micros();
    tinfl_status st = tinfl_decompress(&z->d, in, &inBytes, z->dict, z->dict + z->dictOfs, &outBytes, flags);
    inflateUs += micros() - t;
    in += inBytes;
    len -= inBytes;
    if (outBytes) {
      if (!put(z->dict + z->dictOfs, outBytes)) return false;
      z->dictOfs = (z->dictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
    }
    if (st < 0) return setErr("The compressed firmware is damaged.");
    if (st == TINFL_STATUS_DONE) zDone = true;
    else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && len == 0) break;
  }
  return true;
}

// Length of a complete gzip header in gzHead, or 0 while more bytes are needed.
size_t gzipHeaderLength() {
  const uint8_t *h = gzHead;
  size_t n = gzHeadLen, at = 10;
  if (n < at) return 0;
  uint8_t flags = h[3];
  if (flags & 4) {  // FEXTRA
    if (n < at + 2) return 0;
    at += 2 + (h[at] | h[at + 1] << 8);
  }
  for (int f : {8, 16}) {  // FNAME, FCOMMENT: zero-terminated
    if (!(flags & f)) continue;
    while (at < n && h[at]) at++;
    if (at >= n) return 0;
    at++;
  }
  if (flags & 2) at += 2;  // FHCRC
  return at <= n ? at : 0;
}

void release() {
  for (auto &b : bufs) {
    heap_caps_free(b);
    b = nullptr;
  }
  if (full) vQueueDelete(full);
  if (empty) vQueueDelete(empty);
  if (writerDone) vSemaphoreDelete(writerDone);
  full = empty = nullptr;
  writerDone = nullptr;
  free(z);
  z = nullptr;
  active = false;
}

// Stops the writer task after it has written everything queued.
bool stopWriter() {
  if (!writer) return true;
  Block b = {nullptr, 0, 0};
  xQueueSend(full, &b, portMAX_DELAY);
  xSemaphoreTake(writerDone, portMAX_DELAY);
  writer = nullptr;
  return !writerFailed;
}

void logStats(const char *result, uint32_t verifyMs) {
  Serial.printf("[fwupdate] STATS %s via %s: %lu B in -> %lu B image, total %lu ms "
                "(inflate %lu, waiting for flash %lu, erase %lu, write %lu, verify %lu ms)\n",
                result, via.c_str(), (unsigned long)inBytes, (unsigned long)produced,
                (unsigned long)(millis() - startMs), (unsigned long)(inflateUs / 1000),
                (unsigned long)(waitUs / 1000), (unsigned long)(eraseUs / 1000), (unsigned long)(writeUs / 1000),
                (unsigned long)verifyMs);
}

}  // namespace

bool canBegin(String &why) {
  if (active) {
    why = "Another update is being installed.";
    return false;
  }
  // Right after an update this version is still on trial; writing the other slot now would leave
  // nothing to go back to.
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    why = "yOS was just updated and is still checking itself. Try again in a minute.";
    return false;
  }
  return true;
}

bool begin(const char *how, uint32_t hint, String &why) {
  if (!canBegin(why)) return false;
  {
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    bool was = active;
    active = true;
    portEXIT_CRITICAL(&mux);
    if (was) {
      why = "Another update is being installed.";
      return false;
    }
  }
  err = version = "";
  via = how;
  slot = esp_ota_get_next_update_partition(nullptr);
  if (!slot) {
    why = "No free update slot.";
    active = false;
    return false;
  }
  // Internal RAM is scarce (Wi-Fi needs it too): 2 x 16 KB, or 2 x 4 KB when that isn't there.
  bufSize = BUF_SIZE;
  for (int tries = 0; tries < 2; tries++) {
    for (auto &b : bufs) b = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (bufs[0] && bufs[1]) break;
    for (auto &b : bufs) { heap_caps_free(b); b = nullptr; }
    bufSize = 4096;
  }
  full = xQueueCreate(NBUF + 1, sizeof(Block));
  empty = xQueueCreate(NBUF, sizeof(uint8_t *));
  writerDone = xSemaphoreCreateBinary();
  if (!bufs[0] || !full || !empty || !writerDone) {
    release();
    why = "Not enough memory for the update right now.";
    return false;
  }
  cur = bufs[0];
  xQueueSend(empty, &bufs[1], 0);
  curLen = produced = inBytes = inflateUs = waitUs = 0;
  eraseUs = writeUs = erasedTo = 0;
  sizeHint = hint ? min<uint32_t>((hint + BLOCK - 1) / BLOCK * BLOCK, slot->size) : 0;
  fmt = UNKNOWN;
  zDone = gzHeadDone = checked = writerFailed = false;
  gzHeadLen = gzTailLen = 0;
  crc = 0;
  md5.begin();
  startMs = millis();
  // Above the network stack's helpers and the GitHub task, below Wi-Fi and lwIP.
  if (xTaskCreatePinnedToCore(writerTask, "fwwrite", 4096, nullptr, 2, &writer, 0) != pdPASS) {
    writer = nullptr;
    release();
    why = "Not enough memory for the update right now.";
    return false;
  }
  Serial.printf("[fwupdate] begin via %s into %s, buffers %u x %u B\n", how, slot->label, (unsigned)NBUF,
                (unsigned)bufSize);
  return true;
}

bool write(const uint8_t *data, size_t len) {
  if (!active || !err.isEmpty()) return false;
  inBytes += len;
  if (fmt == UNKNOWN && len) {
    fmt = data[0] == 0x1F ? GZIP : data[0] == 0x78 ? ZLIB : RAW;
    if (fmt != RAW) {
      z = (Inflate *)heap_caps_malloc(sizeof(Inflate), MALLOC_CAP_SPIRAM);
      if (!z) return setErr("Not enough memory for the update right now.");
      tinfl_init(&z->d);
      z->dictOfs = 0;
    }
  }
  if (fmt == RAW) return put(data, len);
  // The decompressor may read a few bytes past the end of the stream, so the gzip trailer is taken
  // from the raw input: always its last 8 bytes.
  if (len >= sizeof(gzTail)) {
    memcpy(gzTail, data + len - sizeof(gzTail), sizeof(gzTail));
  } else {
    size_t keep = min(gzTailLen, sizeof(gzTail) - len);
    memmove(gzTail, gzTail + gzTailLen - keep, keep);
    memcpy(gzTail + keep, data, len);
    gzTailLen = keep;
  }
  gzTailLen = min(gzTailLen + len, sizeof(gzTail));
  if (fmt == GZIP && !gzHeadDone) {
    while (len && !gzHeadDone) {
      if (gzHeadLen == sizeof(gzHead)) return setErr("The compressed firmware is damaged.");
      gzHead[gzHeadLen++] = *data++;
      len--;
      size_t n = gzipHeaderLength();
      if (n) {
        if (gzHead[1] != 0x8B || gzHead[2] != 8) return setErr("The compressed firmware is damaged.");
        gzHeadDone = true;
      }
    }
  }
  return inflate(data, len);
}

bool finish(const char *md5Hex) {
  if (!active) return false;
  bool ok = err.isEmpty();
  if (ok && fmt != RAW && !zDone) ok = setErr("The firmware didn't arrive completely.");
  if (ok && fmt == GZIP) {
    uint32_t wantCrc, wantSize;
    memcpy(&wantCrc, gzTail, 4);
    memcpy(&wantSize, gzTail + 4, 4);
    if (gzTailLen < 8 || wantCrc != crc || wantSize != produced) ok = setErr("The firmware didn't arrive intact.");
  }
  if (ok && curLen) ok = sendCurrent();
  if (ok && !checked) ok = setErr("The firmware image is too short.");
  if (!stopWriter() && ok) ok = setErr("Writing to flash failed.");
  if (ok && md5Hex && strlen(md5Hex) == 32) {
    md5.calculate();
    if (!md5.toString().equalsIgnoreCase(md5Hex)) ok = setErr("The firmware didn't arrive intact (MD5).");
  }
  uint32_t t = millis();
  if (ok && esp_partition_write(slot, 0, head, HEAD) != ESP_OK) ok = setErr("Writing to flash failed.");
  if (ok) {
    // Noted before switching, so the next boot can tell whether this version came up.
    Preferences p;
    p.begin(NVS_NS, false);
    p.putString("ota_pending", version);
    p.putString("ota_slot", slot->label);
    p.end();
    // Checks the whole image (checksum and SHA-256) before switching.
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
      ok = setErr("The firmware image is damaged. Nothing was changed.");
      p.begin(NVS_NS, false);
      p.clear();
      p.end();
    }
  }
  logStats(ok ? "done" : "failed", millis() - t);
  if (!ok) Serial.printf("[fwupdate] %s\n", err.c_str());
  release();
  return ok;
}

void abort() {
  if (!active) return;
  setErr("Cancelled.");
  stopWriter();
  logStats("aborted", 0);
  Serial.printf("[fwupdate] %s\n", err.c_str());
  release();
}

const String &error() { return err; }
bool busy() { return active; }
uint32_t imageBytes() { return produced; }
String imageVersion() { return version; }

// ---------- after an update ----------

namespace {
bool confirmed = false;

bool onTrial() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}
}  // namespace

void bootCheck() {
  Preferences p;
  p.begin(NVS_NS, false);
  String pending = p.getString("ota_pending", ""), pendingSlot = p.getString("ota_slot", "");
  p.end();
  if (pending.isEmpty()) return;
  const char *running = esp_ota_get_running_partition()->label;
  if (pendingSlot == running && pending == YOS_FIRMWARE_VERSION) return;  // came up; confirmLater() clears it
  Serial.printf("[fwupdate] %s didn't start, rolled back to %s\n", pending.c_str(), YOS_FIRMWARE_VERSION);
  p.begin(NVS_NS, false);
  p.clear();
  p.end();
  // Shown once, a moment after the home screen is up.
  static String msg;
  msg = "yOS " + pending + " didn't start properly, so yOS " YOS_FIRMWARE_VERSION " was restored.";
  lv_timer_t *t = lv_timer_create([](lv_timer_t *) {
    ui::alert("Update failed \xE2\x80\x94 rolled back to " YOS_FIRMWARE_VERSION, msg.c_str(),
              {{"OK", ui::AlertAction::CANCEL, nullptr}});
  }, 1500, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

void confirmLater() {
  // An LVGL timer, so it only fires if the UI task is running its loop.
  lv_timer_t *t = lv_timer_create([](lv_timer_t *) {
    if (onTrial()) Serial.println("[fwupdate] this version runs fine: keeping it");
    esp_ota_mark_app_valid_cancel_rollback();
    confirmed = true;
    Preferences p;
    p.begin(NVS_NS, false);
    if (p.isKey("ota_pending")) p.clear();
    p.end();
  }, 30000, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

void loop() {
  static bool checkedTrial = false;
  if (confirmed || checkedTrial || millis() < 150000) return;
  checkedTrial = true;
  if (!onTrial()) return;
  // The UI never got to confirm (hung or stuck): go back to the version that worked.
  Serial.println("[fwupdate] the UI didn't confirm this version: rolling back");
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

}  // namespace fwupdate
