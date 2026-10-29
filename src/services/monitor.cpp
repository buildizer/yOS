#include "services/monitor.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Ping.h>
#include <Preferences.h>
#include <mutex>
#include "services/dns_probe.h"
#include "services/storage.h"

static Snapshot state;
static SemaphoreHandle_t lock;
static TaskHandle_t task;

static const uint16_t HIST_FAIL = 0xFFFF;
static uint16_t history[NUM_PROBES][HISTORY_LEN];  // rtt in 0.1 ms
static int histHead = 0, histCount = 0;

static QueueHandle_t eventQueue;  // events waiting to be written to SD, outside the state lock

static volatile bool enabled = true;
static volatile int8_t pendingProtection = -1;     // -1 none, 0 pause, 1 resume
static volatile uint32_t pendingDurationMs = 0;

static void logEvent(const char *what, bool up, uint32_t downSecs) {
  memmove(&state.events[1], &state.events[0], sizeof(Event) * (MAX_EVENTS - 1));
  Event &e = state.events[0];
  e.at = time(nullptr);
  strlcpy(e.what, what, sizeof(e.what));
  e.up = up;
  e.downSecs = downSecs;
  if (state.numEvents < MAX_EVENTS) state.numEvents++;
  state.totalEvents++;
  xQueueSend(eventQueue, &e, 0);
  Serial.printf("[event] %s %s\n", what, up ? "UP" : "DOWN");
}

// Shared debounce for targets and DNS: down after FAILS_BEFORE_DOWN misses, up on first success.
template <typename S>
static void applyResult(S &s, const char *name, bool ok, float rttMs) {
  time_t now = time(nullptr);
  if (s.known && s.since < 1000000000 && now >= 1000000000) s.since = now;  // first state predates NTP sync
  s.rttMs = ok ? rttMs : 0;
  s.fails = ok ? 0 : min<int>(s.fails + 1, 255);
  bool nowUp = ok || (s.known && s.up && s.fails < FAILS_BEFORE_DOWN);
  if (!s.known) {
    if (!ok && s.fails < FAILS_BEFORE_DOWN) return;  // wait for a definite first state
    s.known = true;
    s.up = nowUp;
    s.since = now;
  } else if (nowUp != s.up) {
    logEvent(name, nowUp, nowUp ? uint32_t(now - s.since) : 0);
    s.up = nowUp;
    s.since = now;
  }
}

static void recordHistory(int probe, bool ok, float rttMs) {
  history[probe][histHead] = ok ? uint16_t(min(rttMs * 10.0f, 65534.0f)) : HIST_FAIL;
}

// AdGuard Home login, entered in Settings › Apps › Netwatch and kept in NVS (never in the source).
static std::mutex cfgMutex;
static AdguardConfig adgCfg;
static volatile bool adguardDirty = false;

static void loadAdguardConfig() {
  Preferences p;
  p.begin("adguard", false);
  std::lock_guard<std::mutex> g(cfgMutex);
  adgCfg.host = p.getString("host", ADGUARD_IP.toString());
  adgCfg.user = p.getString("user", "admin");
  adgCfg.pass = p.getString("pass", "");
  p.end();
}

AdguardConfig monitorAdguardConfig() {
  std::lock_guard<std::mutex> g(cfgMutex);
  return adgCfg;
}

void monitorSetAdguardConfig(const AdguardConfig &c) {
  {
    std::lock_guard<std::mutex> g(cfgMutex);
    adgCfg = c;
  }
  Preferences p;
  p.begin("adguard", false);
  p.putString("host", c.host);
  p.putString("user", c.user);
  p.putString("pass", c.pass);
  p.end();
  adguardDirty = true;  // poll with the new login on the next cycle
  if (task) xTaskNotifyGive(task);
}

static void beginAdguard(HTTPClient &http, const char *path) {
  AdguardConfig c = monitorAdguardConfig();
  http.setTimeout(4000);
  http.useHTTP10(true);  // avoid chunked encoding so the JSON stream parses directly
  http.begin("http://" + c.host + path);
  http.setAuthorization(c.user.c_str(), c.pass.c_str());
}

static void pollAdguard() {
  AdguardConfig c = monitorAdguardConfig();
  if (c.host.isEmpty() || c.pass.isEmpty()) {  // not set up: show nothing rather than an error
    xSemaphoreTake(lock, portMAX_DELAY);
    state.adguard = AdguardState();
    state.version++;
    xSemaphoreGive(lock);
    return;
  }
  AdguardState a;
  a.known = true;

  HTTPClient http;
  beginAdguard(http, "/control/stats");
  int code = http.GET();
  if (code == 200) {
    JsonDocument filter;
    filter["num_dns_queries"] = true;
    filter["num_blocked_filtering"] = true;
    filter["avg_processing_time"] = true;
    filter["top_blocked_domains"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
      a.ok = true;
      a.queries = doc["num_dns_queries"] | 0;
      a.blocked = doc["num_blocked_filtering"] | 0;
      a.avgMs = (doc["avg_processing_time"] | 0.0f) * 1000.0f;
      // Each entry is a single-key object: {"domain": count}
      for (JsonObject entry : doc["top_blocked_domains"].as<JsonArray>()) {
        if (a.numTopBlocked >= TOP_BLOCKED) break;
        for (JsonPair kv : entry) {
          strlcpy(a.topBlocked[a.numTopBlocked], kv.key().c_str(), sizeof(a.topBlocked[0]));
          a.topBlockedCount[a.numTopBlocked++] = kv.value() | 0;
        }
      }
    }
  } else {
    Serial.printf("[adguard] stats HTTP %d\n", code);
  }
  http.end();

  if (a.ok) {
    beginAdguard(http, "/control/status");
    if (http.GET() == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getStream())) {
        a.protectionOn = doc["protection_enabled"] | true;
        a.pausedMs = doc["protection_disabled_duration"] | 0;
      }
    }
    http.end();
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  state.adguard = a;
  state.version++;
  xSemaphoreGive(lock);
}

