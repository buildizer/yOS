// Settings › Bluetooth
#include <functional>
#include <memory>
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/ui.h"
#include "services/ble.h"

namespace settings {

const char *bluetoothSummary() {
  static String s;
  ble::Status st = ble::status();
  if (!st.enabled) return "Off";
  if (st.hasBond && st.link == ble::LINK_CONNECTED) {
    s = st.bondName;
    return s.c_str();
  }
  return "On";
}

namespace {
const int MAX_SHOWN = 12;

// A page whose body is rebuilt whenever the service state changes. The timer dies with the page.
struct Live {
  lv_obj_t *page;
  lv_obj_t *body;
  std::function<void(lv_obj_t *)> render;
  uint32_t version;
  bool dirty;
};

void rebuild(Live *l) {
  lv_coord_t y = lv_obj_get_scroll_y(l->page);
  lv_obj_clean(l->body);
  l->render(l->body);
  lv_obj_update_layout(l->page);
  lv_obj_scroll_to_y(l->page, y, LV_ANIM_OFF);
}

void livePage(lv_obj_t *appRoot, const char *title, std::function<void(lv_obj_t *)> render) {
  ui::push(appRoot, title, [&](lv_obj_t *page) {
    lv_obj_t *body = ui::box(page);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    auto *l = new Live{page, body, std::move(render), ble::status().version, false};
    l->render(body);
    lv_timer_t *t = lv_timer_create([](lv_timer_t *tm) {
      auto *l = (Live *)tm->user_data;
      uint32_t v = ble::status().version;
      if (v != l->version) {
        l->version = v;
        l->dirty = true;
      }
      if (!l->dirty) return;
      // Never rebuild under a finger: the list would jump while the person is tapping or scrolling.
      lv_indev_t *in = lv_indev_get_next(nullptr);
      if (in && in->proc.state == LV_INDEV_STATE_PRESSED) return;
      l->dirty = false;
      rebuild(l);
    }, 400, l);
    lv_obj_add_event_cb(page, [](lv_event_t *e) {
      auto *l = (Live *)lv_event_get_user_data(e);
      // The timer is found by its user data; there is at most one per page.
      for (lv_timer_t *t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t))
        if (t->user_data == l) {
          lv_timer_del(t);
          break;
        }
      delete l;
    }, LV_EVENT_DELETE, l);
    (void)t;
  });
}

const char *signalText(int rssi, char *buf, size_t n) {
  snprintf(buf, n, "%s (%d dBm)", rssi > -60 ? "Strong" : rssi > -75 ? "Good" : rssi > -85 ? "Fair" : "Weak", rssi);
  return buf;
}

void failureNote(lv_obj_t *c, const ble::Status &st) {
  if (!st.connectFailed) return;
  char msg[128];
  snprintf(msg, sizeof(msg), "Couldn't connect to %s. Make sure it's nearby and in pairing mode.", st.target);
  lv_obj_t *n = ui::note(c, msg);
  lv_obj_set_style_text_color(n, theme::p().red, 0);
}

void devicePage(lv_obj_t *appRoot) {
  livePage(appRoot, ble::status().bondName, [](lv_obj_t *c) {
    const Palette &p = theme::p();
    ble::Status st = ble::status();
    if (!st.hasBond) return;  // forgotten: the page is on its way out
    bool connected = st.link == ble::LINK_CONNECTED;
    bool connecting = st.link == ble::LINK_CONNECTING;
    lv_obj_t *g = ui::group(c);
    ui::row(g, nullptr, p.gray, "Status",
            connected ? "Connected" : connecting ? "Connecting\xE2\x80\xA6" : "Not Connected");
    if (connected) ui::row(g, nullptr, p.gray, "Security", st.paired ? "Paired" : "Not Encrypted");
    ui::row(g, nullptr, p.gray, "Address", st.bondAddr);
    failureNote(c, st);
    g = ui::group(c);
    if (connected)
      ui::actionRow(g, "Disconnect", p.accent, [] { ble::disconnect(); });
    else if (connecting)
      ui::row(g, nullptr, p.gray, "Connecting\xE2\x80\xA6", nullptr, nullptr, false);
    else
      ui::actionRow(g, "Connect", p.accent, [] { ble::connectBonded(); });
    g = ui::group(c);
    ui::actionRow(g, "Forget This Device", p.red, [] {
      ui::alert("Forget This Device?", "It will need to be paired again to reconnect.",
                {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                 {"Forget Device", ui::AlertAction::DESTRUCTIVE, [] {
                    ble::forget();
                    ui::pop();
                  }}});
    });
  });
}
}  // namespace

