#include "services/wifi_manager.h"
#include <WiFi.h>
#include <esp_bt.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>
#include "config.h"

namespace wifimgr {
namespace {
const int MAX_SAVED = 8;
const uint32_t ATTEMPT_MS = 12000;
const uint32_t SCAN_TIMEOUT_MS = 15000;
const uint32_t LOST_RETRY_MS = 20000;

struct Saved {
  String ssid, pass;
};

SemaphoreHandle_t mtx;
std::vector<Saved> savedList;
std::vector<Network> scanResults;
bool wifiOn = true;
bool connecting = false;
String connectingTo, failed;
bool scanActive = false;      // a scan is in flight
bool scanWanted = false;
bool autoWanted = false;      // run the boot-style "strongest saved network" connect
bool offWanted = false;       // radio must be (re)configured
bool disconnectWanted = false;
bool connectWanted = false;
bool persistWanted = false;   // saved list changed: write it to NVS from the wifi task
bool persistOnWanted = false; // only the 'on' key changed
String reqSsid, reqPass;

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

// ----- persistence (NVS is written outside the mutex, from the wifi task or begin()) -----
void persistNow() {
  std::vector<Saved> snap;
  bool on;
  {
    Lock l;
    snap = savedList;
    on = wifiOn;
    persistWanted = false;
    persistOnWanted = false;
  }
  Preferences p;
  p.begin("wifimgr", false);
  p.putUChar("n", snap.size());
  for (size_t i = 0; i < snap.size(); i++) {
    p.putString(("s" + String(i)).c_str(), snap[i].ssid);
    p.putString(("p" + String(i)).c_str(), snap[i].pass);
  }
  p.putBool("on", on);
  p.end();
}

void persistOnNow() {
  bool on;
  {
    Lock l;
    on = wifiOn;
    persistOnWanted = false;
  }
  Preferences p;
  p.begin("wifimgr", false);
  p.putBool("on", on);
  p.end();
}

void remember(const String &ssid, const String &pass) {
  Lock l;
  for (size_t i = 0; i < savedList.size(); i++)
    if (savedList[i].ssid == ssid) savedList.erase(savedList.begin() + i--);
  savedList.insert(savedList.begin(), {ssid, pass});
  if ((int)savedList.size() > MAX_SAVED) savedList.resize(MAX_SAVED);
  persistWanted = true;
}

// ----- radio task -----
enum Phase { IDLE, AUTO_SCAN, ATTEMPT };
Phase phase = IDLE;
std::vector<Saved> queue;     // networks still to try
Saved target;
bool targetUserStarted = false;
String previousSsid;          // restored when a person-started attempt fails
uint32_t phaseStart = 0, lostSince = 0;
bool wasConnected = false;

void radioOn() {
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);  // must precede mode() or DHCP sends the default name
  WiFi.mode(WIFI_STA);
  // Mains powered: modem sleep added 70-300 ms of latency. Coexistence requires it while Bluetooth runs.
  WiFi.setSleep(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED);
  WiFi.setAutoReconnect(true);
}

void startScan() {
  WiFi.scanNetworks(true, false);
  scanActive = true;
  phaseStart = millis();
}

void collectScan(int n) {
  std::vector<Network> out;
  Lock l;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (Network &o : out)
      if (o.ssid == s) {
        dup = true;
        if (WiFi.RSSI(i) > o.rssi) o.rssi = WiFi.RSSI(i);
      }
    if (dup) continue;
    bool isSaved = false;
    for (const Saved &sv : savedList) isSaved |= sv.ssid == s;
    out.push_back({s, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN, isSaved});
  }
  std::sort(out.begin(), out.end(), [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  scanResults = std::move(out);
}

void beginAttempt(const Saved &s, bool user) {
  target = s;
  targetUserStarted = user;
  phase = ATTEMPT;
  phaseStart = millis();
  {
    Lock l;
    connecting = true;
    connectingTo = s.ssid;
    if (user) failed = "";
  }
  WiFi.disconnect(false, false);
  wl_status_t r = WiFi.begin(s.ssid.c_str(), s.pass.length() ? s.pass.c_str() : nullptr);
  Serial.printf("[wifi] begin %s -> %d\n", s.ssid.c_str(), r);
}

void finishAttempts() {
  phase = IDLE;
  Lock l;
  connecting = false;
  connectingTo = "";
}

void nextAttempt() {
  if (queue.empty()) return finishAttempts();
  Saved s = queue.front();
  queue.erase(queue.begin());
  beginAttempt(s, false);
}

// Order saved networks by signal (visible first, then the rest in saved order) and start trying them.
void buildAutoQueue() {
  std::vector<Saved> sv;
  std::vector<Network> res;
  {
    Lock l;
    sv = savedList;
    res = scanResults;
  }
  queue.clear();
  for (const Network &n : res)
    for (const Saved &s : sv)
      if (s.ssid == n.ssid) queue.push_back(s);
  for (const Saved &s : sv) {
    bool in = false;
    for (const Saved &q : queue) in |= q.ssid == s.ssid;
    if (!in) queue.push_back(s);
  }
  nextAttempt();
}

void handleConnected() {
  if (targetUserStarted) remember(target.ssid, target.pass);
  queue.clear();
  finishAttempts();
}

void tick() {
  bool pAll, pOn;
  {
    Lock l;
    pAll = persistWanted;
    pOn = persistOnWanted;
  }
  if (pAll) persistNow();
  else if (pOn) persistOnNow();
  // Requests from the UI.
  bool off, dis, aut, conn;
  String rs, rp;
  {
    Lock l;
    off = offWanted; offWanted = false;
    dis = disconnectWanted; disconnectWanted = false;
    aut = autoWanted;
    if (!off) autoWanted = false;  // an off/on reconfigure keeps it for the connect that follows
    conn = connectWanted; connectWanted = false;
    rs = reqSsid; rp = reqPass;
  }
  if (off) {
    if (scanActive) WiFi.scanDelete();
    scanActive = false;
    queue.clear();
    phase = IDLE;
    {
      Lock l;
      connecting = false; connectingTo = ""; scanWanted = false; scanResults.clear();
    }
    if (wifiOn) {
      radioOn();
      lostSince = millis();
      Lock l;
      autoWanted = true;
    } else {
      WiFi.mode(WIFI_OFF);
    }
    return;
  }
  if (!wifiOn) return;
  if (dis) {
    queue.clear();
    WiFi.disconnect(false, false);
    {
      Lock l;
      connecting = false; connectingTo = "";
    }
    phase = IDLE;
    aut = true;
  }
  if (conn) {
    {
      Lock l;
      previousSsid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
    }
    if (scanActive) { WiFi.scanDelete(); scanActive = false; }
    queue.clear();
    Saved s{rs, rp};
    if (!rp.length()) {
      Lock l;
      for (const Saved &sv : savedList)
        if (sv.ssid == rs) s.pass = sv.pass;
    }
    beginAttempt(s, true);
    return;
  }

  static uint32_t lastHeapLog = 0;
  if (millis() - lastHeapLog > 5000) {
    lastHeapLog = millis();
    Serial.printf("[mem] internal free %u largest %u min %u | psram free %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // Stack headroom per task (bytes never used), to size stacks against internal RAM.
    for (const char *n : {"ui", "loopTask", "monitor", "weather", "wifi", "ble", "lcdcopy", "sdjob"}) {
      TaskHandle_t h = xTaskGetHandle(n);
      if (h) Serial.printf("[stack] %s unused %u\n", n, uxTaskGetStackHighWaterMark(h));
    }
  }
  static int lastSt = -1, lastPhase = -1;
  int st = WiFi.status();
  if (st != lastSt || phase != lastPhase) {
    Serial.printf("[wifi] status %d phase %d scan %d target %s\n", st, phase, scanActive, target.ssid.c_str());
    lastSt = st;
    lastPhase = phase;
  }
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wasConnected) {  // new address: restart mDNS so netwatch.local follows the board
    MDNS.end();
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  }
  wasConnected = up;
  if (up) lostSince = 0;
  else if (!lostSince) lostSince = millis();

  // Scan completion
  if (scanActive) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      collectScan(n);
      WiFi.scanDelete();
      scanActive = false;
    } else if (n == WIFI_SCAN_FAILED || millis() - phaseStart > SCAN_TIMEOUT_MS) {
      WiFi.scanDelete();
      scanActive = false;
    }
    if (!scanActive && phase == AUTO_SCAN) buildAutoQueue();
    return;
  }