static void applyPendingProtection() {
  int8_t req = pendingProtection;
  if (req < 0) return;
  pendingProtection = -1;

  JsonDocument body;
  body["enabled"] = req == 1;
  if (req == 0) body["duration"] = pendingDurationMs;
  String json;
  serializeJson(body, json);

  HTTPClient http;
  beginAdguard(http, "/control/protection");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  Serial.printf("[adguard] protection %s -> HTTP %d\n", req == 1 ? "resume" : "pause", code);
  pollAdguard();
}

static void flushEventsToStorage() {
  Event e;
  while (xQueueReceive(eventQueue, &e, 0) == pdTRUE) storageAppendEvent(e);
}

// Once a minute, writes each probe's average RTT and losses over the last minute to SD.
static void logMinute() {
  static int lastMinute = -1;
  time_t now = time(nullptr);
  if (now < 1000000000) return;
  int minute = now / 60;
  if (minute == lastMinute) return;
  bool first = lastMinute < 0;
  lastMinute = minute;
  if (first) return;  // the first minute after boot is partial

  const int perMinute = 60000 / PING_INTERVAL_MS;
  float avg[NUM_PROBES];
  int lost[NUM_PROBES];
  float samples[perMinute];
  for (int p = 0; p < NUM_PROBES; p++) {
    int n = monitorHistory(p, samples, perMinute);
    float sum = 0;
    int ok = 0;
    lost[p] = 0;
    for (int i = 0; i < n; i++) {
      if (isnan(samples[i])) lost[p]++;
      else { sum += samples[i]; ok++; }
    }
    avg[p] = ok ? sum / ok : NAN;
  }
  storageAppendMinute(now - 60, avg, lost, NUM_PROBES);
}

static void monitorTask(void *) {
  static const char *dnsNames[] = DNS_TEST_NAMES;
  const int numDnsNames = sizeof(dnsNames) / sizeof(dnsNames[0]);
  int dnsIdx = 0;
  uint32_t lastAdguard = 0;
  bool firstAdguard = true;

  for (;;) {
    uint32_t cycleStart = millis();
    if (enabled && WiFi.status() == WL_CONNECTED) {
      for (int i = 0; i < NUM_TARGETS; i++) {
        bool ok = Ping.ping(TARGETS[i].ip, 1);
        float rtt = Ping.averageTime();
        recordHistory(i, ok, rtt);
        xSemaphoreTake(lock, portMAX_DELAY);
        applyResult(state.targets[i], TARGETS[i].name, ok, rtt);
        state.version++;
        xSemaphoreGive(lock);
      }

      const char *name = dnsNames[dnsIdx++ % numDnsNames];
      float rtt = 0;
      bool ok = dnsProbe(ADGUARD_IP, name, 2000, rtt);
      recordHistory(NUM_TARGETS, ok, rtt);
      xSemaphoreTake(lock, portMAX_DELAY);
      applyResult(state.dns, "DNS", ok, rtt);
      strlcpy(state.dns.lastName, name, sizeof(state.dns.lastName));
      histHead = (histHead + 1) % HISTORY_LEN;
      if (histCount < HISTORY_LEN) histCount++;
      state.version++;
      xSemaphoreGive(lock);

      flushEventsToStorage();
      logMinute();

      if (firstAdguard || adguardDirty || millis() - lastAdguard >= ADGUARD_INTERVAL_MS) {
        firstAdguard = false;
        adguardDirty = false;
        lastAdguard = millis();
        pollAdguard();
      }
    }
    // Sleep out the rest of the cycle, waking early to handle protection requests from the UI.
    for (;;) {
      uint32_t elapsed = millis() - cycleStart;
      if (elapsed >= PING_INTERVAL_MS) break;
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PING_INTERVAL_MS - elapsed))) applyPendingProtection();
    }
  }
}

void monitorRestoreEvents(const Event *events, int n) {
  n = min(n, MAX_EVENTS);
  memcpy(state.events, events, sizeof(Event) * n);
  state.numEvents = n;
}

void monitorSetEnabled(bool on) { enabled = on; }
bool monitorEnabled() { return enabled; }

void monitorStart() {
  if (lock) return;  // already running
  lock = xSemaphoreCreateMutex();
  loadAdguardConfig();
  eventQueue = xQueueCreate(16, sizeof(Event));
  xTaskCreatePinnedToCore(monitorTask, "monitor", 12288, nullptr, 1, &task, 0);
}

void monitorSnapshot(Snapshot &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = state;
  xSemaphoreGive(lock);
}

uint32_t monitorVersion() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = state.version;
  xSemaphoreGive(lock);
  return v;
}

int monitorHistory(int probe, float *out, int max) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int n = min(histCount, max);
  for (int i = 0; i < n; i++) {
    uint16_t v = history[probe][(histHead - n + i + HISTORY_LEN) % HISTORY_LEN];
    out[i] = v == HIST_FAIL ? NAN : v / 10.0f;
  }
  xSemaphoreGive(lock);
  return n;
}

void monitorSetProtection(bool enable, uint32_t durationMs) {
  pendingDurationMs = durationMs;
  pendingProtection = enable ? 1 : 0;
  xTaskNotifyGive(task);
}
