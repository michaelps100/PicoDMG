#pragma once

#include "binding.hpp"

namespace emu::bindings {

int y8_stat(lua_State *state);
int y8_exit(lua_State *state);
int y8_gc(lua_State *state);
int y8_printh(lua_State *state);
int y8_tostr(lua_State *state);
int y8_tonum(lua_State *state);
int y8_sub(lua_State *state);
int y8_ord(lua_State *state);
int y8_chr(lua_State *state);
int y8_menuitem(lua_State *state);
int y8_extcmd(lua_State *state);
int y8_cartdata(lua_State *state);
int y8_dget(lua_State *state);
int y8_dset(lua_State *state);
int y8_run(lua_State *state);
int y8_reboot(lua_State *state);
int y8_reset(lua_State *state);

} // namespace emu::bindings