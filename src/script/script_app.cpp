// ScriptApp: runs an installed app's main.lua in its own sandboxed Lua state while the app is open,
// and binds the small yOS API (ui, net, json, store, sys) documented in src/script/README.md.
//
// Every entry into Lua (main.lua, tap handlers, alert/input callbacks, tick) goes through
// script::protectedRun, so a script error or an out-of-memory error never reaches LVGL: the app
// shows "This app stopped" with the message instead. Lua is compiled as C++, so errors raised in
// the bindings below unwind C++ objects properly.
//
// API (full description in README.md):
//   ui.page(title, fn(page))            ui.group(page, header?, footer?) -> group
//   ui.row(group, {icon=, title=, value=, on_tap=fn}) -> row (row:set_value(text))
//   ui.button(page, text, fn) -> h      ui.label(page, text) -> label (label:set_text(text))
//   ui.alert(title, message?, {"Cancel","OK"}?, fn(index)?)      ui.input(title, placeholder?, fn(text))
//   net.get(url, fn(status, body | nil, err))   json.decode(text) -> value | nil, err
//   store.get(key) / store.set(key, value)      sys.time(), sys.millis()     global tick() at ~4 Hz
#include "script/script_app.h"
#include <atomic>
#include <new>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "os/ui.h"
#include "script/sandbox.h"

namespace script {

static const size_t MAIN_MAX = 256 * 1024;    // largest main.lua
static const size_t BODY_MAX = 64 * 1024;     // largest net.get body
static const size_t STORE_MAX = 16 * 1024;    // store.json
static const uint16_t NET_TIMEOUT_MS = 4000;
static const uint32_t NET_STACK = 6144;       // bytes of internal RAM for one request's worker task
static const int MAX_OBJECTS = 300;           // pages, groups, rows, buttons and labels per app session
static const size_t TEXT_MAX = 2048;          // longest label/row/button/alert text a script may set
static const size_t PRINT_MAX = 200;          // print() output is cut to this many bytes
static const uint32_t STORE_FLUSH_MS = 5000;  // store.json is written at most this often
static const uint32_t LUA_SLICE_MS = 300;     // longest a tap, alert/input callback or tick may run
static const uint32_t MAIN_SLICE_MS = 1000;   // main.lua builds the first pages, so it gets longer

// One net.get request. The worker task fills it in and sets done last; the UI task reads it after
// seeing done (from tick). The worker never touches Lua or LVGL.
struct NetJob {
  std::atomic<bool> done{false};
  int code = 0;  // HTTP status, or <= 0 with error set
  std::string body, error;
  String url;
};
static std::atomic<int> netWorkers{0};  // running worker tasks, at most 1 in total
struct LuaFn;

// One open app: its Lua state and the UI it owns. Budget comes first so the sandbox's hook and
// allocator find it through the state's extra space.
struct Session : Budget, std::enable_shared_from_this<Session> {
  lua_State *L = nullptr;
  ScriptApp *app = nullptr;
  lv_obj_t *appRoot = nullptr;
  lv_obj_t *building = nullptr;  // page whose builder is running (not on the page stack yet)
  bool alive = true;             // false once the app is closed; late callbacks are ignored
  bool failed = false;           // the app stopped with an error; no more callbacks run
  int objects = 0;               // UI objects created so far, capped at MAX_OBJECTS
  JsonDocument store;
  bool storeLoaded = false;
  bool storeDirty = false;       // store.json is older than the in-memory store
  uint32_t storeFlushed = 0;     // millis() of the last write
  std::shared_ptr<NetJob> job;   // the pending net.get, if any
  std::shared_ptr<LuaFn> netCb;  // its callback
};

static Session *sessionOf(lua_State *L) { return static_cast<Session *>(budgetOf(L)); }

// ---------- errors ----------

static void fail(Session *s, const std::string &err) {
  if (s->failed) return;
  s->failed = true;
  Serial.printf("[app %s] stopped: %s\n", s->app->id(), err.c_str());
  lv_obj_t *page = s->building ? s->building : ui::currentPage();
  if (!page) {
    ui::push(s->appRoot, s->app->name(), [](lv_obj_t *) {});
    page = ui::currentPage();
  }
  std::string msg = "This app stopped: " + err;
  lv_obj_t *g = ui::group(page, nullptr, msg.c_str());
  ui::row(g, ICON_ERROR, theme::p().red, "This app stopped");
  lv_obj_update_layout(page);
  lv_obj_scroll_to_y(page, lv_obj_get_scroll_y(page) + lv_obj_get_scroll_bottom(page), LV_ANIM_ON);
}

// ---------- calling into Lua ----------

// A Lua function kept alive by a C++ callback (a row's tap handler, an alert's completion...).
struct LuaFn {
  std::shared_ptr<Session> s;
  int ref;
  ~LuaFn() {
    if (s->alive && s->L) luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
  }
};
using LuaFnPtr = std::shared_ptr<LuaFn>;

static LuaFnPtr keepFunction(lua_State *L, int idx) {
  if (lua_isnoneornil(L, idx)) return nullptr;
  luaL_checktype(L, idx, LUA_TFUNCTION);
  lua_pushvalue(L, idx);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  return LuaFnPtr(new LuaFn{sessionOf(L)->shared_from_this(), ref});
}

enum ArgKind { ARG_NONE, ARG_STRING, ARG_INT, ARG_PAGE, ARG_NET };
struct CallCtx {
  int ref;  // LUA_NOREF: the global tick()
  ArgKind kind;
  const char *str;
  size_t len;
  lua_Integer n;
  lv_obj_t *page;
};

static void pushHandle(lua_State *L, int kind, lv_obj_t *box, lv_obj_t *text);
enum { H_PAGE = 1, H_GROUP, H_ROW, H_LABEL };

static int trampoline(lua_State *L) {
  CallCtx *c = (CallCtx *)lua_touserdata(L, 1);
  if (c->ref == LUA_NOREF) lua_getglobal(L, "tick");
  else lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref);
  if (!lua_isfunction(L, -1)) return 0;
  int nargs = 1;
  switch (c->kind) {
    case ARG_STRING: lua_pushlstring(L, c->str, c->len); break;
    case ARG_INT: lua_pushinteger(L, c->n); break;
    case ARG_PAGE: pushHandle(L, H_PAGE, c->page, nullptr); break;
    case ARG_NET:  // (status, body) or (nil, message)
      if (c->n > 0) lua_pushinteger(L, c->n);
      else lua_pushnil(L);
      lua_pushlstring(L, c->str, c->len);
      nargs = 2;
      break;
    default: nargs = 0;
  }
  lua_call(L, nargs, 0);
  return 0;
}

