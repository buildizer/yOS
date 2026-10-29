#include "theme.h"

static const Palette LIGHT = {
  false,
  lv_color_hex(0xF2F2F7), lv_color_hex(0xFFFFFF), lv_color_hex(0xE5E5EA),
  lv_color_hex(0x000000), lv_color_hex(0x6C6C70), lv_color_hex(0xC4C4C7), lv_color_hex(0xC6C6C8),
  lv_color_hex(0x0066D6),
  lv_color_hex(0x248A3D), lv_color_hex(0xD70015), lv_color_hex(0xC93400), lv_color_hex(0xB25000),
  lv_color_hex(0x007AFF), lv_color_hex(0x8E8E93),
  lv_color_hex(0xDCE6F7), lv_color_hex(0xB9CBEB), lv_color_hex(0x1C1C1E),
};

static const Palette DARK = {
  true,
  lv_color_hex(0x000000), lv_color_hex(0x1C1C1E), lv_color_hex(0x2C2C2E),
  lv_color_hex(0xFFFFFF), lv_color_hex(0x98989F), lv_color_hex(0x5A5A5F), lv_color_hex(0x38383A),
  lv_color_hex(0x0A84FF),
  lv_color_hex(0x30D158), lv_color_hex(0xFF453A), lv_color_hex(0xFF9F0A), lv_color_hex(0xFFD60A),
  lv_color_hex(0x0A84FF), lv_color_hex(0x8E8E93),
  lv_color_hex(0x0B1220), lv_color_hex(0x1A2A4A), lv_color_hex(0xFFFFFF),
};

static const Palette *current = &DARK;
static void (*changeCb)() = nullptr;

namespace theme {
const lv_font_t *caption = &yos_font_13;
const lv_font_t *footnote = &yos_font_15;
const lv_font_t *footnoteBold = &yos_font_15_semibold;
const lv_font_t *body = &yos_font_17;
const lv_font_t *bodyBold = &yos_font_17_semibold;
const lv_font_t *title = &yos_font_22_semibold;
const lv_font_t *largeTitle = &yos_font_30_bold;
const lv_font_t *display = &yos_font_64;
const lv_font_t *big = &yos_font_88;
const lv_font_t *hero = &yos_font_110;
const lv_font_t *icon36 = &yos_icons_36;
const lv_font_t *icon72 = &yos_icons_72;

const Palette &p() { return *current; }
bool isDark() { return current->dark; }

void apply(bool dark) {
  current = dark ? &DARK : &LIGHT;
  lv_disp_t *disp = lv_disp_get_default();
  lv_theme_t *th = lv_theme_default_init(disp, current->accent, current->red, dark, body);
  lv_disp_set_theme(disp, th);
  if (changeCb) changeCb();
}

void onChange(void (*cb)()) { changeCb = cb; }
}
