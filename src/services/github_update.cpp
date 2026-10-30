#include "services/github_update.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mutex>
#include "config.h"
#include "version.h"
#include "os/ota_screen.h"
#include "services/firmware_update.h"

// TLS root certificates, embedded by board_build.embed_files (tools/gen_cert_bundle.py).
extern const uint8_t certBundle[] asm("_binary_data_x509_crt_bundle_bin_start");
extern const uint8_t certBundleEnd[] asm("_binary_data_x509_crt_bundle_bin_end");

namespace ghupdate {
namespace {

std::mutex m;               // guards st, ver, token, busy, session, jobSession
Status st;
uint32_t ver = 0;
String token;
bool autoCheckOn = true;
bool busy = false;          // a network job is running
volatile bool cancelWanted = false;
// Sign-out and cancel bump the session; a job started under an older session drops its results, so a
// late reply can't sign the board back in or show a stale check.
uint32_t session = 0, jobSession = 0;
TaskHandle_t worker = nullptr;
uint32_t lastAutoAttemptMs = 0;
String assetId;             // AVAILABLE: the firmware asset to download (firmware.bin.gz when the release has it)
String assetSha256;         // its digest from GitHub, when given ("" = rely on the image's own check)
bool assetGz = false;       // the asset is gzip-compressed (inflated on the board while it downloads)

const char *API = "https://api.github.com";

bool onWorker() { return worker && xTaskGetCurrentTaskHandle() == worker; }
bool staleLocked() { return onWorker() && jobSession != session; }
bool stale() {
  std::lock_guard<std::mutex> g(m);
  return staleLocked();
}

void update(std::function<void(Status &)> fn) {
  std::lock_guard<std::mutex> g(m);
  if (staleLocked()) return;
  fn(st);
  st.signedIn = token.length() > 0;
  ver++;
}

void fail(const String &why) {
  update([&](Status &s) {
    s.state = token.isEmpty() ? SIGNED_OUT : FAILED;
    s.error = why;
  });
}

void saveToken(const String &t, const String &login) {
  if (stale()) return;
  Preferences p;
  p.begin("ghupd", false);
  p.putString("token", t);
  p.putString("login", login);
  p.end();
  std::lock_guard<std::mutex> g(m);
  token = t;
}

String bearer() {
  std::lock_guard<std::mutex> g(m);
  return "Bearer " + token;
}

void secure(WiFiClientSecure &c) {
  c.setCACertBundle(certBundle, certBundleEnd - certBundle);
  c.setTimeout(15);
}

// POST a form to github.com and parse the JSON reply. Returns the HTTP status (or <0 on a network error).
int postForm(const char *url, const String &body, JsonDocument &out) {
  WiFiClientSecure client;
  secure(client);
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(15000);  // HTTPClient otherwise resets the socket timeout to 5 s
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(body);
  if (code > 0) deserializeJson(out, http.getString());
  http.end();
  return code;
}

// GET from the REST API with the board's token.
int apiGet(const String &path, JsonDocument &out, JsonDocument *filter = nullptr) {
  WiFiClientSecure client;
  secure(client);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be parsed straight off the stream
  http.begin(client, API + path);
  http.setTimeout(15000);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("Authorization", bearer());
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.setUserAgent("yOS/" YOS_FIRMWARE_VERSION);
  int code = http.GET();
  if (code == 200) {
    if (filter) deserializeJson(out, http.getStream(), DeserializationOption::Filter(*filter));
    else deserializeJson(out, http.getStream());
  }
  http.end();
  return code;
}

bool online() { return WiFi.status() == WL_CONNECTED; }

void jobCheck() {
  update([](Status &s) { s.state = CHECKING; s.error = ""; });
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["body"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["id"] = true;
  filter["assets"][0]["size"] = true;
  filter["assets"][0]["digest"] = true;
  JsonDocument doc;
  int code = apiGet("/repos/" GITHUB_REPO "/releases/latest", doc, &filter);
  if (code == 401) {  // the sign-in was revoked on GitHub
    saveToken("", "");
    update([](Status &s) { s.login = ""; });
    fail("GitHub no longer accepts this board's sign-in. Sign in again.");
    return;
  }
  if (code == 404) return fail("No releases found in " GITHUB_REPO ".");
  if (code != 200) return fail(code < 0 ? "Couldn't reach GitHub." : "GitHub answered with error " + String(code) + ".");

  String tag = doc["tag_name"] | "";
  String latest = tag.startsWith("v") || tag.startsWith("V") ? tag.substring(1) : tag;
  // Releases carry firmware.bin and, since 1.3, firmware.bin.gz: a third smaller, so a third less to download.
  JsonObject asset, gz;
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    if (strcmp(a["name"] | "", GITHUB_FW_ASSET) == 0) asset = a;
    if (strcmp(a["name"] | "", GITHUB_FW_ASSET ".gz") == 0) gz = a;
  }
  bool useGz = !gz.isNull();
  if (useGz) asset = gz;
  if (asset.isNull()) return fail("Release " + tag + " has no " GITHUB_FW_ASSET ".");

  String notes = doc["body"] | "";
  notes.replace("\r", "");
  if (notes.length() > 280) notes = notes.substring(0, 277) + "\xE2\x80\xA6";
  String digest = asset["digest"] | "";
  {
    std::lock_guard<std::mutex> g(m);
    assetId = String((uint64_t)(asset["id"] | 0ULL));
    assetSha256 = digest.startsWith("sha256:") ? digest.substring(7) : "";
    assetGz = useGz;
  }
  Preferences p;
  p.begin("ghupd", false);
  time_t now = time(nullptr);
  p.putLong64("lastchk", now);
  p.end();
  bool newer = compareVersions(latest.c_str(), YOS_FIRMWARE_VERSION) > 0;
  uint32_t size = asset["size"] | 0;
  update([&](Status &s) {
    s.state = newer ? AVAILABLE : UP_TO_DATE;
    s.latest = latest;
    s.notes = notes;
    s.size = size;
    s.lastCheck = now;
  });
}

void jobSignIn() {
  update([](Status &s) { s.error = ""; s.userCode = ""; s.state = SIGNING_IN; });
  JsonDocument d;
  int code = postForm("https://github.com/login/device/code", "client_id=" GITHUB_APP_CLIENT_ID, d);
  String deviceCode = d["device_code"] | "";
  if (code != 200 || deviceCode.isEmpty()) return fail("Couldn't start signing in to GitHub.");
  uint32_t interval = (d["interval"] | 5) * 1000;
  uint32_t expires = millis() + (d["expires_in"] | 900) * 1000;
  String userCode = d["user_code"] | "";
  String uri = d["verification_uri"] | "https://github.com/login/device";
  update([&](Status &s) {
    s.userCode = userCode;
    s.verifyUri = uri;
    s.codeExpiresMs = expires;
  });

  String body = "client_id=" GITHUB_APP_CLIENT_ID "&device_code=" + deviceCode +
                "&grant_type=urn:ietf:params:oauth:grant-type:device_code";
  while (!cancelWanted && (int32_t)(expires - millis()) > 0) {
    for (uint32_t t = 0; t < interval && !cancelWanted; t += 250) vTaskDelay(pdMS_TO_TICKS(250));
    if (cancelWanted) break;
    JsonDocument r;
    if (postForm("https://github.com/login/oauth/access_token", body, r) != 200) continue;  // try again
    String tok = r["access_token"] | "";
    String err = r["error"] | "";
    if (cancelWanted || stale()) break;  // cancelled or signed out while GitHub was answering
    if (tok.length() && !r["refresh_token"].isNull()) {
      // Refreshing needs the app's client secret, which must never be on the board.
      return fail("This GitHub App's sign-ins expire after 8 hours. On GitHub, open the app's settings, turn off "
                  "\"Expire user authorization tokens\", then sign in again.");
    }
    if (tok.length()) {
      saveToken(tok, "");
      JsonDocument user;
      String login = apiGet("/user", user) == 200 ? String(user["login"] | "") : String("");
      saveToken(tok, login);
      update([&](Status &s) { s.login = login; s.userCode = ""; s.state = IDLE; });
      jobCheck();  // show straight away whether an update is waiting
      return;
    }
    if (err == "slow_down") interval += 5000;
    else if (err == "access_denied") return fail("Sign-in was declined on GitHub.");
    else if (err == "expired_token") break;
    else if (err != "authorization_pending") return fail("GitHub sign-in failed (" + err + ").");
  }
  if (cancelWanted) update([](Status &s) { s.state = s.signedIn ? IDLE : SIGNED_OUT; s.userCode = ""; });
  else fail("The code expired. Try again.");
}

// Follows GitHub's redirect from the API to the file host, then streams the image into the OTA slot.
void jobInstall() {
  String id, sha;
  uint32_t size;
  bool gz;
  {
    std::lock_guard<std::mutex> g(m);
    id = assetId;
    sha = assetSha256;
    size = st.size;
    gz = assetGz;
  }
  update([](Status &s) { s.state = INSTALLING; s.error = ""; });

  String location;
  {
    WiFiClientSecure client;
    secure(client);
    HTTPClient http;
    http.begin(client, String(API) + "/repos/" GITHUB_REPO "/releases/assets/" + id);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // the file host must not get our token
    http.addHeader("Accept", "application/octet-stream");
    http.addHeader("Authorization", bearer());
    http.setUserAgent("yOS/" YOS_FIRMWARE_VERSION);
    const char *keep[] = {"Location"};
    http.collectHeaders(keep, 1);
    int code = http.GET();
    location = http.header("Location");
    http.end();
    if (code != 302 || location.isEmpty()) return fail("Couldn't start the download (" + String(code) + ").");
  }

  uint32_t t0 = millis();
  WiFiClientSecure client;
  secure(client);
  HTTPClient http;
  http.useHTTP10(true);  // a plain byte stream: chunk framing must never reach the flash
  http.begin(client, location);
  http.setTimeout(15000);
  http.setUserAgent("yOS/" YOS_FIRMWARE_VERSION);
  int code = http.GET();
  int len = http.getSize();
  if (code != 200 || len != (int)size) {
    http.end();
    return fail("The download didn't start (" + String(code) + ").");
  }
  String err;
  if (!fwupdate::begin(gz ? "GitHub (gz)" : "GitHub", gz ? 0 : size, err)) {
    http.end();
    return fail(err);
  }

  otaScreenBegin("Installing from GitHub");
  uint32_t t1 = millis();
  WiFiClient *s = http.getStreamPtr();
  static const size_t CHUNK = 8192;
  uint8_t *buf = (uint8_t *)malloc(CHUNK);  // PSRAM (extmem threshold); the writer copies to internal RAM
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  mbedtls_sha256_starts(&hash, 0);
  uint32_t done = 0, lastData = millis(), lastShown = 0;
  bool ok = buf != nullptr;
  while (ok && done < size) {
    size_t avail = s->available();
    if (!avail) {
      if (!http.connected() || millis() - lastData > 15000) ok = false;
      else vTaskDelay(1);
      continue;
    }
    int n = s->read(buf, min(avail, min(CHUNK, (size_t)(size - done))));
    if (n <= 0) continue;
    lastData = millis();
    mbedtls_sha256_update(&hash, buf, n);
    if (!fwupdate::write(buf, n)) ok = false;
    done += n;
    if (done - lastShown >= size / 100 || done == size) {
      otaScreenProgress(done, size);
      lastShown = done;
    }
  }
  free(buf);
  http.end();
  Serial.printf("[ghupdate] STATS %lu B in %lu ms (connect %lu ms)\n", (unsigned long)done,
                (unsigned long)(millis() - t1), (unsigned long)(t1 - t0));

  uint8_t digest[32];
  mbedtls_sha256_finish(&hash, digest);
  mbedtls_sha256_free(&hash);
  if (ok && sha.length() == 64) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    ok = sha.equalsIgnoreCase(hex);
    if (!ok) Serial.println("[ghupdate] SHA-256 mismatch");
  }
  // finish() also checks the image's own checksum and SHA-256 before switching the boot slot.
  if (!ok || !fwupdate::finish()) {
    String why = fwupdate::error();
    fwupdate::abort();
    otaScreenFail();
    if (why.isEmpty()) why = "The update didn't download completely. Nothing was changed.";
    return fail(why);
  }
  otaScreenRestarting();
  delay(500);
  ESP.restart();
}

void taskMain(void *arg) {
  ((void (*)())arg)();
  {
    std::lock_guard<std::mutex> g(m);
    busy = false;
    worker = nullptr;
  }
  vTaskDelete(nullptr);
}

void run(void (*job)()) {
  {
    std::lock_guard<std::mutex> g(m);
    if (busy) {
      st.error = "GitHub is still answering an earlier request. Try again in a moment.";
      ver++;
      return;
    }
    busy = true;
    jobSession = session;
  }
  cancelWanted = false;
  if (!online()) {
    { std::lock_guard<std::mutex> g(m); busy = false; }
    return fail("Connect to Wi-Fi first.");
  }
  // TLS needs a deep stack; it lives only while the job runs. The lock makes sure `worker` is set
  // before the job's first update() looks at it.
  std::lock_guard<std::mutex> g(m);
  if (xTaskCreatePinnedToCore(taskMain, "ghupd", 12288, (void *)job, 1, &worker, 0) != pdPASS) {
    busy = false;
    worker = nullptr;
    st.state = token.isEmpty() ? SIGNED_OUT : FAILED;
    st.error = "Not enough memory right now. Try again.";
    ver++;
  }
}

}  // namespace