void bluetoothPage(lv_obj_t *appRoot) {
  auto scannedFlag = std::make_shared<bool>(false);
  livePage(appRoot, "Bluetooth", [appRoot, scannedFlag](lv_obj_t *c) {
    bool &scanned = *scannedFlag;
    const Palette &p = theme::p();
    ble::Status st = ble::status();

    char foot[160];
    ble::HeapCost cost = ble::lastStartCost();
    int kb = ((int)cost.internalBefore - (int)cost.internalAfter) / 1024;
    if (st.enabled && kb > 0)
      snprintf(foot, sizeof(foot), "Bluetooth LE is using about %d KB of memory. Turn it off to free it.", kb);
    else
      snprintf(foot, sizeof(foot), "Connect to nearby Bluetooth LE devices. Uses memory while on; off by default.");
    lv_obj_t *g = ui::group(c, nullptr, st.error[0] ? st.error : foot);
    ui::switchRow(g, ICON_BLUETOOTH, p.blue, "Bluetooth", st.enabled, [](bool on) { ble::setEnabled(on); });
    if (!st.enabled) return;

    g = ui::group(c, nullptr, "Lets nearby phones find this display as \xE2\x80\x9CyOS\xE2\x80\x9D and read its version.");
    ui::switchRow(g, nullptr, p.blue, "Discoverable as yOS", st.discoverable,
                  [](bool on) { ble::setDiscoverable(on); });

    if (st.hasBond) {
      g = ui::group(c, "My Devices");
      const char *state = st.link == ble::LINK_CONNECTED    ? "Connected"
                          : st.link == ble::LINK_CONNECTING ? "Connecting\xE2\x80\xA6"
                                                            : "Not Connected";
      ui::row(g, ICON_BLUETOOTH, st.link == ble::LINK_CONNECTED ? p.blue : p.gray, st.bondName, state,
              [appRoot] { devicePage(appRoot); });
    }

    if (!st.ready) {
      g = ui::group(c, "Other Devices");
      ui::row(g, nullptr, p.gray, "Starting Bluetooth\xE2\x80\xA6", nullptr, nullptr, false);
      return;
    }
    if (!scanned && !st.scanning) {
      scanned = true;
      ble::scan();
      st.scanning = true;
    }

    ble::Device list[ble::MAX_LIST];
    int n = ble::devices(list, ble::MAX_LIST);
    g = ui::group(c, "Other Devices");
    if (st.scanning)
      ui::row(g, nullptr, p.gray, "Scanning\xE2\x80\xA6", nullptr, nullptr, false);
    else
      ui::actionRow(g, "Scan Again", p.accent, [] { ble::scan(); });
    int shown = 0;
    for (int i = 0; i < n && shown < MAX_SHOWN; i++) {
      if (st.hasBond && strcmp(list[i].addr, st.bondAddr) == 0) continue;
      if (!list[i].connectable) continue;
      char sig[32];
      bool busy = st.link == ble::LINK_CONNECTING && strcmp(st.target, list[i].name) == 0;
      ble::Device d = list[i];
      ui::row(g, nullptr, p.gray, d.name, busy ? "Connecting\xE2\x80\xA6" : signalText(d.rssi, sig, sizeof(sig)),
              [d] { ble::connect(d); });
      shown++;
    }
    failureNote(c, st);
    if (!shown && !st.scanning)
      ui::note(c, "No devices found. Make sure the device is nearby and in pairing mode.");
  });
}
}
