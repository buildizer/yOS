// Clock: a big, calm time display for the desk or bedside.
#include "apps/apps.h"
#include "os/os.h"
#include "os/ui.h"
#include "os/prefs.h"
#include "services/weather.h"

#include "apps/clock/cities.h"
#include <Preferences.h>

static const int MAX_CITIES = 3;   // the card at the bottom of the Clock app fits three rows

class ClockApp : public App {
public:
  const char *id() const override { return "clock"; }
  const char *name() const override { return "Clock"; }
  const char *icon() const override { return ICON_CLOCK; }
  lv_color_t tint() const override { return lv_color_hex(0x3A3A3C); }

  // Live icon: the tile shows the current time.
  bool liveIcon(lv_obj_t *tile) override {
    lv_obj_t *l = (lv_obj_t *)lv_obj_get_user_data(tile);
    if (!l) {
      l = ui::label(tile, "", theme::bodyBold, lv_color_white());
      lv_obj_center(l);
      lv_obj_set_user_data(tile, l);
    }
    char buf[8], suffix[3];
    if (!clockText(buf, sizeof(buf), suffix)) return false;
    lv_label_set_text(l, buf);
    return true;
  }

  // Settings › Apps › Clock
  void buildSettings(lv_obj_t *content, lv_obj_t *appRoot) override {
    loadWorld();
    lv_obj_t *g = ui::group(content, nullptr, "Also changes the status bar and the Home Screen clock.");
    ui::switchRow(g, nullptr, lv_color_black(), "24-Hour Time", prefs::use24h(),
                  [](bool on) { prefs::setUse24h(on); });

    // World Clocks: the switch stays put; the city rows below it are redrawn whenever the list changes.
    worldGroup = ui::group(content, "World Clocks");
    ui::switchRow(worldGroup, nullptr, lv_color_black(), "Show in Clock", worldOn, [this](bool on) {
      worldOn = on;
      saveWorld();
      redrawWorldLater();
    });
    worldNote = ui::note(content, "");
    settingsRoot = appRoot;
    redrawWorld();
    lv_obj_add_event_cb(content, [](lv_event_t *e) {
      auto *self = (ClockApp *)lv_event_get_user_data(e);
      self->worldGroup = self->worldNote = nullptr;
    }, LV_EVENT_DELETE, this);
  }

  void open(lv_obj_t *root) override {
    const lv_color_t white = lv_color_white(), grey = lv_color_hex(0x98989F);
    lv_obj_t *page = ui::box(root);
    lv_obj_set_size(page, 480, 480);
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(page, ui::STATUS_H + 14, 0);
    lv_obj_set_style_pad_row(page, 4, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // The time, with AM/PM and seconds stacked at its top right.
    lv_obj_t *row = ui::box(page);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 8, 0);
    timeLabel = ui::label(row, "0:00", theme::hero, white);
    lv_obj_t *side = ui::box(row);
    lv_obj_set_size(side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 4, 0);
    lv_obj_set_style_pad_top(side, 22, 0);
    ampmLabel = ui::label(side, "", theme::title, lv_color_hex(0xFF9F0A));
    secLabel = ui::label(side, "", theme::title, grey);
    lv_obj_set_style_min_width(secLabel, 30, 0);

    dateLabel = ui::label(page, "", theme::title, white);
    lv_obj_t *wrow = ui::box(page);
    lv_obj_set_size(wrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    weatherLabel = ui::label(wrow, "", theme::body, grey);
    weatherRow = wrow;

    // World clocks (opt-in, Settings › Apps › Clock): a card pinned to the bottom.
    loadWorld();
    zoneCount = worldOn ? worldCount : 0;
    lastMinute = -1;
    if (zoneCount) {
      lv_obj_t *card = ui::box(page);
      lv_obj_add_flag(card, LV_OBJ_FLAG_IGNORE_LAYOUT);
      lv_obj_set_size(card, 480 - 2 * ui::SIDE, LV_SIZE_CONTENT);
      lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -30);
      lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(card, 14, 0);
      lv_obj_set_style_clip_corner(card, true, 0);
      lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
      for (int i = 0; i < zoneCount; i++) {
        lv_obj_t *r = ui::box(card);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (i > 0) {   // hairline inset to the text, like the design
          lv_obj_t *sep = ui::box(r);
          lv_obj_add_flag(sep, LV_OBJ_FLAG_IGNORE_LAYOUT);
          lv_obj_set_size(sep, 480, 1);   // runs to the card's edge, clipped by the row
          lv_obj_set_style_bg_color(sep, lv_color_hex(0x38383A), 0);
          lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        }
        lv_obj_t *col = ui::box(r);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        zoneCaption[i] = ui::label(col, "", theme::caption, grey);
        ui::label(col, CITIES[worldCity[i]].name, theme::title, white);
        zoneTime[i] = ui::label(r, "", theme::largeTitle, white);
      }
    }
    tick();
  }
  void close() override { timeLabel = nullptr; }