static void invoke(Session *s, CallCtx c) {
  if (!s->alive || s->failed) return;
  std::string err;
  if (!protectedRun(s->L, trampoline, &c, err)) fail(s, err);
}

static void invoke(const LuaFnPtr &fn, ArgKind kind = ARG_NONE, lua_Integer n = 0, const char *str = nullptr, size_t len = 0) {
  if (!fn) return;
  CallCtx c{fn->ref, kind, str, len, n, nullptr};
  invoke(fn->s.get(), c);
}

// ---------- handles to UI objects ----------

// Lua may keep a handle after LVGL deleted its object (the page was closed); the ref turns null then.
struct ObjRef {
  lv_obj_t *obj;
};
using Ref = std::shared_ptr<ObjRef>;

static Ref watch(lv_obj_t *obj) {
  if (!obj) return nullptr;
  Ref r(new ObjRef{obj});
  lv_obj_add_event_cb(obj, [](lv_event_t *e) {
    auto *held = (Ref *)lv_event_get_user_data(e);
    (*held)->obj = nullptr;
    delete held;
  }, LV_EVENT_DELETE, new Ref(r));
  return r;
}

struct Handle {
  int kind;
  Ref box, text;
};
static const char *HANDLE = "yos.handle";
static const char *KIND_NAMES[] = {"", "page", "group", "row", "label"};

static void pushHandle(lua_State *L, int kind, lv_obj_t *box, lv_obj_t *text) {
  void *mem = lua_newuserdatauv(L, sizeof(Handle), 0);
  new (mem) Handle{kind, watch(box), watch(text)};
  luaL_setmetatable(L, HANDLE);
}

