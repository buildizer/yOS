// iPhone Remote: the board as a Bluetooth keyboard, media remote and mouse for a paired iPhone.
// Media keys, a camera shutter, typed text, eight shortcut buttons (touch and hold to edit) and a
// touchpad for the AssistiveTouch pointer. Everything goes through ble::hid*(), which only queues.
#include <Preferences.h>
#include "apps/apps.h"
#include "os/os.h"
#include "os/ui.h"
#include "services/ble.h"

namespace {
const int SHORTCUTS = 8;
const int POINTER_GAIN = 2;    // the panel is small next to the phone: one pixel here moves two there
const int TAP_SLOP = 10;       // a touch that travels less than this (px) and lifts quickly is a click
const uint32_t TAP_MS = 350;

struct Shortcut {
  char label[14];
  uint8_t mods;   // ble::HidMod bits
  uint8_t key;    // keyboard usage
};

const Shortcut DEFAULTS[SHORTCUTS] = {
  {"Home", ble::MOD_GUI, 0x0B},          // Cmd-H
  {"Search", ble::MOD_GUI, 0x2C},        // Cmd-Space: Spotlight
  {"Switch App", ble::MOD_GUI, 0x2B},    // Cmd-Tab
  {"Escape", 0, 0x29},
  {"\xE2\x86\x90", 0, 0x50},             // left arrow
  {"\xE2\x86\x92", 0, 0x4F},             // right arrow
  {"Tab", 0, 0x2B},
  {"Return", 0, 0x28},
};

struct KeyName {
  uint8_t code;
  const char *name;
  const char *shortName;   // in a combo ("Cmd+←"); null: same as name
};
// Picker order: these, then A–Z, 0–9 and the punctuation keys.
const KeyName NAMED[] = {
  {0x28, "Return", nullptr}, {0x29, "Escape", "Esc"}, {0x2C, "Space", nullptr}, {0x2B, "Tab", nullptr},
  {0x2A, "Delete", "Del"}, {0x4C, "Forward Delete", "Fwd Del"},
  {0x50, "Left Arrow", "\xE2\x86\x90"}, {0x4F, "Right Arrow", "\xE2\x86\x92"},
  {0x52, "Up Arrow", "\xE2\x86\x91"}, {0x51, "Down Arrow", "\xE2\x86\x93"},
  {0x4A, "Home", nullptr}, {0x4D, "End", nullptr}, {0x4B, "Page Up", "PgUp"}, {0x4E, "Page Down", "PgDn"},
};
const KeyName PUNCT[] = {
  {0x2D, "-", nullptr}, {0x2E, "=", nullptr}, {0x2F, "[", nullptr}, {0x30, "]", nullptr}, {0x31, "\\", nullptr},
  {0x33, ";", nullptr}, {0x34, "'", nullptr}, {0x35, "`", nullptr}, {0x36, ",", nullptr}, {0x37, ".", nullptr},
  {0x38, "/", nullptr},
};

const char *keyName(uint8_t code, char *buf, size_t n, bool brief) {
  if (code >= 0x04 && code <= 0x1D) snprintf(buf, n, "%c", 'A' + code - 0x04);
  else if (code >= 0x1E && code <= 0x26) snprintf(buf, n, "%c", '1' + code - 0x1E);
  else if (code == 0x27) snprintf(buf, n, "0");
  else {
    for (const KeyName &k : NAMED)
      if (k.code == code) return snprintf(buf, n, "%s", brief && k.shortName ? k.shortName : k.name), buf;
    for (const KeyName &k : PUNCT)
      if (k.code == code) return snprintf(buf, n, "%s", k.name), buf;
    snprintf(buf, n, "Key %02X", code);
  }
  return buf;
}

// "Cmd+Shift+3", modifiers in the Mac order (Control, Option, Shift, Command).
const char *comboText(const Shortcut &s, char *buf, size_t n) {
  char key[16];
  snprintf(buf, n, "%s%s%s%s%s", s.mods & ble::MOD_CTRL ? "Ctrl+" : "", s.mods & ble::MOD_ALT ? "Opt+" : "",
           s.mods & ble::MOD_SHIFT ? "Shift+" : "", s.mods & ble::MOD_GUI ? "Cmd+" : "",
           keyName(s.key, key, sizeof(key), true));
  return buf;
}

// Runs fn on `code`; the std::function is freed with the object.
void on(lv_obj_t *obj, lv_event_code_t code, ui::Action fn) {
  auto *f = new ui::Action(std::move(fn));
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { (*(ui::Action *)lv_event_get_user_data(e))(); }, code, f);
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { delete (ui::Action *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, f);
}

// A key-cap button inside a card: title, optional caption. Taps on release (not after a long press).
lv_obj_t *padButton(lv_obj_t *parent, int w, int h, const char *title, const char *sub, ui::Action onTap,
                    ui::Action onHold = nullptr) {
  const Palette &p = theme::p();
  lv_obj_t *b = ui::box(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_bg_color(b, p.dark ? lv_color_hex(0x3A3A3C) : lv_color_hex(0xE5E5EA), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(b, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(b, 4, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(b, 2, 0);
  lv_obj_t *t = ui::label(b, title, theme::bodyBold, p.label);
  lv_obj_set_width(t, LV_PCT(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  if (sub) {
    lv_obj_t *s = ui::label(b, sub, theme::caption, p.secondary);
    lv_obj_set_width(s, LV_PCT(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
  }
  on(b, LV_EVENT_SHORT_CLICKED, std::move(onTap));
  if (onHold) on(b, LV_EVENT_LONG_PRESSED, std::move(onHold));
  return b;
}

// A wrapping grid inside a group card.
lv_obj_t *grid(lv_obj_t *group) {
  lv_obj_t *g = ui::box(group);
  lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(g, 12, 0);
  lv_obj_set_style_pad_gap(g, 8, 0);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
  return g;
}
}  // namespace

class RemoteApp : public App {
public:
  const char *id() const override { return "remote"; }
  const char *name() const override { return "iPhone Remote"; }
  const char *icon() const override { return ICON_KEYBOARD; }
  lv_color_t tint() const override { return lv_color_hex(0x5E5CE6); }

  void open(lv_obj_t *root) override {
    appRoot = root;
    load();
    ui::push(root, "iPhone Remote", [this](lv_obj_t *c) { build(c); });
  }
  void close() override {
    page = statusBox = controls = shortcutGrid = nullptr;
    editName = editKey = nullptr;
  }

  // Redraw the status card when what it shows changes, never under a finger (the page would jump).
  void tick() override {
    if (!statusBox) return;
    ble::Status st = ble::status();
    ble::HidState hs = ble::hidState();
    uint32_t sig = hs | st.enabled << 4 | st.discoverable << 5;
    if (sig == shownSig && !strcmp(st.peerName, shownName)) return;
    lv_indev_t *in = lv_indev_get_next(nullptr);
    if (in && in->proc.state == LV_INDEV_STATE_PRESSED) return;
    renderStatus();
  }

private:
  lv_obj_t *appRoot = nullptr, *page = nullptr, *statusBox = nullptr, *controls = nullptr;
  lv_obj_t *shortcutGrid = nullptr;
  lv_obj_t *editName = nullptr, *editKey = nullptr;   // the edit page's value labels, while it's open
  Shortcut sc[SHORTCUTS];
  bool loaded = false;
  uint32_t shownSig = ~0u;
  char shownName[32] = "";
  int padTravel = 0;
  uint32_t padDown = 0;

  // ---------- shortcuts (own NVS namespace) ----------

  void load() {
    if (loaded) return;
    loaded = true;
    memcpy(sc, DEFAULTS, sizeof(sc));
    Preferences p;
    p.begin("yosremote", true);
    if (p.getBytesLength("keys") == sizeof(sc)) p.getBytes("keys", sc, sizeof(sc));
    p.end();
    for (Shortcut &s : sc) s.label[sizeof(s.label) - 1] = 0;
  }

  void save() {
    Preferences p;
    p.begin("yosremote", false);
    p.putBytes("keys", sc, sizeof(sc));
    p.end();
  }

  // ---------- main page ----------

  void build(lv_obj_t *c) {
    page = c;
    statusBox = ui::box(c);
    lv_obj_set_size(statusBox, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(statusBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(statusBox, 8, 0);
    controls = ui::box(c);
    lv_obj_set_size(controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(controls, 8, 0);
    buildMedia(controls);
    buildText(controls);
    buildShortcuts(controls);
    buildTouchpad(controls);
    renderStatus();
  }

  void renderStatus() {
    const Palette &p = theme::p();
    ble::Status st = ble::status();
    ble::HidState hs = ble::hidState();
    shownSig = hs | st.enabled << 4 | st.discoverable << 5;
    snprintf(shownName, sizeof(shownName), "%s", st.peerName);
    lv_obj_clean(statusBox);
    lv_obj_t *g = ui::group(statusBox);
    const char *title = !st.enabled ? "Bluetooth Is Off"
                      : hs == ble::HID_OFF ? "Starting Bluetooth\xE2\x80\xA6"
                      : hs == ble::HID_IDLE ? "Not Connected"
                      : hs == ble::HID_SECURING ? "Connecting\xE2\x80\xA6" : "Connected";
    ui::Row r = ui::statusRow(g, title);
    bool ready = hs == ble::HID_READY;
    lv_label_set_text(r.icon, ready ? ICON_BT_CONNECTED : hs == ble::HID_SECURING ? ICON_BT_SEARCHING : ICON_BLUETOOTH);
    lv_obj_set_style_text_color(r.icon, ready ? p.green : hs == ble::HID_SECURING ? p.orange : p.gray, 0);
    if (hs >= ble::HID_SECURING) lv_label_set_text(r.value, st.peerName[0] ? st.peerName : "iPhone");
    if (!st.enabled)
      ui::switchRow(g, nullptr, p.gray, "Bluetooth", false, [](bool on) { ble::setEnabled(on); });
    else if (hs == ble::HID_IDLE && !st.discoverable)
      ui::switchRow(g, nullptr, p.gray, "Discoverable as yOS", false, [](bool on) { ble::setDiscoverable(on); });
    if (hs == ble::HID_IDLE || !st.enabled)
      ui::note(statusBox, st.discoverable
               ? "Not connected \xE2\x80\x94 pair \xE2\x80\x9CyOS\xE2\x80\x9D in iPhone Settings \xE2\x80\xBA Bluetooth, "
                 "then type the code this display shows. A paired iPhone reconnects on its own."
               : "Not connected \xE2\x80\x94 turn on Discoverable, then pair \xE2\x80\x9CyOS\xE2\x80\x9D in iPhone "
                 "Settings \xE2\x80\xBA Bluetooth. A paired iPhone reconnects while yOS is discoverable.");
    else if (hs == ble::HID_SECURING)
      ui::note(statusBox, "If the iPhone asks for a code, type the one this display shows.");
    lv_obj_set_style_opa(controls, ready ? LV_OPA_COVER : LV_OPA_50, 0);
  }

  void buildMedia(lv_obj_t *c) {
    lv_obj_t *g = ui::group(c, "Media");
    lv_obj_t *pad = grid(g);
    const int w = (480 - 2 * ui::SIDE - 24 - 16) / 3, h = 56;
    padButton(pad, w, h, "Previous", nullptr, [] { ble::hidMedia(ble::MEDIA_PREVIOUS); });
    padButton(pad, w, h, ICON_PLAY " " ICON_PAUSE, nullptr, [] { ble::hidMedia(ble::MEDIA_PLAY_PAUSE); });
    padButton(pad, w, h, "Next", nullptr, [] { ble::hidMedia(ble::MEDIA_NEXT); });
    padButton(pad, w, h, "Vol \xE2\x88\x92", nullptr, [] { ble::hidMedia(ble::MEDIA_VOLUME_DOWN); });
    padButton(pad, w, h, "Mute", nullptr, [] { ble::hidMedia(ble::MEDIA_MUTE); });
    padButton(pad, w, h, "Vol +", nullptr, [] { ble::hidMedia(ble::MEDIA_VOLUME_UP); });
    lv_obj_t *shutter = padButton(pad, LV_PCT(100), h, "Camera Shutter", "Takes a photo in the Camera app",
                                  [] { ble::hidMedia(ble::MEDIA_VOLUME_UP); });
    lv_obj_set_style_bg_color(shutter, theme::p().accent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(shutter, 0), lv_color_white(), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(shutter, 1), lv_color_white(), 0);
  }

  void buildText(lv_obj_t *c) {
    const Palette &p = theme::p();
    lv_obj_t *g = ui::group(c, nullptr, "US keyboard layout: letters, digits and punctuation. Other characters are skipped.");
    lv_obj_t *root = appRoot;
    ui::row(g, ICON_KEYBOARD, p.blue, "Type Text\xE2\x80\xA6", nullptr, [root] {
      ui::textInput(root, "Type Text", "Typed on the iPhone", "", false, [](const String &s) { ble::hidType(s.c_str()); });
    });
  }

  void buildShortcuts(lv_obj_t *c) {
    lv_obj_t *g = ui::group(c, "Shortcuts", "Touch and hold a shortcut to change it.");
    shortcutGrid = grid(g);
    fillShortcuts();
  }

  void fillShortcuts() {
    if (!shortcutGrid) return;
    lv_obj_clean(shortcutGrid);
    const int w = (480 - 2 * ui::SIDE - 24 - 24) / 4;
    for (int i = 0; i < SHORTCUTS; i++) {
      char combo[40];
      padButton(shortcutGrid, w, 64, sc[i].label, comboText(sc[i], combo, sizeof(combo)),
                [this, i] { ble::hidKey(sc[i].mods, sc[i].key); },
                [this, i] { editShortcut(i); });
    }
  }

  // The touchpad holds the page still while a finger is on it, and turns drags into pointer motion.
  void buildTouchpad(lv_obj_t *c) {
    const Palette &p = theme::p();
    lv_obj_t *g = ui::group(c, "Touchpad",
                            "Moves the AssistiveTouch pointer; tap to click. On the iPhone turn on Settings \xE2\x80\xBA "
                            "Accessibility \xE2\x80\xBA Touch \xE2\x80\xBA AssistiveTouch.");
    lv_obj_t *pad = ui::box(g);
    lv_obj_set_size(pad, LV_PCT(100), 200);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(pad, p.cardPressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *hint = ui::label(pad, "Drag to move \xC2\xB7 Tap to click", theme::footnote, p.secondary);
    lv_obj_center(hint);
    lv_obj_add_event_cb(pad, padEvent, LV_EVENT_ALL, this);
  }

  static void padEvent(lv_event_t *e) {
    auto *a = (RemoteApp *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (!a->page) return;
    if (code == LV_EVENT_PRESSED) {
      lv_obj_clear_flag(a->page, LV_OBJ_FLAG_SCROLLABLE);
      a->padTravel = 0;
      a->padDown = millis();
    } else if (code == LV_EVENT_PRESSING) {
      lv_point_t v;
      lv_indev_get_vect(lv_indev_get_act(), &v);
      a->padTravel += abs(v.x) + abs(v.y);
      ble::hidMove(v.x * POINTER_GAIN, v.y * POINTER_GAIN);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
      lv_obj_add_flag(a->page, LV_OBJ_FLAG_SCROLLABLE);
      if (code == LV_EVENT_RELEASED && a->padTravel < TAP_SLOP && millis() - a->padDown < TAP_MS) ble::hidClick();
    }
  }

  // ---------- editing a shortcut ----------

  void changed() {
    save();
    fillShortcuts();
  }

  void editShortcut(int i) {
    ui::push(appRoot, "Edit Shortcut", [this, i](lv_obj_t *c) {
      const Palette &p = theme::p();
      char buf[24];
      lv_obj_t *g = ui::group(c);
      editName = ui::row(g, nullptr, p.gray, "Name", sc[i].label, [this, i] {
        ui::textInput(appRoot, "Name", "Shortcut name", sc[i].label, false, [this, i](const String &s) {
          if (!s.length()) return;
          snprintf(sc[i].label, sizeof(sc[i].label), "%s", s.c_str());
          changed();
          if (editName) lv_label_set_text(editName, sc[i].label);
        });
      }).value;
      editKey = ui::row(g, nullptr, p.gray, "Key", keyName(sc[i].key, buf, sizeof(buf), false),
                        [this, i] { pickKey(i); }).value;
      g = ui::group(c, "Hold Down");
      struct M { uint8_t bit; const char *name; };
      static const M mods[] = {{ble::MOD_GUI, "Command"}, {ble::MOD_SHIFT, "Shift"}, {ble::MOD_ALT, "Option"},
                               {ble::MOD_CTRL, "Control"}};
      for (const M &m : mods) {
        uint8_t bit = m.bit;
        ui::switchRow(g, nullptr, p.gray, m.name, sc[i].mods & bit, [this, i, bit](bool on) {
          sc[i].mods = on ? sc[i].mods | bit : sc[i].mods & ~bit;
          changed();
        });
      }
      g = ui::group(c);
      ui::actionRow(g, "Send Now", p.accent, [this, i] { ble::hidKey(sc[i].mods, sc[i].key); });
      g = ui::group(c);
      ui::actionRow(g, "Restore Default", p.red, [this, i] {
        sc[i] = DEFAULTS[i];
        changed();
        ui::pop();
      });
      lv_obj_add_event_cb(c, [](lv_event_t *e) {
        auto *a = (RemoteApp *)lv_event_get_user_data(e);
        a->editName = a->editKey = nullptr;
      }, LV_EVENT_DELETE, this);
    });
  }

  void keyRow(lv_obj_t *g, int i, uint8_t code) {
    char buf[24];
    ui::Row r = ui::row(g, nullptr, theme::p().gray, keyName(code, buf, sizeof(buf), false), nullptr, [this, i, code] {
      sc[i].key = code;
      changed();
      char b[24];
      if (editKey) lv_label_set_text(editKey, keyName(code, b, sizeof(b), false));
      ui::pop();
    }, false);
    if (code == sc[i].key) ui::label(r.obj, ICON_CHECK, theme::body, theme::p().accent);
  }

  void pickKey(int i) {
    ui::push(appRoot, "Key", [this, i](lv_obj_t *c) {
      lv_obj_t *g = ui::group(c);
      for (const KeyName &k : NAMED) keyRow(g, i, k.code);
      g = ui::group(c, "Letters");
      for (uint8_t k = 0x04; k <= 0x1D; k++) keyRow(g, i, k);
      g = ui::group(c, "Numbers");
      for (uint8_t k = 0x1E; k <= 0x27; k++) keyRow(g, i, k);
      g = ui::group(c, "Punctuation");
      for (const KeyName &k : PUNCT) keyRow(g, i, k.code);
    });
  }
};

App *createRemoteApp() { return new RemoteApp(); }
