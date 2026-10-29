#include "script/sandbox.h"
#include <stdlib.h>
#include <string.h>
#include "lualib.h"

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <chrono>
#include <stdio.h>
#endif

namespace script {

uint32_t nowMs() {
#ifdef ESP_PLATFORM
  return (uint32_t)(esp_timer_get_time() / 1000);
#else
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

// Every allocation of a script app lives in PSRAM and counts against its budget. Frees always succeed.
static void *allocate(void *ud, void *ptr, size_t osize, size_t nsize) {
  Budget *b = (Budget *)ud;
  size_t old = ptr ? osize : 0;
  if (nsize == 0) {
#ifdef ESP_PLATFORM
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
    b->used -= old;
    return nullptr;
  }
  if (nsize > old && b->used - old + nsize > b->limit) return nullptr;  // Lua collects garbage and retries
#ifdef ESP_PLATFORM
  void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *p = realloc(ptr, nsize);
#endif
  if (!p) return nullptr;
  b->used = b->used - old + nsize;
  if (b->used > b->peak) b->peak = b->used;
  return p;
}

static int panic(lua_State *L) {
  const char *msg = lua_tostring(L, -1);
#ifdef ESP_PLATFORM
  ESP_LOGE("script", "unprotected Lua error: %s", msg ? msg : "?");
#else
  fprintf(stderr, "unprotected Lua error: %s\n", msg ? msg : "?");
#endif
  return 0;  // Lua aborts after this; every call into a script goes through protectedRun, so it never happens
}

Budget *budgetOf(lua_State *L) { return *(Budget **)lua_getextraspace(L); }

static const int HOOK_EVERY = 1000;
static void hook(lua_State *L, lua_Debug *);

size_t testStackFree = SIZE_MAX;

size_t stackFree() {
#ifdef ESP_PLATFORM
  return (uintptr_t)__builtin_frame_address(0) - (uintptr_t)pxTaskGetStackStart(nullptr);
#else
  return testStackFree;
#endif
}

// Stops the running callback: keeps raising on every instruction from now on, so a script's own
// pcall can't swallow the error and loop on.
void overrun(lua_State *L, Budget *b) {
  if (!b->overran) lua_sethook(L, hook, LUA_MASKCOUNT, 1);
  b->overran = true;
  luaL_error(L, "script ran longer than %d ms", (int)b->sliceMs);
}

// Runs every HOOK_EVERY VM instructions: stops a callback that has used up its time slice.
static void hook(lua_State *L, lua_Debug *) {
  Budget *b = budgetOf(L);
  if (!b->depth) return;
  if (b->overran || (int32_t)(nowMs() - b->deadline) > 0) overrun(L, b);
}

bool protectedRun(lua_State *L, lua_CFunction fn, void *ctx, std::string &err) {
  Budget *b = budgetOf(L);
  if (b->depth++ == 0) {
    b->deadline = nowMs() + b->sliceMs;
    b->overran = false;
    b->matchSteps = 0;
  }
  lua_pushcfunction(L, fn);
  lua_pushlightuserdata(L, ctx);
  bool ok = lua_pcall(L, 1, 0, 0) == LUA_OK;
  if (!ok) {
    const char *msg = lua_tostring(L, -1);
    err = msg ? msg : "unknown error";
    lua_pop(L, 1);
  }
  if (--b->depth == 0 && b->overran) {
    b->overran = false;
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
  }
  return ok;
}

}  // namespace script

// Called by lstrlib.c's match() for every step: checks the time slice every 4096 steps (a runaway
// pattern never reaches the VM hook, which only runs between instructions).
void yos_match_step(lua_State *L) {
  script::Budget *b = script::budgetOf(L);
  if ((++b->matchSteps & 0xFFF) == 0 && b->depth && (b->overran || (int32_t)(script::nowMs() - b->deadline) > 0))
    script::overrun(L, b);
}

// Called by the Lua core on C-level recursion: true when little stack is left (the UI task has 12 KB).
bool yos_stack_low(lua_State *L) { return script::stackFree() < script::budgetOf(L)->minStack; }

namespace script {

// ---------- json.decode ----------

namespace {
struct Parser {
  Parser(const char *p, const char *end) : p(p), end(end) {}
  const char *p, *end;
  const char *err = nullptr;
  int depth = 0;
};

void skipSpace(Parser &j) {
  while (j.p < j.end && (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' || *j.p == '\r')) j.p++;
}

bool fail(Parser &j, const char *msg) {
  if (!j.err) j.err = msg;
  return false;
}

void putUtf8(std::string &out, uint32_t c) {
  if (c < 0x80) {
    out += (char)c;
  } else if (c < 0x800) {
    out += (char)(0xC0 | (c >> 6));
    out += (char)(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += (char)(0xE0 | (c >> 12));
    out += (char)(0x80 | ((c >> 6) & 0x3F));
    out += (char)(0x80 | (c & 0x3F));
  } else {
    out += (char)(0xF0 | (c >> 18));
    out += (char)(0x80 | ((c >> 12) & 0x3F));
    out += (char)(0x80 | ((c >> 6) & 0x3F));
    out += (char)(0x80 | (c & 0x3F));
  }
}

bool hex4(Parser &j, uint32_t &v) {
  if (j.end - j.p < 4) return fail(j, "bad \\u escape");
  v = 0;
  for (int i = 0; i < 4; i++) {
    char c = *j.p++;
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return fail(j, "bad \\u escape");
  }
  return true;
}

bool parseString(Parser &j, std::string &out) {
  j.p++;  // opening quote
  while (j.p < j.end) {
    char c = *j.p++;
    if (c == '"') return true;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (j.p >= j.end) break;
    char e = *j.p++;
    switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case '\'': out += '\''; break;  // not JSON, but some APIs send it
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        uint32_t c1;
        if (!hex4(j, c1)) return false;
        if (c1 >= 0xD800 && c1 < 0xDC00 && j.end - j.p >= 6 && j.p[0] == '\\' && j.p[1] == 'u') {
          j.p += 2;
          uint32_t c2;
          if (!hex4(j, c2)) return false;
          if (c2 >= 0xDC00 && c2 < 0xE000) c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          else c1 = 0xFFFD;
        } else if (c1 >= 0xD800 && c1 < 0xE000) {
          c1 = 0xFFFD;
        }
        putUtf8(out, c1);
        break;
      }
      default: return fail(j, "bad escape");
    }
  }
  return fail(j, "unterminated string");
}

bool literal(Parser &j, const char *word) {
  size_t n = strlen(word);
  if ((size_t)(j.end - j.p) < n || memcmp(j.p, word, n) != 0) return fail(j, "unexpected character");
  j.p += n;
  return true;
}

bool parseValue(lua_State *L, Parser &j);

bool parseNumber(lua_State *L, Parser &j) {
  char buf[48];
  size_t n = 0;
  while (j.p < j.end && n < sizeof(buf) - 1 && strchr("+-0123456789.eE", *j.p)) buf[n++] = *j.p++;
  buf[n] = 0;
  if (!n || lua_stringtonumber(L, buf) == 0) return fail(j, "bad number");
  return true;
}

bool parseArray(lua_State *L, Parser &j) {
  j.p++;
  lua_newtable(L);
  lua_Integer i = 0;
  skipSpace(j);
  if (j.p < j.end && *j.p == ']') {
    j.p++;
    return true;
  }
  for (;;) {
    if (!parseValue(L, j)) return false;
    lua_rawseti(L, -2, ++i);  // null leaves a hole, like most Lua JSON libraries
    skipSpace(j);
    if (j.p < j.end && *j.p == ',') {
      j.p++;
      continue;
    }
    if (j.p < j.end && *j.p == ']') {
      j.p++;
      return true;
    }
    return fail(j, "expected ',' or ']'");
  }
}

bool parseObject(lua_State *L, Parser &j) {
  j.p++;
  lua_newtable(L);
  skipSpace(j);
  if (j.p < j.end && *j.p == '}') {
    j.p++;
    return true;
  }
  for (;;) {
    skipSpace(j);
    if (j.p >= j.end || *j.p != '"') return fail(j, "expected a key");
    std::string key;
    if (!parseString(j, key)) return false;
    skipSpace(j);
    if (j.p >= j.end || *j.p != ':') return fail(j, "expected ':'");
    j.p++;
    lua_pushlstring(L, key.data(), key.size());
    if (!parseValue(L, j)) return false;
    lua_rawset(L, -3);  // a null value just leaves the key out
    skipSpace(j);
    if (j.p < j.end && *j.p == ',') {
      j.p++;
      continue;
    }
    if (j.p < j.end && *j.p == '}') {
      j.p++;
      return true;
    }
    return fail(j, "expected ',' or '}'");
  }
}

bool parseValue(lua_State *L, Parser &j) {
  skipSpace(j);
  if (j.p >= j.end) return fail(j, "unexpected end");
  if (j.depth >= 64) return fail(j, "nested too deeply");
  if (!lua_checkstack(L, 4)) return fail(j, "nested too deeply");
  char c = *j.p;
  bool ok;
  switch (c) {
    case '{': j.depth++; ok = parseObject(L, j); j.depth--; return ok;
    case '[': j.depth++; ok = parseArray(L, j); j.depth--; return ok;
    case '"': {
      std::string s;
      if (!parseString(j, s)) return false;
      lua_pushlstring(L, s.data(), s.size());
      return true;
    }
    case 't': if (!literal(j, "true")) return false; lua_pushboolean(L, 1); return true;
    case 'f': if (!literal(j, "false")) return false; lua_pushboolean(L, 0); return true;
    case 'n': if (!literal(j, "null")) return false; lua_pushnil(L); return true;
    default:
      if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(L, j);
      return fail(j, "unexpected character");
  }
}
}  // namespace

int jsonDecode(lua_State *L) {
  size_t n;
  const char *s = luaL_checklstring(L, 1, &n);
  int top = lua_gettop(L);
  Parser j(s, s + n);
  bool ok = parseValue(L, j);
  if (ok) {
    skipSpace(j);
    if (j.p != j.end) ok = fail(j, "trailing characters");
  }
  if (ok) return 1;
  lua_settop(L, top);
  lua_pushnil(L);
  lua_pushfstring(L, "json: %s at offset %d", j.err ? j.err : "error", (int)(j.p - s));
  return 2;
}

// ---------- the state ----------

static int openLibs(lua_State *L) {
  static const luaL_Reg libs[] = {
      {LUA_GNAME, luaopen_base},         {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string},  {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
  };
  for (const luaL_Reg &lib : libs) {
    luaL_requiref(L, lib.name, lib.func, 1);
    lua_pop(L, 1);
  }
  // No file access and no bytecode: scripts can't read the flash or feed the VM crafted chunks.
  for (const char *name : {"dofile", "loadfile", "load"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  lua_getglobal(L, "string");
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushcfunction(L, jsonDecode);
  lua_setfield(L, -2, "decode");
  lua_setglobal(L, "json");
  return 0;
}

lua_State *newSandbox(Budget *budget) {
  lua_State *L = lua_newstate(allocate, budget);
  if (!L) return nullptr;
  *(Budget **)lua_getextraspace(L) = budget;
  lua_atpanic(L, panic);
  lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);  // coroutines inherit it
  std::string err;
  if (!protectedRun(L, [](lua_State *L) { return openLibs(L); }, nullptr, err)) {
    lua_close(L);
    return nullptr;
  }
  return L;
}

}  // namespace script