static lv_obj_t *checkObj(lua_State *L, int idx, int kind) {
  Handle *h = (Handle *)luaL_checkudata(L, idx, HANDLE);
  if (h->kind != kind) luaL_error(L, "bad argument #%d (expected a %s, got a %s)", idx, KIND_NAMES[kind], KIND_NAMES[h->kind]);
  if (!h->box || !h->box->obj) luaL_error(L, "this %s is no longer on screen", KIND_NAMES[kind]);
  return h->box->obj;
}

static int handleGc(lua_State *L) {
  ((Handle *)lua_touserdata(L, 1))->~Handle();
  return 0;
}

// Counts one more UI object against the session's cap.
static void countObject(lua_State *L) {
  Session *s = sessionOf(L);
  if (++s->objects > MAX_OBJECTS) luaL_error(L, "too many UI objects (limit %d)", MAX_OBJECTS);
}

static void limitText(lua_State *L, size_t len) {
  if (len > TEXT_MAX) luaL_error(L, "text too long (%d bytes, limit %d)", (int)len, (int)TEXT_MAX);
}

static const char *checkText(lua_State *L, int idx) {
  size_t len;
  const char *t = luaL_checklstring(L, idx, &len);
  limitText(L, len);
  return t;
}

static const char *optText(lua_State *L, int idx, const char *def) {
  if (lua_isnoneornil(L, idx)) return def;
  return checkText(L, idx);
}

// row:set_value(text) / label:set_text(text). Quietly does nothing once the page is gone.
static int handleSetText(lua_State *L) {
  Handle *h = (Handle *)luaL_checkudata(L, 1, HANDLE);
  size_t len;
  const char *text = luaL_tolstring(L, 2, &len);
  limitText(L, len);
  if (h->text && h->text->obj) lv_label_set_text(h->text->obj, text);
  return 0;
}

// ---------- ui ----------

static const char *glyphFor(const char *name);

static int uiPage(lua_State *L) {
  Session *s = sessionOf(L);
  const char *title = checkText(L, 1);
  countObject(L);
  int ref = LUA_NOREF;
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lv_obj_t *outer = s->building;
  ui::push(s->appRoot, title, [&](lv_obj_t *page) {
    s->building = page;
    if (ref != LUA_NOREF) invoke(s, CallCtx{ref, ARG_PAGE, nullptr, 0, 0, page});
  });
  s->building = outer;
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
  if (s->failed) luaL_error(L, "stopped");  // unwind the rest of the script; the error is already shown
  return 0;
}

static int uiGroup(lua_State *L) {
  lv_obj_t *page = checkObj(L, 1, H_PAGE);
  const char *header = optText(L, 2, nullptr), *footer = optText(L, 3, nullptr);
  countObject(L);
  lv_obj_t *g = ui::group(page, header, footer);
  pushHandle(L, H_GROUP, g, nullptr);
  return 1;
}

static const char *optField(lua_State *L, int t, const char *key) {
  lua_getfield(L, t, key);
  size_t len = 0;
  const char *v = lua_isnil(L, -1) ? nullptr : luaL_tolstring(L, -1, &len);
  if (v) limitText(L, len);
  lua_pop(L, v ? 2 : 1);  // the caller copies v before anything else can allocate (and collect it)
  return v;
}

static int uiRow(lua_State *L) {
  Session *s = sessionOf(L);
  lv_obj_t *g = checkObj(L, 1, H_GROUP);
  luaL_checktype(L, 2, LUA_TTABLE);
  countObject(L);
  // Copy the strings: luaL_tolstring results can be collected once popped.
  std::string title, value, icon;
  bool hasIcon = false;
  if (const char *v = optField(L, 2, "title")) title = v;
  if (const char *v = optField(L, 2, "value")) value = v;
  if (const char *v = optField(L, 2, "icon")) {
    icon = v;
    hasIcon = true;
  }
  lua_getfield(L, 2, "on_tap");
  LuaFnPtr fn = keepFunction(L, -1);
  lua_pop(L, 1);
  ui::Action tap = nullptr;
  if (fn) tap = [fn] { invoke(fn); };
  ui::Row r = ui::row(g, hasIcon ? glyphFor(icon.c_str()) : nullptr, s->app->tint(), title.c_str(), value.c_str(), tap);
  pushHandle(L, H_ROW, r.obj, r.value);
  return 1;
}

