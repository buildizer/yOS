// HID over GATT: the board is also a Bluetooth keyboard, media remote and mouse for a paired phone
// (iPhone Remote app). The HID, battery and device information services sit next to the yOS service
// on the one GATT server and use its passkey pairing; the input reports need an encrypted link.
//
// Threads: the UI calls hidKey() & co., which only queue events (pointer motion is merged into one
// pending delta). A worker task sends each press, waits, then sends the release, so a typed string
// never blocks LVGL. It exists only while there is something to send, like the remote's worker.
#include "services/ble_hid.h"
#include "services/ble.h"
#include <Arduino.h>
#include <NimBLEHIDDevice.h>
#include <esp_heap_caps.h>
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"

namespace ble {
namespace {
const uint8_t ID_KEYS = 1, ID_MEDIA = 2, ID_MOUSE = 3;
const uint32_t KEY_MS = 12;           // between a press and its release, and between keys
const uint32_t CLICK_MS = 40;
const uint32_t MOVE_MS = 15;          // pointer reports at most this often (about one per connection event)
const uint32_t SEND_WAIT_MS = 500;    // a report the stack has no buffer for is dropped after this long
const uint32_t IDLE_EXIT_MS = 30000;  // the worker ends after this long with nothing to do
const size_t TEXT_MAX = 256;
const int MOVE_MAX = 2000;            // pending pointer motion is capped, so a stalled link can't wind it up

// Report 1: boot-layout keyboard (modifiers, reserved, 6 keys) with the LED output report.
// Report 2: one consumer-control usage. Report 3: 3-button relative mouse with a wheel.
const uint8_t REPORT_MAP[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, ID_KEYS,   // Generic Desktop, Keyboard, collection, report 1
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,      //   modifiers: 8 bits
  0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                  //   reserved byte
  0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,      //   LEDs out: 5 bits + 3 padding
  0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,      //   6 key codes 0..0x65
  0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
  0xC0,
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, ID_MEDIA,  // Consumer, Consumer Control, report 2
  0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,  // one 16-bit usage 0..0x3FF
  0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
  0xC0,
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, ID_MOUSE,  // Generic Desktop, Mouse, report 3
  0x09, 0x01, 0xA1, 0x00,                              //   Pointer, physical collection
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,      //     3 buttons + 5 padding
  0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,      //     X, Y, wheel: -127..127, relative
  0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0xC0, 0xC0,
};

enum Kind : uint8_t { H_KEY, H_MEDIA, H_TEXT, H_MOVE, H_CLICK };
struct Ev {
  Kind kind;
  uint8_t mods;
  uint16_t code;   // H_KEY: keyboard usage, H_MEDIA: consumer usage
  char *text;      // H_TEXT: PSRAM copy, freed by the worker
};

// Shared between the host task, the worker and the UI (guarded by hmtx)
SemaphoreHandle_t hmtx = nullptr;
QueueHandle_t events = nullptr;
bool workerAlive = false;
NimBLEHIDDevice *dev = nullptr;       // only the wrapper: the stack owns (and deinit deletes) its services
NimBLECharacteristic *keysChr = nullptr, *mediaChr = nullptr, *mouseChr = nullptr;  // null while down
uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
bool secured = false;                 // the link is encrypted
bool listening = false;               // ...and the phone subscribed to key reports
int moveX = 0, moveY = 0;
bool movePending = false;

struct HLock {
  HLock() { xSemaphoreTake(hmtx, portMAX_DELAY); }
  ~HLock() { xSemaphoreGive(hmtx); }
};

// Runs on the NimBLE host task. A bonded phone's subscriptions are restored on reconnect, which
// also lands here.
class KeysCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, ble_gap_conn_desc *desc, uint16_t subValue) override {
    HLock l;
    conn = desc->conn_handle;
    listening = subValue & 1;
  }
};
KeysCb keysCb;

// ---------- worker ----------