void begin() {
  Preferences p;
  p.begin("ghupd", false);
  token = p.getString("token", "");
  st.login = p.getString("login", "");
  st.lastCheck = p.getLong64("lastchk", 0);
  autoCheckOn = p.getBool("auto", true);
  p.end();
  st.signedIn = token.length() > 0;
  if (strlen(GITHUB_APP_CLIENT_ID) == 0) st.state = NOT_CONFIGURED;
  else st.state = token.isEmpty() ? SIGNED_OUT : IDLE;
}

void loop() {
  static uint32_t lastLook = 0;
  if (millis() - lastLook < 60000) return;
  lastLook = millis();
  Status s = status();
  bool running;
  {
    std::lock_guard<std::mutex> g(m);
    running = busy;
  }
  if (!autoCheckOn || !s.signedIn || running || !online() || millis() < 120000) return;
  if (s.state == AVAILABLE || s.state == INSTALLING || s.state == SIGNING_IN) return;
  time_t now = time(nullptr);
  if (now < 1000000000) return;  // no clock yet
  bool due = s.lastCheck == 0 || now - s.lastCheck >= 86400;
  if (due && (lastAutoAttemptMs == 0 || millis() - lastAutoAttemptMs >= 3600000)) {
    lastAutoAttemptMs = millis();  // a failing check retries hourly, not every minute
    check();
  }
}

