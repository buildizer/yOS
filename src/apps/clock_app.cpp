// Clock: a big, calm time display for the desk or bedside.
#include "apps/apps.h"
#include "os/os.h"
#include "os/ui.h"
#include "os/prefs.h"
#include "services/weather.h"

static const int ZONES = 2;
struct ZoneDef { const char *city; bool dst; int stdOffset; };   // London follows EU summer time
static const ZoneDef ZONE_DEFS[ZONES] = {{"London", true, 0}, {"Dubai", false, 4 * 3600}};

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
    lv_obj_t *g = ui::group(content, nullptr, "Off shows 4:05 PM. On shows 16:05. This also changes the status bar.");
    ui::switchRow(g, nullptr, lv_color_black(), "24-Hour Time", prefs::use24h(),
                  [](bool on) { prefs::setUse24h(on); });
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

    // World clock card pinned to the bottom.
    lv_obj_t *card = ui::box(page);
    lv_obj_add_flag(card, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(card, 480 - 2 * ui::SIDE, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < ZONES; i++) {
      lv_obj_t *r = ui::box(card);
      lv_obj_set_size(r, LV_PCT(100), 56);
      lv_obj_set_style_pad_hor(r, 16, 0);
      lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      if (i > 0) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(0x38383A), 0);
      }
      lv_obj_t *col = ui::box(r);
      lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
      zoneCaption[i] = ui::label(col, "", theme::caption, grey);
      ui::label(col, ZONE_DEFS[i].city, theme::title, white);
      zoneTime[i] = ui::label(r, "", theme::largeTitle, white);
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

    time_t now = time(nullptr);
    for (int i = 0; i < ZONES; i++) updateZone(i, now);
  }

private:
  lv_obj_t *timeLabel = nullptr, *secLabel, *ampmLabel, *dateLabel, *weatherLabel, *weatherRow;
  lv_obj_t *zoneCaption[ZONES], *zoneTime[ZONES];

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
  // Unix time of 01:00 UTC on the last Sunday of the month (EU summer-time switch).
  static int64_t lastSundayUtc(int year, int month) {
    int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int last = dim[month - 1] + (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0);
    int64_t days = daysFromCivil(year, month, last);
    int wd = (int)((days % 7 + 11) % 7);   // 0 = Sunday (1970-01-01 was a Thursday)
    return (days - wd) * 86400 + 3600;
  }
  static int zoneOffset(const ZoneDef &z, time_t utc) {
    if (!z.dst) return z.stdOffset;
    struct tm g;
    gmtime_r(&utc, &g);
    int y = g.tm_year + 1900;
    bool summer = utc >= lastSundayUtc(y, 3) && utc < lastSundayUtc(y, 10);
    return z.stdOffset + (summer ? 3600 : 0);
  }

  // Offsets are worked out from UTC so the system time zone is never touched.
  void updateZone(int i, time_t now) {
    if (now < 1000000000) return;
    const ZoneDef &z = ZONE_DEFS[i];
    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
    int localOff = (int)(tmSeconds(lt) - tmSeconds(gt));
    time_t shifted = now + zoneOffset(z, now);
    struct tm zt;
    gmtime_r(&shifted, &zt);
    char buf[24];
    if (prefs::use24h()) snprintf(buf, sizeof(buf), "%02d:%02d", zt.tm_hour, zt.tm_min);
    else snprintf(buf, sizeof(buf), "%d:%02d %s", zt.tm_hour % 12 == 0 ? 12 : zt.tm_hour % 12, zt.tm_min, zt.tm_hour < 12 ? "AM" : "PM");
    setText(zoneTime[i], buf);

    int64_t dd = daysFromCivil(zt.tm_year + 1900, zt.tm_mon + 1, zt.tm_mday) -
                 daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    const char *day = dd == 0 ? "Today" : dd > 0 ? "Tomorrow" : "Yesterday";
    float hrs = (zoneOffset(z, now) - localOff) / 3600.0f;
    char diff[16];
    if (hrs == 0) strcpy(diff, "same time");
    else if (hrs == (int)hrs) snprintf(diff, sizeof(diff), "%+d hr%s", (int)hrs, hrs == 1 || hrs == -1 ? "" : "s");
    else snprintf(diff, sizeof(diff), "%+.1f hrs", hrs);
    snprintf(buf, sizeof(buf), "%s, %s", day, diff);
    setText(zoneCaption[i], buf);
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
