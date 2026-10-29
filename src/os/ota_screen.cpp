#include "os/ota_screen.h"
#include "os/hal.h"
#include "os/ui.h"

static const lv_color_t DONE_GREEN = LV_COLOR_MAKE(0x30, 0xD1, 0x58);
static const char *STEP_TEXT[3] = {"Receiving update", "Verifying", "Restarting"};

static lv_obj_t *overlay = nullptr;
static lv_obj_t *arc = nullptr;
static lv_obj_t *pctLabel = nullptr;
static lv_obj_t *leftLabel = nullptr;
static lv_obj_t *stepIcon[3];
static lv_obj_t *stepText[3];
static lv_obj_t *pill = nullptr;
static lv_obj_t *pillIcon = nullptr;
static lv_obj_t *pillText = nullptr;
static uint32_t startMs = 0;
static int lastPct = -1;
static int phase = 0;

static void setPhase(int p) {  // 0 receiving, 1 verifying, 2 restarting
  phase = p;
  for (int i = 0; i < 3; i++) {
    bool done = i < p;
    bool active = i == p;
    lv_color_t c = done ? DONE_GREEN : active ? lv_color_white() : lv_color_hex(0x636366);
    lv_label_set_text(stepIcon[i], done ? ICON_CHECK : active ? ICON_REFRESH : "\xC2\xB7");
    lv_obj_set_style_text_color(stepIcon[i], c, 0);
    lv_obj_set_style_text_color(stepText[i], c, 0);
  }
}

void otaScreenBegin(const char *via) {
  hal::lock();
  if (overlay) lv_obj_del(overlay);
  overlay = ui::box(lv_layer_top());
  lv_obj_set_size(overlay, 480, 480);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);  // swallow touches

  arc = lv_arc_create(overlay);
  lv_obj_set_size(arc, 168, 168);
  lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 44);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x0A84FF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

  pctLabel = ui::label(overlay, "0%", theme::largeTitle, lv_color_white());
  lv_obj_align(pctLabel, LV_ALIGN_TOP_MID, 0, 44 + 62);
  leftLabel = ui::label(overlay, "Starting\xE2\x80\xA6", theme::caption, lv_color_hex(0x98989F));
  lv_obj_align(leftLabel, LV_ALIGN_TOP_MID, 0, 44 + 100);

  lv_obj_t *t = ui::label(overlay, "Updating yOS", theme::title, lv_color_white());
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 234);
  lv_obj_t *s = ui::label(overlay, via, theme::footnote, lv_color_hex(0x98989F));
  lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 268);

  lv_obj_t *list = ui::box(overlay);
  lv_obj_set_size(list, 300, 90);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 308);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 10, 0);
  for (int i = 0; i < 3; i++) {
    lv_obj_t *row = ui::box(list);
    lv_obj_set_size(row, 300, 20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    stepIcon[i] = ui::label(row, "", theme::footnote, lv_color_white());
    stepText[i] = ui::label(row, STEP_TEXT[i], theme::footnote, lv_color_white());
  }
  setPhase(0);

  pill = ui::box(overlay);
  lv_obj_set_size(pill, 440, 44);
  lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_radius(pill, 12, 0);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x2A2205), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(pill, 8, 0);
  pillIcon = ui::label(pill, ICON_ERROR, theme::footnoteBold, lv_color_hex(0xFFD60A));
  pillText = ui::label(pill, "Keep the panel powered on", theme::footnoteBold, lv_color_hex(0xFFD60A));

  startMs = millis();
  lastPct = -1;
  lv_refr_now(nullptr);
  hal::unlock();
}

void otaScreenProgress(unsigned int done, unsigned int total) {
  if (!overlay) return;
  int pct = total ? (int)((uint64_t)done * 100 / total) : 0;
  if (pct == lastPct) return;
  lastPct = pct;
  hal::lock();
  lv_arc_set_value(arc, pct);
  lv_label_set_text_fmt(pctLabel, "%d%%", pct);
  lv_obj_align(pctLabel, LV_ALIGN_TOP_MID, 0, 44 + 62);
  uint32_t elapsed = millis() - startMs;
  if (pct >= 100) {
    lv_label_set_text(leftLabel, "Done");
    if (phase < 1) setPhase(1);
  } else if (pct > 0 && elapsed > 500) {
    uint32_t secs = (uint64_t)elapsed * (100 - pct) / pct / 1000;
    lv_label_set_text_fmt(leftLabel, "About %u s left", (unsigned)(secs < 1 ? 1 : secs));
  }
  lv_obj_align(leftLabel, LV_ALIGN_TOP_MID, 0, 44 + 100);
  lv_refr_now(nullptr);
  hal::unlock();
}

void otaScreenRestarting() {
  if (!overlay) return;
  hal::lock();
  setPhase(2);
  lv_refr_now(nullptr);
  hal::unlock();
}

void otaScreenFail() {
  if (!overlay) return;
  hal::lock();
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x2A0A08), 0);
  lv_label_set_text(pillIcon, ICON_ERROR);
  lv_obj_set_style_text_color(pillIcon, lv_color_hex(0xFF453A), 0);
  lv_label_set_text(pillText, "Update failed. Try again.");
  lv_obj_set_style_text_color(pillText, lv_color_hex(0xFF453A), 0);
  lv_refr_now(nullptr);
  lv_timer_t *tm = lv_timer_create([](lv_timer_t *) {
    if (overlay) lv_obj_del(overlay);
    overlay = nullptr;
  }, 3000, nullptr);
  lv_timer_set_repeat_count(tm, 1);
  hal::unlock();
}
