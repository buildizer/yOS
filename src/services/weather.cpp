#include "services/weather.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "os/theme.h"

#include <Preferences.h>

// Open-Meteo is free and keyless; plain HTTP keeps TLS off the board for public data.
static const uint32_t REFRESH_MS = 15 * 60 * 1000, RETRY_MS = 60 * 1000;

static Weather state;
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static volatile bool enabled = false;

static WeatherLocation loc;
static bool locLoaded = false;
static volatile uint32_t locRev = 0;   // bumps on every location change, so stale fetches are dropped
static CitySearch search;
static char searchQuery[64], searchCC[3];
static bool searchRunning = false;   // a geocode task is alive (guarded by lock)
static uint32_t stateVersion = 0;    // bumps on every change to state, including errors (guarded by lock)

static void ensureLock() {
  if (!lock) lock = xSemaphoreCreateMutex();
}

// Call with the lock held.
static void loadLocation() {
  if (locLoaded) return;
  locLoaded = true;
  Preferences nvs;
  nvs.begin("weather", true);
  if (nvs.isKey("lat")) {
    nvs.getString("name", loc.name, sizeof(loc.name));
    nvs.getString("admin1", loc.admin1, sizeof(loc.admin1));
    nvs.getString("country", loc.country, sizeof(loc.country));
    nvs.getString("cc", loc.countryCode, sizeof(loc.countryCode));
    loc.lat = nvs.getFloat("lat", 30.0444f);
    loc.lon = nvs.getFloat("lon", 31.2357f);
  } else {
    strcpy(loc.name, "Cairo");
    strcpy(loc.admin1, "Cairo Governorate");
    strcpy(loc.country, "Egypt");
    strcpy(loc.countryCode, "EG");
    loc.lat = 30.0444f;
    loc.lon = 31.2357f;
  }
  nvs.end();
}

static void saveLocation() {
  Preferences nvs;
  nvs.begin("weather", false);
  nvs.putString("name", loc.name);
  nvs.putString("admin1", loc.admin1);
  nvs.putString("country", loc.country);
  nvs.putString("cc", loc.countryCode);
  nvs.putFloat("lat", loc.lat);
  nvs.putFloat("lon", loc.lon);
  nvs.end();
}

// Forget the forecast for the old place and make the app redraw. Lock held.
static void invalidate() {
  locRev++;
  stateVersion++;
  state = Weather();
  state.fetchedAt = millis();
  if (loc.hasCity()) strlcpy(state.city, loc.name, sizeof(state.city));
  else strcpy(state.error, "Choose a city in Settings \xE2\x80\xBA Apps \xE2\x80\xBA Weather.");
}

WeatherLocation weatherLocation() {
  ensureLock();
  xSemaphoreTake(lock, portMAX_DELAY);
  loadLocation();
  WeatherLocation l = loc;
  xSemaphoreGive(lock);
  return l;
}

void weatherSetCountry(const char *code, const char *name) {
  ensureLock();
  xSemaphoreTake(lock, portMAX_DELAY);
  loadLocation();
  strlcpy(loc.countryCode, code, sizeof(loc.countryCode));
  strlcpy(loc.country, name, sizeof(loc.country));
  loc.name[0] = 0;
  loc.admin1[0] = 0;
  saveLocation();
  invalidate();
  xSemaphoreGive(lock);
}

void weatherSetCity(const char *name, const char *admin1, float lat, float lon) {
  ensureLock();
  xSemaphoreTake(lock, portMAX_DELAY);
  loadLocation();
  strlcpy(loc.name, name, sizeof(loc.name));
  strlcpy(loc.admin1, admin1, sizeof(loc.admin1));
  loc.lat = lat;
  loc.lon = lon;
  saveLocation();
  invalidate();
  xSemaphoreGive(lock);
  weatherRefreshNow();
}

static time_t parseDate(const char *s) {
  struct tm tm = {};
  sscanf(s, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_hour = 12;
  return mktime(&tm);
}

static void setError(uint32_t rev, const char *msg) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (rev == locRev) {
    strlcpy(state.error, msg, sizeof(state.error));
    stateVersion++;
  }
  xSemaphoreGive(lock);
}

