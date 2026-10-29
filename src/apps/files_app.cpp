// Files: browse the SD card and the internal app store. Usage, search, browsing, clearing logs, formatting.
#include <SD.h>
#include <LittleFS.h>
#include <algorithm>
#include "apps/apps.h"
#include "os/os.h"
#include "os/ui.h"
#include "services/storage.h"

static void fmtBytes(char *buf, size_t len, uint64_t b) {
  if (b < 1024) snprintf(buf, len, "%llu B", b);
  else if (b < (1ULL << 20)) snprintf(buf, len, "%.1f KB", b / 1024.0);
  else if (b < (1ULL << 30)) snprintf(buf, len, "%.1f MB", b / 1048576.0);
  else snprintf(buf, len, "%.1f GB", b / 1073741824.0);
}

struct Entry {
  String name;
  bool dir;
  uint32_t size;
};

struct FolderStat {
  bool exists = false;
  int files = 0;
  uint64_t bytes = 0;
};

class FilesApp : public App {
public:
  const char *id() const override { return "files"; }
  const char *name() const override { return "Files"; }
  const char *icon() const override { return ICON_FOLDER; }
  lv_color_t tint() const override { return lv_color_hex(0x0A7AFF); }

  void open(lv_obj_t *root) override {
    appRoot = root;
    ui::push(root, "Browse", [this](lv_obj_t *c) { buildMain(c); });
  }
  void close() override { mainPage = nullptr; body = nullptr; jobLabel = nullptr; }

  void tick() override {
    if (!mainPage || ui::currentPage() != mainPage) return;
    JobState js = storageJobState();
    if (js != lastJob) {
      lastJob = js;
      if (js != JOB_RUNNING) refreshMain();   // a job finished: fresh numbers
    }
    if (js == JOB_RUNNING && jobLabel) {
      static const char *dots[] = {"", ".", "..", "..."};
      char buf[40];
      snprintf(buf, sizeof(buf), "%s%s", storageCurrentJob() == JOB_FORMAT ? "Formatting" : "Clearing logs",
               dots[(millis() / 400) % 4]);
      lv_label_set_text(jobLabel, buf);
    }
  }

private:
  lv_obj_t *appRoot = nullptr, *mainPage = nullptr, *body = nullptr, *jobLabel = nullptr;
  JobState lastJob = JOB_IDLE;

  void refreshMain() {
    if (!body) return;
    lv_obj_clean(body);
    fillMain(body);
  }

  void buildMain(lv_obj_t *c) {
    mainPage = c;
    lv_obj_set_style_bg_color(c, theme::p().background, 0);
    body = ui::box(c);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lastJob = storageJobState();
    fillMain(body);
  }

  static void sectionTitle(lv_obj_t *parent, const char *text) {
    lv_obj_t *l = ui::label(parent, text, theme::title, theme::p().label);
    lv_obj_set_style_pad_top(l, 6, 0);
  }

