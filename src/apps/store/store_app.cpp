// App Store: Lua apps from the store/ folder of the yOS repo (Get, Update, Delete), and hiding or
// showing the built-in apps. Layout follows design/AppStore.dc.html and AppStoreDetail.dc.html.
#include <map>
#include "apps/apps.h"
#include "apps/store/catalog.h"
#include "config.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/ui.h"
#include "script/loader.h"
#include "services/github_update.h"
#include "version.h"

namespace {

enum Tab : uint8_t { DISCOVER, UPDATES, INSTALLED };
enum Pill : uint8_t { PILL_PLAIN, PILL_FILLED, PILL_RED, PILL_OFF };

// Apps that can't be hidden: without them there is no way back.
bool hideable(App *a) { return !a->removable() && strcmp(a->id(), "settings") && strcmp(a->id(), "store"); }

const appstore::Listing *findListing(const appstore::Status &s, const String &id) {
  for (const appstore::Listing &l : s.apps)
    if (l.id == id) return &l;
  return nullptr;
}

String formatSize(uint64_t bytes) {
  char buf[24];
  if (bytes >= 1048576) snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  else if (bytes >= 10240) snprintf(buf, sizeof(buf), "%u KB", (unsigned)(bytes / 1024));
  else snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  return buf;
}

bool tooOld(const appstore::Listing &l) {
  return l.requires.length() && ghupdate::compareVersions(YOS_FIRMWARE_VERSION, l.requires.c_str()) < 0;
}

// Opening an app deletes this one's screen, which holds the button being tapped: do it after the event.
void openLater(App *a) {
  lv_async_call([](void *a) { os::open((App *)a); }, a);
}

lv_color_t pillFill() { return theme::isDark() ? lv_color_hex(0x2C2C2E) : lv_color_hex(0xE9E9EE); }

lv_obj_t *wrapText(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = ui::label(parent, text, font, color);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

lv_obj_t *tile(lv_obj_t *parent, int size, const char *glyph, lv_color_t tint, const lv_font_t *font) {
  lv_obj_t *t = ui::box(parent);
  lv_obj_set_size(t, size, size);
  lv_obj_set_style_radius(t, size * 22 / 96, 0);
  lv_obj_set_style_bg_color(t, tint, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(ui::label(t, glyph, font, lv_color_white()));
  return t;
}

lv_obj_t *pill(lv_obj_t *parent, const char *text, Pill style, int height, ui::Action onTap) {
  const Palette &p = theme::p();
  lv_obj_t *b = ui::box(parent);
  lv_obj_set_size(b, LV_SIZE_CONTENT, height);
  lv_obj_set_style_min_width(b, height == 32 ? 76 : 88, 0);
  lv_obj_set_style_pad_hor(b, 14, 0);
  lv_obj_set_style_radius(b, height / 2, 0);
  lv_obj_set_style_bg_color(b, style == PILL_FILLED ? p.accent : pillFill(), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_ext_click_area(b, 6);
  lv_color_t fg = style == PILL_FILLED ? lv_color_white() : style == PILL_RED ? p.red : style == PILL_OFF ? p.tertiary : p.accent;
  lv_obj_t *l = ui::label(b, text, theme::footnoteBold, fg);
  lv_obj_center(l);
  if (style != PILL_OFF && onTap) {
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    auto *fn = new ui::Action(std::move(onTap));
    lv_obj_add_event_cb(b, [](lv_event_t *e) { (*(ui::Action *)lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, fn);
    lv_obj_add_event_cb(b, [](lv_event_t *e) { delete (ui::Action *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, fn);
  }
  return b;
}

// The download ring that replaces the button while an app is queued or downloading.
lv_obj_t *ring(lv_obj_t *parent, int size, uint8_t pct) {
  const Palette &p = theme::p();
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, size, size);
  lv_arc_set_rotation(a, 270);
  lv_arc_set_bg_angles(a, 0, 360);
  lv_arc_set_value(a, pct);
  lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_set_style_arc_width(a, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, p.separator, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, p.accent, LV_PART_INDICATOR);
  lv_obj_t *stop = ui::box(a);
  lv_obj_set_size(stop, size / 4, size / 4);
  lv_obj_set_style_radius(stop, 2, 0);
  lv_obj_set_style_bg_color(stop, p.accent, 0);
  lv_obj_set_style_bg_opa(stop, LV_OPA_COVER, 0);
  lv_obj_center(stop);
  return a;
}

void attachTap(lv_obj_t *obj, ui::Action onTap) {
  lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(obj, LV_OPA_60, LV_STATE_PRESSED);
  auto *fn = new ui::Action(std::move(onTap));
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { (*(ui::Action *)lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, fn);
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { delete (ui::Action *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, fn);
}

// A store row: 48 px tile, name and one line under it, and a pill button (or the download ring).
struct RowSpec {
  const char *glyph;
  lv_color_t tint;
  String name, sub, button;
  Pill style = PILL_PLAIN;
  bool busy = false;
  uint8_t pct = 0;
  ui::Action onButton, onTap;
};

void storeRow(lv_obj_t *group, const RowSpec &r) {
  const Palette &p = theme::p();
  bool first = lv_obj_get_child_cnt(group) == 0;
  lv_obj_t *row = ui::box(group);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(row, 68, 0);
  lv_obj_set_style_pad_hor(row, 14, 0);
  lv_obj_set_style_pad_ver(row, 8, 0);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (!first) {  // hairline from the text, like the other inset lists
    lv_obj_t *sep = ui::box(row);
    lv_obj_add_flag(sep, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_pos(sep, 60, -8);
    lv_obj_set_style_bg_color(sep, p.separator, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
  }
  lv_obj_t *info = ui::box(row);
  lv_obj_set_height(info, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(info, 1);
  lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(info, 12, 0);
  tile(info, 48, r.glyph, r.tint, theme::title);
  lv_obj_t *text = ui::box(info);
  lv_obj_set_height(text, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(text, 1);
  lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(text, 2, 0);
  lv_obj_t *n = ui::label(text, r.name.c_str(), theme::bodyBold, p.label);
  lv_obj_set_width(n, LV_PCT(100));
  lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
  lv_obj_t *s = ui::label(text, r.sub.c_str(), theme::caption, p.secondary);
  lv_obj_set_width(s, LV_PCT(100));
  lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
  if (r.onTap) attachTap(info, r.onTap);
  if (r.busy) ring(row, 32, r.pct);
  else if (r.button.length()) pill(row, r.button.c_str(), r.style, 32, r.onButton);
}

void banner(lv_obj_t *parent, const char *text, ui::Action onTap) {
  const Palette &p = theme::p();
  lv_obj_t *b = ui::box(parent);
  lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, p.dark ? lv_color_hex(0x2A2205) : lv_color_hex(0xFFF4CC), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(b, 14, 0);
  lv_obj_set_style_pad_ver(b, 12, 0);
  lv_obj_set_style_pad_column(b, 10, 0);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_color_t fg = p.dark ? lv_color_hex(0xFFD60A) : lv_color_hex(0x8A6100);
  ui::label(b, ICON_ERROR, theme::body, fg);
  lv_obj_t *l = ui::label(b, text, theme::footnoteBold, fg);
  lv_obj_set_flex_grow(l, 1);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  if (onTap) attachTap(b, onTap);
}

class StoreApp : public App {
public:
  const char *id() const override { return "store"; }
  const char *name() const override { return "App Store"; }
  const char *icon() const override { return ICON_INSTALL; }
  lv_color_t tint() const override { return lv_color_hex(0x0A84FF); }

  void open(lv_obj_t *root) override {
    appRoot = root;
    tab = DISCOVER;
    if (appstore::status().state != appstore::LOADING) appstore::refresh();  // every visit: one small request
    ui::push(root, "App Store", [this](lv_obj_t *c) { buildMain(c); });
  }

  void close() override { appRoot = nullptr, tabs = nullptr, list = nullptr; }

private:
  lv_obj_t *appRoot = nullptr, *tabs = nullptr, *list = nullptr;
  Tab tab = DISCOVER;

  // Live view of one page part: rebuilt whenever the catalog or a job changes.
  struct Live {
    StoreApp *app;
    lv_obj_t *holder;
    std::function<void(lv_obj_t *)> build;
    uint32_t shown;
    bool alerts;  // the main page reports failed jobs; detail pages don't, so each shows once
  };

  void live(lv_obj_t *holder, std::function<void(lv_obj_t *)> build, bool alerts) {
    auto *x = new Live{this, holder, std::move(build), appstore::version(), alerts};
    x->build(holder);
    lv_timer_t *t = lv_timer_create([](lv_timer_t *t) {
      auto *x = (Live *)t->user_data;
      if (x->alerts) {
        appstore::Status s = appstore::status();
        if (s.jobError.length()) {
          static String msg;
          msg = s.jobError;
          appstore::clearJobError();
          ui::alert("Can't Finish", msg.c_str(), {{"OK", ui::AlertAction::CANCEL, nullptr}});
        }
      }
      uint32_t v = appstore::version();
      if (v == x->shown) return;
      x->shown = v;
      rebuild(x->holder, x->build);
    }, 300, x);
    lv_obj_add_event_cb(holder, [](lv_event_t *e) {
      lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
      delete (Live *)t->user_data;
      lv_timer_del(t);
    }, LV_EVENT_DELETE, t);
  }

  static void rebuild(lv_obj_t *holder, const std::function<void(lv_obj_t *)> &build) {
    lv_obj_t *page = lv_obj_get_parent(holder);
    lv_coord_t y = lv_obj_get_scroll_y(page);  // keep the place in the list
    lv_obj_clean(holder);
    build(holder);
    lv_obj_update_layout(page);
    lv_obj_scroll_to_y(page, y, LV_ANIM_OFF);
  }

  static lv_obj_t *column(lv_obj_t *parent, int gap) {
    lv_obj_t *h = ui::box(parent);
    lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(h, gap, 0);
    return h;
  }

  // After a tap on a tab or a row: redraw once the tap's own handler has returned, since it lives in
  // the objects being replaced.
  void refreshList() {
    lv_async_call([](void *p) {
      auto *self = (StoreApp *)p;
      if (self->list) rebuild(self->list, [self](lv_obj_t *h) { self->buildList(h); });
      if (self->tabs) self->buildTabs();
    }, this);
  }

  // ---------- main page ----------

  void buildMain(lv_obj_t *c) {
    const Palette &p = theme::p();
    // Done sits on the large title's line, as in the design.
    lv_obj_t *done = ui::box(c);
    lv_obj_add_flag(done, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(done, LV_SIZE_CONTENT, 44);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, 10);
    lv_obj_set_ext_click_area(done, 8);
    lv_obj_center(ui::label(done, "Done", theme::bodyBold, p.accent));
    attachTap(done, [] { os::goHome(); });

    tabs = ui::box(c);
    lv_obj_set_size(tabs, LV_PCT(100), 38);
    lv_obj_set_style_radius(tabs, 9, 0);
    lv_obj_set_style_bg_color(tabs, pillFill(), 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tabs, 2, 0);
    lv_obj_set_style_pad_column(tabs, 2, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    buildTabs();

    list = column(c, 10);
    live(list, [this](lv_obj_t *h) {
      buildList(h);
      buildTabs();  // the Updates badge follows the catalog
    }, true);
  }

  int updateCount(const appstore::Status &s, const std::vector<ScriptAppInfo> &mine) {
    int n = 0;
    for (const ScriptAppInfo &i : mine) {
      const appstore::Listing *l = findListing(s, i.id);
      if (l && ghupdate::compareVersions(l->version.c_str(), i.version.c_str()) > 0) n++;
    }
    return n;
  }

  void buildTabs() {
    const Palette &p = theme::p();
    lv_obj_clean(tabs);
    int updates = updateCount(appstore::status(), scriptAppList());
    const char *names[] = {"Discover", "Updates", "Installed"};
    for (int i = 0; i < 3; i++) {
      lv_obj_t *t = ui::box(tabs);
      lv_obj_set_height(t, 34);
      lv_obj_set_flex_grow(t, 1);
      lv_obj_set_style_radius(t, 7, 0);
      if (i == tab) {
        lv_obj_set_style_bg_color(t, p.dark ? lv_color_hex(0x636366) : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
      }
      lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(t, 6, 0);
      ui::label(t, names[i], theme::footnoteBold, p.label);
      if (i == UPDATES && updates) {
        lv_obj_t *badge = ui::box(t);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, 18);
        lv_obj_set_style_min_width(badge, 18, 0);
        lv_obj_set_style_pad_hor(badge, 5, 0);
        lv_obj_set_style_radius(badge, 9, 0);
        lv_obj_set_style_bg_color(badge, p.red, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_center(ui::label(badge, String(updates).c_str(), theme::caption, lv_color_white()));
      }
      attachTap(t, [this, i] {
        if (tab == i) return;
        tab = (Tab)i;
        lv_obj_scroll_to_y(lv_obj_get_parent(list), 0, LV_ANIM_OFF);
        refreshList();
      });
    }
  }

  // The row for an app from the catalog, installed or not.
  RowSpec listingRow(const appstore::Status &s, const appstore::Listing &l, const ScriptAppInfo *mine) {
    RowSpec r;
    scriptAppLook(l.icon.c_str(), l.color.c_str(), r.glyph, r.tint);
    r.name = l.name;
    r.sub = l.summary;
    r.busy = appstore::pending(l.id);
    r.pct = s.busy == l.id ? s.pct : 0;
    String id = l.id;
    r.onTap = [this, id] { detailPage(id); };
    if (mine && ghupdate::compareVersions(l.version.c_str(), mine->version.c_str()) > 0) {
      r.button = "Update", r.style = PILL_FILLED;
      r.onButton = [id] { appstore::install(id); };
    } else if (mine) {
      r.button = "Open";
      r.onButton = [id] { if (App *a = os::find(id.c_str())) openLater(a); };
    } else if (tooOld(l)) {
      r.button = "Get", r.style = PILL_OFF;
      r.sub = "Needs yOS " + l.requires + " or later";
    } else {
      r.button = "Get";
      r.onButton = [id] { appstore::install(id); };
    }
    return r;
  }

  RowSpec builtinRow(App *a) {
    RowSpec r;
    r.glyph = a->icon(), r.tint = a->tint(), r.name = a->name();
    r.onTap = [this, a] { ui::push(appRoot, a->name(), [a](lv_obj_t *c) { os::appInfoPage(c, a); }); };
    return r;
  }

  void buildList(lv_obj_t *h) {
    const Palette &p = theme::p();
    appstore::Status s = appstore::status();
    std::vector<ScriptAppInfo> mine = scriptAppList();
    std::map<String, const ScriptAppInfo *> byId;
    for (const ScriptAppInfo &i : mine) byId[i.id] = &i;
    auto installedInfo = [&](const String &id) -> const ScriptAppInfo * {
      auto it = byId.find(id);
      return it == byId.end() ? nullptr : it->second;
    };

    if (s.state == appstore::FAILED) {
      static String msg;
      msg = s.error + " Tap to try again.";
      banner(h, msg.c_str(), [] { appstore::refresh(); });
    }
    bool loading = s.state == appstore::LOADING || (s.state == appstore::EMPTY);

    if (tab == DISCOVER) {
      const appstore::Listing *f = findListing(s, s.featured);
      if (f) featured(h, *f);
      lv_obj_t *g = ui::group(h, "Apps");
      for (const appstore::Listing &l : s.apps) storeRow(g, listingRow(s, l, installedInfo(l.id)));
      for (App *a : os::apps()) {
        if (!hideable(a)) continue;
        RowSpec r = builtinRow(a);
        bool hidden = prefs::appHidden(a->id());
        r.sub = "Built in";
        r.button = hidden ? "Get" : "Open";
        String id = a->id();
        r.onButton = hidden ? ui::Action([this, id] {
          prefs::setAppHidden(id.c_str(), false);  // nothing to download: it comes back
          os::rebuildHome();
          refreshList();
        }) : ui::Action([a] { openLater(a); });
        storeRow(g, r);
      }
      if (loading && s.apps.empty()) {
        RowSpec r;
        r.glyph = ICON_REFRESH, r.tint = p.gray;
        r.name = "Loading the App Store\xE2\x80\xA6", r.sub = "From " GITHUB_REPO " on GitHub";
        storeRow(g, r);
      }
      ui::note(h, "Apps come from the yOS repo on GitHub and install to /apps on internal flash, or on the SD card when "
                  "the flash is full.");
      return;
    }

    if (tab == UPDATES) {
      int n = updateCount(s, mine);
      lv_obj_t *head = ui::box(h);
      lv_obj_set_size(head, LV_PCT(100), 44);
      lv_obj_set_style_pad_hor(head, 4, 0);
      static char text[40];
      if (n) snprintf(text, sizeof(text), n == 1 ? "1 update available" : "%d updates available", n);
      else strlcpy(text, loading ? "Checking for updates\xE2\x80\xA6" : "All apps are up to date", sizeof(text));
      lv_obj_align(ui::label(head, text, theme::footnote, p.secondary), LV_ALIGN_LEFT_MID, 0, 0);
      if (n) {
        lv_obj_t *all = ui::box(head);
        lv_obj_set_size(all, LV_SIZE_CONTENT, 44);
        lv_obj_align(all, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_center(ui::label(all, "Update All", theme::bodyBold, p.accent));
        std::vector<String> ids;
        for (const ScriptAppInfo &i : mine) {
          const appstore::Listing *l = findListing(s, i.id);
          if (l && ghupdate::compareVersions(l->version.c_str(), i.version.c_str()) > 0) ids.push_back(i.id);
        }
        attachTap(all, [ids] { for (const String &id : ids) appstore::install(id); });
        lv_obj_t *g = ui::group(h);
        for (const String &id : ids) {
          const appstore::Listing *l = findListing(s, id);
          RowSpec r = listingRow(s, *l, installedInfo(id));
          r.sub = "Version " + l->version + (l->notes.length() ? String(" \xC2\xB7 ") + l->notes : String());
          storeRow(g, r);
        }
      }
      return;
    }

    // Installed: what the store put here, then the built-ins that are showing.
    uint64_t used = 0;
    for (const ScriptAppInfo &i : mine) used += i.bytes;
    storageCard(h, used);
    lv_obj_t *g = ui::group(h);
    for (const ScriptAppInfo &i : mine) {
      const appstore::Listing *l = findListing(s, i.id);
      RowSpec r;
      String id = i.id, name = i.name;
      bool onSd = i.onSd;
      if (l) {
        r = listingRow(s, *l, &i);
      } else {  // installed from the web page, not from the store
        App *a = os::find(i.id.c_str());
        if (!a) continue;
        r = builtinRow(a);
      }
      r.sub = formatSize(i.bytes) + (onSd ? " on the SD card" : " on internal flash");
      r.button = "Delete", r.style = PILL_RED;
      r.onButton = [id, name, onSd] { confirmDelete(id, name, onSd); };
      storeRow(g, r);
    }
    for (App *a : os::apps()) {
      if (!hideable(a) || prefs::appHidden(a->id())) continue;
      RowSpec r = builtinRow(a);
      r.sub = "Built in \xC2\xB7 no download needed";
      r.button = "Delete", r.style = PILL_RED;
      r.onButton = [this, a] {
        static String title;
        title = String("Delete \xE2\x80\x9C") + a->name() + "\xE2\x80\x9D?";
        ui::alert(title.c_str(), "It disappears from the Home Screen. You can get it back from the App Store at any time.",
                  {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                   {"Delete", ui::AlertAction::DESTRUCTIVE, [this, a] {
                     prefs::setAppHidden(a->id(), true);
                     os::rebuildHome();
                     refreshList();
                   }}});
      };
      storeRow(g, r);
    }
    ui::note(h, "Deleting an App Store app removes its folder and its data. Built-in apps are only hidden, and Get "
                "shows them again.");
  }

  static void confirmDelete(const String &id, const String &name, bool onSd) {
    static String title, id0;
    title = "Delete \xE2\x80\x9C" + name + "\xE2\x80\x9D?";
    id0 = id;
    ui::alert(title.c_str(), onSd ? "This removes the app and its data from the SD card."
                                  : "This removes the app and its data from this device.",
              {{"Cancel", ui::AlertAction::CANCEL, nullptr},
               {"Delete", ui::AlertAction::DESTRUCTIVE, [] { appstore::remove(id0); }}});
  }

  void featured(lv_obj_t *h, const appstore::Listing &l) {
    const char *glyph;
    lv_color_t tint;
    scriptAppLook(l.icon.c_str(), l.color.c_str(), glyph, tint);
    lv_obj_t *card = column(h, 6);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_darken(tint, LV_OPA_40), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_lighten(tint, LV_OPA_20), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_t *k = ui::label(card, "NEW IN THE STORE", theme::caption, lv_color_white());
    lv_obj_set_style_text_opa(k, LV_OPA_80, 0);
    ui::label(card, l.name.c_str(), theme::title, lv_color_white());
    lv_obj_t *sub = wrapText(card, l.summary.c_str(), theme::footnote, lv_color_white());
    lv_obj_set_style_text_opa(sub, LV_OPA_90, 0);
    String id = l.id;
    attachTap(card, [this, id] { detailPage(id); });
  }

  void storageCard(lv_obj_t *h, uint64_t used) {
    const Palette &p = theme::p();
    uint64_t freeBytes = scriptInternalFree();
    lv_obj_t *card = column(h, 8);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, p.card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_ver(card, 12, 0);
    lv_obj_t *line = ui::box(card);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(ui::label(line, "App Storage", theme::footnote, p.label), LV_ALIGN_LEFT_MID, 0, 0);
    String text = formatSize(used) + " used by apps \xC2\xB7 " + formatSize(freeBytes) + " free";
    lv_obj_align(ui::label(line, text.c_str(), theme::footnote, p.secondary), LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *track = ui::box(card);
    lv_obj_set_size(track, LV_PCT(100), 8);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_bg_color(track, p.separator, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_t *bar = ui::box(track);
    uint64_t all = used + freeBytes;
    lv_obj_set_size(bar, LV_PCT(all ? max<int>(2, (int)(used * 100 / all)) : 2), 8);
    lv_obj_set_style_bg_color(bar, p.accent, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  }

  // ---------- detail page ----------

  void detailPage(const String &id) {
    const appstore::Listing *l0 = findListing(appstore::status(), id);
    if (!l0) return;
    String name = l0->name;
    ui::push(appRoot, name.c_str(), [this, id](lv_obj_t *c) {
      // The header below shows the name next to the icon, so the page's own large title goes.
      lv_obj_add_flag(lv_obj_get_child(c, lv_obj_get_child_cnt(c) - 1), LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_pad_row(c, 12, 0);
      live(column(c, 12), [this, id](lv_obj_t *h) { buildDetail(h, id); }, false);
    });
  }

  void buildDetail(lv_obj_t *h, const String &id) {
    const Palette &p = theme::p();
    appstore::Status s = appstore::status();
    const appstore::Listing *lp = findListing(s, id);
    if (!lp) return;
    const appstore::Listing &l = *lp;
    ScriptAppInfo mine{};
    bool installed = false;
    for (const ScriptAppInfo &i : scriptAppList())
      if (i.id == id) mine = i, installed = true;
    RowSpec r = listingRow(s, l, installed ? &mine : nullptr);

    // Header: big tile, name, summary, and Get / Update / Open (or the download ring).
    lv_obj_t *head = ui::box(h);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 16, 0);
    tile(head, 96, r.glyph, r.tint, theme::largeTitle);
    lv_obj_t *col = column(head, 4);
    lv_obj_set_width(col, 0);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_t *n = ui::label(col, l.name.c_str(), theme::title, p.label);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    wrapText(col, r.sub.c_str(), theme::footnote, p.secondary);
    lv_obj_t *act = ui::box(col);
    lv_obj_set_size(act, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(act, 6, 0);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act, 10, 0);
    if (r.busy) {
      ring(act, 32, r.pct);
      ui::label(act, s.busy == id ? "Downloading\xE2\x80\xA6" : "Waiting\xE2\x80\xA6", theme::caption, p.secondary);
    } else {
      pill(act, r.button.c_str(), r.style == PILL_OFF ? PILL_OFF : PILL_FILLED, 34, r.onButton);
    }

    // SIZE · VERSION · NEEDS · TYPE
    lv_obj_t *chips = ui::box(h);
    lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(chips, 10, 0);
    lv_obj_set_style_border_side(chips, (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_width(chips, 1, 0);
    lv_obj_set_style_border_color(chips, p.separator, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    String chip[4][2] = {{"SIZE", formatSize(l.size)},
                         {"VERSION", l.version},
                         {"NEEDS", l.needs.length() ? l.needs : String("\xE2\x80\x94")},
                         {"TYPE", "Script"}};
    for (int i = 0; i < 4; i++) {
      lv_obj_t *cell = column(chips, 2);
      lv_obj_set_width(cell, 0);
      lv_obj_set_flex_grow(cell, 1);
      lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      if (i) {
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, p.separator, 0);
      }
      ui::label(cell, chip[i][0].c_str(), theme::caption, p.secondary);
      ui::label(cell, chip[i][1].c_str(), theme::bodyBold, p.label);
    }

    if (l.description.length()) wrapText(h, l.description.c_str(), theme::footnote, p.label);
    if (l.notes.length()) {
      String what = "What's New \xC2\xB7 Version " + l.version;
      lv_obj_set_style_pad_hor(ui::label(h, what.c_str(), theme::footnote, p.secondary), 4, 0);
      wrapText(h, l.notes.c_str(), theme::footnote, p.label);
    }

    lv_obj_t *g = ui::group(h, "Information");
    ui::row(g, nullptr, p.gray, "Developer", l.developer.c_str(), nullptr, false);
    ui::row(g, nullptr, p.gray, "Source", "GitHub \xC2\xB7 " GITHUB_REPO, nullptr, false);
    String where = !installed ? String("Flash or SD card")
                              : String(String(mine.onSd ? "SD card" : "Flash") + " \xE2\x80\xBA /apps/" + id);
    ui::row(g, nullptr, p.gray, "Installs to", where.c_str(), nullptr, false);
    String req = l.requires.length() ? String("yOS " + l.requires + " or later") : String("Any yOS");
    ui::row(g, nullptr, p.gray, "Requires", req.c_str(), nullptr, false);
    ui::row(g, nullptr, p.gray, "Permissions", l.needs.length() ? "Network, its own folder" : "Its own folder", nullptr, false);

    if (installed && !r.busy) {
      g = ui::group(h);
      String name = l.name;
      bool onSd = mine.onSd;
      ui::actionRow(g, "Delete App", p.red, [id, name, onSd] { confirmDelete(id, name, onSd); });
    }
  }
};

}  // namespace

App *createStoreApp() { return new StoreApp(); }