static bool fetch() {
  WeatherLocation where = weatherLocation();
  if (!where.hasCity()) return true;   // nothing to fetch until a city is chosen
  uint32_t rev = locRev;
  char url[480];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,is_day"
           "&hourly=temperature_2m,weather_code&forecast_hours=6"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&timezone=auto&forecast_days=5",
           where.lat, where.lon);
  HTTPClient http;
  http.setTimeout(8000);
  http.useHTTP10(true);
  http.begin(url);
  int code = http.GET();
  Weather w;
  if (code != 200) {
    http.end();
    char msg[64];
    if (code < 0) strcpy(msg, "Can't reach the weather service. Check the Wi-Fi connection.");
    else snprintf(msg, sizeof(msg), "Weather service returned %d. It will retry shortly.", code);
    setError(rev, msg);
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    setError(rev, "The forecast couldn't be read. It will retry shortly.");
    return false;
  }

  JsonObject cur = doc["current"];
  w.temp = cur["temperature_2m"] | 0.0f;
  w.feelsLike = cur["apparent_temperature"] | 0.0f;
  w.humidity = cur["relative_humidity_2m"] | 0;
  w.wind = cur["wind_speed_10m"] | 0.0f;
  w.code = cur["weather_code"] | 0;
  w.windDir = cur["wind_direction_10m"] | 0;
  w.isDay = (cur["is_day"] | 1) == 1;
  JsonObject daily = doc["daily"];
  const char *sr = daily["sunrise"][0] | "";
  const char *ss = daily["sunset"][0] | "";
  if (strlen(sr) >= 16) strlcpy(w.sunrise, sr + 11, sizeof(w.sunrise));
  if (strlen(ss) >= 16) strlcpy(w.sunset, ss + 11, sizeof(w.sunset));
  JsonArray dates = daily["time"];
  for (size_t i = 0; i < dates.size() && i < 5; i++) {
    w.days[i].date = parseDate(dates[i] | "");
    w.days[i].code = daily["weather_code"][i] | 0;
    w.days[i].high = daily["temperature_2m_max"][i] | 0.0f;
    w.days[i].low = daily["temperature_2m_min"][i] | 0.0f;
    w.numDays = i + 1;
  }
  JsonObject hourly = doc["hourly"];
  JsonArray htimes = hourly["time"];
  for (size_t i = 0; i < htimes.size() && i < 6; i++) {
    const char *t = htimes[i] | "";   // "2026-10-29T18:00"
    if (strlen(t) < 16) break;
    HourForecast &h = w.hours[i];
    h.hour = atoi(t + 11);
    h.code = hourly["weather_code"][i] | 0;
    h.temp = hourly["temperature_2m"][i] | 0.0f;
    // Daylight: between today's sunrise and sunset (HH:MM strings compare correctly).
    char hhmm[6];
    strlcpy(hhmm, t + 11, sizeof(hhmm));
    h.isDay = w.sunrise[0] && w.sunset[0] ? (strcmp(hhmm, w.sunrise) >= 0 && strcmp(hhmm, w.sunset) < 0) : true;
    w.numHours = i + 1;
  }
  w.valid = true;
  w.fetchedAt = millis();
  strlcpy(w.city, where.name, sizeof(w.city));
  xSemaphoreTake(lock, portMAX_DELAY);
  if (rev == locRev) {   // dropped if the place changed mid-fetch
    state = w;
    stateVersion++;
  }
  xSemaphoreGive(lock);
  return true;
}

static void weatherTask(void *) {
  for (;;) {
    uint32_t wait = RETRY_MS;
    if (enabled && WiFi.status() == WL_CONNECTED) wait = fetch() ? REFRESH_MS : RETRY_MS;
    ulTaskNotifyTake(pdTRUE, enabled ? pdMS_TO_TICKS(wait) : portMAX_DELAY);  // stopped: sleep until started
  }
}

void weatherStart() {
  enabled = true;
  if (!task) {
    ensureLock();
    xTaskCreatePinnedToCore(weatherTask, "weather", 8192, nullptr, 1, &task, 0);
  } else {
    xTaskNotifyGive(task);
  }
}

void weatherStop() { enabled = false; }
void weatherRefreshNow() { if (task) xTaskNotifyGive(task); }

Weather weatherGet() {
  ensureLock();
  xSemaphoreTake(lock, portMAX_DELAY);
  loadLocation();
  if (!state.city[0] && loc.hasCity()) strlcpy(state.city, loc.name, sizeof(state.city));
  if (!loc.hasCity() && !state.error[0]) strcpy(state.error, "Choose a city in Settings \xE2\x80\xBA Apps \xE2\x80\xBA Weather.");
  Weather w = state;
  w.version = stateVersion;
  xSemaphoreGive(lock);
  return w;
}

