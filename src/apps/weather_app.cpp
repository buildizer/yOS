// Weather: current conditions and a 5-day forecast for a chosen city, from Open-Meteo.
#include "apps/apps.h"
#include "os/os.h"
#include "os/ui.h"
#include "services/weather.h"
#include "apps/weather/countries.h"
#include <algorithm>

class WeatherApp : public App {
public:
  const char *id() const override { return "weather"; }
  const char *name() const override { return "Weather"; }
  const char *icon() const override { return ICON_PARTLY; }
  lv_color_t tint() const override { return lv_color_hex(0x1E88E5); }

  bool hasService() const override { return true; }
  bool defaultAutostart() const override { return true; }
  const char *serviceSummary() const override {
    WeatherLocation l = weatherLocation();
    static char buf[96];
    if (l.hasCity()) snprintf(buf, sizeof(buf), "Fetches the forecast for %s from Open-Meteo every 15 minutes.", l.name);
    else snprintf(buf, sizeof(buf), "Fetches the forecast from Open-Meteo every 15 minutes once a city is chosen.");
    return buf;
  }
  void startService() override { weatherStart(); }
  void stopService() override { weatherStop(); }

  // Live icon: condition glyph and temperature.
  bool liveIcon(lv_obj_t *tile) override {
    Weather w = weatherGet();
    lv_obj_set_style_bg_color(tile, w.valid && !w.isDay ? lv_color_hex(0x283593) : tint(), 0);
    lv_obj_t *l = (lv_obj_t *)lv_obj_get_user_data(tile);
    if (!w.valid || !running) {
      if (l) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
      return false;
    }
    if (!l) {
      l = ui::label(tile, "", theme::bodyBold, lv_color_white());
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_user_data(tile, l);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%s\n%.0f\xC2\xB0", weatherIcon(w.code, w.isDay), w.temp);
    lv_label_set_text(l, buf);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(l);
    return true;
  }

  void open(lv_obj_t *root) override {
    static char title[40];
    WeatherLocation l = weatherLocation();
    strlcpy(title, l.hasCity() ? l.name : "Weather", sizeof(title));
    ui::push(root, title, [this](lv_obj_t *c) { build(c); });
  }
  void close() override { page = nullptr; }

  void tick() override {
    if (!page) return;
    Weather w = weatherGet();
    if (w.version != shownVersion) refresh(w);
  }

  // Settings › Apps › Weather: where the forecast is for.
  void buildSettings(lv_obj_t *content, lv_obj_t *appRoot) override {
    WeatherLocation l = weatherLocation();
    lv_obj_t *g = ui::group(content, "Location", "The forecast follows the city you choose. Choosing a new country clears the city.");
    ui::Row country = ui::row(g, nullptr, lv_color_black(), "Country", l.country[0] ? l.country : "Not set",
                              [this, appRoot]() { pickCountry(appRoot); });
    ui::Row city = ui::row(g, nullptr, lv_color_black(), "City", l.hasCity() ? l.name : "Not set",
                           [this, appRoot]() { searchCity(appRoot); });
    setCountryValue = country.value;
    setCityValue = city.value;
    lv_obj_add_event_cb(content, [](lv_event_t *e) {
      auto *self = (WeatherApp *)lv_event_get_user_data(e);
      self->setCountryValue = self->setCityValue = nullptr;
    }, LV_EVENT_DELETE, this);
  }

private:
  lv_obj_t *setCountryValue = nullptr, *setCityValue = nullptr;
  lv_obj_t *page = nullptr, *body = nullptr;
  uint32_t shownVersion = UINT32_MAX;

  void updateSettingsValues() {
    WeatherLocation l = weatherLocation();
    if (setCountryValue) lv_label_set_text(setCountryValue, l.country[0] ? l.country : "Not set");
    if (setCityValue) lv_label_set_text(setCityValue, l.hasCity() ? l.name : "Not set");
  }

  // The list has ~200 rows; building them in one frame would freeze the UI, so a timer adds a chunk per tick.
  struct CountryBuild {
    lv_obj_t *page, *group;
    lv_timer_t *timer;
    WeatherApp *app;
    char letter;
    int next;
    char cur[3];
  };

  void buildCountryChunk(CountryBuild *b) {
    const int CHUNK = 25;
    lv_obj_t *selected = nullptr;
    int end = std::min(b->next + CHUNK, (int)COUNTRY_COUNT);
    for (int i = b->next; i < end; i++) {
      const Country &k = COUNTRIES[i];
      if (k.name[0] != b->letter) {   // alphabetical sections
        b->letter = k.name[0];
        char head[2] = {b->letter, 0};
        b->group = ui::group(b->page, head);
      }
      bool isCur = strcmp(k.code, b->cur) == 0;
      ui::Row r = ui::row(b->group, nullptr, lv_color_black(), k.name, isCur ? ICON_OK : nullptr, [this, i]() {
        const Country &k = COUNTRIES[i];
        WeatherLocation now = weatherLocation();
        if (strcmp(k.code, now.countryCode) != 0) weatherSetCountry(k.code, k.name);
        ui::pop();
        updateSettingsValues();
      }, false);
      if (isCur) {
        lv_obj_set_style_text_color(r.value, theme::p().accent, 0);
        selected = r.obj;
      }
    }
    b->next = end;
    if (selected) {   // later chunks only append below, so the position stays valid
      lv_obj_update_layout(b->page);
      lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
    }
    if (b->next >= COUNTRY_COUNT) lv_timer_pause(b->timer);
  }

  void pickCountry(lv_obj_t *appRoot) {
    ui::push(appRoot, "Country", [this](lv_obj_t *c) {
      auto *b = new CountryBuild{c, nullptr, nullptr, this, 0, 0, {0}};
      strlcpy(b->cur, weatherLocation().countryCode, sizeof(b->cur));
      buildCountryChunk(b);
      if (b->next < COUNTRY_COUNT) {
        b->timer = lv_timer_create([](lv_timer_t *t) {
          auto *b = (CountryBuild *)t->user_data;
          b->app->buildCountryChunk(b);
        }, 30, b);
      }
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *b = (CountryBuild *)lv_event_get_user_data(e);
        if (b->timer) lv_timer_del(b->timer);
        delete b;
      }, LV_EVENT_DELETE, b);
    });
  }

  void searchCity(lv_obj_t *appRoot) {
    WeatherLocation l = weatherLocation();
    if (!l.countryCode[0]) {
      ui::alert("Choose a country first", "City search is limited to one country. Pick a country, then search for a city.",
                {{"OK", ui::AlertAction::NORMAL, nullptr}});
      return;
    }
    ui::textInput(appRoot, "City", "Search for a city", "", false, [this, appRoot](const String &text) {
      String q = text;
      q.trim();
      if (q.length() < 2) {
        ui::alert("Search is too short", "Type at least two letters of the city name.",
                  {{"OK", ui::AlertAction::NORMAL, nullptr}});
        return;
      }
      showResults(appRoot, q);
    });
  }

  struct ResultsView {
    lv_obj_t *body;   // holds everything below the title, so it can be redrawn alone
    uint32_t id;
    WeatherApp *app;
    lv_timer_t *timer;
    bool done;
  };

  void showResults(lv_obj_t *appRoot, const String &query) {
    WeatherLocation l = weatherLocation();
    uint32_t id = weatherSearchStart(query.c_str(), l.countryCode);
    ui::push(appRoot, "Results", [this, id](lv_obj_t *c) {
      lv_obj_t *body = ui::box(c);
      lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(body, 8, 0);
      auto *v = new ResultsView{body, id, this, nullptr, false};
      lv_obj_t *g = ui::group(body);
      ui::row(g, nullptr, lv_color_black(), "Searching\xE2\x80\xA6", nullptr, nullptr, false);
      v->timer = lv_timer_create([](lv_timer_t *t) {
        auto *v = (ResultsView *)t->user_data;
        v->app->drawResults(v);
      }, 200, v);
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *v = (ResultsView *)lv_event_get_user_data(e);
        lv_timer_del(v->timer);
        delete v;
      }, LV_EVENT_DELETE, v);
    });
  }

  // Polled by a timer while the Results page is open; draws once the search task has finished.
  void drawResults(ResultsView *v) {
    static CitySearch s;
    weatherSearchGet(s);
    if (v->done || s.id != v->id || s.state == SEARCH_LOADING) return;
    v->done = true;
    lv_timer_pause(v->timer);
    lv_obj_clean(v->body);
    if (s.state == SEARCH_FAILED) {
      ui::note(v->body, s.error);
      return;
    }
    if (s.count == 0) {
      ui::note(v->body, "No cities found. Check the spelling, or search for a larger nearby city.");
      return;
    }
    lv_obj_t *g = ui::group(v->body);
    for (int i = 0; i < s.count; i++) {
      CityResult r = s.results[i];
      ui::row(g, nullptr, lv_color_black(), r.name, r.admin1[0] ? r.admin1 : nullptr,
              [this, r]() {
                weatherSetCity(r.name, r.admin1, r.lat, r.lon);
                ui::pop();
                updateSettingsValues();
              }, false);
    }
  }

  static constexpr uint32_t SKY = 0x23508E;

  static lv_obj_t *dim(lv_obj_t *l) {   // white at 78%
    lv_obj_set_style_text_opa(l, 200, 0);
    return l;
  }

  static lv_obj_t *card(lv_obj_t *parent) {
    lv_obj_t *k = ui::box(parent);
    lv_obj_set_size(k, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(k, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(k, 36, 0);   // 14%
    lv_obj_set_style_radius(k, 16, 0);
    lv_obj_set_style_pad_hor(k, 14, 0);
    lv_obj_set_style_pad_ver(k, 10, 0);
    lv_obj_set_flex_flow(k, LV_FLEX_FLOW_COLUMN);
    return k;
  }

  static void hairline(lv_obj_t *parent) {
    lv_obj_t *h = ui::box(parent);
    lv_obj_set_size(h, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(h, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(h, 46, 0);   // 18%
  }

  void build(lv_obj_t *c) {
    page = c;
    lv_obj_set_style_bg_color(c, lv_color_hex(SKY), 0);
    lv_obj_set_style_pad_top(c, ui::STATUS_H + 8, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_add_flag(lv_obj_get_child(c, 0), LV_OBJ_FLAG_HIDDEN);   // the big page title; the design centres the city
    body = ui::box(c);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    shownVersion = UINT32_MAX;
    tick();
  }

  static const char *compass(int deg) {
    static const char *n[] = {"north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest"};
    return n[((deg + 22) / 45) % 8];
  }

  void tile(lv_obj_t *row, const char *label, const char *value, const char *note) {
    lv_obj_t *t = card(row);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_style_min_height(t, 92, 0);
    lv_obj_set_style_pad_row(t, 4, 0);
    dim(ui::label(t, label, theme::caption, lv_color_white()));
    ui::label(t, value, theme::largeTitle, lv_color_white());
    lv_obj_t *n = dim(ui::label(t, note, theme::caption, lv_color_white()));
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
  }

  void refresh(const Weather &w) {
    const lv_color_t white = lv_color_white();
    shownVersion = w.version;
    lv_obj_clean(body);
    if (!w.valid) {
      lv_obj_t *m = ui::label(body, !running ? "Weather is stopped. Start it from App Info."
                                : strlen(w.error) ? w.error : "Getting the forecast\xE2\x80\xA6", theme::body, white);
      lv_obj_set_width(m, LV_PCT(100));
      lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_pad_top(m, 60, 0);
      return;
    }
    char buf[64];

    // Header
    lv_obj_t *head = ui::box(body);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui::label(head, w.city, theme::largeTitle, white);
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", w.temp);
    ui::label(head, buf, theme::big, white);
    ui::label(head, weatherText(w.code), theme::bodyBold, white);
    if (w.numDays) {
      snprintf(buf, sizeof(buf), "H:%.0f\xC2\xB0  L:%.0f\xC2\xB0", w.days[0].high, w.days[0].low);
      ui::label(head, buf, theme::body, white);
    }
    if (w.error[0]) dim(ui::label(head, w.error, theme::caption, white));   // a failed refresh keeps the last forecast

    // Summary + hourly strip
    lv_obj_t *k = card(body);
    lv_obj_set_style_pad_row(k, 8, 0);
    snprintf(buf, sizeof(buf), "%s. Wind up to %.0f km/h.", weatherText(w.code), w.wind);
    dim(ui::label(k, buf, theme::caption, white));
    hairline(k);
    lv_obj_t *hr = ui::box(k);
    lv_obj_set_size(hr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hr, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < w.numHours; i++) {
      lv_obj_t *col = ui::box(hr);
      lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(col, 1);
      lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(col, 6, 0);
      if (i == 0) strcpy(buf, "Now"); else snprintf(buf, sizeof(buf), "%02d", w.hours[i].hour);
      ui::label(col, buf, theme::footnoteBold, white);
      ui::label(col, weatherIcon(w.hours[i].code, w.hours[i].isDay), theme::title, white);
      snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", w.hours[i].temp);
      ui::label(col, buf, theme::footnoteBold, white);
    }

    // 5-day forecast
    k = card(body);
    lv_obj_set_style_pad_row(k, 0, 0);
    lv_obj_set_style_pad_bottom(k, 4, 0);
    lv_obj_t *hd = dim(ui::label(k, "5-DAY FORECAST", theme::caption, white));
    lv_obj_set_style_pad_bottom(hd, 6, 0);
    hairline(k);
    float mn = 1000, mx = -1000;
    for (int i = 0; i < w.numDays; i++) { mn = min(mn, w.days[i].low); mx = max(mx, w.days[i].high); }
    float span = max(mx - mn, 1.0f);
    const int BAR_W = 204;   // 420 row - 80 day - 32 icon - 2x40 temps - 4x6 gaps
    for (int i = 0; i < w.numDays; i++) {
      lv_obj_t *r = ui::box(k);
      lv_obj_set_size(r, LV_PCT(100), 44);
      lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(r, 6, 0);
      if (i > 0) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, white, 0);
        lv_obj_set_style_border_opa(r, 46, 0);
      }
      char day[12];
      if (i == 0) strcpy(day, "Today");
      else { struct tm tm; localtime_r(&w.days[i].date, &tm); strftime(day, sizeof(day), "%a", &tm); }
      lv_obj_t *d = ui::label(r, day, theme::body, white);
      lv_obj_set_width(d, 80);
      lv_obj_t *ic = ui::label(r, weatherIcon(w.days[i].code, true), theme::title, white);
      lv_obj_set_width(ic, 32);
      snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", w.days[i].low);
      lv_obj_t *lo = dim(ui::label(r, buf, theme::body, white));
      lv_obj_set_width(lo, 40);
      lv_obj_set_style_text_align(lo, LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_t *track = ui::box(r);
      lv_obj_set_size(track, BAR_W, 5);
      lv_obj_set_style_radius(track, 3, 0);
      lv_obj_set_style_bg_color(track, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(track, 56, 0);   // 22%
      int x0 = (int)((w.days[i].low - mn) / span * BAR_W);
      int x1 = (int)((w.days[i].high - mn) / span * BAR_W);
      lv_obj_t *fill = ui::box(track);
      lv_obj_set_size(fill, max(x1 - x0, 6), 5);
      lv_obj_set_pos(fill, x0, 0);
      lv_obj_set_style_radius(fill, 3, 0);
      lv_obj_set_style_bg_color(fill, lv_color_hex(0xFFC145), 0);
      lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
      snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", w.days[i].high);
      lv_obj_t *hi = ui::label(r, buf, theme::body, white);
      lv_obj_set_width(hi, 40);
      lv_obj_set_style_text_align(hi, LV_TEXT_ALIGN_RIGHT, 0);
    }

    // 2x2 tiles
    lv_obj_t *r1 = ui::box(body), *r2 = ui::box(body);
    for (lv_obj_t *r : {r1, r2}) {
      lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(r, 10, 0);
    }
    char v[24], n[48];
    float d = w.feelsLike - w.temp;
    snprintf(v, sizeof(v), "%.0f\xC2\xB0", w.feelsLike);
    tile(r1, "FEELS LIKE", v, d > 1.5f ? "Warmer than the actual temperature." : d < -1.5f ? "Cooler than the actual temperature."
                                                                                   : "Similar to the actual temperature.");
    snprintf(v, sizeof(v), "%d%%", w.humidity);
    tile(r1, "HUMIDITY", v, w.humidity < 30 ? "Dry air." : w.humidity < 60 ? "Comfortable." : "Humid.");
    snprintf(v, sizeof(v), "%.0f km/h", w.wind);
    snprintf(n, sizeof(n), "From the %s.", compass(w.windDir));
    tile(r2, "WIND", v, n);
    snprintf(n, sizeof(n), "Sunrise %s", w.sunrise);
    tile(r2, "SUNSET", w.sunset[0] ? w.sunset : "--", w.sunrise[0] ? n : "");
    dim(ui::label(body, "Data from Open-Meteo.", theme::caption, white));
  }
};

App *createWeatherApp() { return new WeatherApp(); }
