// Netwatch: pings the home network, checks DNS through AdGuard, and keeps an outage log.
#include "apps/apps.h"
#include "os/os.h"
#include "os/ui.h"
#include "services/monitor.h"
#include "services/storage.h"

static void fmtDur(char *buf, size_t len, uint32_t s) {
  if (s < 60) snprintf(buf, len, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(buf, len, "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(buf, len, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
  else snprintf(buf, len, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
}

static void fmtTime(char *buf, size_t len, time_t t, const char *fmt) {
  struct tm tm;
  if (t > 1000000000 && localtime_r(&t, &tm)) strftime(buf, len, fmt, &tm);
  else strlcpy(buf, "--", len);
}

struct Probe {
  const char *name;
  bool known, up;
  float rtt;
  time_t since;
};

static Probe probe(const Snapshot &s, int i) {
  if (i < NUM_TARGETS) {
    const TargetState &t = s.targets[i];
    return {TARGETS[i].name, t.known, t.up, t.rttMs, t.since};
  }
  return {"DNS", s.dns.known, s.dns.up, s.dns.rttMs, s.dns.since};
}

static const char *ADG_FIELDS[] = {"Server", "Username", "Password"};

class NetwatchApp : public App {
  lv_obj_t *adgValues[3] = {};  // value labels on the settings page while it is open

  static String *adgField(AdguardConfig &c, int i) { return i == 0 ? &c.host : i == 1 ? &c.user : &c.pass; }
  static const char *adgSummary(AdguardConfig &c, int i) {
    String *f = adgField(c, i);
    if (f->isEmpty()) return "Not set";
    return i == 2 ? "Saved" : f->c_str();
  }

  void editAdguard(lv_obj_t *appRoot, int i) {
    static const char *hints[] = {"192.168.88.90", "admin", "Password"};
    AdguardConfig c = monitorAdguardConfig();
    bool secret = i == 2;
    // The password is never shown again; entering it starts from empty.
    ui::textInput(appRoot, ADG_FIELDS[i], hints[i], secret ? "" : adgField(c, i)->c_str(), secret,
                  [this, i, secret](const String &text) {
      AdguardConfig c = monitorAdguardConfig();
      String v = text;
      if (!secret) v.trim();
      if (i == 0) {  // accept a pasted URL: keep host[:port] only
        if (v.startsWith("http://")) v.remove(0, 7);
        if (v.endsWith("/")) v.remove(v.length() - 1);
      }
      *adgField(c, i) = v;
      monitorSetAdguardConfig(c);
      if (adgValues[i]) lv_label_set_text(adgValues[i], adgSummary(c, i));
    });
  }

public:
  const char *id() const override { return "netwatch"; }
  const char *name() const override { return "Netwatch"; }
  const char *icon() const override { return ICON_NETWATCH; }
  lv_color_t tint() const override { return alerting ? lv_color_hex(0xFF3B30) : lv_color_hex(0x34C759); }

  bool hasService() const override { return true; }
  bool defaultAutostart() const override { return true; }
  const char *serviceSummary() const override {
    return "Pings your router, gateways, AdGuard and Deco every 5 seconds and logs outages to the SD card.";
  }
  void startService() override {
    monitorStart();
    monitorSetEnabled(true);
  }
  void stopService() override { monitorSetEnabled(false); }

  uint64_t storageBytes() override { return storageInfo().logBytes; }

  // Settings › Apps › Netwatch: the AdGuard Home login, kept on the device (NVS), never in the source.
  void buildSettings(lv_obj_t *content, lv_obj_t *appRoot) override {
    AdguardConfig c = monitorAdguardConfig();
    lv_obj_t *g = ui::group(content, "AdGuard Home",
                            "Stats and the Pause button use this login. Leave the password empty to turn them off.");
    for (int i = 0; i < 3; i++) {
      adgValues[i] = ui::row(g, nullptr, lv_color_black(), ADG_FIELDS[i], adgSummary(c, i),
                             [this, appRoot, i] { editAdguard(appRoot, i); }).value;
    }
    lv_obj_add_event_cb(content, [](lv_event_t *e) {
      auto *self = (NetwatchApp *)lv_event_get_user_data(e);
      for (lv_obj_t *&v : self->adgValues) v = nullptr;
    }, LV_EVENT_DELETE, this);
  }

  bool liveIcon(lv_obj_t *tile) override {
    lv_obj_set_style_bg_color(tile, tint(), 0);  // the icon turns red during an outage
    return false;
  }

  // ---- system status for the capsule ----
  static SystemStatus status() {
    SystemStatus st{STATUS_UNKNOWN, ""};
    if (!self->running) {
      strcpy(st.text, "Netwatch is off");
      self->alerting = false;
      return st;
    }
    monitorSnapshot(self->snap);
    const Snapshot &s = self->snap;
    int down = 0, known = 0;
    const char *firstDown = nullptr;
    time_t since = 0;
    for (int i = 0; i < NUM_PROBES; i++) {
      Probe p = probe(s, i);
      if (!p.known) continue;
      known++;
      if (!p.up) {
        if (!down++) { firstDown = p.name; since = p.since; }
      }
    }
    self->alerting = down > 0;
    if (known == 0) {
      strcpy(st.text, "Checking network" "\xE2\x80\xA6");
    } else if (down == 1) {
      char d[12];
      fmtDur(d, sizeof(d), since > 1000000000 ? time(nullptr) - since : 0);
      st.level = STATUS_ALERT;
      snprintf(st.text, sizeof(st.text), "%s down %s", firstDown, d);
    } else if (down > 1) {
      st.level = STATUS_ALERT;
      snprintf(st.text, sizeof(st.text), "%d hosts down", down);
    } else if (s.adguard.ok && !s.adguard.protectionOn) {
      st.level = STATUS_WARN;
      strcpy(st.text, "Ad blocking paused");
    } else {
      st.level = STATUS_OK;
      strcpy(st.text, "All systems OK");
    }
    return st;
  }

  // ---- UI ----
  void open(lv_obj_t *root) override {
    appRoot = root;
    ui::push(root, "Netwatch", [this](lv_obj_t *c) { buildMain(c); });
  }
  void close() override { mainPage = detailPage = logPage = nullptr; }

  void tick() override {
    monitorSnapshot(snap);
    lv_obj_t *cur = ui::currentPage();
    if (cur == mainPage) updateMain();
    else if (cur == detailPage) updateDetail();
  }

  static NetwatchApp *self;
  bool alerting = false;

private:
  Snapshot snap;
  lv_obj_t *appRoot = nullptr;

  // main page
  lv_obj_t *mainPage = nullptr, *heroIcon, *heroTitle, *heroSub;
  ui::Row hostRows[NUM_PROBES];
  ui::Row agQueries, agBlocked, agAvg, agProtection;
  lv_obj_t *agButtonLabel = nullptr, *agButton = nullptr, *logCount = nullptr;
  bool pending = false;
  uint32_t pendingAt = 0;

  // detail page
  lv_obj_t *detailPage = nullptr, *logPage = nullptr, *chart = nullptr;
  lv_chart_series_t *series = nullptr;
  int detailProbe = 0;
  ui::Row dStatus, dLatency, dLoss, dRange;

  void buildMain(lv_obj_t *c) {
    const Palette &p = theme::p();
    mainPage = c;

    // Summary card: one glance tells whether anything needs attention.
    lv_obj_t *hero = ui::group(c);
    lv_obj_t *row = ui::box(hero);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 16, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    heroIcon = ui::label(row, ICON_OK, theme::icon36, p.green);
    lv_obj_t *col = ui::box(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    heroTitle = ui::label(col, "", theme::title, p.label);
    heroSub = ui::label(col, "", theme::footnote, p.secondary);

    lv_obj_t *hosts = ui::group(c, "Hosts");
    for (int i = 0; i < NUM_PROBES; i++) {
      hostRows[i] = ui::row(hosts, nullptr, p.gray, probe(snap, i).name, "", [this, i] { openDetail(i); });
      // status glyph in front of the name
      lv_obj_t *g = ui::label(hostRows[i].obj, ICON_OK, theme::body, p.green);
      lv_obj_move_to_index(g, 0);
      hostRows[i].icon = g;
      ui::insetSeparator(hostRows[i].obj, 29);
      lv_obj_set_style_translate_y(g, 2, 0);  // icon glyphs sit high against Inter's baseline
    }

    lv_obj_t *ag = ui::group(c, "AdGuard Home", "Totals for the last 24 hours, from AdGuard's own statistics.");
    agQueries = ui::row(ag, ICON_DNS, p.blue, "Queries", "");
    agBlocked = ui::row(ag, ICON_SHIELD, p.green, "Blocked", "");
    agAvg = ui::row(ag, ICON_MOTION, p.gray, "Average Time", "");
    agProtection = ui::row(ag, ICON_SHIELD, p.orange, "Protection", "");
    ui::Row btn = ui::actionRow(ag, "Pause for 10 Minutes", p.accent, [this] { toggleProtection(); });
    agButton = btn.obj;
    agButtonLabel = btn.title;

    lv_obj_t *hist = ui::group(c, "History");
    ui::Row log = ui::row(hist, ICON_ERROR, p.red, "Outage Log", "", [this] { openLog(); });
    logCount = log.value;
    updateMain();
  }

  void updateMain() {
    const Palette &p = theme::p();
    SystemStatus st = status();
    lv_label_set_text(heroIcon, st.level == STATUS_ALERT ? ICON_ERROR : ICON_OK);
    lv_obj_set_style_text_color(heroIcon, st.level == STATUS_ALERT ? p.red : st.level == STATUS_WARN ? p.orange
                                          : st.level == STATUS_OK ? p.green : p.gray, 0);
    lv_label_set_text(heroTitle, st.text);
    lv_label_set_text(heroSub, running ? "Checking every 5 seconds" : "Paused. Start it from App Info.");

    for (int i = 0; i < NUM_PROBES; i++) {
      Probe pr = probe(snap, i);
      char v[24], d[12];
      lv_color_t c = p.secondary;
      if (!pr.known) strcpy(v, "Checking" "\xE2\x80\xA6");
      else if (!pr.up) {
        fmtDur(d, sizeof(d), pr.since > 1000000000 ? time(nullptr) - pr.since : 0);
        snprintf(v, sizeof(v), "Down %s", d);
        c = p.red;
      } else snprintf(v, sizeof(v), "%.0f ms", pr.rtt);
      lv_label_set_text(hostRows[i].value, v);
      lv_obj_set_style_text_color(hostRows[i].value, c, 0);
      lv_label_set_text(hostRows[i].icon, pr.known && !pr.up ? ICON_ERROR : ICON_OK);
      lv_obj_set_style_text_color(hostRows[i].icon, !pr.known ? p.gray : pr.up ? p.green : p.red, 0);
    }

    const AdguardState &a = snap.adguard;
    char buf[24];
    if (a.ok) {
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)a.queries);
      lv_label_set_text(agQueries.value, buf);
      snprintf(buf, sizeof(buf), "%.1f%%", a.queries ? 100.0f * a.blocked / a.queries : 0.0f);
      lv_label_set_text(agBlocked.value, buf);
      snprintf(buf, sizeof(buf), "%.0f ms", a.avgMs);
      lv_label_set_text(agAvg.value, buf);
      if (a.protectionOn) strcpy(buf, "On");
      else { char d[12]; fmtDur(d, sizeof(d), a.pausedMs / 1000); snprintf(buf, sizeof(buf), "Paused, %s left", d); }
      lv_label_set_text(agProtection.value, buf);
      if (pending && (millis() - pendingAt > 6000)) pending = false;
      lv_label_set_text(agButtonLabel, pending ? "Working" "\xE2\x80\xA6" : a.protectionOn ? "Pause for 10 Minutes"
                                                                                       : "Resume Protection");
    } else {
      const char *msg = a.known ? "Unreachable" : "\xE2\x80\x94";
      lv_label_set_text(agQueries.value, msg);
      lv_label_set_text(agBlocked.value, "\xE2\x80\x94");
      lv_label_set_text(agAvg.value, "\xE2\x80\x94");
      lv_label_set_text(agProtection.value, "\xE2\x80\x94");
    }
    snprintf(buf, sizeof(buf), "%d", snap.numEvents);
    lv_label_set_text(logCount, buf);
  }

  void toggleProtection() {
    if (pending || !snap.adguard.ok) return;
    bool resume = !snap.adguard.protectionOn;
    auto run = [this, resume] {
      pending = true;
      pendingAt = millis();
      monitorSetProtection(resume, 10 * 60 * 1000);
    };
    if (resume) run();
    else ui::alert("Pause ad blocking?", "Every device on your network will see ads and trackers for 10 minutes.",
                   {{"Cancel", ui::AlertAction::CANCEL, nullptr}, {"Pause", ui::AlertAction::DESTRUCTIVE, run}});
  }

  // ---- host detail ----
  void openDetail(int i) {
    detailProbe = i;
    ui::push(appRoot, probe(snap, i).name, [this](lv_obj_t *c) { buildDetail(c); });
  }

  void buildDetail(lv_obj_t *c) {
    const Palette &p = theme::p();
    detailPage = c;
    lv_obj_t *g = ui::group(c, detailProbe < NUM_TARGETS ? TARGETS[detailProbe].ip.toString().c_str()
                                                         : "Resolving through AdGuard");
    dStatus = ui::row(g, nullptr, p.gray, "Status", "");
    dLatency = ui::row(g, nullptr, p.gray, "Latency", "");
    dLoss = ui::row(g, nullptr, p.gray, "Packet Loss", "");
    dRange = ui::row(g, nullptr, p.gray, "Min / Avg / Max", "");

    lv_obj_t *cg = ui::group(c, "Last 30 Minutes");
    chart = lv_chart_create(cg);
    lv_obj_set_size(chart, LV_PCT(100), 150);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 12, 0);
    lv_obj_set_style_line_color(chart, p.separator, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);  // no point markers
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_chart_set_point_count(chart, HISTORY_LEN);
    series = lv_chart_add_series(chart, p.green, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *og = ui::group(c, "Recent Outages");
    int shown = 0;
    const char *name = probe(snap, detailProbe).name;
    for (int i = 0; i < snap.numEvents && shown < 5; i++) {
      const Event &e = snap.events[i];
      if (strcmp(e.what, name)) continue;
      char ts[24], val[24], d[12];
      fmtTime(ts, sizeof(ts), e.at, "%d %b, %H:%M");
      if (e.up) { fmtDur(d, sizeof(d), e.downSecs); snprintf(val, sizeof(val), "Back after %s", d); }
      else strcpy(val, "Went down");
      ui::Row r = ui::row(og, nullptr, p.gray, ts, val);
      lv_obj_set_style_text_color(r.value, e.up ? p.green : p.red, 0);
      shown++;
    }
    if (!shown) ui::row(og, nullptr, p.gray, "No outages recorded");
    updateDetail();
  }

  void updateDetail() {
    const Palette &p = theme::p();
    static float hist[HISTORY_LEN];
    int n = monitorHistory(detailProbe, hist, HISTORY_LEN);
    int lost = 0, ok = 0;
    float mn = 1e9, mx = 0, sum = 0;
    for (int i = 0; i < n; i++) {
      if (isnan(hist[i])) { lost++; continue; }
      ok++; mn = min(mn, hist[i]); mx = max(mx, hist[i]); sum += hist[i];
    }
    Probe pr = probe(snap, detailProbe);
    char buf[40], d[12];
    fmtDur(d, sizeof(d), pr.since > 1000000000 ? time(nullptr) - pr.since : 0);
    snprintf(buf, sizeof(buf), "%s for %s", !pr.known ? "Checking" : pr.up ? "Up" : "Down", d);
    lv_label_set_text(dStatus.value, buf);
    lv_obj_set_style_text_color(dStatus.value, pr.known && !pr.up ? p.red : p.secondary, 0);
    if (pr.up) snprintf(buf, sizeof(buf), "%.1f ms", pr.rtt); else strcpy(buf, "\xE2\x80\x94");
    lv_label_set_text(dLatency.value, buf);
    snprintf(buf, sizeof(buf), "%.1f%%", n ? 100.0f * lost / n : 0.0f);
    lv_label_set_text(dLoss.value, buf);
    if (ok) snprintf(buf, sizeof(buf), "%.0f / %.0f / %.0f ms", mn, sum / ok, mx); else strcpy(buf, "\xE2\x80\x94");
    lv_label_set_text(dRange.value, buf);

    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, max(20, int(mx * 1.2f)));
    for (int i = 0; i < HISTORY_LEN; i++) {
      int src = i - (HISTORY_LEN - n);
      series->y_points[i] = src < 0 || isnan(hist[src]) ? LV_CHART_POINT_NONE : lv_coord_t(hist[src]);
    }
    lv_chart_refresh(chart);
  }

  // ---- outage log ----
  void openLog() {
    ui::push(appRoot, "Outage Log", [this](lv_obj_t *c) {
      const Palette &p = theme::p();
      logPage = c;
      lv_obj_t *g = ui::group(c, nullptr, storageReady() ? "Saved to the SD card, so it survives restarts." : nullptr);
      for (int i = 0; i < snap.numEvents; i++) {
        const Event &e = snap.events[i];
        char ts[24], title[48], val[24], d[12];
        fmtTime(ts, sizeof(ts), e.at, "%d %b %H:%M");
        snprintf(title, sizeof(title), "%s  %s", e.what, ts);
        if (e.up) { fmtDur(d, sizeof(d), e.downSecs); snprintf(val, sizeof(val), "Up after %s", d); }
        else strcpy(val, "Down");
        ui::Row r = ui::row(g, nullptr, p.gray, title, val);
        lv_obj_set_style_text_color(r.value, e.up ? p.green : p.red, 0);
      }
      if (!snap.numEvents) ui::row(g, ICON_OK, p.green, "No outages recorded");
    });
  }
};

NetwatchApp *NetwatchApp::self = nullptr;

App *createNetwatchApp() {
  NetwatchApp::self = new NetwatchApp();
  os::setStatusProvider(NetwatchApp::status, NetwatchApp::self);
  return NetwatchApp::self;
}
