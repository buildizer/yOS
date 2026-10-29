/*
** yOS build of Lua 5.4.7: every library source compiled as one C++ translation unit
** (the layout of Lua's own onelua.c). Compiling Lua as C++ makes lua_error() throw a C++
** exception instead of longjmp, so C++ objects in yOS bindings are destroyed properly when a
** script errors. The vendored lua.org sources next to this file are unmodified except for the small
** #ifdef YOS_HOOKS patches in ldo.c, lstate.c and lstrlib.c (see lua_port.h).
**
** Not built: lua.c / luac.c (stand-alone programs, not vendored), and the io, os, package and
** debug libraries plus linit.c: script apps are sandboxed and only get base (minus file
** loading), coroutine, table, string, math and utf8, opened by src/script/loader.cpp.
*/
#include "lua_port.h"
#include "lprefix.h"

/* core */
#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"

/* auxiliary library and the standard libraries script apps may use */
#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "lmathlib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