const char *weatherText(int c) {
  if (c == 0) return "Clear";
  if (c <= 2) return "Partly Cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c >= 85 && c <= 86) return "Snow Showers";
  if (c >= 95) return "Thunderstorm";
  return "Unknown";
}

const char *weatherIcon(int c, bool isDay) {
  if (c == 0) return isDay ? ICON_SUN : ICON_MOON;
  if (c <= 2) return isDay ? ICON_PARTLY : ICON_MOON;
  if (c == 3) return ICON_CLOUD;
  if (c == 45 || c == 48) return ICON_FOG;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return ICON_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return ICON_SNOW;
  if (c >= 95) return ICON_STORM;
  return ICON_CLOUD;
}

// ---- City search -----------------------------------------------------------------------------

static void urlEncode(const char *s, char *out, size_t len) {
  size_t o = 0;
  for (; *s && o + 4 < len; s++) {
    unsigned char ch = *s;
    if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out[o++] = ch;
    else o += snprintf(out + o, len - o, "%%%02X", ch);
  }
  out[o] = 0;
}

static void finishSearch(uint32_t id, SearchState s, const char *err) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (search.id == id) {
    search.state = s;
    strlcpy(search.error, err, sizeof(search.error));
  }
  xSemaphoreGive(lock);
}

// All work (HTTPClient, JsonDocument) lives here so their destructors run before the task deletes itself.
static void doSearch(uint32_t id) {
  if (WiFi.status() != WL_CONNECTED) {
    finishSearch(id, SEARCH_FAILED, "Not connected to Wi-Fi. Connect in Settings and try again.");
    return;
  }
  char q[200], url[320];
  urlEncode(searchQuery, q, sizeof(q));
  snprintf(url, sizeof(url),
           "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=20&language=en&format=json&countryCode=%s", q,
           searchCC);
  HTTPClient http;
  http.setTimeout(5000);
  http.useHTTP10(true);
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    http.end();
    finishSearch(id, SEARCH_FAILED,
                 code < 0 ? "Can't reach the search service. Check the Wi-Fi connection and try again."
                          : "The search service had a problem. Try again in a moment.");
    return;
  }
  JsonDocument filter;
  filter["results"][0]["name"] = true;
  filter["results"][0]["admin1"] = true;
  filter["results"][0]["latitude"] = true;
  filter["results"][0]["longitude"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    finishSearch(id, SEARCH_FAILED, "The search results couldn't be read. Try again.");
    return;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (search.id == id) {
    int n = 0;
    for (JsonObject r : doc["results"].as<JsonArray>()) {
      if (n >= 20) break;
      CityResult &c = search.results[n++];
      strlcpy(c.name, r["name"] | "", sizeof(c.name));
      strlcpy(c.admin1, r["admin1"] | "", sizeof(c.admin1));
      c.lat = r["latitude"] | 0.0f;
      c.lon = r["longitude"] | 0.0f;
    }
    search.count = n;
    search.state = SEARCH_DONE;
  }
  xSemaphoreGive(lock);
}

static void searchTask(void *arg) {
  doSearch((uint32_t)(uintptr_t)arg);
  xSemaphoreTake(lock, portMAX_DELAY);
  searchRunning = false;
  xSemaphoreGive(lock);
  vTaskDelete(nullptr);
}

uint32_t weatherSearchStart(const char *query, const char *countryCode) {
  ensureLock();
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t id = ++search.id;
  search.count = 0;
  search.error[0] = 0;
  if (searchRunning) {   // one geocode task at a time; it reads searchQuery/searchCC
    search.state = SEARCH_FAILED;
    strlcpy(search.error, "The previous search is still finishing. Try again in a moment.", sizeof(search.error));
    xSemaphoreGive(lock);
    return id;
  }
  search.state = SEARCH_LOADING;
  strlcpy(searchQuery, query, sizeof(searchQuery));
  strlcpy(searchCC, countryCode, sizeof(searchCC));
  searchRunning = true;
  if (xTaskCreatePinnedToCore(searchTask, "geocode", 8192, (void *)(uintptr_t)id, 1, nullptr, 0) != pdPASS) {
    searchRunning = false;
    search.state = SEARCH_FAILED;
    strlcpy(search.error, "Couldn't start the search. Try again in a moment.", sizeof(search.error));
  }
  xSemaphoreGive(lock);
  return id;
}

void weatherSearchGet(CitySearch &out) {
  ensureLock();
  xSemaphoreTake(lock, portMAX_DELAY);
  out = search;
  xSemaphoreGive(lock);
}