static int uiButton(lua_State *L) {
  lv_obj_t *page = checkObj(L, 1, H_PAGE);
  const char *text = checkText(L, 2);
  countObject(L);
  LuaFnPtr fn = keepFunction(L, 3);
  lv_obj_t *b = ui::button(page, text, theme::p().accent, [fn] { invoke(fn); });
  pushHandle(L, H_LABEL, b, lv_obj_get_child(b, 0));
  return 1;
}

static int uiLabel(lua_State *L) {
  lv_obj_t *page = checkObj(L, 1, H_PAGE);
  const char *text = optText(L, 2, "");
  countObject(L);
  lv_obj_t *l = ui::label(page, text, theme::body, theme::p().label);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(l, ui::SIDE, 0);
  pushHandle(L, H_LABEL, l, l);
  return 1;
}

static int uiAlert(lua_State *L) {
  std::string title = checkText(L, 1);
  const char *message = optText(L, 2, nullptr);
  std::string msg = message ? message : "";
  std::string labels[3] = {"OK"};
  int n = 1;
  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    n = (int)luaL_len(L, 3);
    luaL_argcheck(L, n >= 1 && n <= 3, 3, "between 1 and 3 buttons");
    for (int i = 0; i < n; i++) {
      lua_geti(L, 3, i + 1);
      labels[i] = checkText(L, -1);
      lua_pop(L, 1);
    }
  }
  LuaFnPtr fn = keepFunction(L, 4);
  auto action = [&](int i) {
    ui::AlertAction a{labels[i].c_str(), labels[i] == "Cancel" ? ui::AlertAction::CANCEL : ui::AlertAction::NORMAL,
                      [fn, i] { invoke(fn, ARG_INT, i + 1); }};
    return a;
  };
  const char *m = message ? msg.c_str() : nullptr;
  if (n == 1) ui::alert(title.c_str(), m, {action(0)});
  else if (n == 2) ui::alert(title.c_str(), m, {action(0), action(1)});
  else ui::alert(title.c_str(), m, {action(0), action(1), action(2)});
  return 0;
}

static int uiInput(lua_State *L) {
  Session *s = sessionOf(L);
  const char *title = checkText(L, 1);
  const char *placeholder = optText(L, 2, "");
  LuaFnPtr fn = keepFunction(L, 3);
  ui::textInput(s->appRoot, title, placeholder, "", false, [fn](const String &text) { invoke(fn, ARG_STRING, 0, text.c_str(), text.length()); });
  return 0;
}

// ---------- net ----------

