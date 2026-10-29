// Settings › General › About › Software Update: updates from GitHub Releases, and the GitHub sign-in.
#include "apps/settings/settings.h"
#include "os/prefs.h"
#include "os/ui.h"
#include "services/github_update.h"
#include "version.h"

namespace settings {
namespace {

struct Ctx {
  lv_obj_t *holder, *appRoot;
  uint32_t shown;
};

void signInPage(lv_obj_t *appRoot);

void formatChecked(char *buf, size_t len, time_t t) {
  struct tm tm;
  if (t < 1000000000 || !localtime_r(&t, &tm)) { strlcpy(buf, "Not checked yet", len); return; }
  char when[24];
  strftime(when, sizeof(when), prefs::use24h() ? "%e %b at %H:%M" : "%e %b at %l:%M %p", &tm);
  snprintf(buf, len, "Checked %s", when[0] == ' ' ? when + 1 : when);
}

lv_obj_t *wrapLabel(lv_obj_t *group, const char *text) {
  lv_obj_t *box = ui::box(group);
  lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(box, 16, 0);
  lv_obj_set_style_pad_ver(box, 10, 0);
  lv_obj_t *l = ui::label(box, text, theme::footnote, theme::p().secondary);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

void build(Ctx *x) {
  using namespace ghupdate;
  const Palette &p = theme::p();
  lv_obj_t *h = x->holder, *appRoot = x->appRoot;
  lv_obj_clean(h);
  Status s = status();
  char checked[40], title[64], size[24];
  formatChecked(checked, sizeof(checked), s.lastCheck);
  snprintf(size, sizeof(size), "%.1f MB", s.size / 1048576.0f);

  const char *icon = ICON_INFO, *value = nullptr, *footer = nullptr, *action = nullptr;
  lv_color_t tint = p.gray;
  ui::Action onAction = nullptr;
  snprintf(title, sizeof(title), "yOS " YOS_FIRMWARE_VERSION);
  String footerText;
  switch (s.state) {
    case NOT_CONFIGURED:
      value = "Installed";
      footer = "Updates from GitHub aren't set up in this build (GITHUB_APP_CLIENT_ID in config.h).";
      break;
    case SIGNED_OUT:
      value = "Installed";
      footerText = s.error.length() ? s.error : String("Sign in once to get updates from the yOS repo on GitHub.");
      action = "Sign in to GitHub";
      onAction = [appRoot] { ghupdate::signIn(); signInPage(appRoot); };
      break;
    case SIGNING_IN:
      value = "Installed";
      action = "Continue Signing In";
      onAction = [appRoot] { signInPage(appRoot); };
      break;
    case IDLE:
      icon = ICON_CHECK, tint = p.green, value = checked;
      action = "Check for Update";
      onAction = [] { ghupdate::check(); };
      break;
    case CHECKING:
    case INSTALLING:
      icon = ICON_REFRESH, tint = p.gray;
      snprintf(title, sizeof(title), s.state == CHECKING ? "Checking for Update\xE2\x80\xA6" : "Installing\xE2\x80\xA6");
      action = s.state == CHECKING ? "Checking\xE2\x80\xA6" : "Installing\xE2\x80\xA6";
      break;
    case UP_TO_DATE:
      icon = ICON_CHECK, tint = p.green, value = checked;
      snprintf(title, sizeof(title), "yOS " YOS_FIRMWARE_VERSION " is up to date");
      action = "Check for Update";
      onAction = [] { ghupdate::check(); };
      break;
    case AVAILABLE:
      icon = ICON_INSTALL, tint = p.blue, value = size;
      snprintf(title, sizeof(title), "yOS %s available", s.latest.c_str());
      action = "Download and Install";
      onAction = [latest = s.latest] {
        static String msg;
        msg = "yOS " + latest + " downloads over Wi-Fi and the board restarts when it's done. Keep it powered on.";
        ui::alert("Install Update?", msg.c_str(),
                  {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                   {"Install", ui::AlertAction::NORMAL, [] { ghupdate::install(); }}});
      };
      break;
    case FAILED:
      icon = ICON_ERROR, tint = p.red;
      snprintf(title, sizeof(title), "Update check failed");
      footerText = s.error;
      action = "Try Again";
      onAction = [] { ghupdate::check(); };
      break;
  }
  if (footerText.length()) footer = footerText.c_str();

  lv_obj_t *g = ui::group(h, "Software Update", footer);
  ui::row(g, icon, tint, title, value, nullptr, false);
  if (s.state == AVAILABLE && s.notes.length()) wrapLabel(g, s.notes.c_str());
  if (action) {
    ui::Row r = ui::actionRow(g, action, onAction ? p.accent : p.tertiary, onAction);
    if (s.state == AVAILABLE) lv_obj_set_style_text_font(r.title, theme::bodyBold, 0);
  }

  if (s.state == NOT_CONFIGURED) return;
  g = ui::group(h, nullptr, "Checks run once a day. Wi-Fi updates from a Mac use the Update Password.");
  ui::switchRow(g, ICON_REFRESH, p.blue, "Check Automatically", ghupdate::autoCheck(),
                [](bool on) { ghupdate::setAutoCheck(on); });
  bool signedIn = s.signedIn;
  ui::row(g, ICON_GLOBE, p.gray, "GitHub Account", signedIn ? (s.login.length() ? s.login.c_str() : "Signed in") : "Not signed in",
          [signedIn, appRoot] {
            if (!signedIn) {
              ghupdate::signIn();
              signInPage(appRoot);
              return;
            }
            ui::alert("Sign Out of GitHub?", "The board stops checking for updates until you sign in again.",
                      {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                       {"Sign Out", ui::AlertAction::DESTRUCTIVE, [] { ghupdate::signOut(); }}});
          });
  ui::row(g, ICON_LOCK, p.gray, "Update Password", prefs::otaPassword().c_str(), [appRoot, x] {
    ui::textInput(appRoot, "Update Password", "At least 8 characters", prefs::otaPassword().c_str(), false,
                  [x](const String &text) {
      String pw = text;
      pw.trim();
      if (pw.length() < 8) {
        ui::alert("Password Too Short", "Use at least 8 characters.", {{"OK", ui::AlertAction::CANCEL, nullptr}});
        return;
      }
      prefs::setOtaPassword(pw);
      x->shown--;  // redraw so the row shows the new password
    });
  });
}

// GitHub device flow: the board shows a code; the person approves it on github.com/login/device.
void signInPage(lv_obj_t *appRoot) {
  ui::push(appRoot, "Sign in to GitHub", [](lv_obj_t *c) {
    const Palette &p = theme::p();
    lv_obj_t *intro = ui::label(c, "On your phone or computer, open github.com/login/device and enter this code:",
                                theme::footnote, p.secondary);
    lv_obj_set_width(intro, LV_PCT(100));
    lv_label_set_long_mode(intro, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(intro, ui::SIDE, 0);

    lv_obj_t *row = ui::box(c);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_t *qr = lv_qrcode_create(row, 150, lv_color_black(), lv_color_white());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);  // quiet zone, so phones read it on a dark page
    lv_obj_t *code = ui::label(row, "\xE2\x80\xA6", theme::largeTitle, p.label);

    lv_obj_t *statusLabel = ui::label(c, "Getting a code from GitHub\xE2\x80\xA6", theme::footnote, p.secondary);
    lv_obj_set_width(statusLabel, LV_PCT(100));
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(statusLabel, ui::SIDE, 0);
    ui::button(c, "Cancel", p.gray, [] { ui::pop(); });

    struct Page {
      lv_obj_t *page, *qr, *code, *status;
      String shownCode;
      bool done;
    };
    auto *pg = new Page{c, qr, code, statusLabel, String(), false};
    lv_timer_t *t = lv_timer_create([](lv_timer_t *t) {
      auto *pg = (Page *)t->user_data;
      if (pg->done) return;
      if (ui::currentPage() != pg->page) return;  // already leaving (Cancel or Back): never pop the page below
      ghupdate::Status s = ghupdate::status();
      if (s.signedIn && s.state != ghupdate::SIGNING_IN) {
        pg->done = true;  // signed in: back to About, which shows the account and the first check's result
        ui::pop();
        return;
      }
      if (s.state != ghupdate::SIGNING_IN) {
        lv_label_set_text(pg->status, s.error.length() ? s.error.c_str() : "Signing in stopped.");
        lv_obj_set_style_text_color(pg->status, theme::p().red, 0);
        return;
      }
      lv_obj_set_style_text_color(pg->status, theme::p().secondary, 0);
      if (s.userCode.length() && s.userCode != pg->shownCode) {
        pg->shownCode = s.userCode;
        lv_label_set_text(pg->code, s.userCode.c_str());
        lv_qrcode_update(pg->qr, s.verifyUri.c_str(), s.verifyUri.length());
      }
      if (pg->shownCode.length()) {
        int32_t left = (int32_t)(s.codeExpiresMs - millis()) / 1000;
        char buf[64];
        snprintf(buf, sizeof(buf), "Waiting for you to approve\xE2\x80\xA6 The code works for %ld:%02ld more.",
                 (long)max<int32_t>(left, 0) / 60, (long)max<int32_t>(left, 0) % 60);
        lv_label_set_text(pg->status, buf);
      }
    }, 500, pg);
    struct Owner { lv_timer_t *t; Page *pg; };
    lv_obj_add_event_cb(c, [](lv_event_t *e) {
      auto *o = (Owner *)lv_event_get_user_data(e);
      lv_timer_del(o->t);
      if (!o->pg->done && ghupdate::status().state == ghupdate::SIGNING_IN) ghupdate::cancelSignIn();
      delete o->pg;
      delete o;
    }, LV_EVENT_DELETE, new Owner{t, pg});
  });
}

}  // namespace

void softwareUpdateSection(lv_obj_t *content, lv_obj_t *appRoot) {
  lv_obj_t *holder = ui::box(content);
  lv_obj_set_size(holder, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(holder, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(holder, 8, 0);  // same rhythm as the page
  auto *x = new Ctx{holder, appRoot, ghupdate::version()};
  build(x);
  // Rebuild whenever the update service changes state (check finished, signed in, ...).
  lv_timer_t *t = lv_timer_create([](lv_timer_t *t) {
    auto *x = (Ctx *)t->user_data;
    uint32_t v = ghupdate::version();
    if (v == x->shown) return;
    x->shown = v;
    build(x);
  }, 300, x);
  lv_obj_add_event_cb(holder, [](lv_event_t *e) {
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    delete (Ctx *)t->user_data;
    lv_timer_del(t);
  }, LV_EVENT_DELETE, t);
}

}  // namespace settings
