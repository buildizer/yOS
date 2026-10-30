#pragma once
#include <lvgl.h>
#include <functional>
#include <Arduino.h>
#include "os/theme.h"

// Small UI kit in the iOS idiom: pages with large titles, inset grouped lists, rows, switches, alerts.
namespace ui {
static const int STATUS_H = 32;    // status bar height, overlaid on every screen
static const int SIDE = 16;        // content inset

using Builder = std::function<void(lv_obj_t *content)>;
using Action = std::function<void()>;

lv_obj_t *box(lv_obj_t *parent);  // a style-free container
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
uint32_t motion(uint32_t ms);      // animation length, shortened when Reduce Motion is on

// Page stack inside the open app. The first push is the app's root page.
lv_obj_t *push(lv_obj_t *appRoot, const char *title, Builder build);
bool pop();                        // false when already at the root page
void resetStack();
lv_obj_t *currentPage();

// Inset grouped list
lv_obj_t *group(lv_obj_t *content, const char *header = nullptr, const char *footer = nullptr);
lv_obj_t *note(lv_obj_t *content, const char *text);  // wrapping caption between groups
struct Row {
  lv_obj_t *obj;
  lv_obj_t *title;
  lv_obj_t *value;   // right-hand detail text (may be null)
  lv_obj_t *icon;    // leading glyph label (may be null)
};
Row row(lv_obj_t *group, const char *icon, lv_color_t iconBg, const char *title, const char *value = nullptr,
        Action onTap = nullptr, bool chevron = true);
void insetSeparator(lv_obj_t *row, int x);  // start a row's top hairline at x (content coords)
Row statusRow(lv_obj_t *group, const char *title);  // leading glyph without a tile, coloured by state
lv_obj_t *switchRow(lv_obj_t *group, const char *icon, lv_color_t iconBg, const char *title, bool on,
                    std::function<void(bool)> onChange);
Row actionRow(lv_obj_t *group, const char *title, lv_color_t color, Action onTap);  // centred text button row
lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t fill, Action onTap);

// Full-page text entry with the on-screen keyboard. onDone gets the text when the person taps Done.
void textInput(lv_obj_t *appRoot, const char *title, const char *placeholder, const char *initial, bool password,
               std::function<void(const String &)> onDone);

struct AlertAction {
  const char *label;
  enum Style { NORMAL, CANCEL, DESTRUCTIVE } style;
  Action run;
};
// Returns the alert's scrim (on lv_layer_top), so a caller can close it early with lv_obj_del().
lv_obj_t *alert(const char *title, const char *message, std::initializer_list<AlertAction> actions);
}
