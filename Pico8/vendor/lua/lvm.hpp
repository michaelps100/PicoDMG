/*
** $Id: lvm.h,v 2.18 2013/01/08 14:06:55 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#ifndef lvm_h
#define lvm_h


#include "ldo.hpp"
#include "lobject.hpp"
#include "ltm.hpp"


#define tostring(L,o) (ttisstring(o) || (luaV_tostring(L, o)))

#define tonumber(o,n)	(ttisnumber(o) || (((o) = luaV_tonumber(o,n)) != NULL))
#define tonumbermasked(o,n,mask)	(ttisnumber(o) || (((o) = luaV_tonumber(o,n,mask)) != NULL))

#define equalobj(L,o1,o2)  (ttisequal(o1, o2) && luaV_equalobj_(L, o1, o2))

#define luaV_rawequalobj(o1,o2)		equalobj(NULL,o1,o2)


/* not to called directly */
LUA_FAST LUAI_FUNC int luaV_equalobj_ (lua_State *L, const TValue *t1, const TValue *t2);


LUA_FAST LUAI_FUNC int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r);
LUA_FAST LUAI_FUNC int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r);
LUA_FAST LUAI_FUNC const TValue *luaV_tonumber (const TValue *obj, TValue *n, int parse_mask=0);
LUA_FAST LUAI_FUNC int luaV_tostring (lua_State *L, StkId obj);
LUA_FAST LUAI_FUNC void luaV_gettable (lua_State *L, const TValue *t, TValue *key,
                                            StkId val);
LUA_FAST LUAI_FUNC void luaV_gettable_upvalue_fast (lua_State *L, const TValue *t, TValue *key,
                                            StkId val);
LUA_FAST LUAI_FUNC void luaV_settable (lua_State *L, const TValue *t, TValue *key,
                                            StkId val);
LUA_FAST LUAI_FUNC void luaV_settable_upvalue_fast (lua_State *L, const TValue *t, TValue *key,
                                            StkId val);
LUA_FAST LUAI_FUNC void luaV_finishOp (lua_State *L);
LUA_FAST LUAI_FUNC void luaV_execute (lua_State *L);
LUA_FAST LUAI_FUNC void luaV_concat (lua_State *L, int total);
LUA_FAST LUAI_FUNC void luaV_arith (lua_State *L, StkId ra, const TValue *rb,
                           const TValue *rc, TMS op);
LUA_FAST LUAI_FUNC void luaV_objlen (lua_State *L, StkId ra, const TValue *rb);

#endif