  switch (phase) {
    case ATTEMPT: {
      uint32_t el = millis() - phaseStart;
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) return handleConnected();
      if (el > ATTEMPT_MS || (el > 2000 && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL))) {
        if (targetUserStarted) {
          Lock l;
          failed = target.ssid;
        }
        WiFi.disconnect(false, false);
        if (targetUserStarted && previousSsid.length() && previousSsid != target.ssid) {
          Saved prev;
          bool have = false;
          {
            Lock l;
            for (const Saved &sv : savedList)
              if (sv.ssid == previousSsid) { prev = sv; have = true; }
          }
          if (have) return beginAttempt(prev, false);
        }
        nextAttempt();
      }
      return;
    }
    case AUTO_SCAN:
      return;
    case IDLE:
      break;
  }

  bool haveSaved, scan;
  {
    Lock l;
    haveSaved = !savedList.empty();
    scan = scanWanted;
  }
  if (aut || (!up && haveSaved && lostSince && millis() - lostSince > LOST_RETRY_MS)) {
    if (haveSaved) {
      lostSince = millis();
      phase = AUTO_SCAN;
      {
        Lock l;
        scanWanted = false;
      }
      startScan();
      return;
    }
  }
  if (scan) {
    {
      Lock l;
      scanWanted = false;
    }
    startScan();
  }
}

