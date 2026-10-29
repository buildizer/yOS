// Settings: system settings, plus each app's own settings (Settings › Apps › <app>), like iOS.
#include "apps/apps.h"
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/ui.h"

class SettingsApp : public App {
public:
  const char *id() const override { return "settings"; }
  const char *name() const override { return "Settings"; }
  const char *icon() const override { return ICON_SETTINGS; }
  lv_color_t tint() const override { return lv_color_hex(0x8E8E93); }

  void open(lv_obj_t *root) override {
    appRoot = root;
    ui::push(root, "Settings", [this](lv_obj_t *c) { buildMain(c); });
  }

private:
  lv_obj_t *appRoot = nullptr;

  void buildMain(lv_obj_t *c) {
    const Palette &p = theme::p();
    lv_obj_t *g = ui::group(c);
    ui::row(g, ICON_WIFI, p.blue, "Wi-Fi", settings::wifiSummary(), [this] { settings::wifiPage(appRoot); });
    ui::row(g, ICON_BLUETOOTH, p.blue, "Bluetooth", settings::bluetoothSummary(),
            [this] { settings::bluetoothPage(appRoot); });
    g = ui::group(c);
    ui::row(g, ICON_BRIGHTNESS, p.blue, "Display & Brightness", nullptr, [this] { settings::displayPage(appRoot); });
    ui::row(g, ICON_INFO, p.gray, "About", nullptr, [this] { settings::aboutPage(appRoot); });

    // Every app gets its own page here: its settings, background service, and storage.
    g = ui::group(c, "Apps");
    for (App *a : os::apps()) {
      if (a == this) continue;
      ui::row(g, a->icon(), a->tint(), a->name(), nullptr, [this, a] {
        ui::push(appRoot, a->name(), [a](lv_obj_t *c2) { os::appInfoPage(c2, a); });
      });
    }

    g = ui::group(c);
    ui::actionRow(g, "Restart", p.red, [] {
      ui::alert("Restart yOS?", "Apps will start again with their Start at Boot settings.",
                {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                 {"Restart", ui::AlertAction::DESTRUCTIVE, [] { ESP.restart(); }}});
    });
  }
};

App *createSettingsApp() { return new SettingsApp(); }
