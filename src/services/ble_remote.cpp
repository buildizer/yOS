// Phone remote over Bluetooth LE: a JSON command characteristic and a raw data characteristic in the
// yOS service, both usable only over an encrypted, passkey-authenticated link. Protocol: README.md.
//
// Threads: NimBLE calls onWrite() on its host task, which only copies bytes (commands into a queue,
// upload data into a PSRAM ring). A worker task runs the commands and streams the files; it exists
// only while there is something to do. UI requests wait in a small list that the LVGL thread polls
// (settings/bluetooth.cpp), like the passkey prompt, so nothing here touches LVGL objects.
#include "services/ble_remote.h"
#include "services/ble.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "version.h"
#include "os/hal.h"
#include "os/os.h"
#include "os/prefs.h"
#include "services/storage.h"

namespace ble {
namespace {
const char *CMD_UUID = "7e5a0004-4f53-4c59-9a1d-79a05b1e0001";   // write: JSON command; notify/read: replies
const char *DATA_UUID = "7e5a0005-4f53-4c59-9a1d-79a05b1e0001";  // raw file bytes, both ways
const size_t CMD_MAX = 512;              // a command, and the readable copy of the last reply
const uint32_t RING = 16 * 1024;         // upload bytes received but not yet written (PSRAM)
const uint32_t WINDOW = 8 * 1024;        // a phone may send this far past the last ack
const uint32_t ACK_EVERY = 4 * 1024;
const size_t IO_BUF = 4096;              // one file read / write (PSRAM)
const uint32_t STALL_MS = 15000;         // a transfer that makes no progress this long is dropped
const uint32_t IDLE_EXIT_MS = 30000;     // the worker ends after this long with nothing to do
const uint32_t PUT_MAX = 32UL << 20;     // largest upload
const uint64_t FLASH_KEEP = 128 * 1024;  // internal flash left free after an upload
const uint64_t SD_KEEP = 64 * 1024;
const size_t PATH_LEN = 120;
const int LIST_MAX = 200;                // entries in one "files" reply
const int REQ_MAX = 4;

enum Dir : uint8_t { T_NONE, T_GET, T_PUT };

// Shared between the host task, the worker and the UI (guarded by rmtx)
SemaphoreHandle_t rmtx = nullptr;
QueueHandle_t cmds = nullptr;            // char* in PSRAM; the worker frees them
bool workerAlive = false;
NimBLECharacteristic *cmdChr = nullptr, *dataChr = nullptr;  // null while the stack is down
uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
RemoteRequest reqs[REQ_MAX];
int reqCount = 0;
struct Xfer {
  Dir dir;
  bool onSd;
  bool abort;            // cancelled, or the phone / stack went away
  char path[PATH_LEN + 1];
  char name[32];
  uint32_t size, done;   // done: sent (get) / written (put)
  uint8_t *ring;         // put: filled by the host task
  uint32_t head, tail;   // put: bytes received / taken out of the ring
  bool overflow;         // put: the phone ignored the window
} x = {};

// Worker-only
File file;
uint8_t *io = nullptr;
uint32_t lastAck = 0, lastMove = 0;

struct RLock {
  RLock() { xSemaphoreTake(rmtx, portMAX_DELAY); }
  ~RLock() { xSemaphoreGive(rmtx); }
};

// ---------- link ----------

// Largest payload one notification carries (0 when the link is gone).
size_t payload() {
  uint16_t mtu = conn == BLE_HS_CONN_HANDLE_NONE ? 0 : ble_att_mtu(conn);
  return mtu > 3 ? min<size_t>(mtu - 3, 512) : 0;
}

// Sends bytes as notifications of at most MTU-3 each, waiting while the stack is out of buffers.
bool notifyAll(bool data, const uint8_t *p, size_t n) {
  size_t off = 0;
  uint32_t t0 = millis();
  while (off < n) {
    int rc;
    {
      RLock l;
      NimBLECharacteristic *c = data ? dataChr : cmdChr;
      size_t max = payload();
      if (!c || !max) return false;
      size_t len = min(n - off, max);
      os_mbuf *om = ble_hs_mbuf_from_flat(p + off, len);
      rc = om ? ble_gattc_notify_custom(conn, c->getHandle(), om) : BLE_HS_ENOMEM;  // consumes om
      if (rc == 0) off += len;
    }
    if (rc == 0) {
      t0 = millis();
      continue;
    }
    if (rc != BLE_HS_ENOMEM || millis() - t0 > STALL_MS) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

// One reply: a line of JSON on the command characteristic. Replies (not events) are also kept there
// for a plain read, cut to 512 bytes.
void send(JsonDocument &doc, bool keep = true) {
  String s;
  serializeJson(doc, s);
  if (keep) {
    RLock l;
    if (cmdChr) cmdChr->setValue((const uint8_t *)s.c_str(), min<size_t>(s.length(), CMD_MAX));
  }
  s += '\n';
  notifyAll(false, (const uint8_t *)s.c_str(), s.length());
}

// ---------- storage ----------

// Paths the phone may name: absolute, short, no "..", no backslash or control characters, no empty
// segment. Each root is its own file system ("/" is its top), so nothing else can be reached.
bool validPath(const char *p, bool allowRoot) {
  size_t n = strlen(p);
  if (n == 0 || n > PATH_LEN || p[0] != '/') return false;
  if (n == 1) return allowRoot;
  if (p[n - 1] == '/' || strstr(p, "//") || strstr(p, "..")) return false;
  for (const char *c = p; *c; c++)
    if ((unsigned char)*c < 0x20 || *c == '\\' || *c == 0x7f) return false;
  return true;
}

bool flashReady() { return LittleFS.totalBytes() > 0; }

// Runs fn with the root's file system, holding the SD lock for the card.
bool withFs(bool sd, std::function<void(fs::FS &)> fn) {
  if (sd ? !storageReady() : !flashReady()) return false;
  if (sd) storageWithLock([&] { fn(SD); });
  else fn(LittleFS);
  return true;
}

bool parseRoot(JsonDocument &req, bool &sd, const char *&path, bool allowRoot, String &err) {
  const char *root = req["root"] | "";
  path = req["path"] | "";
  if (!strcmp(root, "sd")) sd = true;
  else if (!strcmp(root, "flash")) sd = false;
  else {
    err = "root must be \"flash\" or \"sd\"";
    return false;
  }
  if (!validPath(path, allowRoot)) {
    err = "bad path";
    return false;
  }
  if (sd ? !storageReady() : !flashReady()) {
    err = sd ? "no SD card" : "internal flash not mounted";
    return false;
  }
  return true;
}

void freeSpace(bool sd, uint64_t &total, uint64_t &used) {
  withFs(sd, [&](fs::FS &) {
    total = sd ? SD.totalBytes() : LittleFS.totalBytes();
    used = sd ? SD.usedBytes() : LittleFS.usedBytes();
  });
}

const char *baseName(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

// ---------- transfers ----------

bool busy() {
  RLock l;
  return x.dir != T_NONE;
}

// Closes the file, keeps or drops an upload, and reports the result.
void endTransfer(bool ok, const char *why) {
  Dir dir;
  bool sd;
  char path[PATH_LEN + 1], part[PATH_LEN + 6];
  uint32_t size;
  {
    RLock l;
    dir = x.dir;
    sd = x.onSd;
    size = x.size;
    snprintf(path, sizeof(path), "%s", x.path);
  }
  snprintf(part, sizeof(part), "%s.part", path);
  withFs(sd, [&](fs::FS &fs) {
    if (file) file.close();
    if (dir != T_PUT) return;
    if (ok) {
      fs.remove(path);  // FAT can't rename over a file
      ok = fs.rename(part, path);
      if (!ok) why = "couldn't save the file";
    }
    if (!ok) fs.remove(part);
  });
  free(io);
  io = nullptr;
  {
    RLock l;
    free(x.ring);
    x = {};
  }
  JsonDocument res;
  res["cmd"] = dir == T_GET ? "get_done" : "put_done";
  res["ok"] = ok;
  res["path"] = path;
  if (ok) res["size"] = size;
  else res["error"] = why;
  send(res);
  Serial.printf("[remote] %s %s: %s\n", dir == T_GET ? "get" : "put", path, ok ? "done" : why);
}

void stepGet() {
  size_t chunk;
  uint32_t left;
  {
    RLock l;
    chunk = payload();
    left = x.size - x.done;
  }
  if (!chunk) return endTransfer(false, "connection lost");
  if (!left) return endTransfer(true, nullptr);
  // A file read fills a few notifications; then the worker looks at its queue again.
  size_t want = min<size_t>(left, IO_BUF / chunk * chunk);
  int n = 0;
  withFs(x.onSd, [&](fs::FS &) { n = file.read(io, want); });
  if (n <= 0) return endTransfer(false, "read error");
  if (!notifyAll(true, io, n)) return endTransfer(false, "connection lost");
  RLock l;
  x.done += n;
}

void stepPut() {
  uint32_t n, tail;
  bool overflow;
  {
    RLock l;
    overflow = x.overflow;
    tail = x.tail;
    n = min<uint32_t>(x.head - x.tail, IO_BUF);
    uint32_t at = tail % RING, first = min(n, RING - at);
    memcpy(io, x.ring + at, first);
    memcpy(io + first, x.ring, n - first);
  }
  if (overflow) return endTransfer(false, "data sent past the window");
  if (n == 0) {
    if (x.done == x.size) return endTransfer(true, nullptr);
    if (millis() - lastMove > STALL_MS) return endTransfer(false, "timed out waiting for data");
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }
  size_t w = 0;
  withFs(x.onSd, [&](fs::FS &) { w = file.write(io, n); });
  if (w != n) return endTransfer(false, "write failed (storage full?)");
  uint32_t done;
  {
    RLock l;
    x.tail += n;
    x.done += n;
    done = x.done;
  }
  lastMove = millis();
  if (done - lastAck >= ACK_EVERY || done == x.size) {
    lastAck = done;
    JsonDocument ev;
    ev["ev"] = "ack";
    ev["done"] = done;
    send(ev, false);
  }
}

void stepTransfer() {
  bool abort;
  Dir dir;
  {
    RLock l;
    abort = x.abort;
    dir = x.dir;
  }
  if (abort) return endTransfer(false, "cancelled");
  if (dir == T_GET) stepGet();
  else if (dir == T_PUT) stepPut();
}

bool startGet(JsonDocument &req, JsonDocument &res, String &err) {
  bool sd;
  const char *path;
  if (!parseRoot(req, sd, path, false, err)) return false;
  if (busy()) return err = "another transfer is running", false;
  io = (uint8_t *)heap_caps_malloc(IO_BUF, MALLOC_CAP_SPIRAM);
  if (!io) return err = "out of memory", false;
  uint32_t size = 0;
  withFs(sd, [&](fs::FS &fs) {
    file = fs.open(path, FILE_READ);
    if (file && file.isDirectory()) file.close();
    if (file) size = file.size();
  });
  if (!file) {
    free(io);
    io = nullptr;
    return err = "no such file", false;
  }
  {
    RLock l;
    x = {};
    x.dir = T_GET;
    x.onSd = sd;
    x.size = size;
    snprintf(x.path, sizeof(x.path), "%s", path);
    snprintf(x.name, sizeof(x.name), "%s", baseName(path));
  }
  lastMove = millis();
  res["size"] = size;
  return true;
}

bool startPut(JsonDocument &req, JsonDocument &res, String &err) {
  bool sd;
  const char *path;
  if (!parseRoot(req, sd, path, false, err)) return false;
  if (!req["size"].is<uint32_t>()) return err = "size missing", false;
  uint32_t size = req["size"];
  if (size > PUT_MAX) return err = "file too large (32 MB max)", false;
  if (busy()) return err = "another transfer is running", false;
  uint64_t total = 0, used = 0;
  freeSpace(sd, total, used);
  if (used + size + (sd ? SD_KEEP : FLASH_KEEP) > total) return err = "not enough free space", false;
  io = (uint8_t *)heap_caps_malloc(IO_BUF, MALLOC_CAP_SPIRAM);
  uint8_t *ring = (uint8_t *)heap_caps_malloc(RING, MALLOC_CAP_SPIRAM);
  if (!io || !ring) {
    free(io);
    free(ring);
    io = nullptr;
    return err = "out of memory", false;
  }
  char part[PATH_LEN + 6];
  snprintf(part, sizeof(part), "%s.part", path);
  withFs(sd, [&](fs::FS &fs) {
    bool isDir = false;
    if (fs.exists(path)) {
      File d = fs.open(path);
      isDir = d && d.isDirectory();
      d.close();
    }
    if (!isDir) file = fs.open(part, FILE_WRITE);  // fails when the folder doesn't exist
  });
  if (!file) {
    free(io);
    free(ring);
    io = nullptr;
    return err = "can't create the file (is it a folder?)", false;
  }
  {
    RLock l;
    x = {};
    x.dir = T_PUT;
    x.onSd = sd;
    x.size = size;
    x.ring = ring;
    snprintf(x.path, sizeof(x.path), "%s", path);
    snprintf(x.name, sizeof(x.name), "%s", baseName(path));
  }
  lastAck = 0;
  lastMove = millis();
  res["window"] = WINDOW;
  RLock l;
  res["chunk"] = payload();
  return true;
}

// ---------- commands ----------

bool listFiles(JsonDocument &req, JsonDocument &res, String &err) {
  bool sd;
  const char *path;
  if (!parseRoot(req, sd, path, true, err)) return false;
  bool found = false;
  JsonArray list = res["entries"].to<JsonArray>();
  withFs(sd, [&](fs::FS &fs) {
    if (!fs.exists(path)) return;  // (LittleFS logs an error for opening a missing path)
    File d = fs.open(path);
    if (!d || !d.isDirectory()) return;
    found = true;
    int n = 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (n++ == LIST_MAX) {
        res["more"] = true;
        f.close();
        break;
      }
      JsonObject o = list.add<JsonObject>();
      o["name"] = f.name();
      o["dir"] = f.isDirectory();
      if (!f.isDirectory()) o["size"] = f.size();
      f.close();
    }
    d.close();
  });
  if (!found) return err = "no such folder", false;
  uint64_t total = 0, used = 0;
  freeSpace(sd, total, used);
  res["total"] = total;
  res["used"] = used;
  return true;
}

bool removePath(JsonDocument &req, String &err) {
  bool sd;
  const char *path;
  if (!parseRoot(req, sd, path, false, err)) return false;
  {
    RLock l;
    if (x.dir != T_NONE && x.onSd == sd && !strcmp(x.path, path)) return err = "file is being transferred", false;
  }
  bool ok = false;
  withFs(sd, [&](fs::FS &fs) {
    if (!fs.exists(path)) return;
    File f = fs.open(path);
    if (!f) return;
    bool isDir = f.isDirectory();
    f.close();
    ok = isDir ? fs.rmdir(path) : fs.remove(path);
  });
  if (!ok) err = "couldn't delete (missing, or a folder that isn't empty)";
  return ok;
}

bool makeDir(JsonDocument &req, String &err) {
  bool sd;
  const char *path;
  if (!parseRoot(req, sd, path, false, err)) return false;
  bool ok = false;
  withFs(sd, [&](fs::FS &fs) { ok = fs.mkdir(path); });
  if (!ok) err = "couldn't create the folder";
  return ok;
}

void info(JsonDocument &res) {
  res["name"] = "yOS";
  res["version"] = YOS_FIRMWARE_VERSION;
  res["uptime"] = millis() / 1000;
  JsonObject heap = res["heap"].to<JsonObject>();
  heap["free"] = ESP.getFreeHeap();
  heap["internal"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  heap["psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  res["battery"] = nullptr;  // mains powered: no battery to report
  JsonObject w = res["wifi"].to<JsonObject>();
  bool up = WiFi.status() == WL_CONNECTED;
  w["connected"] = up;
  if (up) {
    w["ssid"] = WiFi.SSID();
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  res["brightness"] = ((prefs::brightness() - 10) * 100 + 122) / 245;  // percent, as "brightness" takes it
  res["screen"] = !hal::screenAsleep();
  res["sd"] = storageReady();
  RLock l;
  res["mtu"] = payload() + 3;
}

void listApps(JsonDocument &res) {
  JsonArray list = res["apps"].to<JsonArray>();
  hal::lock();
  App *open = os::current();
  for (App *a : os::apps()) {
    JsonObject o = list.add<JsonObject>();
    o["id"] = a->id();
    o["name"] = a->name();
    o["running"] = a->running;
    o["open"] = a == open;
  }
  hal::unlock();
}

bool queueRequest(const RemoteRequest &r, String &err) {
  RLock l;
  if (reqCount == REQ_MAX) return err = "busy, try again", false;
  reqs[reqCount++] = r;
  return true;
}

bool uiCommand(const char *cmd, JsonDocument &req, String &err) {
  RemoteRequest r = {};
  if (!strcmp(cmd, "home")) {
    r.action = RA_HOME;
  } else if (!strcmp(cmd, "open")) {
    const char *id = req["id"] | "";
    hal::lock();
    bool known = os::find(id) != nullptr;
    hal::unlock();
    if (!known) return err = "no such app", false;
    r.action = RA_OPEN;
    snprintf(r.app, sizeof(r.app), "%s", id);
  } else if (!strcmp(cmd, "brightness")) {
    if (!req["level"].is<int>()) return err = "level 0-100 missing", false;
    r.action = RA_BRIGHTNESS;
    r.value = 10 + constrain(req["level"].as<int>(), 0, 100) * 245 / 100;
    r.save = req["save"] | true;
  } else {  // screen
    if (!req["on"].is<bool>()) return err = "on (true/false) missing", false;
    r.action = RA_SCREEN;
    r.value = req["on"].as<bool>();
  }
  return queueRequest(r, err);
}

void run(const char *text) {
  JsonDocument req, res;
  String err;
  bool ok = false;
  if (deserializeJson(req, text) || !req.is<JsonObject>()) {
    res["cmd"] = "";
    err = "bad JSON";
  } else {
    const char *cmd = req["cmd"] | "";
    res["cmd"] = cmd;
    // "tag" comes back unchanged, so a client can match replies to requests ("id" is the app id).
    if (!req["tag"].isNull()) res["tag"] = req["tag"];
    if (!strcmp(cmd, "info")) {
      info(res);
      ok = true;
    } else if (!strcmp(cmd, "apps")) {
      listApps(res);
      ok = true;
    } else if (!strcmp(cmd, "home") || !strcmp(cmd, "open") || !strcmp(cmd, "brightness") || !strcmp(cmd, "screen"))
      ok = uiCommand(cmd, req, err);
    else if (!strcmp(cmd, "files")) ok = listFiles(req, res, err);
    else if (!strcmp(cmd, "get")) ok = startGet(req, res, err);
    else if (!strcmp(cmd, "put")) ok = startPut(req, res, err);
    else if (!strcmp(cmd, "delete")) ok = removePath(req, err);
    else if (!strcmp(cmd, "mkdir")) ok = makeDir(req, err);
    else if (!strcmp(cmd, "cancel")) {
      RLock l;
      x.abort = true;  // the worker ends it (and reports get_done / put_done) on its next step
      ok = true;
    } else err = "unknown command";
  }
  res["ok"] = ok;
  if (!ok) res["error"] = err;
  send(res);
}

void worker(void *) {
  uint32_t idleSince = millis();
  for (;;) {
    char *text = nullptr;
    bool moving = busy();
    if (xQueueReceive(cmds, &text, moving ? 0 : pdMS_TO_TICKS(500)) == pdTRUE) {
      run(text);
      free(text);
      idleSince = millis();
      continue;
    }
    if (moving) {
      stepTransfer();
      idleSince = millis();
      continue;
    }
    RLock l;
    if (x.dir == T_NONE && !uxQueueMessagesWaiting(cmds) && millis() - idleSince > IDLE_EXIT_MS) {
      workerAlive = false;
      break;
    }
  }
  vTaskDelete(nullptr);
}

// ---------- GATT callbacks (NimBLE host task: copy and return) ----------

// The stack already refuses these writes on a link that isn't encrypted and passkey-authenticated
// (SECURE_WRITE); checking again costs nothing.
bool trusted(const ble_gap_conn_desc *d) { return d && d->sec_state.encrypted && d->sec_state.authenticated; }

class CmdCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *d) override {
    if (!trusted(d)) return;
    NimBLEAttValue v = c->getValue();
    if (!v.size() || v.size() > CMD_MAX) return;
    char *text = (char *)heap_caps_malloc(v.size() + 1, MALLOC_CAP_SPIRAM);
    if (!text) return;
    memcpy(text, v.data(), v.size());
    text[v.size()] = 0;
    RLock l;
    conn = d->conn_handle;
    if (!workerAlive) {
      // 6 KB of internal RAM while it runs: file systems and SD are no place for a PSRAM stack.
      workerAlive = xTaskCreatePinnedToCore(worker, "bleremote", 6144, nullptr, 1, nullptr, 0) == pdPASS;
    }
    if (!workerAlive || xQueueSend(cmds, &text, 0) != pdTRUE) free(text);
  }
};
CmdCb cmdCb;

class DataCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *d) override {
    if (!trusted(d)) return;
    NimBLEAttValue v = c->getValue();
    uint32_t n = v.size();
    RLock l;
    if (x.dir != T_PUT || x.abort || !x.ring) return;
    if (x.head - x.tail + n > RING || x.head + n > x.size) {
      x.overflow = true;
      return;
    }
    uint32_t at = x.head % RING, first = min(n, RING - at);
    memcpy(x.ring + at, v.data(), first);
    memcpy(x.ring, v.data() + first, n - first);
    x.head += n;
  }
};
DataCb dataCb;
}  // namespace

namespace remote {
void init() {
  rmtx = xSemaphoreCreateMutex();
  cmds = xQueueCreate(6, sizeof(char *));
}

void addTo(NimBLEService *svc) {
  NimBLECharacteristic *c = svc->createCharacteristic(CMD_UUID, SECURE_READ | SECURE_WRITE | NIMBLE_PROPERTY::NOTIFY);
  c->setCallbacks(&cmdCb);
  NimBLECharacteristic *d = svc->createCharacteristic(
      DATA_UUID, SECURE_WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  d->setCallbacks(&dataCb);
  RLock l;
  cmdChr = c;
  dataChr = d;
}

void detach() {
  RLock l;
  cmdChr = dataChr = nullptr;
  conn = BLE_HS_CONN_HANDLE_NONE;
  x.abort = true;
}

void peerGone() {
  RLock l;
  conn = BLE_HS_CONN_HANDLE_NONE;
  x.abort = true;
}
}  // namespace remote

bool takeRemoteRequest(RemoteRequest &out) {
  if (!rmtx) return false;
  RLock l;
  if (!reqCount) return false;
  out = reqs[0];
  memmove(reqs, reqs + 1, --reqCount * sizeof(RemoteRequest));
  return true;
}

Transfer transfer() {
  Transfer t = {};
  if (!rmtx) return t;
  RLock l;
  t.active = x.dir != T_NONE;
  t.upload = x.dir == T_PUT;
  snprintf(t.name, sizeof(t.name), "%s", x.name);
  t.done = x.done;
  t.total = x.size;
  return t;
}
}  // namespace ble
