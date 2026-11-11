/*
** $Id: ldo.h,v 2.20 2011/11/29 15:55:08 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#ifndef ldo_h
#define ldo_h


#include "lobject.hpp"
#include "lstate.hpp"
#include "lzio.hpp"

/// Exception type that passes through Lua.
/// Usually a fatal exception to the VM; which will be closed after.
struct EmulatorPassthroughException {};

/// Exception that signals a reset request.
struct EmulatorReset : EmulatorPassthroughException {};
struct EmulatorPanic : EmulatorPassthroughException {};
struct EmulatorExit : EmulatorPassthroughException {};

#define luaD_checkstack(L,n)	if (L->stack_last - L->top <= (n)) \
				    luaD_growstack(L, n); else condmovestack(L);


#define incr_top(L) {L->top++; luaD_checkstack(L,0);}

#define savestack(L,p)		((char *)(p) - (char *)L->stack)
#define restorestack(L,n)	((TValue *)((char *)L->stack + (n)))


/* type of protected functions, to be ran by `runprotected' */
typedef void (*Pfunc) (lua_State *L, void *ud);

LUAI_FUNC int luaD_protectedparser (lua_State *L, ZIO *z, const char *name,
                                                  const char *mode);
LUA_FAST LUAI_FUNC void luaD_hook (lua_State *L, int event, int line);
LUA_FAST LUAI_FUNC int luaD_precall (lua_State *L, StkId func, int nresults);
LUA_FAST LUAI_FUNC void luaD_call (lua_State *L, StkId func, int nResults,
                                        int allowyield);
LUA_FAST LUAI_FUNC int luaD_pcall (lua_State *L, Pfunc func, void *u,
                                        ptrdiff_t oldtop, ptrdiff_t ef);
LUA_FAST LUAI_FUNC int luaD_poscall (lua_State *L, StkId firstResult);
LUA_FAST LUAI_FUNC void luaD_reallocstack (lua_State *L, int newsize);
LUA_FAST LUAI_FUNC void luaD_growstack (lua_State *L, int n);
LUA_FAST LUAI_FUNC void luaD_shrinkstack (lua_State *L);

LUAI_FUNC l_noret luaD_throw (lua_State *L, int errcode);
LUA_FAST LUAI_FUNC int luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud);

#endif

