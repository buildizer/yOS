#include "services/ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace ble {
namespace {
const int MAX_DEVICES = MAX_LIST;
const uint32_t SCAN_SECONDS = 5;
const uint32_t RECONNECT_MS = 20000;
const char *SVC_UUID = "7e5a0001-4f53-4c59-9a1d-79a05b1e0001";   // yOS device-info service
const char *CHR_UUID = "7e5a0002-4f53-4c59-9a1d-79a05b1e0001";   // read-only "yOS <build>"

enum Cmd : uint8_t { C_ENABLE, C_DISABLE, C_DISCOVERABLE, C_SCAN, C_CONNECT, C_DISCONNECT, C_FORGET };
struct Msg {
  Cmd cmd;
  bool flag;
  uint8_t type;
  char addr[18];
  char name[32];
};

SemaphoreHandle_t mtx = nullptr;
QueueHandle_t queue = nullptr;
bool taskAlive = false;

// Shared state (guarded by mtx)
Status st = {};
Device devs[MAX_DEVICES];
int devCount = 0;
uint8_t bondType = 0;
HeapCost startCost = {}, stopCost = {};

// Task-only state
bool stackUp = false;
NimBLEServer *server = nullptr;
NimBLEClient *client = nullptr;
bool autoReconnect = true;
uint32_t lastTry = 0;

Preferences nvs;

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};
void touch() { st.version++; }  // call with the lock held

void setError(const char *msg) {
  Lock l;
  snprintf(st.error, sizeof(st.error), "%s", msg);
  touch();
}

// ---------- NimBLE callbacks ----------

class ScanCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *d) override {
    std::string a = d->getAddress().toString();
    Lock l;
    int i = 0;
    while (i < devCount && strcmp(devs[i].addr, a.c_str()) != 0) i++;
    if (i == devCount) {
      if (devCount >= MAX_DEVICES) return;
      devCount++;
      snprintf(devs[i].addr, sizeof(devs[i].addr), "%s", a.c_str());
      devs[i].name[0] = 0;
    }
    if (d->haveName()) snprintf(devs[i].name, sizeof(devs[i].name), "%s", d->getName().c_str());
    if (!devs[i].name[0]) snprintf(devs[i].name, sizeof(devs[i].name), "Unknown");
    devs[i].type = d->getAddressType();
    devs[i].rssi = constrain(d->getRSSI(), -127, 0);
    devs[i].connectable = d->isConnectable();
    touch();
  }
};
ScanCb scanCb;

class ClientCb : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *) override {
    Lock l;
    if (st.link == LINK_CONNECTED) st.link = LINK_IDLE;
    st.paired = false;
    touch();
  }
};
ClientCb clientCb;

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s) override {
    {
      Lock l;
      st.peerConnected = true;
      touch();
    }
  }
  void onDisconnect(NimBLEServer *s) override {
    bool disc;
    {
      Lock l;
      st.peerConnected = s->getConnectedCount() > 0;
      disc = st.discoverable;
      touch();
    }
    if (disc) NimBLEDevice::getAdvertising()->start();
  }
};
ServerCb serverCb;

// ---------- persistence ----------

void loadBond() {
  Lock l;
  String a = nvs.getString("addr", "");
  st.hasBond = a.length() == 17;
  snprintf(st.bondAddr, sizeof(st.bondAddr), "%s", st.hasBond ? a.c_str() : "");
  snprintf(st.bondName, sizeof(st.bondName), "%s", nvs.getString("name", "Unknown").c_str());
  bondType = nvs.getUChar("atype", 0);
}

void saveBond(const char *addr, uint8_t type, const char *name) {
  nvs.putString("addr", addr);
  nvs.putUChar("atype", type);
  nvs.putString("name", name);
  loadBond();
}

// ---------- stack lifecycle (ble task only) ----------

uint32_t freeHeap() { return ESP.getFreeHeap(); }
uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

void applyAdvertising(bool on) {
  if (!stackUp) return;
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (on) adv->start();
  else adv->stop();
}

