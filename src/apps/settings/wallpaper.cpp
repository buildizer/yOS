// Settings › Display & Brightness › Wallpaper
#include "apps/settings/settings.h"
#include "os/os.h"
#include "os/ui.h"
#include "os/wallpaper.h"

namespace settings {

const char *wallpaperSummary() {
  static char buf[40];
  const String &n = wallpaper::current();
  snprintf(buf, sizeof(buf), "%s", n.isEmpty() ? "Default" : n.c_str());
  return buf;
}

namespace {
// Owned by the page: thumbnails live in PSRAM and are freed when the page is deleted.
struct Picker {
  std::vector<lv_img_dsc_t *> thumbs;
  std::vector<lv_obj_t *> tiles, checks;  // index 0 = built-in gradient
  std::vector<String> names;              // "" for the gradient
  std::function<void()> onChange;
};

void mark(Picker *pk) {
  const Palette &p = theme::p();
  const String &cur = wallpaper::current();
  for (size_t i = 0; i < pk->tiles.size(); i++) {
    bool on = pk->names[i] == cur;
    lv_obj_set_style_outline_width(pk->tiles[i], on ? 3 : 0, 0);
    lv_obj_set_style_outline_color(pk->tiles[i], p.accent, 0);
    if (on) lv_obj_clear_flag(pk->checks[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pk->checks[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void addCell(lv_obj_t *grid, Picker *pk, const String &name, lv_img_dsc_t *thumb) {
  const Palette &p = theme::p();
  lv_obj_t *cell = ui::box(grid);
  lv_obj_set_size(cell, 120, 128);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(cell, 6, 0);
  lv_obj_set_style_pad_top(cell, 6, 0);
  lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *tile = ui::box(cell);
  lv_obj_set_size(tile, wallpaper::THUMB, wallpaper::THUMB);
  lv_obj_set_style_radius(tile, 14, 0);
  lv_obj_set_style_clip_corner(tile, true, 0);
  lv_obj_set_style_outline_pad(tile, 3, 0);
  if (thumb) {
    lv_obj_t *im = lv_img_create(tile);
    lv_img_set_src(im, thumb);
  } else if (name.isEmpty()) {
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);  // the built-in gradient, from its palette colours
    lv_obj_set_style_bg_color(tile, p.wallpaperTop, 0);
    lv_obj_set_style_bg_grad_color(tile, p.wallpaperBottom, 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
  } else {
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);  // unreadable thumbnail
    lv_obj_set_style_bg_color(tile, p.separator, 0);
  }
  lv_obj_t *chk = ui::box(tile);
  lv_obj_add_flag(chk, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(chk, 26, 26);
  lv_obj_align(chk, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
  lv_obj_set_style_radius(chk, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(chk, p.accent, 0);
  lv_obj_set_style_bg_opa(chk, LV_OPA_COVER, 0);
  lv_obj_center(ui::label(chk, ICON_CHECK, theme::footnoteBold, lv_color_white()));

  lv_obj_t *cap = ui::label(cell, name.isEmpty() ? "Default" : name.c_str(), theme::caption, p.secondary);
  lv_obj_set_width(cap, 112);
  lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

  size_t idx = pk->tiles.size();
  pk->tiles.push_back(tile);
  pk->checks.push_back(chk);
  pk->names.push_back(name);
  lv_obj_add_event_cb(cell, [](lv_event_t *e) {
    Picker *pk = (Picker *)lv_event_get_user_data(e);
    lv_obj_t *cell = lv_event_get_target(e);
    for (size_t i = 0; i < pk->tiles.size(); i++) {
      if (lv_obj_get_parent(pk->tiles[i]) != cell) continue;
      if (pk->names[i] == wallpaper::current()) return;
      wallpaper::set(pk->names[i]);
      mark(pk);
      if (pk->onChange) pk->onChange();
      lv_async_call([](void *) { os::rebuildHome(); }, nullptr);  // rebuild once this event has finished
      return;
    }
  }, LV_EVENT_CLICKED, pk);
}
}

void wallpaperPage(lv_obj_t *appRoot, std::function<void()> onChange) {
  ui::push(appRoot, "Wallpaper", [onChange](lv_obj_t *c) {
    Picker *pk = new Picker();
    pk->onChange = onChange;
    lv_obj_add_event_cb(c, [](lv_event_t *e) {
      Picker *pk = (Picker *)lv_event_get_user_data(e);
      for (lv_img_dsc_t *t : pk->thumbs) wallpaper::freeThumb(t);
      delete pk;
    }, LV_EVENT_DELETE, pk);

    lv_obj_t *grid = ui::box(c);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 8, 0);

    addCell(grid, pk, "", nullptr);
    for (const wallpaper::Info &w : wallpaper::list()) {
      lv_img_dsc_t *t = wallpaper::loadThumb(w.name);
      if (t) pk->thumbs.push_back(t);
      addCell(grid, pk, w.name, t);
    }
    mark(pk);
    ui::note(c, "Add wallpapers on the web dashboard");
  });
}
}
