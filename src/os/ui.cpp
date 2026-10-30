#include "ui.h"
#include <vector>
#include <Arduino.h>
#include "os/prefs.h"

namespace ui {

lv_obj_t *box(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  // Plain containers don't take touches, so a tap lands on the nearest object that opted in.
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

uint32_t motion(uint32_t ms) { return prefs::reduceMotion() ? ms / 3 : ms; }

// ---------- page stack ----------

struct PageEntry {
  lv_obj_t *page;
  String title;  // copied: the back button of the next page shows it
};
static std::vector<PageEntry> stack;

static void animX(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
static void animOpa(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, v, 0); }

static void slide(lv_obj_t *page, int from, int to, bool deleteAfter) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, page);
  if (prefs::reduceMotion()) {
    // Reduce Motion: cross-fade in place instead of sliding.
    lv_obj_set_x(page, 0);
    lv_anim_set_exec_cb(&a, animOpa);
    lv_anim_set_values(&a, deleteAfter ? 255 : 0, deleteAfter ? 0 : 255);
    lv_anim_set_time(&a, 120);
  } else {
    lv_anim_set_exec_cb(&a, animX);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  }
  if (deleteAfter) lv_anim_set_ready_cb(&a, [](lv_anim_t *an) { lv_obj_del_async((lv_obj_t *)an->var); });
  lv_anim_start(&a);
}

lv_obj_t *push(lv_obj_t *appRoot, const char *title, Builder build) {
  const Palette &p = theme::p();
  lv_obj_t *page = box(appRoot);
  lv_obj_set_size(page, 480, 480);
  lv_obj_set_style_bg_color(page, p.background, 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);  // clickable so drags anywhere scroll
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_top(page, STATUS_H, 0);
  lv_obj_set_style_pad_bottom(page, 36, 0);  // clear of the home indicator
  lv_obj_set_style_pad_hor(page, SIDE, 0);
  lv_obj_set_style_pad_row(page, 8, 0);

  // Navigation row: back button carries the previous page's title.
  if (!stack.empty()) {
    lv_obj_t *nav = box(page);
    lv_obj_set_size(nav, LV_PCT(100), 40);
    lv_obj_t *back = box(nav);
    lv_obj_set_size(back, LV_SIZE_CONTENT, 44);
    lv_obj_set_ext_click_area(back, 8);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(back, 4, 0);
    label(back, ICON_BACK, theme::body, p.accent);
    label(back, stack.back().title.c_str(), theme::body, p.accent);
    lv_obj_set_style_opa(back, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, [](lv_event_t *) { pop(); }, LV_EVENT_CLICKED, nullptr);
  }
  lv_obj_t *t = label(page, title, theme::largeTitle, p.label);
  lv_obj_set_style_pad_top(t, stack.empty() ? 14 : 0, 0);
  lv_obj_set_style_pad_bottom(t, 6, 0);

  build(page);

  if (!stack.empty()) slide(page, 480, 0, false);
  stack.push_back({page, title});
  return page;
}

bool pop() {
  if (stack.size() <= 1) return false;
  lv_obj_t *page = stack.back().page;
  stack.pop_back();
  slide(page, 0, 480, true);
  return true;
}

void resetStack() { stack.clear(); }
lv_obj_t *currentPage() { return stack.empty() ? nullptr : stack.back().page; }

// ---------- lists ----------

lv_obj_t *group(lv_obj_t *content, const char *header, const char *footer) {
  const Palette &p = theme::p();
  if (header) {
    lv_obj_t *h = label(content, header, theme::footnote, p.secondary);
    lv_obj_set_style_pad_left(h, SIDE, 0);
    lv_obj_set_style_pad_top(h, 10, 0);
  }
  lv_obj_t *g = box(content);
  lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g, p.card, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g, 12, 0);
  lv_obj_set_style_clip_corner(g, true, 0);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  if (footer) {
    lv_obj_t *f = label(content, footer, theme::caption, p.secondary);
    lv_obj_set_width(f, LV_PCT(100));
    lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(f, SIDE, 0);
  }
  return g;
}