  void tick() override {
    if (!timeLabel) return;
    char buf[40], suffix[3];
    if (!clockText(buf, sizeof(buf), suffix)) return;
    setText(timeLabel, buf);
    setText(ampmLabel, suffix);
    format(buf, sizeof(buf), "%S");
    setText(secLabel, buf);
    format(buf, sizeof(buf), "%A, %e %B");
    setText(dateLabel, buf);

    Weather w = weatherGet();
    if (w.valid) {
      snprintf(buf, sizeof(buf), "%s  %s \xC2\xB7 %.0f\xC2\xB0", weatherIcon(w.code, w.isDay), w.city, w.temp);
      setText(weatherLabel, buf);
      lv_obj_clear_flag(weatherRow, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(weatherRow, LV_OBJ_FLAG_HIDDEN);
    }

    // Zone times only change on the minute (or with the 24-hour setting), so the TZ work runs once a minute.
    time_t now = time(nullptr);
    if (zoneCount && now >= 1000000000 && (now / 60 != lastMinute || prefs::use24h() != last24h)) {
      lastMinute = now / 60;
      last24h = prefs::use24h();
      updateZones(now);
    }
  }

private:
  lv_obj_t *timeLabel = nullptr, *secLabel, *ampmLabel, *dateLabel, *weatherLabel, *weatherRow;
  lv_obj_t *zoneCaption[MAX_CITIES], *zoneTime[MAX_CITIES];
  int zoneCount = 0;
  time_t lastMinute = -1;
  bool last24h = false;

  // World clock settings, kept in the app's own NVS namespace. Cities are stored by name so the table can grow.
  bool worldLoaded = false, worldOn = false;
  int worldCity[MAX_CITIES], worldCount = 0;
  lv_obj_t *worldGroup = nullptr, *worldNote = nullptr, *settingsRoot = nullptr;

  static void setText(lv_obj_t *l, const char *t) {
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
  }

  // Days since 1970-01-01 for a civil date (proleptic Gregorian).
  static int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
  }
  static int64_t tmSeconds(const struct tm &t) {
    return daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  }
  // UTC offset of each city at `now`. newlib only knows one TZ rule at a time, so each city's rule is swapped in,
  // read with localtime_r and the system rule restored, all on the LVGL thread and only about once a minute.
  static void cityOffsets(const int *cities, int n, time_t now, int *out) {
    const char *cur = getenv("TZ");
    String saved = cur ? cur : "";   // copied: setenv frees the old string
    for (int i = 0; i < n; i++) {
      setenv("TZ", CITIES[cities[i]].tz, 1);
      tzset();
      struct tm t;
      localtime_r(&now, &t);
      out[i] = (int)(tmSeconds(t) - now);
    }
    if (saved.length()) setenv("TZ", saved.c_str(), 1);
    else unsetenv("TZ");
    tzset();
  }
  static int localOffset(time_t now) {
    struct tm t;
    localtime_r(&now, &t);
    return (int)(tmSeconds(t) - now);
  }

  // "+1 hr", "−2 hrs", "+3:30 hrs" or "same time", relative to the device's own zone.
  static void diffText(char *buf, size_t len, int secs, bool capital) {
    if (secs == 0) {
      strlcpy(buf, capital ? "Same time" : "same time", len);
      return;
    }
    const char *sign = secs > 0 ? "+" : "\xE2\x88\x92";   // a true minus sign, as in the design
    int mins = abs(secs) / 60, h = mins / 60, m = mins % 60;
    if (m) snprintf(buf, len, "%s%d:%02d hrs", sign, h, m);
    else snprintf(buf, len, "%s%d hr%s", sign, h, h == 1 ? "" : "s");
  }