  static FolderStat folderStat(const char *path) {
    FolderStat st;
    storageWithLock([&] {
      File d = SD.open(path);
      if (!d || !d.isDirectory()) return;
      st.exists = true;
      for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (!f.isDirectory()) { st.files++; st.bytes += f.size(); }
        f.close();
      }
      d.close();
    });
    return st;
  }

  // One location row: icon, name, "used of total", usage bar, chevron. Tappable when onTap is set.
  void locationRow(lv_obj_t *g, const char *icon, lv_color_t tint, const char *name, const char *detail,
                   int permille, lv_color_t barColor, ui::Action onTap, lv_obj_t **detailOut = nullptr) {
    const Palette &p = theme::p();
    bool tappable = (bool)onTap;
    ui::Row r = ui::row(g, nullptr, tint, "", nullptr, std::move(onTap), false);
    // Keep the row chrome (tap handler, pressed state, separator is the first child when present) and
    // replace the title/value content.
    lv_obj_del(r.title);
    lv_obj_set_style_min_height(r.obj, 62, 0);
    lv_obj_set_style_pad_ver(r.obj, 12, 0);
    ui::label(r.obj, icon, theme::title, tint);
    lv_obj_t *col = ui::box(r.obj);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_t *top = ui::box(col);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui::label(top, name, theme::body, p.label);
    lv_obj_t *d = ui::label(top, detail, theme::footnote, p.secondary);
    if (detailOut) *detailOut = d;
    lv_obj_t *track = ui::box(col);
    lv_obj_set_size(track, LV_PCT(100), 6);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_bg_color(track, p.separator, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    if (permille > 0) {
      lv_obj_t *fill = ui::box(track);
      lv_obj_set_size(fill, LV_PCT(std::max(permille / 10, 3)), 6);
      lv_obj_set_style_radius(fill, 3, 0);
      lv_obj_set_style_bg_color(fill, barColor, 0);
      lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    }
    if (tappable) ui::label(r.obj, ICON_CHEVRON, theme::body, p.tertiary);
  }

  void fillMain(lv_obj_t *c) {
    const Palette &p = theme::p();
    jobLabel = nullptr;
    bool busy = storageJobState() == JOB_RUNNING;
    StorageInfo info = busy ? StorageInfo{false, 0, 0, 0, 0} : storageInfo();

    // Search field
    lv_obj_t *search = ui::box(c);
    lv_obj_set_size(search, LV_PCT(100), 40);
    lv_obj_set_style_radius(search, 10, 0);
    lv_obj_set_style_bg_color(search, p.card, 0);
    lv_obj_set_style_bg_opa(search, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(search, 12, 0);
    lv_obj_set_flex_flow(search, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui::label(search, "Search", theme::body, p.tertiary);
    if (info.ready) {
      lv_obj_add_flag(search, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(search, p.cardPressed, LV_STATE_PRESSED);
      lv_obj_add_event_cb(search, [](lv_event_t *e) { ((FilesApp *)lv_event_get_user_data(e))->askSearch(); },
                          LV_EVENT_CLICKED, this);
    }

    // Locations
    sectionTitle(c, "Locations");
    lv_obj_t *g = ui::group(c);
    if (busy) {
      locationRow(g, ICON_SD, p.orange, "SD Card", "", 0, p.orange, nullptr, &jobLabel);
    } else if (!info.ready) {
      locationRow(g, ICON_SD, p.gray, "SD Card", "No card", 0, p.gray, nullptr);
    } else {
      char used[16], total[16], line[48];
      fmtBytes(used, sizeof(used), info.used);
      fmtBytes(total, sizeof(total), info.total);
      snprintf(line, sizeof(line), "%s of %s", used, total);
      int pm = info.total ? (int)(1000 * info.used / info.total) : 0;
      locationRow(g, ICON_SD, p.blue, "SD Card", line, pm, info.used * 10 > info.total * 9 ? p.red : p.blue,
                  [this] { openDir("/", false, "SD Card"); });
    }
    {
      uint64_t iu = LittleFS.usedBytes(), it = LittleFS.totalBytes();
      char used[16], total[16], line[48];
      fmtBytes(used, sizeof(used), iu);
      fmtBytes(total, sizeof(total), it);
      snprintf(line, sizeof(line), "%s of %s", used, total);
      ui::Action go = it ? ui::Action([this] { openDir("/apps", true, "On This Device"); }) : ui::Action();
      locationRow(g, ICON_STORAGE, p.gray, "On This Device", it ? line : "Unavailable",
                  it ? (int)(1000 * iu / it) : 0, p.gray, go);
    }

    // Folders
    sectionTitle(c, "Folders");
    lv_obj_t *grid = ui::box(c);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, 10, 0);
    static const struct { const char *name, *path; } folders[] = {
        {"Netwatch", "/netwatch"}, {"Wallpapers", "/wallpapers"}, {"Apps", "/apps"}};
    for (auto &f : folders) {
      lv_obj_t *k = ui::box(grid);
      lv_obj_set_size(k, 1, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(k, 1);
      lv_obj_set_style_min_height(k, 96, 0);
      lv_obj_set_style_radius(k, 12, 0);
      lv_obj_set_style_bg_color(k, p.card, 0);
      lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
      lv_obj_set_style_pad_all(k, 12, 0);
      lv_obj_set_style_pad_row(k, 4, 0);
      lv_obj_set_flex_flow(k, LV_FLEX_FLOW_COLUMN);
      ui::label(k, ICON_FOLDER, theme::icon36, p.blue);
      lv_obj_t *n = ui::label(k, f.name, theme::footnoteBold, p.label);
      lv_obj_set_width(n, LV_PCT(100));
      lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
      char detail[40] = "\xE2\x80\x93";
      if (info.ready) {
        FolderStat st = folderStat(f.path);
        if (!st.exists || st.files == 0) strcpy(detail, "Empty");
        else {
          char sz[16];
          fmtBytes(sz, sizeof(sz), st.bytes);
          snprintf(detail, sizeof(detail), "%d file%s \xC2\xB7 %s", st.files, st.files == 1 ? "" : "s", sz);
        }
        String path = f.path, title = f.name;
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(k, p.cardPressed, LV_STATE_PRESSED);
        auto *fn = new ui::Action([this, path, title] { openDir(path, false, title.c_str()); });
        lv_obj_add_event_cb(k, [](lv_event_t *e) { (*(ui::Action *)lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, fn);
        lv_obj_add_event_cb(k, [](lv_event_t *e) { delete (ui::Action *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, fn);
      }
      lv_obj_t *d = ui::label(k, detail, theme::caption, p.secondary);
      lv_obj_set_width(d, LV_PCT(100));
      lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    }

    // Storage maintenance
    sectionTitle(c, "Storage");
    g = ui::group(c, nullptr, "Formatting erases everything on the card. You'll be asked to confirm.");
    if (info.ready) {
      ui::Row r = ui::row(g, nullptr, p.red, "Clear Netwatch Logs", nullptr, [] {
        ui::alert("Clear Netwatch logs?", "The outage log and daily latency files will be deleted.",
                  {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                   {"Clear", ui::AlertAction::DESTRUCTIVE, [] { storageStartJob(JOB_CLEAR_LOGS); }}});
      }, false);
      lv_obj_set_style_text_color(r.title, p.red, 0);
    }
    ui::Row r = ui::row(g, nullptr, p.red, "Format SD Card\xE2\x80\xA6", nullptr, [] {
      ui::alert("Format the SD card?", "Everything on the card will be erased. This can't be undone.",
                {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                 {"Format", ui::AlertAction::DESTRUCTIVE, [] { storageStartJob(JOB_FORMAT); }}});
    }, false);
    lv_obj_set_style_text_color(r.title, p.red, 0);
  }

  // ---- Browsing ----

  static std::vector<Entry> list(const String &path, bool internal) {
    std::vector<Entry> out;
    auto scan = [&](fs::FS &fsys) {
      File d = fsys.open(path);
      if (!d || !d.isDirectory()) return;
      for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        out.push_back({String(f.name()), f.isDirectory(), (uint32_t)f.size()});
        f.close();
      }
      d.close();
    };
    if (internal) scan(LittleFS);
    else storageWithLock([&] { scan(SD); });
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
      return a.dir != b.dir ? a.dir : a.name < b.name;  // folders first, then by name
    });
    return out;
  }

  void openDir(const String &path, bool internal, const char *titleOverride = nullptr) {
    String title = titleOverride ? String(titleOverride)
                 : path == "/" ? String("All Files") : path.substring(path.lastIndexOf('/') + 1);
    ui::push(appRoot, title.c_str(), [this, path, internal](lv_obj_t *c) {
      const Palette &p = theme::p();
      auto entries = list(path, internal);
      lv_obj_t *g = ui::group(c);
      for (const Entry &e : entries) {
        String full = (path == "/" ? "" : path) + "/" + e.name;
        char size[16];
        fmtBytes(size, sizeof(size), e.size);
        if (e.dir) ui::row(g, ICON_FOLDER, p.blue, e.name.c_str(), nullptr, [this, full, internal] { openDir(full, internal); });
        else ui::row(g, ICON_FILE, p.gray, e.name.c_str(), size, [this, full, e, internal] { openFile(full, e.size, internal); });
      }
      if (entries.empty()) ui::row(g, ICON_FOLDER, p.gray, "This folder is empty");
    });
  }

  void openFile(const String &path, uint32_t size, bool internal) {
    String name = path.substring(path.lastIndexOf('/') + 1);
    ui::push(appRoot, name.c_str(), [path, size, internal](lv_obj_t *c) {
      const Palette &p = theme::p();
      char buf[16];
      fmtBytes(buf, sizeof(buf), size);
      lv_obj_t *g = ui::group(c, nullptr, internal ? "Apps are removed from Settings \xE2\x80\xBA Apps."
                                                   : "Download it from the web dashboard's SD card page.");
      ui::row(g, nullptr, p.gray, "Size", buf);
      ui::row(g, nullptr, p.gray, "Location", path.substring(0, path.lastIndexOf('/') + 1).c_str());
      if (internal) return;   // the app store on internal flash is managed by the OS
      g = ui::group(c);
      ui::actionRow(g, "Delete File", p.red, [path] {
        ui::alert("Delete this file?", path.c_str(),
                  {{"Cancel", ui::AlertAction::CANCEL, nullptr},
                   {"Delete", ui::AlertAction::DESTRUCTIVE, [path] {
                      storageWithLock([&] { SD.remove(path); });
                      ui::pop();
                    }}});
      });
    });
  }

  // ---- Search ----

  struct Hit {
    String path;
    bool dir;
    uint32_t size;
  };

  // Bounded walk of the SD card (depth and entry budget) so the UI never stalls for long.
  static void walk(const String &path, const String &needle, int depth, int &budget, std::vector<Hit> &out) {
    std::vector<String> subdirs;
    File d = SD.open(path);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f && budget > 0; f = d.openNextFile()) {
      budget--;
      String n = f.name();
      String full = (path == "/" ? "" : path) + "/" + n;
      String lower = n;
      lower.toLowerCase();
      if (lower.indexOf(needle) >= 0 && out.size() < 60) out.push_back({full, f.isDirectory(), (uint32_t)f.size()});
      if (f.isDirectory() && depth > 0) subdirs.push_back(full);
      f.close();
    }
    d.close();
    for (const String &s : subdirs) walk(s, needle, depth - 1, budget, out);
  }

  void askSearch() {
    ui::textInput(appRoot, "Search", "File name", "", false, [this](const String &text) {
      String q = text;
      q.trim();
      q.toLowerCase();
      if (!q.length()) return;
      std::vector<Hit> hits;
      int budget = 600;
      storageWithLock([&] { walk("/", q, 4, budget, hits); });
      showHits(hits);
    });
  }

  void showHits(const std::vector<Hit> &hits) {
    ui::push(appRoot, "Results", [this, hits](lv_obj_t *c) {
      const Palette &p = theme::p();
      lv_obj_t *g = ui::group(c);
      for (const Hit &h : hits) {
        String name = h.path.substring(h.path.lastIndexOf('/') + 1);
        char size[16];
        fmtBytes(size, sizeof(size), h.size);
        String path = h.path;
        uint32_t sz = h.size;
        if (h.dir) ui::row(g, ICON_FOLDER, p.blue, name.c_str(), nullptr, [this, path] { openDir(path, false); });
        else ui::row(g, ICON_FILE, p.gray, name.c_str(), size, [this, path, sz] { openFile(path, sz, false); });
      }
      if (hits.empty()) ui::row(g, ICON_FILE, p.gray, "No files found");
    });
  }
};

App *createFilesApp() { return new FilesApp(); }
