/*
** yOS: include before lua.h. The ESP32 toolchain's <limits.h> (GCC 8) hides LLONG_MAX in C++11
** mode, which makes luaconf.h think there is no 'long long'. Lua integers stay 64-bit.
*/
#ifndef lua_port_h
#define lua_port_h
#include <limits.h>
#if !defined(LLONG_MAX) && defined(__LONG_LONG_MAX__)
#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif

/*
** yOS sandbox limits. Script apps run on the UI task's small (12 KB) stack, so nesting of C calls,
** parser levels and pattern-matching recursion is kept low, and the hooks below (defined in
** src/script/sandbox.cpp) let the sandbox stop a script before the stack runs out or a pattern
** runs past its time slice. lstrlib.c and ldo.c call them under #ifdef YOS_HOOKS.
*/
#define LUAI_MAXCCALLS 40
#define MAXCCALLS 40
#define YOS_HOOKS 1
struct lua_State;
bool yos_stack_low(lua_State *L);   /* the task's free stack is below the sandbox minimum */
void yos_match_step(lua_State *L);  /* once per pattern-matching step; may raise a Lua error */
#endif
