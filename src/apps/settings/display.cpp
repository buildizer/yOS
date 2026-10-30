// Settings › Display & Brightness
#include <WiFi.h>
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/prefs.h"
#include "os/ui.h"
#include "os/hal.h"

namespace settings {

// iOS-style segmented control: a rounded track with one raised, selected segment.
lv_obj_t *segmented(lv_obj_t *parent, const char **opts) {
  const Palette &p = theme::p();
  lv_obj_t *bm = lv_btnmatrix_create(parent);
  lv_btnmatrix_set_map(bm, opts);
  lv_btnmatrix_set_one_checked(bm, true);
  lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_obj_set_size(bm, LV_PCT(100), 40);
  lv_obj_set_style_pad_all(bm, 2, 0);
  lv_obj_set_style_pad_gap(bm, 2, 0);
  lv_obj_set_style_radius(bm, 9, 0);
  lv_obj_set_style_border_width(bm, 0, 0);
  lv_obj_set_style_bg_color(bm, p.dark ? lv_color_hex(0x2C2C2E) : lv_color_hex(0xE9E9EB), 0);
  lv_obj_set_style_radius(bm, 7, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(bm, p.dark ? lv_color_hex(0x636366) : lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(bm, p.label, LV_PART_ITEMS);
  lv_obj_set_style_text_color(bm, p.label, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_font(bm, theme::footnoteBold, LV_PART_ITEMS);
  return bm;
}

// Backlight frequency on trial: a choice the panel can't show leaves it black, so a new frequency
// goes back to the old one after 10 s unless the person taps Keep (like a resolution change on a PC).
static const uint32_t freqValues[] = {100, 1000, 20000, 40000};
static lv_obj_t *freqSeg;            // the segmented control, while its page is open
static lv_obj_t *trialAlert;         // the Keep/Revert alert, while it is up
static lv_timer_t *trialTimer;       // pending revert
static uint32_t trialFrom;           // the frequency to go back to

static void markFreq() {
  if (!freqSeg) return;
  lv_btnmatrix_clear_btn_ctrl_all(freqSeg, LV_BTNMATRIX_CTRL_CHECKED);
  for (int i = 0; i < 4; i++)
    if (freqValues[i] == prefs::backlightFreq()) lv_btnmatrix_set_btn_ctrl(freqSeg, i, LV_BTNMATRIX_CTRL_CHECKED);
}

static void endTrial(bool keep) {
  if (trialTimer) { lv_timer_del(trialTimer); trialTimer = nullptr; }
  if (keep) {
    prefs::setBacklightFreq(prefs::backlightFreq(), true);
  } else {
    prefs::setBacklightFreq(trialFrom, false);  // never written, so NVS still has it
    hal::applyBacklightFreq();
    markFreq();
  }
}

static void tryBacklightFreq(uint32_t hz) {
  if (!trialTimer) trialFrom = prefs::backlightFreq();
  else lv_timer_del(trialTimer);
  if (trialAlert) lv_obj_del(trialAlert);
  prefs::setBacklightFreq(hz, false);
  hal::applyBacklightFreq();
  hal::wake();
  ui::alert("Keep This Frequency?", "If the screen looks wrong, wait: it goes back to the previous frequency in 10 seconds.",
            {{"Revert", ui::AlertAction::CANCEL, [] { endTrial(false); }},
             {"Keep", ui::AlertAction::NORMAL, [] { endTrial(true); }}});
  trialAlert = lv_obj_get_child(lv_layer_top(), -1);  // ui::alert's scrim, the newest top-layer object
  lv_obj_add_event_cb(trialAlert, [](lv_event_t *) { trialAlert = nullptr; }, LV_EVENT_DELETE, nullptr);
  trialTimer = lv_timer_create([](lv_timer_t *) {  // endTrial deletes the timer, so it fires once
    endTrial(false);
    if (trialAlert) lv_obj_del(trialAlert);
  }, 10000, nullptr);
}

void displayPage(lv_obj_t *appRoot) {
  ui::push(appRoot, "Display & Brightness", [appRoot](lv_obj_t *c) {
      const Palette &p = theme::p();
      lv_obj_t *g = ui::group(c, "Appearance", "Automatic switches to dark at 18:30 and back to light at 06:30.");
      lv_obj_t *seg = ui::box(g);
      lv_obj_set_size(seg, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_all(seg, 12, 0);
      static const char *opts[] = {"Automatic", "Light", "Dark", ""};
      lv_obj_t *bm = segmented(seg, opts);
      lv_btnmatrix_set_btn_ctrl(bm, prefs::appearance(), LV_BTNMATRIX_CTRL_CHECKED);
      lv_obj_add_event_cb(bm, [](lv_event_t *e) {
        uint16_t sel = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
        if (sel > 2) return;
        prefs::setAppearance(Appearance(sel));
        lv_async_call([](void *) { os::refreshAppearance(); }, nullptr);
      }, LV_EVENT_VALUE_CHANGED, nullptr);

      g = ui::group(c, "Home Screen");
      static lv_obj_t *wpValue;
      ui::Row wp = ui::row(g, ICON_APPS, p.blue, "Wallpaper", wallpaperSummary(), [appRoot] {
        wallpaperPage(appRoot, [] { if (wpValue) lv_label_set_text(wpValue, wallpaperSummary()); });
      });
      wpValue = wp.value;
      lv_obj_add_event_cb(wp.obj, [](lv_event_t *) { wpValue = nullptr; }, LV_EVENT_DELETE, nullptr);

      g = ui::group(c, "Brightness", "If the screen goes dark, touch and hold anywhere for 10 seconds "
                                       "to return to full brightness.");
      lv_obj_t *sr = ui::box(g);
      lv_obj_set_size(sr, LV_PCT(100), 56);
      lv_obj_set_style_pad_hor(sr, 16, 0);
      lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(sr, 14, 0);
      ui::label(sr, ICON_BRIGHTNESS, theme::body, p.secondary);
      lv_obj_t *sl = lv_slider_create(sr);
      lv_obj_set_flex_grow(sl, 1);
      lv_obj_set_height(sl, 6);
      // The slider is a percentage of the stored 10..255 level; hal maps that level through a gamma curve.
      lv_slider_set_range(sl, 4, 100);
      lv_slider_set_value(sl, max(4, (prefs::brightness() * 100 + 127) / 255), LV_ANIM_OFF);
      lv_obj_t *pct = ui::label(sr, "", theme::footnote, p.secondary);
      lv_obj_set_width(pct, 44);
      lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
      lv_label_set_text_fmt(pct, "%d%%", (int)lv_slider_get_value(sl));
      lv_obj_set_style_bg_color(sl, p.separator, 0);
      lv_obj_set_style_bg_color(sl, p.blue, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
      lv_obj_set_style_pad_all(sl, 10, LV_PART_KNOB);
      lv_obj_set_style_shadow_width(sl, 6, LV_PART_KNOB);
      lv_obj_set_style_shadow_opa(sl, LV_OPA_30, LV_PART_KNOB);
      lv_obj_set_ext_click_area(sl, 16);
      lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        int v = lv_slider_get_value(lv_event_get_target(e));
        prefs::setBrightness(max(10, v * 255 / 100), false);  // live while dragging
        lv_label_set_text_fmt((lv_obj_t *)lv_event_get_user_data(e), "%d%%", v);
      }, LV_EVENT_VALUE_CHANGED, pct);
      lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        prefs::setBrightness(prefs::brightness(), true);  // saved once, when the finger lifts
      }, LV_EVENT_RELEASED, nullptr);

      g = ui::group(c, "Rotation", "Turns the whole screen and touch, for panels mounted sideways or upside down. "
                                     "Animations are a little slower at 90\xC2\xB0 and 270\xC2\xB0.");
      lv_obj_t *rotSeg = ui::box(g);
      lv_obj_set_size(rotSeg, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_all(rotSeg, 12, 0);
      static const char *rotOpts[] = {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0", ""};
      lv_obj_t *rm = segmented(rotSeg, rotOpts);
      lv_btnmatrix_set_btn_ctrl(rm, prefs::rotation(), LV_BTNMATRIX_CTRL_CHECKED);
      lv_obj_add_event_cb(rm, [](lv_event_t *e) {
        uint16_t sel = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
        if (sel > 3) return;
        prefs::setRotation(sel);
        hal::setRotation(sel);
      }, LV_EVENT_VALUE_CHANGED, nullptr);
      ui::switchRow(g, nullptr, p.gray, "Show on Home Screen", prefs::rotationWidget(), [](bool on) {
        prefs::setRotationWidget(on);
        lv_async_call([](void *) { os::rebuildHome(); }, nullptr);
      });

      g = ui::group(c, "Dim After");
      lv_obj_t *dimSeg = ui::box(g);
      lv_obj_set_size(dimSeg, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_all(dimSeg, 12, 0);
      static const char *dimOpts[] = {"1 min", "5 min", "15 min", "Never", ""};
      static const uint8_t dimValues[] = {1, 5, 15, 0};
      lv_obj_t *dm = segmented(dimSeg, dimOpts);
      for (int i = 0; i < 4; i++)
        if (dimValues[i] == prefs::dimAfterMin()) lv_btnmatrix_set_btn_ctrl(dm, i, LV_BTNMATRIX_CTRL_CHECKED);
      lv_obj_add_event_cb(dm, [](lv_event_t *e) {
        uint16_t sel = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
        if (sel < 4) prefs::setDimAfterMin(dimValues[sel]);
      }, LV_EVENT_VALUE_CHANGED, nullptr);
      ui::note(c, "After this long without a touch the screen dims to 40%, and it turns off after 30 minutes. "
                  "It stays bright during an outage.");

      g = ui::group(c, "Backlight Frequency", "Try another frequency if the screen flickers or goes dark "
                                                "below 100% brightness. 100% lights at any frequency.");
      lv_obj_t *freqBox = ui::box(g);
      lv_obj_set_size(freqBox, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_all(freqBox, 12, 0);
      static const char *freqOpts[] = {"100 Hz", "1 kHz", "20 kHz", "40 kHz", ""};
      freqSeg = segmented(freqBox, freqOpts);
      markFreq();
      lv_obj_add_event_cb(freqSeg, [](lv_event_t *) { freqSeg = nullptr; }, LV_EVENT_DELETE, nullptr);
      lv_obj_add_event_cb(freqSeg, [](lv_event_t *e) {
        uint16_t sel = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
        if (sel < 4 && freqValues[sel] != prefs::backlightFreq()) tryBacklightFreq(freqValues[sel]);
      }, LV_EVENT_VALUE_CHANGED, nullptr);

      g = ui::group(c, "Performance");
      ui::Row fps = ui::row(g, ICON_MOTION, p.green, "Frame Rate", "Idle", nullptr, false);
      char hz[16];
      snprintf(hz, sizeof(hz), "%.0f Hz", hal::refreshHz());
      ui::row(g, ICON_BRIGHTNESS, p.orange, "Refresh Rate", hz, nullptr, false);
      lv_timer_t *tm = lv_timer_create([](lv_timer_t *t) {
        uint32_t f = hal::currentFps();
        lv_obj_t *l = (lv_obj_t *)t->user_data;
        // The label update itself redraws about twice a second, so a couple of frames still means idle.
        if (f <= 3) lv_label_set_text(l, "Idle");
        else lv_label_set_text_fmt(l, "%u fps", (unsigned)f);
      }, 500, fps.value);
      lv_obj_add_event_cb(fps.value, [](lv_event_t *e) { lv_timer_del((lv_timer_t *)lv_event_get_user_data(e)); },
                          LV_EVENT_DELETE, tm);

      g = ui::group(c, "Accessibility");
      ui::switchRow(g, ICON_MOTION, p.blue, "Reduce Motion", prefs::reduceMotion(),
                    [](bool on) { prefs::setReduceMotion(on); });
    });
}
}
