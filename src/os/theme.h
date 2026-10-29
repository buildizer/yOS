#pragma once
#include <lvgl.h>

// Semantic colours, iOS-style. Every pair used for text is at least 4.5:1 against its surface.
struct Palette {
  bool dark;
  lv_color_t background;     // grouped page background
  lv_color_t card;           // inset group / row surface
  lv_color_t cardPressed;
  lv_color_t label;          // primary text
  lv_color_t secondary;      // secondary text
  lv_color_t tertiary;       // chevrons, placeholders (never body text)
  lv_color_t separator;
  lv_color_t accent;         // tint for links and selected states, readable as text
  lv_color_t green, red, orange, yellow, blue, gray;
  lv_color_t wallpaperTop, wallpaperBottom, homeLabel;
};

namespace theme {
const Palette &p();
bool isDark();
void apply(bool dark);       // re-themes LVGL and fires the change hook
void onChange(void (*cb)());

extern const lv_font_t *caption;     // 13
extern const lv_font_t *footnote;    // 15
extern const lv_font_t *footnoteBold;
extern const lv_font_t *body;        // 17
extern const lv_font_t *bodyBold;
extern const lv_font_t *title;       // 22 semibold
extern const lv_font_t *largeTitle;  // 30 bold
extern const lv_font_t *display;     // 64 numerals
extern const lv_font_t *big;         // 88 numerals (+ AM/PM letters, degree)
extern const lv_font_t *hero;        // 110 numerals
extern const lv_font_t *icon36;
extern const lv_font_t *icon72;
}

// Lucide icons (the set shadcn/ui uses, ISC licence), generated into every text font by tools/gen_fonts.sh.
#define ICON_NETWATCH "\xEE\x80\xB8"        // activity e038
#define ICON_CLOCK "\xEE\x82\x8B"           // clock e08b
#define ICON_SUN "\xEE\x85\xBC"             // sun e17c
#define ICON_CLOUD "\xEE\x82\x8C"           // cloud e08c
#define ICON_PARTLY "\xEE\x88\x96"          // cloud-sun e216
#define ICON_STORM "\xEE\x82\x90"           // cloud-lightning e090
#define ICON_SNOW "\xEE\x85\xA9"            // snowflake e169
#define ICON_RAIN "\xEE\x82\x92"            // cloud-rain e092
#define ICON_FOG "\xEE\x88\x94"             // cloud-fog e214
#define ICON_MOON "\xEE\x84\xA2"            // moon e122
#define ICON_WIND "\xEE\x86\xB0"            // wind e1b0
#define ICON_FOLDER "\xEE\x83\x9C"          // folder e0dc
#define ICON_SD "\xEE\x83\xB0"              // hard-drive e0f0
#define ICON_FILE "\xEE\x83\x90"            // file-text e0d0
#define ICON_DELETE "\xEE\x86\x8E"          // trash-2 e18e
#define ICON_SETTINGS "\xEE\x85\x98"        // settings e158
#define ICON_WIFI "\xEE\x86\xAE"            // wifi e1ae
#define ICON_WIFI_OFF "\xEE\x86\xAF"        // wifi-off e1af
#define ICON_LIGHT "\xEE\x8A\xB1"           // sun-medium e2b1
#define ICON_DARK "\xEE\x90\x95"            // moon-star e415
#define ICON_APPS "\xEE\x84\x85"            // layout-grid e105
#define ICON_INFO "\xEE\x83\xBE"            // info e0fe
#define ICON_ROTATE "\xEE\x85\x8D"              // rotate-cw e14d
#define ICON_RESTART "\xEE\x85\x8C"         // rotate-ccw e14c
#define ICON_OK "\xEE\x88\xA6"              // circle-check e226
#define ICON_ERROR "\xEE\x81\xBB"           // circle-alert e07b
#define ICON_CHEVRON "\xEE\x81\xB3"         // chevron-right e073
#define ICON_BACK "\xEE\x81\xB2"            // chevron-left e072
#define ICON_PLAY "\xEE\x85\x80"            // play e140
#define ICON_STOP "\xEE\x85\xAB"            // square e16b
#define ICON_OPEN "\xEE\x81\x91"            // arrow-up-right e051
#define ICON_ROUTER "\xEE\x8F\x83"          // router e3c3
#define ICON_DNS "\xEE\x85\x97"             // server e157
#define ICON_SHIELD "\xEE\x85\x9C"          // shield e15c
#define ICON_STORAGE "\xEE\x82\xB1"         // database e0b1
#define ICON_PAUSE "\xEE\x84\xB2"           // pause e132
#define ICON_POWER "\xEE\x85\x84"           // power e144
#define ICON_BRIGHTNESS "\xEE\x8A\x99"      // sun-dim e299
#define ICON_MOTION "\xEE\x90\x97"          // sparkles e417
#define ICON_THERMO "\xEE\x86\x86"          // thermometer e186
#define ICON_GRAIN "\xEE\x82\xB8"           // droplet e0b8
#define ICON_UMBRELLA "\xEE\x86\x99"        // umbrella e199
#define ICON_CLOUDY "\xEE\x82\x8C"          // cloud e08c
#define ICON_BLUETOOTH "\xEE\x81\xA0"       // bluetooth e060
#define ICON_BT_CONNECTED "\xEE\x86\xB8"    // bluetooth-connected e1b8
#define ICON_BT_SEARCHING "\xEE\x86\xBA"    // bluetooth-searching e1ba
#define ICON_LOCK "\xEE\x84\x8F"            // lock e10f
#define ICON_GLOBE "\xEE\x83\xAB"           // globe e0eb
#define ICON_LOCATION "\xEE\x84\x95"        // map-pin e115
#define ICON_EXTENSION "\xEE\x8A\x9C"       // puzzle e29c
#define ICON_INSTALL "\xEE\x82\xB6"         // download e0b6
#define ICON_REFRESH "\xEE\x85\x89"         // refresh-cw e149
#define ICON_CHECK "\xEE\x81\xB0"           // check e070
#define ICON_ADD "\xEE\x85\x81"             // plus e141
#define ICON_KEYBOARD "\xEE\x8A\x84"        // keyboard e284
#define ICON_BACKSPACE "\xEE\x82\xB2"       // delete e0b2
#define ICON_DROPLET "\xEE\x82\xB8"         // droplet e0b8
#define ICON_GAUGE "\xEE\x86\xBF"           // gauge e1bf
#define ICON_MONITOR "\xEE\x84\xA1"         // monitor e121
