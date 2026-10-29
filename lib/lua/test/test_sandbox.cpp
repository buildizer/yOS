// Host-side tests for the script sandbox limits. Run: lib/lua/test/run.sh
#include <stdio.h>
#include <string.h>
#include "script/sandbox.h"

using namespace script;

static int failures = 0;

// Runs src in a fresh sandbox; returns true when it finished without error.
static bool run(const char *src, std::string &err, uint32_t sliceMs = 300, size_t limit = 512 * 1024) {
  Budget b;
  b.sliceMs = sliceMs;
  b.limit = limit;
  lua_State *L = newSandbox(&b);
  if (!L) {
    err = "no state";
    return false;
  }
  struct Ctx { const char *src; } ctx{src};
  bool ok = protectedRun(L, [](lua_State *L) {
    Ctx *c = (Ctx *)lua_touserdata(L, 1);
    if (luaL_loadstring(L, c->src) != LUA_OK) return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
  }, &ctx, err);
  lua_close(L);
  return ok;
}

static void expectError(const char *name, const char *src, const char *needle, uint32_t maxMs = 1500) {
  std::string err;
  uint32_t t0 = nowMs();
  bool ok = run(src, err);
  uint32_t dt = nowMs() - t0;
  bool pass = !ok && err.find(needle) != std::string::npos && dt <= maxMs;
  printf("%s %-34s (%u ms) %s\n", pass ? "PASS" : "FAIL", name, (unsigned)dt, ok ? "no error" : err.c_str());
  if (!pass) failures++;
}

static void expectOk(const char *name, const char *src) {
  std::string err;
  bool ok = run(src, err);
  printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name, ok ? "" : err.c_str());
  if (!ok) failures++;
}

int main() {
  expectOk("basic", "local t = {} for i=1,100 do t[i]=i end assert(#t==100)");
  expectOk("string.find works", "assert(string.find('hello world', 'o w'))");
  expectOk("gsub with pattern", "assert(('abc'):gsub('%w', string.upper) == 'ABC')");
  expectOk("nesting 20 deep is fine", "local function f(n) if n==0 then return 0 end local ok,v=pcall(f,n-1) return v end assert(f(20)==0)");

  expectError("infinite loop", "while true do end", "ran longer");
  expectError("runaway pattern", "string.find(('a'):rep(3000), ('a*'):rep(12)..'b')", "ran longer");
  expectError("runaway gsub pattern", "(('a'):rep(3000)):gsub(('.-'):rep(12)..'b', 'x')", "ran longer");
  expectError("pcall can't swallow pattern overrun",
              "while true do pcall(string.find, ('a'):rep(3000), ('a*'):rep(12)..'b') end", "ran longer");
  expectError("deep pcall nesting", "local function f() local ok, e = pcall(f) if not ok then error(e, 0) end end f()", "C stack overflow");
  expectError("deep metamethod nesting",
              "local t = setmetatable({}, {__index=function(t,k) return t[k+1] end}) local _ = t[1]", "C stack overflow");
  expectError("deep gsub callback nesting",
              "local function f(n) return (('x'):gsub('x', function() return f(n+1) end)) end f(1)", "C stack overflow");
  expectError("deep nested source",
              ("local x = " + std::string(100, '(') + "1" + std::string(100, ')')).c_str(), "overflow");
  expectError("memory limit", "local t = {} while true do t[#t+1] = ('x'):rep(10000) end", "not enough memory");

  // Low free stack: the Lua core refuses to nest C calls any deeper.
  {
    std::string err;
    testStackFree = 1000;  // below the 3 KB minimum
    bool ok = run("local function f(n) if n == 0 then return end local ok, e = pcall(f, n - 1) if not ok then error(e, 0) end end f(10)", err);
    testStackFree = SIZE_MAX;
    bool pass = !ok && err.find("C stack overflow") != std::string::npos;
    printf("%s %-34s %s\n", pass ? "PASS" : "FAIL", "low stack stops nesting", err.c_str());
    if (!pass) failures++;
  }
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures != 0;
}