static lv_obj_t *rowBase(lv_obj_t *group, bool tappable) {
  const Palette &p = theme::p();
  bool first = lv_obj_get_child_cnt(group) == 0;
  lv_obj_t *r = box(group);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(r, 50, 0);
  lv_obj_set_style_pad_hor(r, 14, 0);
  lv_obj_set_style_pad_ver(r, 8, 0);
  lv_obj_set_style_pad_column(r, 12, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (!first) {
    // Separator inset to the text, drawn as a floating hairline (moved past the icon in insetSeparator()).
    lv_obj_t *sep = box(r);
    lv_obj_add_flag(sep, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_pos(sep, 0, -8);  // row has 8px vertical padding
    lv_obj_set_style_bg_color(sep, p.separator, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_user_data(r, sep);
  }
  if (tappable) {
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, p.cardPressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  }
  return r;
}

// With a leading icon the hairline starts under the title, like iOS.
void insetSeparator(lv_obj_t *row, int x) {
  lv_obj_t *sep = (lv_obj_t *)lv_obj_get_user_data(row);
  if (!sep) return;
  lv_obj_set_x(sep, x);
  lv_obj_set_width(sep, 480);  // clipped by the group's rounded corners
}

static lv_obj_t *iconTile(lv_obj_t *parent, const char *icon, lv_color_t bg) {
  lv_obj_t *tile = box(parent);
  lv_obj_set_size(tile, 30, 30);
  lv_obj_set_style_radius(tile, 7, 0);
  lv_obj_set_style_bg_color(tile, bg, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_t *g = label(tile, icon, theme::body, lv_color_white());
  lv_obj_center(g);
  return g;
}

static void attachTap(lv_obj_t *obj, Action onTap) {
  if (!onTap) return;
  auto *fn = new Action(std::move(onTap));
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { (*(Action *)lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, fn);
  lv_obj_add_event_cb(obj, [](lv_event_t *e) { delete (Action *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, fn);
}

Row row(lv_obj_t *group, const char *icon, lv_color_t iconBg, const char *title, const char *value, Action onTap,
        bool chevron) {
  const Palette &p = theme::p();
  Row r{};
  r.obj = rowBase(group, onTap != nullptr);
  if (icon) {
    r.icon = iconTile(r.obj, icon, iconBg);
    insetSeparator(r.obj, 42);
  }
  r.title = label(r.obj, title, theme::body, p.label);
  lv_obj_set_flex_grow(r.title, 1);
  lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
  if (value) r.value = label(r.obj, value, theme::body, p.secondary);
  if (onTap && chevron) label(r.obj, ICON_CHEVRON, theme::body, p.tertiary);
  attachTap(r.obj, std::move(onTap));
  return r;
}

lv_obj_t *note(lv_obj_t *content, const char *text) {
  lv_obj_t *f = label(content, text, theme::caption, theme::p().secondary);
  lv_obj_set_width(f, LV_PCT(100));
  lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(f, SIDE, 0);
  return f;
}

Row statusRow(lv_obj_t *group, const char *title) {
  const Palette &p = theme::p();
  Row r{};
  r.obj = rowBase(group, false);
  r.icon = label(r.obj, ICON_OK, theme::title, p.green);
  r.title = label(r.obj, title, theme::body, p.label);
  lv_obj_set_flex_grow(r.title, 1);
  r.value = label(r.obj, "", theme::body, p.secondary);
  return r;
}

lv_obj_t *switchRow(lv_obj_t *group, const char *icon, lv_color_t iconBg, const char *title, bool on,
                    std::function<void(bool)> onChange) {
  const Palette &p = theme::p();
  lv_obj_t *r = rowBase(group, false);
  if (icon) {
    iconTile(r, icon, iconBg);
    insetSeparator(r, 42);
  }
  lv_obj_t *t = label(r, title, theme::body, p.label);
  lv_obj_set_flex_grow(t, 1);
  lv_obj_t *sw = lv_switch_create(r);
  lv_obj_set_size(sw, 51, 31);
  lv_obj_set_style_bg_color(sw, p.dark ? lv_color_hex(0x39393D) : lv_color_hex(0xE9E9EA), 0);
  lv_obj_set_style_bg_color(sw, p.green, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sw, -2, LV_PART_KNOB);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  auto *fn = new std::function<void(bool)>(std::move(onChange));
  lv_obj_add_event_cb(sw, [](lv_event_t *e) {
    auto *f = (std::function<void(bool)> *)lv_event_get_user_data(e);
    (*f)(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
  }, LV_EVENT_VALUE_CHANGED, fn);
  lv_obj_add_event_cb(sw, [](lv_event_t *e) { delete (std::function<void(bool)> *)lv_event_get_user_data(e); },
                      LV_EVENT_DELETE, fn);
  return sw;
}

Row actionRow(lv_obj_t *group, const char *title, lv_color_t color, Action onTap) {
  Row r{};
  r.obj = rowBase(group, true);
  lv_obj_set_flex_align(r.obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  r.title = label(r.obj, title, theme::body, color);
  attachTap(r.obj, std::move(onTap));
  return r;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t fill, Action onTap) {
  lv_obj_t *b = box(parent);
  lv_obj_set_size(b, LV_PCT(100), 50);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, fill, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_t *l = label(b, text, theme::bodyBold, lv_color_white());
  lv_obj_center(l);
  attachTap(b, std::move(onTap));
  return b;
}

// ---------- alert ----------

lv_obj_t *alert(const char *title, const char *message, std::initializer_list<AlertAction> actions) {
  const Palette &p = theme::p();
  lv_obj_t *scrim = box(lv_layer_top());
  lv_obj_set_size(scrim, 480, 480);
  lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // blocks touches behind the alert

  lv_obj_t *card = box(scrim);
  lv_obj_set_width(card, 300);
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_bg_color(card, p.dark ? lv_color_hex(0x2C2C2E) : lv_color_hex(0xF2F2F2), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(card, true, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *text = box(card);
  lv_obj_set_size(text, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(text, 18, 0);
  lv_obj_set_style_pad_row(text, 4, 0);
  lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *t = label(text, title, theme::bodyBold, p.label);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  if (message) {
    lv_obj_t *m = label(text, message, theme::footnote, p.label);
    lv_obj_set_width(m, LV_PCT(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
  }

  // Two actions sit side by side, more stack vertically.
  bool sideBySide = actions.size() == 2;
  lv_obj_t *bar = box(card);
  lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bar, sideBySide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  for (const AlertAction &a : actions) {
    lv_obj_t *b = box(bar);
    lv_obj_set_height(b, 48);
    if (sideBySide) lv_obj_set_flex_grow(b, 1);
    else lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_border_side(b, sideBySide && &a != actions.begin()
                                        ? LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, p.separator, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, p.cardPressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_color_t c = a.style == AlertAction::DESTRUCTIVE ? p.red : p.accent;
    lv_obj_t *l = label(b, a.label, a.style == AlertAction::CANCEL ? theme::bodyBold : theme::body, c);
    lv_obj_center(l);
    auto *fn = new std::pair<Action, lv_obj_t *>(a.run, scrim);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      auto *f = (std::pair<Action, lv_obj_t *> *)lv_event_get_user_data(e);
      Action run = f->first;
      lv_obj_del_async(f->second);
      if (run) run();
    }, LV_EVENT_CLICKED, fn);
    lv_obj_add_event_cb(b, [](lv_event_t *e) { delete (std::pair<Action, lv_obj_t *> *)lv_event_get_user_data(e); },
                        LV_EVENT_DELETE, fn);
  }
  return scrim;
}
}

// ---------- text input ----------

namespace ui {
void textInput(lv_obj_t *appRoot, const char *title, const char *placeholder, const char *initial, bool password,
               std::function<void(const String &)> onDone) {
  auto *done = new std::function<void(const String &)>(std::move(onDone));
  lv_obj_t *page = push(appRoot, title, [&](lv_obj_t *c) {
    const Palette &p = theme::p();
    lv_obj_t *ta = lv_textarea_create(c);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, initial ? initial : "");
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_style_text_font(ta, theme::body, 0);
    lv_obj_set_style_bg_color(ta, p.card, 0);
    lv_obj_set_style_text_color(ta, p.label, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_pad_all(ta, 14, 0);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);  // shows the cursor

    // The keyboard is a floating child of the page (deleted with it), pinned to the bottom half.
    lv_obj_t *kb = lv_keyboard_create(c);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(kb, 480, 230);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 36);  // 36 = the page's bottom padding
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_style_text_font(kb, theme::footnoteBold, 0);
    lv_obj_set_style_bg_color(kb, p.dark ? lv_color_hex(0x1C1C1E) : lv_color_hex(0xD1D3D9), 0);
    lv_obj_set_style_bg_color(kb, p.dark ? lv_color_hex(0x48484A) : lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, p.label, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, [](lv_event_t *e) {
      lv_obj_t *kb = lv_event_get_target(e);
      auto *fn = (std::function<void(const String &)> *)lv_event_get_user_data(e);
      String text = lv_textarea_get_text(lv_keyboard_get_textarea(kb));
      pop();
      (*fn)(text);
    }, LV_EVENT_READY, done);
    lv_obj_add_event_cb(kb, [](lv_event_t *) { pop(); }, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(kb, [](lv_event_t *e) { delete (std::function<void(const String &)> *)lv_event_get_user_data(e); },
                        LV_EVENT_DELETE, done);
  });
  (void)page;
}
}
