#pragma once

#include "binding.hpp"

namespace emu::bindings {

Y8_FAST_BINDING int y8_rnd(lua_State *state);
Y8_FAST_BINDING int y8_srand(lua_State *state);

} // namespace emu::bindings