  void updateZones(time_t now) {
    if (now < 1000000000) return;
    int off[MAX_CITIES];
    cityOffsets(worldCity, zoneCount, now, off);
    int localOff = localOffset(now);
    struct tm lt;
    localtime_r(&now, &lt);
    for (int i = 0; i < zoneCount; i++) {
      time_t shifted = now + off[i];
      struct tm zt;
      gmtime_r(&shifted, &zt);
      char buf[40], diff[20];
      if (prefs::use24h()) snprintf(buf, sizeof(buf), "%02d:%02d", zt.tm_hour, zt.tm_min);
      else snprintf(buf, sizeof(buf), "%d:%02d %s", zt.tm_hour % 12 == 0 ? 12 : zt.tm_hour % 12, zt.tm_min, zt.tm_hour < 12 ? "AM" : "PM");
      setText(zoneTime[i], buf);

      int64_t dd = daysFromCivil(zt.tm_year + 1900, zt.tm_mon + 1, zt.tm_mday) -
                   daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
      const char *day = dd == 0 ? "Today" : dd > 0 ? "Tomorrow" : "Yesterday";
      diffText(diff, sizeof(diff), off[i] - localOff, false);
      snprintf(buf, sizeof(buf), "%s, %s", day, diff);
      setText(zoneCaption[i], buf);
    }
  }

  // ---- world clock settings ----

  static int findCity(const char *name) {
    for (int i = 0; i < CITY_COUNT; i++)
      if (strcmp(CITIES[i].name, name) == 0) return i;
    return -1;
  }
  bool hasCity(int c) const {
    for (int i = 0; i < worldCount; i++)
      if (worldCity[i] == c) return true;
    return false;
  }

  void loadWorld() {
    if (worldLoaded) return;
    worldLoaded = true;
    Preferences p;
    p.begin("clock", true);
    worldOn = p.getBool("world", false);
    String list = p.getString("cities", "London|Dubai");   // a sensible pair until the person picks their own
    p.end();
    worldCount = 0;
    int from = 0;
    while (from < (int)list.length() && worldCount < MAX_CITIES) {
      int bar = list.indexOf('|', from);
      if (bar < 0) bar = list.length();
      int c = findCity(list.substring(from, bar).c_str());
      if (c >= 0 && !hasCity(c)) worldCity[worldCount++] = c;
      from = bar + 1;
    }
  }

  void saveWorld() {
    String list;
    for (int i = 0; i < worldCount; i++) {
      if (i) list += '|';
      list += CITIES[worldCity[i]].name;
    }
    Preferences p;
    p.begin("clock", false);
    p.putBool("world", worldOn);
    p.putString("cities", list);
    p.end();
  }

  // Rows are redrawn from an async call: the tap that changed the list may come from a row about to be deleted.
  void redrawWorldLater() {
    lv_async_call([](void *p) { ((ClockApp *)p)->redrawWorld(); }, this);
  }

