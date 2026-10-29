// Settings › Wi-Fi
#include <WiFi.h>
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/ui.h"
#include "services/wifi_manager.h"

namespace settings {
namespace {
using wifimgr::Network;

const uint32_t SCAN_EVERY_MS = 10000;

const char *signalText(int rssi) { return rssi > -60 ? "Strong" : rssi > -70 ? "Good" : rssi > -80 ? "Fair" : "Weak"; }

void networkDetail(lv_obj_t *appRoot, const String &ssid);

// Tapping an in-range network: saved joins directly, secured asks for the password, open joins.
void join(lv_obj_t *appRoot, const Network &n) {
  if (n.saved || !n.secure) {
    wifimgr::connect(n.ssid, "");
    return;
  }
  String ssid = n.ssid;
  ui::textInput(appRoot, "Password", "Password", "", true, [ssid](const String &pw) {
    if (pw.length()) wifimgr::connect(ssid, pw);
  });
}

void otherNetwork(lv_obj_t *appRoot) {
  ui::textInput(appRoot, "Network Name", "Name", "", false, [appRoot](const String &name) {
    if (!name.length()) return;
    String ssid = name;
    ui::textInput(appRoot, "Password", "Password (leave empty if open)", "", true,
                  [ssid](const String &pw) { wifimgr::connect(ssid, pw); });
  });
}

void forgetAlert(const String &ssid) {
  String msg = "Your yOS board will no longer join \"" + ssid + "\" automatically.";
  ui::alert("Forget This Network?", msg.c_str(),
            {{"Cancel", ui::AlertAction::CANCEL, nullptr},
             {"Forget Network", ui::AlertAction::DESTRUCTIVE, [ssid]() {
                wifimgr::forget(ssid);
                ui::pop();
              }}});
}

void networkDetail(lv_obj_t *appRoot, const String &ssid) {
  ui::push(appRoot, ssid.c_str(), [ssid](lv_obj_t *c) {
    const Palette &p = theme::p();
    bool current = wifimgr::state() == wifimgr::CONNECTED && wifimgr::currentSsid() == ssid;
    if (current) {
      lv_obj_t *g = ui::group(c);
      char buf[24];
      int rssi = wifimgr::rssi();
      snprintf(buf, sizeof(buf), "%s (%d dBm)", signalText(rssi), rssi);
      ui::row(g, nullptr, p.gray, "Signal", buf);
      g = ui::group(c, "Addresses",
                    "The address changes when the board joins a different network. The dashboard follows "
                    "netwatch.local.");
      ui::row(g, nullptr, p.gray, "IP Address", WiFi.localIP().toString().c_str());
      ui::row(g, nullptr, p.gray, "Router", WiFi.gatewayIP().toString().c_str());
      ui::row(g, nullptr, p.gray, "DNS", WiFi.dnsIP().toString().c_str());
      ui::row(g, nullptr, p.gray, "MAC Address", WiFi.macAddress().c_str());
    } else {
      lv_obj_t *g = ui::group(c);
      ui::row(g, nullptr, p.gray, "Status", "Not Connected");
      g = ui::group(c);
      ui::actionRow(g, "Join This Network", p.accent, [ssid]() {
        wifimgr::connect(ssid, "");
        ui::pop();
      });
    }
    lv_obj_t *g = ui::group(c);
    ui::actionRow(g, "Forget This Network", p.red, [ssid]() { forgetAlert(ssid); });
  });
}

struct PageState {
  lv_obj_t *appRoot;
  lv_obj_t *dyn;
  String sig;
  uint32_t lastScan;
};

String signature() {
  String s = String((int)wifimgr::state()) + "|" + wifimgr::currentSsid() + "|" + wifimgr::connectingSsid() + "|" +
             wifimgr::failedSsid() + "|" + (wifimgr::scanning() ? "s" : "") + "|";
  if (wifimgr::state() == wifimgr::CONNECTED) s += signalText(wifimgr::rssi());
  for (const Network &n : wifimgr::results())
    s += n.ssid + (n.secure ? "!" : "") + (n.saved ? "*" : "") + signalText(n.rssi) + ";";
  s += "|";
  for (const String &x : wifimgr::saved()) s += x + ";";
  return s;
}

String valueFor(const Network &n) {
  return String(n.secure ? ICON_LOCK " " : "") + signalText(n.rssi);
}

void fill(PageState *st) {
  const Palette &p = theme::p();
  lv_obj_t *c = st->dyn;
  lv_obj_t *appRoot = st->appRoot;
  lv_obj_clean(c);
  if (!wifimgr::enabled()) return;

  String cur = wifimgr::currentSsid();
  String conn = wifimgr::connectingSsid();
  String failed = wifimgr::failedSsid();
  std::vector<Network> nets = wifimgr::results();
  std::vector<String> savedList = wifimgr::saved();

  if (cur.length()) {
    lv_obj_t *g = ui::group(c);
    String v = String(ICON_OK " ") + signalText(wifimgr::rssi());
    for (const Network &n : nets)
      if (n.ssid == cur && n.secure) v = String(ICON_LOCK " ") + v;
    ui::row(g, ICON_WIFI, p.blue, cur.c_str(), v.c_str(), [appRoot, cur]() { networkDetail(appRoot, cur); });
  }
  if (failed.length()) {
    String msg = String(ICON_ERROR " Couldn't join ") + failed + ". Check the password and try again.";
    lv_obj_t *n = ui::note(c, msg.c_str());
    lv_obj_set_style_text_color(n, p.red, 0);
    lv_obj_set_style_text_font(n, theme::footnote, 0);
  }

  lv_obj_t *g = ui::group(c, "Networks");
  if (conn.length()) ui::row(g, ICON_WIFI, p.gray, conn.c_str(), "Connecting\xE2\x80\xA6", nullptr);
  for (const Network &n : nets) {
    if (n.ssid == cur || n.ssid == conn) continue;
    String v = valueFor(n);
    ui::row(g, ICON_WIFI, p.blue, n.ssid.c_str(), v.c_str(), [appRoot, n]() { join(appRoot, n); }, false);
  }
  if (wifimgr::scanning()) ui::row(g, nullptr, p.gray, "Scanning\xE2\x80\xA6", nullptr);
  ui::row(g, nullptr, p.gray, "Other Network\xE2\x80\xA6", nullptr, [appRoot]() { otherNetwork(appRoot); });

  bool any = false;
  for (const String &s : savedList) any |= s != cur;
  if (any) {
    g = ui::group(c, "My Networks");
    for (const String &s : savedList) {
      if (s == cur) continue;
      bool inRange = false;
      for (const Network &n : nets) inRange |= n.ssid == s;
      ui::row(g, ICON_WIFI, p.gray, s.c_str(), inRange ? "In Range" : "Not in Range",
              [appRoot, s]() { networkDetail(appRoot, s); });
    }
  }
}

void poll(lv_timer_t *t) {
  auto *st = (PageState *)t->user_data;
  uint32_t now = millis();
  if (wifimgr::enabled() && now - st->lastScan > SCAN_EVERY_MS) {
    st->lastScan = now;
    wifimgr::requestScan();
  }
  String sig = signature();
  if (sig != st->sig) {
    st->sig = sig;
    fill(st);
  }
}
}  // namespace

const char *wifiSummary() {
  static String s;
  switch (wifimgr::state()) {
    case wifimgr::OFF: return "Off";
    case wifimgr::CONNECTED: s = wifimgr::currentSsid(); return s.c_str();
    default: return "Not Connected";
  }
}

void wifiPage(lv_obj_t *appRoot) {
  ui::push(appRoot, "Wi-Fi", [appRoot](lv_obj_t *c) {
    const Palette &p = theme::p();
    auto *st = new PageState{appRoot, nullptr, "", millis()};
    lv_obj_t *g = ui::group(c);
    ui::switchRow(g, ICON_WIFI, p.blue, "Wi-Fi", wifimgr::enabled(), [st](bool on) {
      wifimgr::setEnabled(on);
      if (on) wifimgr::requestScan();
      st->sig = "";  // redraw on the next poll
    });
    st->dyn = ui::box(c);
    lv_obj_set_size(st->dyn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st->dyn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(st->dyn, 8, 0);
    wifimgr::requestScan();
    lv_timer_t *t = lv_timer_create(poll, 400, st);
    lv_obj_add_event_cb(c, [](lv_event_t *e) {
      auto *x = (std::pair<lv_timer_t *, PageState *> *)lv_event_get_user_data(e);
      lv_timer_del(x->first);
      delete x->second;
      delete x;
    }, LV_EVENT_DELETE, new std::pair<lv_timer_t *, PageState *>(t, st));
    fill(st);
    st->sig = signature();
  });
}
}