bool stackStart() {
  if (stackUp) return true;
  HeapCost c = {};
  c.freeBefore = freeHeap();
  c.internalBefore = freeInternal();
  // NimBLE aborts (ESP_ERROR_CHECK) when its controller can't get memory, so a failed start would
  // reboot-loop the board. Record that a start is in progress; bleBegin() switches Bluetooth off
  // if the previous boot died here.
  {
    Preferences g;
    g.begin("yosble", false);
    g.putBool("starting", true);
    g.end();
  }
  // The controller's buffers must be internal RAM: restore the default 4 KB PSRAM threshold just for init.
  heap_caps_malloc_extmem_enable(4096);
  // Wi-Fi and Bluetooth share the radio; ESP-IDF's coexistence aborts unless Wi-Fi modem sleep is on.
  // Wi-Fi runs without sleep (lower latency) only while Bluetooth is off.
  WiFi.setSleep(true);
  NimBLEDevice::init("yOS");
  heap_caps_malloc_extmem_enable(256);
  {
    Preferences g;
    g.begin("yosble", false);
    g.putBool("starting", false);
    g.end();
  }
  if (!NimBLEDevice::getInitialized()) {
    setError("Bluetooth couldn't start. Not enough memory.");
    return false;
  }
  if (!st.hasBond) NimBLEDevice::deleteAllBonds();            // drop bonds left by a forget while Bluetooth was off
  NimBLEDevice::setSecurityAuth(true, false, true);          // bonding, no MITM (Just Works), LE Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCb);
  NimBLEService *svc = server->createService(SVC_UUID);
  NimBLECharacteristic *chr = svc->createCharacteristic(CHR_UUID, NIMBLE_PROPERTY::READ);
  chr->setValue(std::string("yOS (" __DATE__ ")"));
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  stackUp = true;
  c.freeAfter = freeHeap();
  c.internalAfter = freeInternal();
  Serial.printf("BLE up: heap %u -> %u (cost %d), internal %u -> %u (cost %d)\n", c.freeBefore, c.freeAfter,
                (int)c.freeBefore - (int)c.freeAfter, c.internalBefore, c.internalAfter,
                (int)c.internalBefore - (int)c.internalAfter);
  Lock l;
  startCost = c;
  st.ready = true;
  st.error[0] = 0;
  touch();
  if (st.discoverable) adv->start();
  return true;
}

void dropClient() {
  if (!client) return;
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteClient(client);
  client = nullptr;
}

void stackStop() {
  if (!stackUp) return;
  HeapCost c = {};
  c.freeBefore = freeHeap();
  c.internalBefore = freeInternal();
  NimBLEDevice::getScan()->stop();
  applyAdvertising(false);
  dropClient();
  NimBLEDevice::deinit(true);
  WiFi.setSleep(false);  // Bluetooth is off: Wi-Fi can drop modem sleep again
  server = nullptr;
  stackUp = false;
  c.freeAfter = freeHeap();
  c.internalAfter = freeInternal();
  Serial.printf("BLE down: heap %u -> %u (returned %d), internal %u -> %u (returned %d)\n", c.freeBefore, c.freeAfter,
                (int)c.freeAfter - (int)c.freeBefore, c.internalBefore, c.internalAfter,
                (int)c.internalAfter - (int)c.internalBefore);
  Lock l;
  stopCost = c;
  st.ready = false;
  st.scanning = false;
  st.peerConnected = false;
  st.link = LINK_IDLE;
  st.paired = false;
  devCount = 0;
  touch();
}

// ---------- commands ----------

void doScan() {
  if (!stackUp) return;
  {
    Lock l;
    devCount = 0;
    st.scanning = true;
    touch();
  }
  NimBLEScan *s = NimBLEDevice::getScan();
  s->setAdvertisedDeviceCallbacks(&scanCb, false);
  s->setActiveScan(true);
  s->setInterval(100);
  s->setWindow(99);
  s->start(SCAN_SECONDS, false);  // blocks this task only
  s->clearResults();
  Lock l;
  st.scanning = false;
  touch();
}

// Connect, bond and (on success) remember the device. `remember` is false for silent reconnects.
bool doConnect(const char *addr, uint8_t type, const char *name, bool user) {
  if (!stackUp) return false;
  {
    Lock l;
    st.link = LINK_CONNECTING;
    st.connectFailed = false;
    snprintf(st.target, sizeof(st.target), "%s", name);
    touch();
  }
  dropClient();
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCb, false);
  client->setConnectTimeout(user ? 10 : 5);
  bool ok = client->connect(NimBLEAddress(std::string(addr), type), false);
  bool paired = false;
  if (ok) {
    paired = client->secureConnection();   // Just Works pairing + bonding; many peripherals simply don't need it
    ok = client->isConnected();
  }
  if (!ok) {
    dropClient();
    Lock l;
    st.link = LINK_IDLE;
    st.connectFailed = user;
    touch();
    return false;
  }
  if (user) saveBond(addr, type, name);
  autoReconnect = true;
  Lock l;
  st.link = LINK_CONNECTED;
  st.paired = paired;
  st.connectFailed = false;
  touch();
  return true;
}

void handle(const Msg &m) {
  switch (m.cmd) {
    case C_ENABLE:
      if (stackStart()) {
        bool bonded;
        { Lock l; bonded = st.hasBond; }
        if (bonded) doConnect(st.bondAddr, bondType, st.bondName, false);
        lastTry = millis();
      } else {
        nvs.putBool("en", false);
        Lock l;
        st.enabled = false;
        touch();
      }
      break;
    case C_DISABLE: stackStop(); break;
    case C_DISCOVERABLE: applyAdvertising(m.flag); break;
    case C_SCAN: doScan(); break;
    case C_CONNECT:
      if (st.hasBond && strcmp(st.bondAddr, m.addr) != 0) dropClient();
      doConnect(m.addr, m.type, m.name, true);
      break;
    case C_DISCONNECT:
      autoReconnect = false;
      dropClient();
      { Lock l; st.link = LINK_IDLE; st.paired = false; touch(); }
      break;
    case C_FORGET: {
      autoReconnect = false;
      dropClient();
      if (stackUp && st.bondAddr[0]) NimBLEDevice::deleteBond(NimBLEAddress(std::string(st.bondAddr), bondType));
      nvs.remove("addr");
      nvs.remove("name");
      nvs.remove("atype");
      loadBond();
      Lock l;
      st.link = LINK_IDLE;
      st.paired = false;
      st.connectFailed = false;
      touch();
      break;
    }
  }
}

