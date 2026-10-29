#pragma once
// The Lua sandbox for script apps, independent of LVGL and Arduino (it also builds on a desktop
// for tests): a capped allocator in PSRAM, a CPU budget per callback, the safe standard libraries
// and json.decode(). See src/script/README.md for the API script apps see.
#include <stddef.h>
#include <stdint.h>
#include <string>
#include "lua_port.h"
#include "lua.h"
#include "lauxlib.h"

namespace script {

struct Budget {
  size_t used = 0;                 // bytes the state holds now
  size_t peak = 0;
  size_t limit = 512 * 1024;       // allocations past this fail with "not enough memory"
  uint32_t sliceMs = 300;          // longest (wall-clock) a single callback may run, network waits included
  uint32_t deadline = 0;
  int depth = 0;                   // nested protected calls (only the outermost one sets the deadline)
  bool overran = false;            // the CPU budget ran out; the hook keeps raising until the call unwinds
  uint32_t matchSteps = 0;         // pattern-matching steps since the last deadline check
  size_t minStack = 3072;          // a call into Lua fails when the task has less free stack than this
};

uint32_t nowMs();

// A new state with base (without dofile/loadfile/load), coroutine, table, string, math, utf8 and
// json. Returns nullptr when even that does not fit in the budget. Close it with lua_close().
lua_State *newSandbox(Budget *budget);
Budget *budgetOf(lua_State *L);

// Bytes of stack the current task still has. On a desktop build it returns testStackFree, so tests
// can simulate a nearly full stack.
size_t stackFree();
extern size_t testStackFree;

// Calls fn(L) in protected mode with ctx as its only argument (a light userdata), under the CPU
// budget. Everything that may allocate belongs inside fn, so out-of-memory errors are caught too.
// Returns false and fills err when the script raised an error.
bool protectedRun(lua_State *L, lua_CFunction fn, void *ctx, std::string &err);

// json.decode(text) -> value | nil, message   (null becomes nil; objects and arrays become tables)
int jsonDecode(lua_State *L);

}  // namespace script