// One input report as a notification, waiting briefly while the stack is out of buffers.
bool send(NimBLECharacteristic *const *which, const uint8_t *p, size_t n) {
  uint32_t t0 = millis();
  for (;;) {
    int rc;
    {
      HLock l;
      NimBLECharacteristic *c = *which;
      if (!c || conn == BLE_HS_CONN_HANDLE_NONE || !secured) return false;
      os_mbuf *om = ble_hs_mbuf_from_flat(p, n);
      rc = om ? ble_gattc_notify_custom(conn, c->getHandle(), om) : BLE_HS_ENOMEM;  // consumes om
    }
    if (rc == 0) return true;
    if (rc != BLE_HS_ENOMEM || millis() - t0 > SEND_WAIT_MS) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool keyReport(uint8_t mods, uint8_t key) {
  uint8_t r[8] = {mods, 0, key};
  return send(&keysChr, r, sizeof(r));
}

// Press, wait, release, wait. The release is sent even if the press failed, so no key can stick.
bool tap(uint8_t mods, uint8_t key) {
  bool ok = keyReport(mods, key);
  vTaskDelay(pdMS_TO_TICKS(KEY_MS));
  ok = keyReport(0, 0) && ok;
  vTaskDelay(pdMS_TO_TICKS(KEY_MS));
  return ok;
}

// US layout: the key and whether it needs Shift. False for anything a US keyboard can't type.
bool asciiKey(char c, uint8_t &key, bool &shift) {
  static const char plain[] = "-=[]\\;'`,./";
  static const char shifted[] = "_+{}|:\"~<>?";
  static const uint8_t codes[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
  static const char digitsShifted[] = ")!@#$%^&*(";  // shifted 0..9
  shift = false;
  if (c >= 'a' && c <= 'z') key = 0x04 + (c - 'a');
  else if (c >= 'A' && c <= 'Z') { key = 0x04 + (c - 'A'); shift = true; }
  else if (c >= '1' && c <= '9') key = 0x1E + (c - '1');
  else if (c == '0') key = 0x27;
  else if (c == '\n') key = 0x28;
  else if (c == '\t') key = 0x2B;
  else if (c == ' ') key = 0x2C;
  else if (const char *s = strchr(plain, c)) key = codes[s - plain];
  else if (const char *s = strchr(shifted, c)) { key = codes[s - shifted]; shift = true; }
  else if (const char *s = strchr(digitsShifted, c)) { key = s == digitsShifted ? 0x27 : 0x1E + (s - digitsShifted - 1); shift = true; }
  else return false;
  return true;
}

void typeText(const char *t) {
  for (; *t; t++) {
    uint8_t key;
    bool shift;
    if (!asciiKey(*t, key, shift)) continue;
    if (!tap(shift ? MOD_SHIFT : 0, key)) return;  // link gone: drop the rest
  }
}

// Sends the pending pointer motion in steps of at most 127, until nothing is left.
void flushMove() {
  for (;;) {
    int8_t dx, dy;
    {
      HLock l;
      if (!movePending) return;
      dx = constrain(moveX, -127, 127);
      dy = constrain(moveY, -127, 127);
      moveX -= dx;
      moveY -= dy;
      movePending = moveX || moveY;
    }
    uint8_t r[4] = {0, (uint8_t)dx, (uint8_t)dy, 0};
    if (!send(&mouseChr, r, sizeof(r))) {
      HLock l;
      moveX = moveY = 0;
      movePending = false;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(MOVE_MS));
  }
}

void run(const Ev &e) {
  switch (e.kind) {
    case H_KEY: tap(e.mods, e.code); break;
    case H_MEDIA: {
      uint8_t r[2] = {(uint8_t)e.code, (uint8_t)(e.code >> 8)};
      send(&mediaChr, r, 2);
      vTaskDelay(pdMS_TO_TICKS(KEY_MS));
      r[0] = r[1] = 0;
      send(&mediaChr, r, 2);
      break;
    }
    case H_TEXT:
      typeText(e.text);
      free(e.text);
      break;
    case H_MOVE: flushMove(); break;
    case H_CLICK: {
      flushMove();   // click where the pointer ends up
      uint8_t r[4] = {1, 0, 0, 0};
      send(&mouseChr, r, 4);
      vTaskDelay(pdMS_TO_TICKS(CLICK_MS));
      r[0] = 0;
      send(&mouseChr, r, 4);
      break;
    }
  }
}

void worker(void *) {
  uint32_t idleSince = millis();
  for (;;) {
    Ev e;
    if (xQueueReceive(events, &e, pdMS_TO_TICKS(250)) == pdTRUE) {
      run(e);
      idleSince = millis();
      continue;
    }
    flushMove();   // motion whose wake-up event didn't fit in the queue
    HLock l;
    if (millis() - idleSince > IDLE_EXIT_MS && !movePending && uxQueueMessagesWaiting(events) == 0) {
      workerAlive = false;
      break;
    }
  }
  vTaskDelete(nullptr);
}

bool ready() { return keysChr && conn != BLE_HS_CONN_HANDLE_NONE && secured && listening; }  // lock held

bool post(Ev e) {
  HLock l;
  if (!ready()) return false;
  if (!workerAlive) {
    if (xTaskCreatePinnedToCore(worker, "hid", 3072, nullptr, 1, nullptr, 0) != pdPASS) return false;
    workerAlive = true;
  }
  return xQueueSend(events, &e, 0) == pdTRUE;
}
}  // namespace

namespace hid {
void init() {
  hmtx = xSemaphoreCreateMutex();
  events = xQueueCreate(12, sizeof(Ev));
}

void addTo(NimBLEServer *server) {
  NimBLEHIDDevice *d = new NimBLEHIDDevice(server);
  d->manufacturer(std::string("yOS"));
  d->pnp(0x02, 0x303A, 0x8100, 0x0100);  // USB-IF vendor ID source; Espressif's VID, our own PID
  d->hidInfo(0x00, 0x02);                 // not localised; normally connectable
  d->reportMap((uint8_t *)REPORT_MAP, sizeof(REPORT_MAP));
  NimBLECharacteristic *k = d->inputReport(ID_KEYS);
  d->outputReport(ID_KEYS);               // Caps Lock etc. LEDs: accepted and ignored
  NimBLECharacteristic *m = d->inputReport(ID_MEDIA);
  NimBLECharacteristic *p = d->inputReport(ID_MOUSE);
  uint8_t zero[8] = {};
  k->setValue(zero, 8);
  m->setValue(zero, 2);
  p->setValue(zero, 4);
  k->setCallbacks(&keysCb);
  d->setBatteryLevel(100);                // mains powered
  d->startServices();
  ble_svc_gap_device_appearance_set(HID_KEYBOARD);
  HLock l;
  dev = d;
  keysChr = k;
  mediaChr = m;
  mouseChr = p;
}

void detach() {
  HLock l;
  keysChr = mediaChr = mouseChr = nullptr;
  delete dev;
  dev = nullptr;
  conn = BLE_HS_CONN_HANDLE_NONE;
  secured = listening = false;
  moveX = moveY = 0;
  movePending = false;
}

void peerSecured(uint16_t c, bool encrypted) {
  HLock l;
  conn = c;
  secured = encrypted;
}

void peerGone() {
  HLock l;
  conn = BLE_HS_CONN_HANDLE_NONE;
  secured = listening = false;
  moveX = moveY = 0;
  movePending = false;
}
}  // namespace hid

HidState hidState() {
  if (!hmtx) return HID_OFF;
  {
    HLock l;
    if (!keysChr) return HID_OFF;
    if (ready()) return HID_READY;
  }
  return status().peerConnected ? HID_SECURING : HID_IDLE;
}

bool hidKey(uint8_t mods, uint8_t key) { return post({H_KEY, mods, key, nullptr}); }
bool hidMedia(uint16_t usage) { return post({H_MEDIA, 0, usage, nullptr}); }
bool hidClick() { return post({H_CLICK, 0, 0, nullptr}); }

bool hidType(const char *text) {
  size_t n = strnlen(text, TEXT_MAX);
  if (!n) return false;
  char *t = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!t) return false;
  memcpy(t, text, n);
  t[n] = 0;
  if (post({H_TEXT, 0, 0, t})) return true;
  free(t);
  return false;
}

void hidMove(int dx, int dy) {
  bool wake;
  {
    HLock l;
    if (!ready() || (!dx && !dy)) return;
    moveX = constrain(moveX + dx, -MOVE_MAX, MOVE_MAX);
    moveY = constrain(moveY + dy, -MOVE_MAX, MOVE_MAX);
    wake = !movePending;
    movePending = true;
  }
  if (wake) post({H_MOVE, 0, 0, nullptr});
}
}  // namespace ble