// Collects a response body, refusing more than BODY_MAX bytes.
class BodyStream : public Stream {
public:
  std::string body;
  bool tooLarge = false;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t n) override {
    if (body.size() + n > BODY_MAX) {
      tooLarge = true;
      return 0;
    }
    body.append((const char *)buf, n);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// The request runs on a short-lived task (netGet starts it, pollNet collects the result).
static void netWorker(void *arg) {
  auto *held = (std::shared_ptr<NetJob> *)arg;
  NetJob &job = **held;
  BodyStream out;
  {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(NET_TIMEOUT_MS);
    http.setTimeout(NET_TIMEOUT_MS);
    http.setUserAgent("yOS");
    if (!http.begin(client, job.url)) {
      job.code = -1;
      job.error = "bad URL";
    } else {
      job.code = http.GET();
      if (job.code <= 0) {
        job.error = HTTPClient::errorToString(job.code).c_str();
      } else if (http.getSize() > (int)BODY_MAX) {
        out.tooLarge = true;
      } else {
        http.writeToStream(&out);
      }
      http.end();
    }
  }
  if (job.code > 0 && out.tooLarge) {
    job.code = -1;
    job.error = "response larger than 64 KB";
  } else if (job.code > 0) {
    job.body = std::move(out.body);
  }
  job.done = true;  // last: the UI task may now read and drop the job
  delete held;
  netWorkers--;
  vTaskDelete(nullptr);
}

// net.get(url, function(status, body) ... end): the callback runs later from tick(), with
// (nil, message) when the request failed. One request at a time.
static int netGet(lua_State *L) {
  Session *s = sessionOf(L);
  String url = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (s->job) luaL_error(L, "a request is already running");
  auto job = std::make_shared<NetJob>();
  job->url = url;
  if (!url.startsWith("http://")) {
    job->code = -1;
    job->error = "only http:// URLs are supported";
    job->done = true;
  } else if (WiFi.status() != WL_CONNECTED) {
    job->code = -1;
    job->error = "not connected to Wi-Fi";
    job->done = true;
  } else {
    if (netWorkers++ >= 1) {
      netWorkers--;
      luaL_error(L, "a request is already running");
    }
    auto *held = new std::shared_ptr<NetJob>(job);
    if (xTaskCreate(netWorker, "luanet", NET_STACK, held, 1, nullptr) != pdPASS) {
      delete held;
      netWorkers--;
      job->code = -1;
      job->error = "out of memory";
      job->done = true;
    }
  }
  s->netCb = keepFunction(L, 2);
  s->job = job;
  return 0;
}

// From tick: hands a finished request's result to its callback.
static void pollNet(Session *s) {
  if (!s->job || !s->job->done) return;
  std::shared_ptr<NetJob> job = std::move(s->job);
  s->job.reset();
  LuaFnPtr cb = std::move(s->netCb);
  s->netCb.reset();
  if (job->code > 0) invoke(cb, ARG_NET, job->code, job->body.data(), job->body.size());
  else invoke(cb, ARG_NET, 0, job->error.data(), job->error.size());
}

// ---------- store ----------

static void loadStore(Session *s) {
  if (s->storeLoaded) return;
  s->storeLoaded = true;
  std::string text;
  if (readFile(s->app->loc, s->app->folder() + "/store.json", text, STORE_MAX) &&
      deserializeJson(s->store, text) == DeserializationError::Ok && s->store.is<JsonObject>())
    return;
  s->store.clear();
  s->store.to<JsonObject>();
}

static int storeGet(lua_State *L) {
  Session *s = sessionOf(L);
  const char *key = luaL_checkstring(L, 1);
  loadStore(s);
  JsonVariant v = s->store[key];
  if (v.is<bool>()) lua_pushboolean(L, v.as<bool>());
  else if (v.is<long long>()) lua_pushinteger(L, v.as<long long>());
  else if (v.is<double>()) lua_pushnumber(L, v.as<double>());
  else if (v.is<const char *>()) lua_pushstring(L, v.as<const char *>());
  else lua_pushnil(L);
  return 1;
}

static int storeSet(lua_State *L) {
  Session *s = sessionOf(L);
  const char *key = luaL_checkstring(L, 1);
  loadStore(s);
  switch (lua_type(L, 2)) {
    case LUA_TNONE:
    case LUA_TNIL: s->store.remove(key); break;
    case LUA_TBOOLEAN: s->store[key] = (bool)lua_toboolean(L, 2); break;
    case LUA_TNUMBER:
      if (lua_isinteger(L, 2)) s->store[key] = (long long)lua_tointeger(L, 2);
      else s->store[key] = lua_tonumber(L, 2);
      break;
    case LUA_TSTRING: s->store[key] = lua_tostring(L, 2); break;
    default: return luaL_argerror(L, 2, "store values are strings, numbers, booleans or nil");
  }
  if (measureJson(s->store) > STORE_MAX) {
    s->store.remove(key);
    return luaL_error(L, "store is full (16 KB)");
  }
  s->storeDirty = true;  // written by flushStore: from tick at most every 5 s, and when the app closes
  return 0;
}

static void flushStore(Session *s) {
  if (!s->storeDirty) return;
  s->storeDirty = false;
  s->storeFlushed = millis();
  std::string text;
  serializeJson(s->store, text);
  if (!writeFile(s->app->loc, s->app->folder() + "/store.json", text))
    Serial.printf("[app %s] could not save the store\n", s->app->id());
}

// ---------- sys, print ----------

static int sysTime(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)time(nullptr));
  return 1;
}

static int sysMillis(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)millis());
  return 1;
}