  void redrawWorld() {
    if (!worldGroup) return;
    const Palette &p = theme::p();
    while (lv_obj_get_child_cnt(worldGroup) > 1) lv_obj_del(lv_obj_get_child(worldGroup, 1));   // keep the switch
    lv_label_set_text(worldNote, worldOn ? "These cities appear at the bottom of the Clock app, up to three."
                                         : "Off: the Clock app shows only the local time.");
    if (!worldOn) return;

    time_t now = time(nullptr);
    bool synced = now >= 1000000000;
    int off[MAX_CITIES];
    if (synced) cityOffsets(worldCity, worldCount, now, off);
    int localOff = synced ? localOffset(now) : 0;
    for (int i = 0; i < worldCount; i++) {
      char diff[20] = "";
      if (synced) diffText(diff, sizeof(diff), off[i] - localOff, true);
      ui::Row r = ui::row(worldGroup, nullptr, lv_color_black(), CITIES[worldCity[i]].name, diff);
      lv_obj_set_style_text_font(r.value, theme::footnote, 0);

      // Red remove badge ahead of the name, with a 44 px touch target.
      lv_obj_t *rm = ui::box(r.obj);
      lv_obj_move_to_index(rm, 0);
      lv_obj_set_size(rm, 22, 22);
      lv_obj_set_style_radius(rm, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(rm, p.red, 0);
      lv_obj_set_style_bg_opa(rm, LV_OPA_COVER, 0);
      lv_obj_set_style_opa(rm, LV_OPA_50, LV_STATE_PRESSED);
      lv_obj_add_flag(rm, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(rm, 11);
      lv_obj_t *bar = ui::box(rm);
      lv_obj_set_size(bar, 10, 2);
      lv_obj_set_style_radius(bar, 1, 0);
      lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_center(bar);
      ui::insetSeparator(r.obj, 22 + 12);   // hairline starts at the name, past the badge
      lv_obj_set_user_data(rm, (void *)(intptr_t)i);
      lv_obj_add_event_cb(rm, [](lv_event_t *e) {
        auto *self = (ClockApp *)lv_event_get_user_data(e);
        int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        if (idx >= self->worldCount) return;
        for (int j = idx; j < self->worldCount - 1; j++) self->worldCity[j] = self->worldCity[j + 1];
        self->worldCount--;
        self->saveWorld();
        self->redrawWorldLater();
      }, LV_EVENT_CLICKED, this);
    }
    if (worldCount < MAX_CITIES) {
      ui::Row add = ui::row(worldGroup, nullptr, lv_color_black(), "Add City" "\xE2\x80\xA6", nullptr,
                            [this] { pickCity(); }, false);
      lv_obj_set_style_text_color(add.title, p.accent, 0);
    }
  }

  // The picker lists every city not already shown; rows go in a chunk per timer tick to keep the UI responsive.
  struct CityPicker {
    lv_obj_t *page, *group;
    lv_timer_t *timer;
    ClockApp *app;
    char letter;
    int next;
  };

  void buildCityChunk(CityPicker *b) {
    const int CHUNK = 16;
    int idx[CHUNK], n = 0;
    while (b->next < CITY_COUNT && n < CHUNK) {
      if (!hasCity(b->next)) idx[n++] = b->next;
      b->next++;
    }
    time_t now = time(nullptr);
    bool synced = now >= 1000000000;
    int off[CHUNK];
    if (synced && n) cityOffsets(idx, n, now, off);
    int localOff = synced ? localOffset(now) : 0;
    for (int k = 0; k < n; k++) {
      const City &c = CITIES[idx[k]];
      if (c.name[0] != b->letter) {   // alphabetical sections
        b->letter = c.name[0];
        char head[2] = {b->letter, 0};
        b->group = ui::group(b->page, head);
      }
      char diff[20] = "";
      if (synced) diffText(diff, sizeof(diff), off[k] - localOff, true);
      int city = idx[k];
      ui::row(b->group, nullptr, lv_color_black(), c.name, diff, [this, city] {
        if (worldCount < MAX_CITIES && !hasCity(city)) {
          worldCity[worldCount++] = city;
          saveWorld();
        }
        ui::pop();
        redrawWorldLater();
      }, false);
    }
    if (b->next >= CITY_COUNT && b->timer) lv_timer_pause(b->timer);
  }

  void pickCity() {
    if (!settingsRoot) return;
    ui::push(settingsRoot, "Add City", [this](lv_obj_t *c) {
      auto *b = new CityPicker{c, nullptr, nullptr, this, 0, 0};
      buildCityChunk(b);
      if (b->next < CITY_COUNT) {
        b->timer = lv_timer_create([](lv_timer_t *t) {
          auto *b = (CityPicker *)t->user_data;
          b->app->buildCityChunk(b);
        }, 30, b);
      }
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *b = (CityPicker *)lv_event_get_user_data(e);
        if (b->timer) lv_timer_del(b->timer);
        delete b;
      }, LV_EVENT_DELETE, b);
    });
  }

  // "16:05" in 24-hour mode; "4:05" plus "PM" in 12-hour mode (suffix is empty in 24-hour mode).
  static bool clockText(char *buf, size_t len, char *suffix) {
    time_t now = time(nullptr);
    if (now < 1000000000) return false;
    struct tm tm;
    localtime_r(&now, &tm);
    if (prefs::use24h()) {
      snprintf(buf, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
      suffix[0] = 0;
    } else {
      snprintf(buf, len, "%d:%02d", tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12, tm.tm_min);
      strcpy(suffix, tm.tm_hour < 12 ? "AM" : "PM");
    }
    return true;
  }

  static bool format(char *buf, size_t len, const char *fmt) {
    time_t now = time(nullptr);
    if (now < 1000000000) return false;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, fmt, &tm);
    return true;
  }
};

App *createClockApp() { return new ClockApp(); }
