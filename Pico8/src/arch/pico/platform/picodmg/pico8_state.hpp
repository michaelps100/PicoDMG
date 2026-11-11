#pragma once

#include <cstdint>
#include <cstddef>
#include <lua.hpp>
#include <lauxlib.hpp>

#ifdef __cplusplus
extern "C" {
#endif

void p8_state_init_lua(lua_State *L);
bool pico8_internal_save_state(const char *path);
bool pico8_internal_load_state(const char *path);

#ifdef __cplusplus
}
#endif
