#include "rng.hpp"

#include <cstdlib>
#include <devices/random.hpp>
#include <emu/emulator.hpp>
#include <lauxlib.hpp>
#include <lua.hpp>

namespace emu::bindings {

int y8_rnd(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	if (argument_count >= 1 && lua_istable(state, 1)) {
		const auto len = lua_rawlen(state, 1);
		if (len == 0) {
			lua_pushnil(state);
			return 1;
		}
		const auto r = device<devices::Random>.next(fix16_from_int((int)len));
		int idx = fix16_to_int(r) + 1;
		if (idx < 1) idx = 1;
		if (idx > (int)len) idx = (int)len;
		lua_rawgeti(state, 1, idx);
		return 1;
	}

	auto range = LuaFix16(1);

	if (argument_count >= 1 && !lua_isnil(state, 1)) {
		range = lua_tonumber(state, 1);
	}

	lua_pushnumber(
		state, LuaFix16::from_fix16(device<devices::Random>.next(range.value)));

	return 1;
}

int y8_srand(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto seed = LuaFix16(0);

	if (argument_count >= 1) {
		seed = lua_tonumber(state, 1);
	}

	device<devices::Random>.set_seed(seed.value);

	return 0;
}

} // namespace emu::bindings