Status status() {
  std::lock_guard<std::mutex> g(m);
  return st;
}

uint32_t version() {
  std::lock_guard<std::mutex> g(m);
  return ver;
}

void signIn() {
  if (status().state != NOT_CONFIGURED) run(jobSignIn);
}

void cancelSignIn() { cancelWanted = true; }  // the job notices, and never saves a token after this

void signOut() {
  cancelWanted = true;
  {
    std::lock_guard<std::mutex> g(m);
    session++;  // any running job now drops its results
  }
  saveToken("", "");
  update([](Status &s) {
    s = Status();
    s.state = SIGNED_OUT;
  });
}

bool installing() { return status().state == INSTALLING; }

void check() {
  State s = status().state;
  if (s != NOT_CONFIGURED && s != SIGNED_OUT && s != SIGNING_IN) run(jobCheck);
}

void install() {
  if (status().state == AVAILABLE) run(jobInstall);
}

bool autoCheck() { return autoCheckOn; }

void setAutoCheck(bool on) {
  autoCheckOn = on;
  Preferences p;
  p.begin("ghupd", false);
  p.putBool("auto", on);
  p.end();
}

int compareVersions(const char *a, const char *b) {
  while (*a || *b) {
    long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
    if (x != y) return x < y ? -1 : 1;
    if (*a == '.') a++;
    if (*b == '.') b++;
    if (!isdigit((unsigned char)*a) && *a) a++;  // skip anything else ("-beta")
    if (!isdigit((unsigned char)*b) && *b) b++;
  }
  return 0;
}

}  // namespace ghupdate
