#include "input.hpp"

#include <devices/buttonstate.hpp>
#include <emu/emulator.hpp>
#include <lauxlib.hpp>
#include <lua.hpp>

#include <string_view>
#include <cstdlib>

namespace emu::bindings {

static int parse_button_arg(lua_State *state, int idx) {
	if (lua_isnoneornil(state, idx)) return -1;
	if (lua_isnumber(state, idx)) {
		int isnum = 0;
		lua_Number n = lua_tonumberx(state, idx, &isnum);
		if (!isnum) return -1;
		int b = (int)(lua_Unsigned)n;
		if (b >= 0 && b < 8) return b;
		return -1;
	}
	if (lua_isstring(state, idx)) {
		size_t len = 0;
		const char *s = lua_tolstring(state, idx, &len);
		if (!s || len == 0) return -1;
		if (len == 1) {
			uint8_t c = (uint8_t)s[0];
			if (c >= '0' && c <= '7') return c - '0';
			if (c == 0x8b) return 0; // ⬅️ Left
			if (c == 0x91) return 1; // ➡️ Right
			if (c == 0x94) return 2; // ⬆️ Up
			if (c == 0x83) return 3; // ⬇️ Down
			if (c == 0x8e) return 4; // 🅾️ Button O
			if (c == 0x97) return 5; // ❎ Button X
		}
		std::string_view sv(s, len);
		if (sv == "⬅️" || sv == "⬅") return 0;
		if (sv == "➡️" || sv == "➡") return 1;
		if (sv == "⬆️" || sv == "⬆") return 2;
		if (sv == "⬇️" || sv == "⬇") return 3;
		if (sv == "🅾️" || sv == "🅾") return 4;
		if (sv == "❎") return 5;

		char *end = nullptr;
		long val = std::strtol(s, &end, 10);
		if (end != s && val >= 0 && val < 8) return (int)val;
	}
	return -1;
}

int y8_btn(lua_State *state) {
	const auto argument_count = lua_gettop(state);
	const auto mask = emu::emulator.get_button_state().held_key_mask;

	if (argument_count >= 1) {
		const int button = parse_button_arg(state, 1);
		if (button < 0) {
			lua_pushboolean(state, false);
			return 1;
		}

		if (argument_count >= 2) {
			const auto player = lua_tounsigned(state, 2);
			if (player != 0) {
				lua_pushboolean(state, false);
				return 1;
			}
		}

		lua_pushboolean(state, (mask >> button) & 0b1);
		return 1;
	}

	// Return the entire bitset when no argument is provided
	lua_pushunsigned(state, mask);
	return 1;
}

int y8_btnp(lua_State *state) {
	const auto argument_count = lua_gettop(state);
	const auto mask = emu::emulator.get_button_state().pressed_key_mask;

	if (argument_count >= 1) {
		const int button = parse_button_arg(state, 1);
		if (button < 0) {
			lua_pushboolean(state, false);
			return 1;
		}

		if (argument_count >= 2) {
			const auto player = lua_tounsigned(state, 2);
			if (player != 0) {
				lua_pushboolean(state, false);
				return 1;
			}
		}

		lua_pushboolean(state, (mask >> button) & 0b1);
		return 1;
	}

	// Return the entire bitset when no argument is provided
	lua_pushunsigned(state, mask);
	return 1;
}

} // namespace emu::bindings