void bleTask(void *) {
  for (;;) {
    Msg m;
    if (xQueueReceive(queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
      handle(m);
      if (!stackUp) {
        Lock l;
        if (uxQueueMessagesWaiting(queue) == 0) {
          taskAlive = false;
          break;
        }
      }
      continue;
    }
    // Idle: keep trying to get back to the remembered device.
    bool retry;
    {
      Lock l;
      retry = stackUp && autoReconnect && st.hasBond && st.link == LINK_IDLE && st.enabled;
    }
    if (retry && millis() - lastTry > RECONNECT_MS) {
      lastTry = millis();
      doConnect(st.bondAddr, bondType, st.bondName, false);
    }
  }
  vTaskDelete(nullptr);
}

void post(const Msg &m) {
  Lock l;
  if (!taskAlive) {
    if (m.cmd != C_ENABLE) return;   // nothing to do while the stack is down
    taskAlive = true;
    xTaskCreatePinnedToCore(bleTask, "ble", 4608, nullptr, 1, nullptr, 0);
  }
  xQueueSend(queue, &m, 0);
}

Msg msg(Cmd c, bool flag = false) {
  Msg m = {};
  m.cmd = c;
  m.flag = flag;
  return m;
}
}  // namespace

Status status() {
  Lock l;
  return st;
}

int devices(Device *out, int max) {
  Lock l;
  int n = min(devCount, max);
  memcpy(out, devs, n * sizeof(Device));
  for (int i = 1; i < n; i++)  // insertion sort, strongest signal first
    for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) {
      Device t = out[j];
      out[j] = out[j - 1];
      out[j - 1] = t;
    }
  return n;
}

void setEnabled(bool on) {
  {
    Lock l;
    if (st.enabled == on) return;
    st.enabled = on;
    touch();
  }
  nvs.putBool("en", on);
  post(msg(on ? C_ENABLE : C_DISABLE));
}

void setDiscoverable(bool on) {
  {
    Lock l;
    st.discoverable = on;
    touch();
  }
  nvs.putBool("disc", on);
  post(msg(C_DISCOVERABLE, on));
}

void scan() {
  Lock l;
  if (!st.ready || st.scanning) return;
  st.scanning = true;   // shown immediately; the task re-asserts it
  devCount = 0;
  touch();
  Msg m = msg(C_SCAN);
  xQueueSend(queue, &m, 0);
}

void connect(const Device &d) {
  Msg m = msg(C_CONNECT);
  m.type = d.type;
  snprintf(m.addr, sizeof(m.addr), "%s", d.addr);
  snprintf(m.name, sizeof(m.name), "%s", d.name);
  {
    Lock l;
    st.link = LINK_CONNECTING;
    st.connectFailed = false;
    snprintf(st.target, sizeof(st.target), "%s", d.name);
    touch();
  }
  post(m);
}

void connectBonded() {
  Device d = {};
  {
    Lock l;
    if (!st.hasBond) return;
    snprintf(d.addr, sizeof(d.addr), "%s", st.bondAddr);
    snprintf(d.name, sizeof(d.name), "%s", st.bondName);
    d.type = bondType;
  }
  connect(d);
}

void disconnect() { post(msg(C_DISCONNECT)); }

void forget() {
  {
    Lock l;
    // Clear immediately so the UI reflects it; the task finishes the bond deletion.
    st.hasBond = false;
    touch();
  }
  Msg m = msg(C_FORGET);
  // The task may be down (Bluetooth off): handle inline in that case.
  bool alive;
  { Lock l; alive = taskAlive; }
  if (alive) {
    xQueueSend(queue, &m, 0);
  } else {
    nvs.remove("addr");
    nvs.remove("name");
    nvs.remove("atype");
    loadBond();
  }
}

HeapCost lastStartCost() { Lock l; return startCost; }
HeapCost lastStopCost() { Lock l; return stopCost; }
}  // namespace ble

void bleBegin() {
  using namespace ble;
  mtx = xSemaphoreCreateMutex();
  queue = xQueueCreate(8, sizeof(Msg));
  nvs.begin("yosble", false);
  loadBond();
  bool en = nvs.getBool("en", false);
  if (nvs.getBool("starting", false)) {  // the last boot died starting Bluetooth: come up with it off
    Serial.println("BLE: previous start crashed, Bluetooth switched off");
    nvs.putBool("starting", false);
    nvs.putBool("en", false);
    en = false;
  }
  {
    Lock l;
    st.discoverable = nvs.getBool("disc", false);
    st.enabled = en;
  }
  if (en) post(msg(C_ENABLE));
}
