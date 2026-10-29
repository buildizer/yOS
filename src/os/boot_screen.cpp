#include "os/boot_screen.h"
#include <Arduino.h>
#include "os/ui.h"
#include "version.h"

static lv_obj_t *fill = nullptr;
static lv_obj_t *stageLabel = nullptr;
static uint32_t shownAt = 0;
static const int BAR_W = 200;

static void setFillWidth(void *obj, int32_t w) { lv_obj_set_width((lv_obj_t *)obj, w); }

void bootScreenStart() {
  shownAt = millis();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t *tile = ui::box(scr);
  lv_obj_set_size(tile, 96, 96);
  lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 128);
  lv_obj_set_style_radius(tile, 24, 0);
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x1C1C1E), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(tile, lv_color_hex(0x2C2C2E), 0);
  lv_obj_set_style_border_width(tile, 1, 0);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(tile, 0, 0);
  ui::label(tile, "y", theme::largeTitle, lv_color_white());
  ui::label(tile, "OS", theme::largeTitle, lv_color_hex(0x0A84FF));

  lv_obj_t *ver = ui::label(scr, "Version " YOS_FIRMWARE_VERSION, theme::footnote, lv_color_hex(0x98989F));
  lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 242);

  lv_obj_t *track = ui::box(scr);
  lv_obj_set_size(track, BAR_W, 4);
  lv_obj_align(track, LV_ALIGN_TOP_MID, 0, 308);
  lv_obj_set_style_radius(track, 2, 0);
  lv_obj_set_style_bg_color(track, lv_color_hex(0x2C2C2E), 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(track, true, 0);
  fill = ui::box(track);
  lv_obj_set_size(fill, 0, 4);
  lv_obj_set_pos(fill, 0, 0);
  lv_obj_set_style_radius(fill, 2, 0);
  lv_obj_set_style_bg_color(fill, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

  stageLabel = ui::label(scr, "", theme::footnote, lv_color_hex(0x98989F));
  lv_obj_align(stageLabel, LV_ALIGN_TOP_MID, 0, 326);
  bootScreenStage(0, "Starting\xE2\x80\xA6");
}

void bootScreenStage(int step, const char *text) {
  if (!fill) return;
  if (step < 0) step = 0;
  if (step > 4) step = 4;
  lv_label_set_text(stageLabel, text);
  lv_obj_align(stageLabel, LV_ALIGN_TOP_MID, 0, 326);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, fill);
  lv_anim_set_exec_cb(&a, setFillWidth);
  lv_anim_set_values(&a, lv_obj_get_width(fill), step * BAR_W / 4);
  lv_anim_set_time(&a, 250);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
  lv_timer_handler();
}

void bootScreenEnd() {
  if (!fill) return;
  bootScreenStage(4, "Ready");
  uint32_t t0 = millis();
  // Hold "Ready" briefly, and keep the screen up at least 1.5 s in all, so a fast boot doesn't just flash.
  uint32_t hold = max<int32_t>(400, 1500 - int32_t(millis() - shownAt));
  while (millis() - t0 < hold) {
    lv_timer_handler();
    delay(10);
  }
  lv_obj_clean(lv_scr_act());
  fill = nullptr;
  stageLabel = nullptr;
}
