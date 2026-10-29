#include "os.h"
#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
#include "os/hal.h"
#include "os/prefs.h"
#include "os/ui.h"
#include "os/wallpaper.h"
#include "services/storage.h"
#include "services/weather.h"

namespace os {

static std::vector<App *> registry;
static App *openApp = nullptr;
static lv_obj_t *home = nullptr, *appRoot = nullptr;
static lv_obj_t *statusBar = nullptr, *clockLabel = nullptr, *sysIcons = nullptr, *homeIndicator = nullptr;
static SystemStatus (*statusFn)() = nullptr;
static App *statusOwner = nullptr;
static int barState = -2;
static uint32_t lastSecond = 0, lastTick = 0;

struct Tile {
  App *app;
  lv_obj_t *tile, *glyph, *dot;   // dot is null in the All Apps grid
};
static std::vector<Tile> tiles;      // the dock
static std::vector<Tile> libTiles;   // the All Apps grid, while it is open
static lv_obj_t *homeClock = nullptr, *homeAmPm = nullptr, *homeDate = nullptr, *homeWx = nullptr;

// All Apps: a full-screen page that rises over the dimmed Home Screen.
enum LibClose { LIB_ANIMATE, LIB_LINGER, LIB_NOW };
static lv_obj_t *libRoot = nullptr, *libPage = nullptr, *libGrid = nullptr, *libSearchText = nullptr,
                *libSearchClear = nullptr;
static String libFilter;
static void closeLibrary(LibClose how);
static void rebuildLibrary();
static void closeMenu();
static void animOpa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
static void animY(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }

// The wallpaper is rendered once per appearance into a PSRAM image: LVGL's gradient cache can hand
// back a stale gradient after a theme change, and a dithered gradient avoids 16-bit banding.
// It's a 4 x 480 strip in internal RAM that LVGL tiles across the screen, so drawing it never
// touches PSRAM, which the panel needs for its own refresh.
static const int WP_W = 4;
static lv_img_dsc_t gradientImg;
static lv_timer_t *wpRetry = nullptr;
static int wpTries = 0;
static uint16_t *wallpaperPx = nullptr;

static void renderWallpaper(lv_color_t top, lv_color_t bottom) {
  if (!wallpaperPx) wallpaperPx = (uint16_t *)heap_caps_malloc(WP_W * 480 * 2, MALLOC_CAP_INTERNAL);
  static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  lv_color32_t a, b;
  a.full = lv_color_to32(top);
  b.full = lv_color_to32(bottom);
  for (int y = 0; y < 480; y++) {
    float t = y / 479.0f;
    float r = a.ch.red + (b.ch.red - a.ch.red) * t, g = a.ch.green + (b.ch.green - a.ch.green) * t,
          bl = a.ch.blue + (b.ch.blue - a.ch.blue) * t;
    for (int x = 0; x < WP_W; x++) {
      float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f;  // ordered dither before truncating to 5/6/5 bits
      int R = min(31, int(r / 8 + d)), G = min(63, int(g / 4 + d)), B = min(31, int(bl / 8 + d));
      wallpaperPx[y * WP_W + x] = (R << 11) | (G << 5) | B;
    }
  }
  gradientImg.header.always_zero = 0;
  gradientImg.header.w = WP_W;
  gradientImg.header.h = 480;
  gradientImg.header.cf = LV_IMG_CF_TRUE_COLOR;
  gradientImg.data_size = WP_W * 480 * 2;
  gradientImg.data = (const uint8_t *)wallpaperPx;
  lv_img_cache_invalidate_src(&gradientImg);
}

void registerApp(App *app) { registry.push_back(app); }
void unregisterApp(App *app) {
  if (openApp == app) goHome();
  stop(app);
  registry.erase(std::remove(registry.begin(), registry.end(), app), registry.end());
}
static void buildHome();
void rebuildHome() {
  buildHome();
  rebuildLibrary();
}
const std::vector<App *> &apps() { return registry; }
App *current() { return openApp; }
App *find(const char *id) {
  for (App *a : registry)
    if (!strcmp(a->id(), id)) return a;
  return nullptr;
}

// ---------- appearance ----------

static bool wantDark() {
  switch (prefs::appearance()) {
    case APPEARANCE_LIGHT: return false;
    case APPEARANCE_DARK: return true;
    default: {
      time_t now = time(nullptr);
      if (now < 1000000000) return true;  // clock not set yet: dark is kinder at night
      struct tm tm;
      localtime_r(&now, &tm);
      int minutes = tm.tm_hour * 60 + tm.tm_min;
      return minutes < 6 * 60 + 30 || minutes >= 18 * 60 + 30;
    }
  }
}

static void buildHome();
static void buildStatusBar();

void refreshAppearance() {
  bool dark = wantDark();
  if (dark == theme::isDark() && home) return;
  theme::apply(dark);
  buildStatusBar();
  buildHome();
  rebuildLibrary();
  if (openApp) {
    // Rebuild the open app in the new colours, back at its first page.
    App *a = openApp;
    a->close();
    ui::resetStack();
    lv_obj_clean(appRoot);
    a->open(appRoot);
  }
}

// ---------- status bar ----------

static void updateStatusBar() {
  const Palette &p = theme::p();
  lv_color_t fg = openApp ? p.label : libRoot ? lv_color_white() : p.homeLabel;  // All Apps is always dark
  time_t now = time(nullptr);
  char buf[16] = "--:--";
  if (now > 1000000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    if (prefs::use24h()) snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    else snprintf(buf, sizeof(buf), "%d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min,
                  tm.tm_hour < 12 ? "AM" : "PM");
  }
  lv_label_set_text(clockLabel, buf);
  lv_obj_set_style_text_color(clockLabel, fg, 0);

  String icons = WiFi.status() == WL_CONNECTED ? ICON_WIFI : ICON_WIFI_OFF;
  if (storageReady()) icons += "  " ICON_SD;
  lv_label_set_text(sysIcons, icons.c_str());
  lv_obj_set_style_text_color(sysIcons, fg, 0);

  // The provider only keeps the screen bright during an outage; Netwatch's icon shows the state.
  if (statusFn) hal::setAlarm(statusFn().level == STATUS_ALERT);
  // In an app the bar is opaque in the page colour so scrolled content never shows behind it.
  int want = openApp ? (int)lv_color_to32(p.background) : -1;  // -1: transparent over the wallpaper
  if (want != barState) {
    barState = want;
    lv_obj_set_style_bg_color(statusBar, p.background, 0);
    lv_obj_set_style_bg_opa(statusBar, openApp ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  }
  if (homeIndicator) {
    lv_obj_set_style_bg_color(homeIndicator, p.label, 0);
    if (openApp) lv_obj_clear_flag(homeIndicator, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(homeIndicator, LV_OBJ_FLAG_HIDDEN);
  }
}

static void buildStatusBar() {
  if (statusBar) lv_obj_del(statusBar);
  if (homeIndicator) lv_obj_del(homeIndicator);
  statusBar = ui::box(lv_layer_top());
  barState = -2;  // new object: apply the background again
  lv_obj_set_size(statusBar, 480, ui::STATUS_H);
  lv_obj_set_style_pad_hor(statusBar, 18, 0);
  lv_obj_clear_flag(statusBar, LV_OBJ_FLAG_CLICKABLE);

  clockLabel = ui::label(statusBar, "--:--", theme::footnoteBold, theme::p().label);
  lv_obj_align(clockLabel, LV_ALIGN_LEFT_MID, 0, 0);
  sysIcons = ui::label(statusBar, "", theme::footnote, theme::p().label);
  lv_obj_align(sysIcons, LV_ALIGN_RIGHT_MID, 0, 0);


  homeIndicator = ui::box(lv_layer_top());
  lv_obj_set_size(homeIndicator, 140, 5);
  lv_obj_align(homeIndicator, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_radius(homeIndicator, 3, 0);
  lv_obj_set_style_bg_opa(homeIndicator, LV_OPA_60, 0);
  lv_obj_clear_flag(homeIndicator, LV_OBJ_FLAG_CLICKABLE);
  updateStatusBar();
}

// ---------- home screen ----------

static void showMenu(App *app, lv_obj_t *cell, int tileSize, int tileRadius);
static void openLibrary(bool animate);

static void showIf(lv_obj_t *o, bool shown) {
  if (shown) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void refreshTileList(std::vector<Tile> &list) {
  const Palette &p = theme::p();
  for (Tile &t : list) {
    showIf(t.glyph, !t.app->liveIcon(t.tile));
    if (t.dot) {
      lv_obj_set_style_bg_color(t.dot, p.homeLabel, 0);
      showIf(t.dot, t.app->running);
    }
  }
}

static void refreshTiles() {
  refreshTileList(tiles);
  refreshTileList(libTiles);
}

static void setText(lv_obj_t *l, const char *s) {
  if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

// Big clock, date and weather line on the Home Screen (refreshed every second).
static void updateHomeText() {
  if (!home || !homeClock) return;
  time_t now = time(nullptr);
  char buf[16] = "--:--", ampm[3] = "", date[48] = "";
  if (now > 1000000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    if (prefs::use24h()) {
      snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
      snprintf(buf, sizeof(buf), "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
      strcpy(ampm, tm.tm_hour < 12 ? "AM" : "PM");
    }
    char wd[16], mo[16];
    strftime(wd, sizeof(wd), "%A", &tm);
    strftime(mo, sizeof(mo), "%B", &tm);
    snprintf(date, sizeof(date), "%s, %d %s", wd, tm.tm_mday, mo);
  }
  setText(homeClock, buf);
  setText(homeAmPm, ampm);
  showIf(homeAmPm, ampm[0] != 0);
  setText(homeDate, date);

  Weather w = weatherGet();
  if (w.valid) {
    char wx[96];
    int n = snprintf(wx, sizeof(wx), "%s  %s", weatherIcon(w.code, w.isDay), weatherText(w.code));
    if (w.numDays > 0)
      snprintf(wx + n, sizeof(wx) - n, " \xC2\xB7 H %.0f\xC2\xB0 L %.0f\xC2\xB0", w.days[0].high, w.days[0].low);
    setText(homeWx, wx);
  }
  showIf(homeWx, w.valid);
}

// A dock or grid cell: rounded tile with a glyph, name underneath. The tile and glyph are returned.
static lv_obj_t *buildCell(lv_obj_t *parent, int cellW, int ts, int radius, const char *name, const char *icon,
                           lv_color_t tint, lv_opa_t tintOpa, lv_color_t labelColor, lv_obj_t **tileOut,
                           lv_obj_t **glyphOut) {
  lv_obj_t *cell = ui::box(parent);
  lv_obj_set_size(cell, cellW, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(cell, 6, 0);
  lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(cell, LV_OPA_60, LV_STATE_PRESSED);

  lv_obj_t *tile = ui::box(cell);
  lv_obj_set_size(tile, ts, ts);
  lv_obj_set_style_radius(tile, radius, 0);
  lv_obj_set_style_bg_color(tile, tint, 0);
  lv_obj_set_style_bg_opa(tile, tintOpa, 0);
  lv_obj_set_style_clip_corner(tile, true, 0);
  lv_obj_t *glyph = ui::label(tile, icon, theme::icon36, lv_color_white());
  lv_obj_center(glyph);

  lv_obj_t *l = ui::label(cell, name, theme::caption, labelColor);
  lv_obj_set_width(l, cellW - 4);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  *tileOut = tile;
  *glyphOut = glyph;
  return cell;
}

static void wireApp(lv_obj_t *cell, App *app, int ts, int radius) {
  struct Ctx { App *app; int ts, radius; };
  auto *ctx = new Ctx{app, ts, radius};
  lv_obj_add_event_cb(cell, [](lv_event_t *e) { open(((Ctx *)lv_event_get_user_data(e))->app); },
                      LV_EVENT_SHORT_CLICKED, ctx);
  lv_obj_add_event_cb(cell, [](lv_event_t *e) {
    Ctx *c = (Ctx *)lv_event_get_user_data(e);
    showMenu(c->app, lv_event_get_target(e), c->ts, c->radius);
  }, LV_EVENT_LONG_PRESSED, ctx);
  lv_obj_add_event_cb(cell, [](lv_event_t *e) { delete (Ctx *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, ctx);
}

static void buildHome() {
  const Palette &p = theme::p();
  if (home) lv_obj_del(home);
  tiles.clear();
  home = ui::box(lv_scr_act());
  lv_obj_set_size(home, 480, 480);
  lv_obj_move_background(home);
  // A custom SD wallpaper (cached in PSRAM, scrim already baked in) or the built-in gradient strip.
  const lv_img_dsc_t *custom = wallpaper::image();
  lv_obj_t *wp = lv_img_create(home);
  if (custom) {
    lv_img_set_src(wp, custom);
  } else {
    renderWallpaper(p.wallpaperTop, p.wallpaperBottom);
    lv_img_set_src(wp, &gradientImg);
    lv_obj_set_size(wp, 480, 480);  // larger than the source: LVGL repeats the strip
    // A selected wallpaper that can't be loaded yet (SD card still mounting): retry a few times.
    if (!wallpaper::current().isEmpty() && !wpRetry) {
      wpTries = 10;
      wpRetry = lv_timer_create([](lv_timer_t *t) {
        if (wallpaper::image() || --wpTries <= 0) {
          lv_timer_del(t);
          wpRetry = nullptr;
          if (wallpaper::image()) buildHome();
        }
      }, 3000, nullptr);
    }
  }

  // Glance: big clock, date, weather.
  lv_obj_t *col = ui::box(home);
  lv_obj_set_size(col, 424, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 4, 0);
  int lh = lv_font_get_line_height(theme::big);
  lv_obj_set_pos(col, 28, 52 - (lh - 88) / 2);  // the 88 px glyphs start at y 52
  lv_obj_t *row = ui::box(col);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 8, 0);
  homeClock = ui::label(row, "--:--", theme::big, p.homeLabel);
  lv_obj_set_style_text_letter_space(homeClock, -3, 0);
  homeAmPm = ui::label(row, "", theme::title, p.homeLabel);
  lv_obj_set_style_pad_bottom(homeAmPm, 16, 0);
  homeDate = ui::label(col, "", theme::title, p.homeLabel);
  homeWx = ui::label(col, "", theme::body, p.homeLabel);
  lv_obj_set_style_text_opa(homeWx, LV_OPA_90, 0);
  lv_obj_add_flag(homeWx, LV_OBJ_FLAG_HIDDEN);
  updateHomeText();

  // Rotation widget: one tap turns the screen a quarter turn (Settings › Display has the full control).
  if (prefs::rotationWidget()) {
    lv_obj_t *w = ui::box(home);
    lv_obj_set_size(w, 150, 92);
    lv_obj_set_pos(w, 28, 236);
    lv_obj_set_style_radius(w, 20, 0);
    lv_obj_set_style_bg_color(w, p.dark ? lv_color_hex(0x141828) : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(w, p.dark ? LV_OPA_50 + 12 : LV_OPA_70 + 4, 0);
    lv_obj_set_style_border_width(w, 1, 0);
    lv_obj_set_style_border_color(w, lv_color_white(), 0);
    lv_obj_set_style_border_opa(w, p.dark ? LV_OPA_10 + 2 : LV_OPA_60, 0);
    lv_obj_set_style_pad_all(w, 12, 0);
    lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(w, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_t *ic = ui::label(w, ICON_ROTATE, theme::title, p.homeLabel);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
    char deg[8];
    snprintf(deg, sizeof(deg), "%d\xC2\xB0", prefs::rotation() * 90);
    lv_obj_t *v = ui::label(w, deg, theme::title, p.homeLabel);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *t = ui::label(w, "Rotation", theme::footnoteBold, p.homeLabel);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, -16);
    lv_obj_t *hint = ui::label(w, "Tap to turn 90\xC2\xB0", theme::caption, p.homeLabel);
    lv_obj_set_style_text_opa(hint, LV_OPA_80, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    lv_obj_add_event_cb(w, [](lv_event_t *) {
      prefs::setRotation((prefs::rotation() + 1) & 3);
      hal::setRotation(prefs::rotation());
      lv_async_call([](void *) { buildHome(); }, nullptr);  // refresh the angle shown
    }, LV_EVENT_CLICKED, nullptr);
  }

  // Dock: Netwatch, Clock, Weather, Files, All Apps, Settings.
  lv_obj_t *dock = ui::box(home);
  lv_obj_set_size(dock, 448, 112);
  lv_obj_set_pos(dock, 16, 480 - 22 - 112);
  lv_obj_set_style_radius(dock, 28, 0);
  lv_obj_set_style_bg_color(dock, p.dark ? lv_color_hex(0x141828) : lv_color_white(), 0);
  lv_obj_set_style_bg_opa(dock, p.dark ? LV_OPA_50 + 12 : LV_OPA_70 + 4, 0);  // ~55 % dark, ~72 % light
  lv_obj_set_style_border_width(dock, 1, 0);
  lv_obj_set_style_border_color(dock, lv_color_white(), 0);
  lv_obj_set_style_border_opa(dock, p.dark ? LV_OPA_10 + 2 : LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(dock, 4, 0);
  lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  static const char *const slots[] = {"netwatch", "clock", "weather", "files", nullptr, "settings"};
  for (const char *id : slots) {
    lv_obj_t *tile, *glyph, *cell;
    if (!id) {
      cell = buildCell(dock, 72, 56, 14, "All Apps", ICON_APPS, lv_color_white(), LV_OPA_20 + 2, p.homeLabel, &tile,
                       &glyph);
      if (!p.dark) lv_obj_set_style_bg_color(tile, lv_color_hex(0x636366), 0), lv_obj_set_style_bg_opa(tile, LV_OPA_50, 0);
      lv_obj_add_event_cb(cell, [](lv_event_t *) { openLibrary(true); }, LV_EVENT_SHORT_CLICKED, nullptr);
      continue;
    }
    App *app = find(id);
    if (!app) continue;
    cell = buildCell(dock, 72, 56, 14, app->name(), app->icon(), app->tint(), LV_OPA_COVER, p.homeLabel, &tile,
                     &glyph);
    lv_obj_set_style_pad_bottom(cell, 8, 0);
    lv_obj_t *dot = ui::box(cell);  // running indicator, kept small and quiet under the label
    lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(dot, 4, 4);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, 6);
    lv_obj_set_style_radius(dot, 2, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_60, 0);
    tiles.push_back({app, tile, glyph, dot});
    wireApp(cell, app, 56, 14);
  }
  refreshTiles();
}

// ---------- All Apps ----------

static bool containsNoCase(const char *hay, const String &needle) {
  if (needle.isEmpty()) return true;
  String h(hay);
  h.toLowerCase();
  String n(needle);
  n.toLowerCase();
  return h.indexOf(n) >= 0;
}

static void fillLibraryGrid() {
  libTiles.clear();
  lv_obj_clean(libGrid);
  lv_obj_scroll_to_y(libGrid, 0, LV_ANIM_OFF);
  lv_color_t white = lv_color_white();
  int shown = 0;
  for (int pass = 0; pass < 2; pass++) {
    std::vector<App *> list;
    for (App *a : registry)
      if (a->removable() == (pass == 1) && containsNoCase(a->name(), libFilter)) list.push_back(a);
    if (list.empty()) continue;
    shown += list.size();
    lv_obj_t *head = ui::label(libGrid, pass ? "INSTALLED \xC2\xB7 ON SD CARD" : "BUILT IN", theme::caption, white);
    lv_obj_set_style_text_opa(head, LV_OPA_70, 0);
    lv_obj_set_style_text_letter_space(head, 1, 0);
    lv_obj_t *grid = ui::box(libGrid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 14, 0);
    for (App *app : list) {
      lv_obj_t *tile, *glyph;
      lv_obj_t *cell = buildCell(grid, 110, 60, 15, app->name(), app->icon(), app->tint(), LV_OPA_COVER, white,
                                 &tile, &glyph);
      libTiles.push_back({app, tile, glyph, nullptr});
      wireApp(cell, app, 60, 15);
    }
  }
  if (!shown) {
    lv_obj_t *none = ui::label(libGrid, "No Results", theme::body, white);
    lv_obj_set_style_text_opa(none, LV_OPA_70, 0);
  }
  refreshTileList(libTiles);
}

static void updateSearchField() {
  bool has = !libFilter.isEmpty();
  lv_label_set_text(libSearchText, has ? libFilter.c_str() : "Search");
  lv_obj_set_style_text_opa(libSearchText, has ? LV_OPA_COVER : LV_OPA_60, 0);
  showIf(libSearchClear, has);
}

static void buildLibraryPage(lv_obj_t *page) {
  libPage = page;
  lv_color_t white = lv_color_white();
  // Dimmed wallpaper behind: the page itself is the scrim.
  lv_obj_set_style_bg_color(page, lv_color_hex(0x060A14), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_70 + 4, 0);  // ~72 %
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_hor(page, 20, 0);
  lv_obj_set_style_pad_bottom(page, 24, 0);
  lv_obj_set_style_pad_row(page, 10, 0);
  lv_obj_set_style_text_color(lv_obj_get_child(page, 0), white, 0);  // the large title

  lv_obj_t *done = ui::box(page);
  lv_obj_add_flag(done, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(done, 72, 44);
  lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, 10);
  lv_obj_set_style_opa(done, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_t *dl = ui::label(done, "Done", theme::bodyBold, white);
  lv_obj_align(dl, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(done, [](lv_event_t *) { closeLibrary(LIB_ANIMATE); }, LV_EVENT_CLICKED, nullptr);

  // Search field: tapping it opens the full-page text entry; the grid filters by name.
  lv_obj_t *field = ui::box(page);
  lv_obj_set_size(field, LV_PCT(100), 44);
  lv_obj_set_style_radius(field, 12, 0);
  lv_obj_set_style_bg_color(field, white, 0);
  lv_obj_set_style_bg_opa(field, LV_OPA_20 - 2, 0);
  lv_obj_set_style_pad_hor(field, 14, 0);
  lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(field, LV_OPA_70, LV_STATE_PRESSED);
  libSearchText = ui::label(field, "Search", theme::body, white);
  lv_obj_set_width(libSearchText, 340);
  lv_label_set_long_mode(libSearchText, LV_LABEL_LONG_DOT);
  lv_obj_align(libSearchText, LV_ALIGN_LEFT_MID, 0, 0);
  libSearchClear = ui::box(field);
  lv_obj_add_flag(libSearchClear, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(libSearchClear, 44, 44);
  lv_obj_align(libSearchClear, LV_ALIGN_RIGHT_MID, 14, 0);
  lv_obj_t *x = ui::label(libSearchClear, ICON_BACKSPACE, theme::body, white);
  lv_obj_center(x);
  lv_obj_add_event_cb(libSearchClear, [](lv_event_t *) {
    libFilter = "";
    updateSearchField();
    fillLibraryGrid();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(field, [](lv_event_t *) {
    ui::textInput(libRoot, "Search", "Search apps", libFilter.c_str(), false, [](const String &t) {
      libFilter = t;
      libFilter.trim();
      if (!libGrid) return;
      updateSearchField();
      fillLibraryGrid();
    });
  }, LV_EVENT_CLICKED, nullptr);

  libGrid = ui::box(page);
  lv_obj_set_size(libGrid, LV_PCT(100), 304);
  lv_obj_add_flag(libGrid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(libGrid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(libGrid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(libGrid, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(libGrid, 10, 0);
  updateSearchField();
  fillLibraryGrid();
}

static void animateRoot(lv_obj_t *root, bool in, void (*done)(lv_anim_t *)) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, root);
  if (prefs::reduceMotion()) {
    lv_anim_set_exec_cb(&a, animOpa);
    lv_anim_set_values(&a, in ? 0 : 255, in ? 255 : 0);
    lv_anim_set_time(&a, 120);
  } else {
    lv_anim_set_exec_cb(&a, animY);
    lv_anim_set_values(&a, in ? 480 : 0, in ? 0 : 480);
    lv_anim_set_time(&a, in ? 300 : 260);
    lv_anim_set_path_cb(&a, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
  }
  if (done) lv_anim_set_ready_cb(&a, done);
  lv_anim_start(&a);
}

static void openLibrary(bool animate) {
  if (libRoot || openApp) return;
  closeMenu();
  hal::wake();
  libRoot = ui::box(lv_scr_act());
  lv_obj_set_size(libRoot, 480, 480);
  ui::resetStack();
  ui::push(libRoot, "All Apps", buildLibraryPage);
  if (animate) animateRoot(libRoot, true, nullptr);
  updateStatusBar();
}

static void closeLibrary(LibClose how) {
  if (!libRoot) return;
  closeMenu();
  libTiles.clear();
  libGrid = libPage = libSearchText = libSearchClear = nullptr;
  ui::resetStack();
  lv_obj_t *root = libRoot;
  libRoot = nullptr;
  if (how == LIB_NOW) {
    lv_obj_del(root);
  } else if (how == LIB_LINGER) {
    // An app is opening over it: keep it underneath until the app has risen.
    lv_timer_t *t = lv_timer_create([](lv_timer_t *t) { lv_obj_del_async((lv_obj_t *)t->user_data); }, 400, root);
    lv_timer_set_repeat_count(t, 1);
  } else {
    animateRoot(root, false, [](lv_anim_t *an) { lv_obj_del_async((lv_obj_t *)an->var); });
  }
  updateStatusBar();
}

static void rebuildLibrary() {
  if (!libRoot || ui::currentPage() != libPage) return;  // a text entry is open on top: leave it
  closeLibrary(LIB_NOW);
  openLibrary(false);
}


// ---------- long-press menu ----------

static lv_obj_t *menuScrim = nullptr;

static void closeMenu() {
  if (menuScrim) lv_obj_del_async(menuScrim);
  menuScrim = nullptr;
}

static void menuItem(lv_obj_t *card, const char *text, const char *icon, lv_color_t color, ui::Action run) {
  const Palette &p = theme::p();
  lv_obj_t *r = ui::box(card);
  lv_obj_set_size(r, LV_PCT(100), 50);
  lv_obj_set_style_pad_hor(r, 16, 0);
  if (lv_obj_get_child_cnt(card) > 1) {
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, p.separator, 0);
  }
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r, p.cardPressed, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_t *t = ui::label(r, text, theme::body, color);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *i = ui::label(r, icon, theme::body, color);
  lv_obj_align(i, LV_ALIGN_RIGHT_MID, 0, 0);
  auto *fn = new ui::Action(std::move(run));
  lv_obj_add_event_cb(r, [](lv_event_t *e) {
    ui::Action run = *(ui::Action *)lv_event_get_user_data(e);
    closeMenu();
    run();
  }, LV_EVENT_CLICKED, fn);
  lv_obj_add_event_cb(r, [](lv_event_t *e) { delete (ui::Action *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, fn);
}

static void showMenu(App *app, lv_obj_t *cell, int ts, int tileRadius) {
  const Palette &p = theme::p();
  closeMenu();
  menuScrim = ui::box(lv_layer_top());
  lv_obj_set_size(menuScrim, 480, 480);
  lv_obj_set_style_bg_color(menuScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(menuScrim, LV_OPA_50, 0);
  lv_obj_add_flag(menuScrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(menuScrim, [](lv_event_t *e) {
    if (lv_event_get_target(e) == menuScrim) closeMenu();  // tap outside the menu dismisses it
  }, LV_EVENT_CLICKED, nullptr);

  // Lifted copy of the icon where it sits, menu below it (or above when near the bottom).
  lv_area_t a;
  lv_obj_get_coords(cell, &a);
  lv_obj_t *icon = ui::box(menuScrim);
  int ix = a.x1 + (lv_area_get_width(&a) - ts) / 2;
  lv_obj_set_size(icon, ts, ts);
  lv_obj_set_pos(icon, ix, a.y1);
  lv_obj_set_style_radius(icon, tileRadius, 0);
  lv_obj_set_style_bg_color(icon, app->tint(), 0);
  lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
  lv_obj_t *g = ui::label(icon, app->icon(), theme::icon36, lv_color_white());
  lv_obj_center(g);

  lv_obj_t *card = ui::box(menuScrim);
  lv_obj_set_size(card, 230, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_bg_color(card, p.dark ? lv_color_hex(0x2C2C2E) : lv_color_hex(0xF9F9F9), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(card, true, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *head = ui::label(card, app->name(), theme::footnote, p.secondary);
  lv_obj_set_style_pad_all(head, 12, 0);

  menuItem(card, "Open", ICON_OPEN, p.label, [app] { open(app); });
  menuItem(card, "App Info", ICON_INFO, p.label, [app] { showAppInfo(app); });
  if (app->hasService()) {
    if (app->running) menuItem(card, "Stop", ICON_STOP, p.red, [app] { stop(app); });
    else menuItem(card, "Start", ICON_PLAY, p.label, [app] { start(app); });
  }
  lv_obj_update_layout(card);
  int x = constrain(ix + ts / 2 - 115, 12, 480 - 230 - 12);
  int h = lv_obj_get_height(card);
  int y = a.y1 + ts + 10;
  if (y + h > 470) y = a.y1 - h - 10;
  lv_obj_set_pos(card, x, y);
}

// ---------- app lifecycle ----------

void start(App *app) {
  if (!app->hasService() || app->running) return;
  app->startService();
  app->running = true;
  refreshTiles();
}

void stop(App *app) {
  if (!app->running) return;
  app->stopService();
  app->running = false;
  refreshTiles();
}

void open(App *app) {
  closeMenu();
  if (openApp == app) return;
  closeLibrary(LIB_LINGER);
  if (openApp) {
    openApp->close();
    ui::resetStack();
    lv_obj_del(appRoot);
  }
  hal::wake();
  openApp = app;
  if (app->hasService() && !app->running) start(app);  // opening an app runs it
  appRoot = ui::box(lv_scr_act());
  lv_obj_set_size(appRoot, 480, 480);
  app->open(appRoot);

  // Open: the app rises from the bottom edge. Moving opaque pixels is cheap; a whole-screen fade
  // blends every pixel, so it's kept for Reduce Motion only.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, appRoot);
  if (prefs::reduceMotion()) {
    lv_anim_set_exec_cb(&a, animOpa);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 120);
  } else {
    lv_anim_set_exec_cb(&a, animY);
    lv_anim_set_values(&a, 480, 0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  }
  lv_anim_start(&a);
  updateStatusBar();
}

void goHome() {
  closeMenu();
  if (!openApp) {
    closeLibrary(LIB_ANIMATE);
    return;
  }
  openApp->close();
  openApp = nullptr;
  ui::resetStack();
  lv_obj_t *root = appRoot;
  appRoot = nullptr;
  // Home: the app follows the swipe up and away (a short fade with Reduce Motion).
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, root);
  if (prefs::reduceMotion()) {
    lv_anim_set_exec_cb(&a, animOpa);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 120);
  } else {
    lv_anim_set_exec_cb(&a, animY);
    lv_anim_set_values(&a, 0, -480);
    lv_anim_set_time(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  }
  lv_anim_set_ready_cb(&a, [](lv_anim_t *an) { lv_obj_del_async((lv_obj_t *)an->var); });
  lv_anim_start(&a);
  refreshTiles();
  updateStatusBar();
}

void showAppInfo(App *app) {
  App *settings = find("settings");
  if (!settings) return;
  open(settings);
  ui::push(appRoot, app->name(), [app](lv_obj_t *c) { appInfoPage(c, app); });
}

void appInfoPage(lv_obj_t *c, App *app) {
  const Palette &p = theme::p();
  lv_obj_t *head = ui::box(c);
  lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(head, 14, 0);
  lv_obj_t *tile = ui::box(head);
  lv_obj_set_size(tile, 56, 56);
  lv_obj_set_style_radius(tile, 13, 0);
  lv_obj_set_style_bg_color(tile, app->tint(), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_t *g = ui::label(tile, app->icon(), theme::icon36, lv_color_white());
  lv_obj_center(g);
  lv_obj_t *col = ui::box(head);
  lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui::label(col, app->name(), theme::title, p.label);
  ui::label(col, app->hasService() ? (app->running ? "Running" : "Stopped") : "Opens on demand", theme::footnote,
            p.secondary);

  lv_obj_t *grp = ui::group(c);
  ui::row(grp, ICON_OPEN, p.blue, "Open", nullptr, [app] { open(app); });

  app->buildSettings(c, ui::currentPage() ? lv_obj_get_parent(ui::currentPage()) : nullptr);

  if (app->hasService()) {
    grp = ui::group(c, "Background", app->serviceSummary());
    ui::switchRow(grp, ICON_PLAY, p.green, "Running", app->running, [app](bool on) { on ? start(app) : stop(app); });
    ui::switchRow(grp, ICON_POWER, p.gray, "Start at Boot", prefs::autostart(app->id(), app->defaultAutostart()),
                  [app](bool on) { prefs::setAutostart(app->id(), on); });
  }
  uint64_t bytes = app->storageBytes();
  if (bytes) {
    char buf[24];
    if (bytes < 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    grp = ui::group(c, "Storage");
    ui::row(grp, ICON_STORAGE, p.orange, "Storage Used", buf);
  }
}

void setStatusProvider(SystemStatus (*fn)(), App *owner) {
  statusFn = fn;
  statusOwner = owner;
}

// ---------- main loop ----------

void begin() {
  theme::apply(wantDark());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
  for (App *a : registry)
    if (a->hasService() && prefs::autostart(a->id(), a->defaultAutostart())) start(a);
  buildHome();
  buildStatusBar();
}

void loop() {
  hal::loop();
  switch (hal::takeGesture()) {
    case GESTURE_HOME: goHome(); break;
    case GESTURE_BACK: if (openApp || libRoot) ui::pop(); break;
    default: break;
  }
  uint32_t now = millis();
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    updateStatusBar();
    if (!openApp) {
      updateHomeText();
      refreshTiles();
    }
    refreshAppearance();
  }
  if (openApp && now - lastTick >= 250) {
    lastTick = now;
    openApp->tick();
  }
}
}
