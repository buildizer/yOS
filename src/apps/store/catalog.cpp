#include "apps/store/catalog.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <deque>
#include <functional>
#include <mutex>
#include "config.h"
#include "version.h"
#include "script/loader.h"

// TLS root certificates, embedded by board_build.embed_files (tools/gen_cert_bundle.py).
extern const uint8_t certBundle[] asm("_binary_data_x509_crt_bundle_bin_start");

namespace appstore {
namespace {

enum Kind : uint8_t { REFRESH, INSTALL, REMOVE };
struct Job {
  Kind kind;
  String id;
};

std::mutex m;  // guards st, ver, jobs, worker
Status st;
uint32_t ver = 0;
std::deque<Job> jobs;
TaskHandle_t worker = nullptr;

const size_t INDEX_MAX = 64 * 1024;
const size_t FILE_MAX = 256 * 1024;  // the loader's per-file limit; it re-checks
const int TOO_BIG = -1000, CUT_SHORT = -1001;

void update(std::function<void(Status &)> fn) {
  std::lock_guard<std::mutex> g(m);
  fn(st);
  ver++;
}

// The token the board signed in to GitHub with (ghupdate keeps it in NVS), or "".
String githubToken() {
  Preferences p;
  if (!p.begin("ghupd", true)) return "";
  String t = p.getString("token", "");
  p.end();
  return t;
}

// Ids and file names go into URLs; anything the loader would accept is plain enough for that.
bool urlSafe(const String &s) {
  if (s.isEmpty() || s.length() > 48 || s[0] == '.') return false;
  for (char c : s)
    if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') return false;
  return true;
}

// GETs one file of the store (path relative to STORE_DIR) into out. Returns the HTTP status, <0 for a
// network error, TOO_BIG or CUT_SHORT. onBytes sees each piece as it arrives.
int fetch(const String &token, const String &path, std::string &out, size_t maxBytes,
          std::function<void(size_t)> onBytes = nullptr) {
  WiFiClientSecure client;
  client.setCACertBundle(certBundle);
  client.setTimeout(15);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding: the body is a plain byte stream
  if (token.length()) {
    http.begin(client, "https://api.github.com/repos/" GITHUB_REPO "/contents/" STORE_DIR "/" + path + "?ref=" STORE_BRANCH);
    http.addHeader("Accept", "application/vnd.github.raw");
    http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  } else {
    http.begin(client, STORE_BASE_URL "/" + path);
  }
  http.setTimeout(15000);
  http.setUserAgent("yOS/" YOS_FIRMWARE_VERSION);
  int code = http.GET();
  if (code != 200) {
    http.end();
    return code;
  }
  int len = http.getSize();  // -1 when the server doesn't say
  if (len > (int)maxBytes) {
    http.end();
    return TOO_BIG;
  }
  out.clear();
  if (len > 0) out.reserve(len);
  WiFiClient *s = http.getStreamPtr();
  static uint8_t buf[1024];  // only the worker downloads
  uint32_t lastData = millis();
  while (len < 0 || out.size() < (size_t)len) {
    size_t avail = s->available();
    if (!avail) {
      if (!http.connected() || millis() - lastData > 15000) break;
      vTaskDelay(1);
      continue;
    }
    int n = s->read(buf, min(avail, sizeof(buf)));
    if (n <= 0) continue;
    lastData = millis();
    out.append((const char *)buf, n);
    if (out.size() > maxBytes) {
      http.end();
      return TOO_BIG;
    }
    if (onBytes) onBytes(n);
  }
  http.end();
  if (len >= 0 && out.size() != (size_t)len) return CUT_SHORT;
  return 200;
}

String fetchError(int code, bool signedIn) {
  if (code == 401) return "GitHub no longer accepts this board's sign-in. Sign in again in Settings \xE2\x80\xBA General \xE2\x80\xBA About.";
  if (code == 404)
    return signedIn ? "The App Store isn't in " GITHUB_REPO ", or this board's GitHub sign-in can't read it."
                    : "Couldn't find the App Store. While " GITHUB_REPO " is private, sign in to GitHub in Settings \xE2\x80\xBA General \xE2\x80\xBA About.";
  if (code == TOO_BIG) return "A file is larger than the App Store allows.";
  if (code == CUT_SHORT) return "The download stopped part way. Try again.";
  if (code < 0) return "Couldn't reach GitHub.";
  return "GitHub answered with error " + String(code) + ".";
}

void jobRefresh() {
  update([](Status &s) { s.state = LOADING; s.error = ""; });
  String token = githubToken();
  std::string body;
  int code = fetch(token, "index.json", body, INDEX_MAX);
  if (code != 200) {
    String why = fetchError(code, token.length());
    update([&](Status &s) { s.state = FAILED; s.error = why; });
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    update([](Status &s) { s.state = FAILED; s.error = "The App Store's catalog is damaged."; });
    return;
  }
  std::vector<Listing> apps;
  for (JsonObject a : doc["apps"].as<JsonArray>()) {
    Listing l;
    l.id = a["id"] | "";
    l.name = a["name"] | "";
    l.icon = a["icon"] | "extension";
    l.color = a["color"] | "";
    l.version = a["version"] | "";
    l.summary = a["summary"] | "";
    l.description = a["description"] | "";
    l.notes = a["notes"] | "";
    l.requires = a["requires"] | "";
    l.needs = a["needs"] | "";
    l.developer = a["developer"] | "buildizer";
    l.size = a["size"] | 0;
    bool ok = urlSafe(l.id) && l.name.length();
    for (JsonVariant f : a["files"].as<JsonArray>()) {
      String name = f | "";
      ok = ok && urlSafe(name);
      l.files.push_back(name);
    }
    if (!ok || l.files.empty()) {
      Serial.printf("[store] skipping a broken listing (\"%s\")\n", l.id.c_str());
      continue;
    }
    apps.push_back(std::move(l));
  }
  String featured = doc["featured"] | "";
  update([&](Status &s) {
    s.state = READY;
    s.apps = std::move(apps);
    s.featured = featured;
  });
}

void jobInstall(const String &id) {
  Listing l;
  bool found = false;
  {
    std::lock_guard<std::mutex> g(m);
    for (const Listing &a : st.apps)
      if (a.id == id) l = a, found = true;
  }
  if (!found) return;
  String token = githubToken();
  std::vector<ScriptAppFile> files;
  uint32_t got = 0, total = max<uint32_t>(l.size, 1);
  uint8_t shown = 0;
  for (const String &name : l.files) {
    ScriptAppFile f{name, std::string()};
    int code = fetch(token, l.id + "/" + name, f.data, FILE_MAX, [&](size_t n) {
      got += n;
      uint8_t pct = min<uint32_t>(99, (uint64_t)got * 100 / total);
      if (pct >= shown + 5) {  // redraw in steps, not on every packet
        shown = pct;
        update([&](Status &s) { s.pct = pct; });
      }
    });
    if (code != 200) {
      String why = fetchError(code, token.length());
      update([&](Status &s) { s.jobError = "\xE2\x80\x9C" + l.name + "\xE2\x80\x9D couldn't be downloaded. " + why; });
      return;
    }
    files.push_back(std::move(f));
  }
  // The listing must describe the app it downloads, or Update would replace some other app.
  for (const ScriptAppFile &f : files) {
    if (f.name != "app.json") continue;
    JsonDocument doc;
    deserializeJson(doc, f.data);
    if (l.id != (doc["id"] | "")) {
      update([&](Status &s) { s.jobError = "\xE2\x80\x9C" + l.name + "\xE2\x80\x9D in the App Store doesn't match its app.json."; });
      return;
    }
  }
  String err;
  if (!scriptAppInstall(files, err)) {  // validates, stores and registers it
    update([&](Status &s) { s.jobError = "\xE2\x80\x9C" + l.name + "\xE2\x80\x9D couldn't be installed. " + err; });
    return;
  }
  Serial.printf("[store] installed %s %s\n", l.id.c_str(), l.version.c_str());
}

void jobRemove(const String &id) {
  String err;
  if (!scriptAppRemove(id.c_str(), err)) update([&](Status &s) { s.jobError = err; });
}

void taskMain(void *) {
  for (;;) {
    Job job;
    {
      std::lock_guard<std::mutex> g(m);
      if (jobs.empty()) {
        st.busy = "";
        st.queued.clear();
        worker = nullptr;
        ver++;
        break;
      }
      job = jobs.front();
      jobs.pop_front();
      st.busy = job.kind == REFRESH ? "" : job.id;
      st.pct = 0;
      st.queued.clear();
      for (const Job &j : jobs)
        if (j.kind != REFRESH) st.queued.push_back(j.id);
      ver++;
    }
    if (job.kind == INSTALL && WiFi.status() != WL_CONNECTED) {
      update([](Status &s) { s.jobError = "Connect to Wi-Fi first."; });
      continue;
    }
    if (job.kind == REFRESH) jobRefresh();
    else if (job.kind == INSTALL) jobInstall(job.id);
    else jobRemove(job.id);
  }
  vTaskDelete(nullptr);
}

void queue(Kind kind, const String &id) {
  std::lock_guard<std::mutex> g(m);
  for (const Job &j : jobs)
    if (j.kind == kind && j.id == id) return;  // already waiting
  if (kind != REFRESH && st.busy == id) return;
  jobs.push_back({kind, id});
  if (kind != REFRESH) st.queued.push_back(id);
  ver++;
  if (worker) return;  // the running task picks it up
  // TLS needs a deep stack; it lives only while there is work. The lock makes sure `worker` is set
  // before the task looks at the queue.
  if (xTaskCreatePinnedToCore(taskMain, "store", 12288, nullptr, 1, &worker, 0) != pdPASS) {
    worker = nullptr;
    jobs.clear();
    st.queued.clear();
    if (kind == REFRESH) st.state = FAILED, st.error = "Not enough memory right now. Try again.";
    else st.jobError = "Not enough memory right now. Try again.";
  }
}

}  // namespace

void refresh() {
  if (WiFi.status() != WL_CONNECTED) {
    update([](Status &s) {
      if (s.state != READY) s.state = FAILED;
      s.error = "Connect to Wi-Fi to see the App Store.";
    });
    return;
  }
  queue(REFRESH, "");
}

void install(const String &id) { queue(INSTALL, id); }
void remove(const String &id) { queue(REMOVE, id); }

bool pending(const String &id) {
  std::lock_guard<std::mutex> g(m);
  if (st.busy == id) return true;
  for (const String &q : st.queued)
    if (q == id) return true;
  return false;
}

Status status() {
  std::lock_guard<std::mutex> g(m);
  return st;
}

uint32_t version() {
  std::lock_guard<std::mutex> g(m);
  return ver;
}

void clearJobError() { update([](Status &s) { s.jobError = ""; }); }

}  // namespace appstore