void task(void *) {
  for (;;) {
    tick();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
}  // namespace

void begin() {
  mtx = xSemaphoreCreateMutex();
  Preferences p;
  p.begin("wifimgr", false);
  wifiOn = p.getBool("on", true);
  int n = p.getUChar("n", 0);
  for (int i = 0; i < n && i < MAX_SAVED; i++) {
    Saved s{p.getString(("s" + String(i)).c_str()), p.getString(("p" + String(i)).c_str())};
    if (s.ssid.length()) savedList.push_back(s);
  }
  p.end();
  autoWanted = true;
  offWanted = true;  // first tick configures the radio (on or off) and kicks off the boot connect
  xTaskCreatePinnedToCore(task, "wifi", 4608, nullptr, 1, nullptr, 0);
}

State state() {
  Lock l;
  if (!wifiOn) return OFF;
  if (connecting) return CONNECTING;
  return WiFi.status() == WL_CONNECTED ? CONNECTED : DISCONNECTED;
}

bool enabled() {
  Lock l;
  return wifiOn;
}

void setEnabled(bool on) {
  Lock l;
  if (wifiOn == on) return;
  wifiOn = on;
  persistOnWanted = true;
  offWanted = true;
}

String currentSsid() { return state() == CONNECTED ? WiFi.SSID() : String(); }

String connectingSsid() {
  Lock l;
  return connecting ? connectingTo : String();
}

String failedSsid() {
  Lock l;
  return failed;
}

String firstSavedSsid() {
  Lock l;
  return savedList.empty() ? String() : savedList[0].ssid;
}

int rssi() { return WiFi.RSSI(); }

void requestScan() {
  Lock l;
  if (wifiOn) scanWanted = true;
}

bool scanning() {
  Lock l;
  return wifiOn && !connecting && (scanActive || scanWanted || phase == AUTO_SCAN);
}

std::vector<Network> results() {
  Lock l;
  std::vector<Network> out = scanResults;
  for (Network &n : out) {
    n.saved = false;
    for (const Saved &s : savedList) n.saved |= s.ssid == n.ssid;
  }
  return out;
}

std::vector<String> saved() {
  Lock l;
  std::vector<String> out;
  for (const Saved &s : savedList) out.push_back(s.ssid);
  return out;
}

bool isSaved(const String &ssid) {
  Lock l;
  for (const Saved &s : savedList)
    if (s.ssid == ssid) return true;
  return false;
}

void connect(const String &ssid, const String &pass) {
  Lock l;
  if (!wifiOn || !ssid.length()) return;
  reqSsid = ssid;
  reqPass = pass;
  connectWanted = true;
  connecting = true;  // visible to the UI at once
  connectingTo = ssid;
  failed = "";
}

void forget(const String &ssid) {
  Lock l;
  bool wasCurrent = WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid;
  for (size_t i = 0; i < savedList.size(); i++)
    if (savedList[i].ssid == ssid) savedList.erase(savedList.begin() + i--);
  persistWanted = true;
  if (wasCurrent || (connecting && connectingTo == ssid)) disconnectWanted = true;
  if (failed == ssid) failed = "";
}
}