static int luaPrint(lua_State *L) {
  int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; i++) {
    if (i > 1) luaL_addchar(&b, '\t');
    luaL_tolstring(L, i, nullptr);
    luaL_addvalue(&b);
  }
  luaL_pushresult(&b);
  size_t len;
  const char *text = lua_tolstring(L, -1, &len);
  Serial.printf("[app %s] %.*s%s\n", sessionOf(L)->app->id(), (int)(len > PRINT_MAX ? PRINT_MAX : len), text, len > PRINT_MAX ? "..." : "");
  return 0;
}

static int openApi(lua_State *L) {
  luaL_newmetatable(L, HANDLE);
  lua_pushcfunction(L, handleGc);
  lua_setfield(L, -2, "__gc");
  static const luaL_Reg methods[] = {{"set_value", handleSetText}, {"set_text", handleSetText}, {nullptr, nullptr}};
  luaL_newlib(L, methods);
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);

  static const luaL_Reg uiLib[] = {{"page", uiPage},   {"group", uiGroup}, {"row", uiRow},     {"button", uiButton},
                                   {"label", uiLabel}, {"alert", uiAlert}, {"input", uiInput}, {nullptr, nullptr}};
  static const luaL_Reg netLib[] = {{"get", netGet}, {nullptr, nullptr}};
  static const luaL_Reg storeLib[] = {{"get", storeGet}, {"set", storeSet}, {nullptr, nullptr}};
  static const luaL_Reg sysLib[] = {{"time", sysTime}, {"millis", sysMillis}, {nullptr, nullptr}};
  luaL_newlib(L, uiLib);
  lua_setglobal(L, "ui");
  luaL_newlib(L, netLib);
  lua_setglobal(L, "net");
  luaL_newlib(L, storeLib);
  lua_setglobal(L, "store");
  luaL_newlib(L, sysLib);
  lua_setglobal(L, "sys");
  lua_pushcfunction(L, luaPrint);
  lua_setglobal(L, "print");
  return 0;
}

static int runMain(lua_State *L) {
  const std::string *src = (const std::string *)lua_touserdata(L, 1);
  if (luaL_loadbufferx(L, src->data(), src->size(), "=main.lua", "t") != LUA_OK) return lua_error(L);
  lua_call(L, 0, 0);
  return 0;
}

// ---------- icons ----------

static const char *glyphFor(const char *name) {
  static const struct {
    const char *name, *glyph;
  } icons[] = {
      {"extension", ICON_EXTENSION}, {"globe", ICON_GLOBE},       {"clock", ICON_CLOCK},        {"sun", ICON_SUN},
      {"cloud", ICON_CLOUD},         {"folder", ICON_FOLDER},     {"settings", ICON_SETTINGS},  {"wifi", ICON_WIFI},
      {"router", ICON_ROUTER},       {"info", ICON_INFO},         {"moon", ICON_MOON},          {"storage", ICON_STORAGE},
      {"shield", ICON_SHIELD},       {"lock", ICON_LOCK},         {"location", ICON_LOCATION},  {"thermometer", ICON_THERMO},
      {"bluetooth", ICON_BLUETOOTH}, {"refresh", ICON_REFRESH},   {"keyboard", ICON_KEYBOARD},  {"file", ICON_FILE},
      {"dns", ICON_DNS},             {"power", ICON_POWER},       {"play", ICON_PLAY},          {"apps", ICON_APPS},
      {"add", ICON_ADD},             {"check", ICON_CHECK},       {"delete", ICON_DELETE},      {"sd", ICON_SD},
  };
  for (auto &i : icons)
    if (!strcmp(i.name, name)) return i.glyph;
  return ICON_EXTENSION;
}

bool parseManifest(const std::string &json, Manifest &m, String &err) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    err = "app.json is not valid JSON";
    return false;
  }
  const char *id = doc["id"] | "";
  size_t n = strlen(id);
  bool idOk = n >= 1 && n <= 32;
  for (size_t i = 0; i < n && idOk; i++) idOk = islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '-' || id[i] == '_';
  if (!idOk) {
    err = "app.json: \"id\" must be 1-32 characters of a-z, 0-9, - or _";
    return false;
  }
  const char *name = doc["name"] | "";
  if (!*name || strlen(name) > 24) {
    err = "app.json: \"name\" must be 1-24 characters";
    return false;
  }
  m.id = id;
  m.name = name;
  m.version = doc["version"] | "1.0";
  m.glyph = glyphFor(doc["icon"] | "extension");
  const char *color = doc["color"] | "#5E5CE6";
  m.color = lv_color_hex(*color == '#' && strlen(color) == 7 ? strtoul(color + 1, nullptr, 16) : 0x5E5CE6);
  return true;
}

// ---------- the app ----------

void ScriptApp::open(lv_obj_t *root) {
  if (removed) {
    ui::push(root, name(), [](lv_obj_t *c) { ui::note(c, "This app was removed."); });
    return;
  }
  session = std::make_shared<Session>();
  Session *s = session.get();
  s->sliceMs = LUA_SLICE_MS;
  s->app = this;
  s->appRoot = root;
  s->L = newSandbox(s);
  if (!s->L) {
    fail(s, "not enough memory to start");
    return;
  }
  std::string err, src;
  if (!protectedRun(s->L, openApi, nullptr, err)) {
    fail(s, err);
    return;
  }
  if (!readFile(loc, folder() + "/main.lua", src, MAIN_MAX)) {
    fail(s, "main.lua is missing or larger than 256 KB");
    return;
  }
  s->sliceMs = MAIN_SLICE_MS;
  if (!protectedRun(s->L, runMain, &src, err)) fail(s, err);
  s->sliceMs = LUA_SLICE_MS;
  if (!ui::currentPage()) ui::push(root, name(), [](lv_obj_t *c) { ui::note(c, "This app has nothing to show."); });
  Serial.printf("[app %s] started, %u KB of Lua memory\n", id(), (unsigned)(s->used / 1024));
}

void ScriptApp::close() {
  if (!session) return;
  Session *s = session.get();
  s->alive = false;
  s->netCb.reset();  // breaks the Session <-> LuaFn cycle; a running request just finishes unobserved
  s->job.reset();
  if (!removed) flushStore(s);
  if (s->L) {
    Serial.printf("[app %s] closed, peak %u KB of Lua memory\n", id(), (unsigned)(s->peak / 1024));
    lua_close(s->L);
    s->L = nullptr;
  }
  session.reset();  // callbacks still held by LVGL objects keep the (dead) session until they go
  if (!removed) bytes = folderBytes(loc, folder());  // store.json may have grown
}

void ScriptApp::tick() {
  if (!session) return;
  Session *s = session.get();
  pollNet(s);
  invoke(s, CallCtx{LUA_NOREF, ARG_NONE, nullptr, 0, 0, nullptr});
  if (s->storeDirty && (uint32_t)(millis() - s->storeFlushed) >= STORE_FLUSH_MS) flushStore(s);
}

void ScriptApp::buildSettings(lv_obj_t *content, lv_obj_t *) {
  const Palette &p = theme::p();
  char size[24];
  if (bytes < 1024 * 1024) snprintf(size, sizeof(size), "%.1f KB", bytes / 1024.0);
  else snprintf(size, sizeof(size), "%.1f MB", bytes / 1048576.0);
  lv_obj_t *g = ui::group(content, "Installed App");
  ui::row(g, loc == SDCARD ? ICON_SD : ICON_STORAGE, p.gray, "Location", loc == SDCARD ? "SD Card" : "On This Device");
  ui::row(g, ICON_FILE, p.gray, "Size", size);
  ui::row(g, ICON_INFO, p.gray, "Version", manifest.version.c_str());
  if (removed) return;
  g = ui::group(content);
  ui::actionRow(g, "Remove App", p.red, [this] {
    std::string title = std::string("Remove \xE2\x80\x9C") + name() + "\xE2\x80\x9D?";
    ui::alert(title.c_str(), "The app and its data will be deleted.",
              {{"Cancel", ui::AlertAction::CANCEL, nullptr},
               {"Remove", ui::AlertAction::DESTRUCTIVE, [this] {
                  detachApp(this);
                  removeFolder(loc, folder());
                  os::goHome();  // Settings' app list was built before the removal; leave it
                }}});
  });
}

}  // namespace script
