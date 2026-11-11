#pragma once

#include <string_view>

namespace emu {
class Emulator;
}

namespace p8 {

bool is_png_cart(std::string_view cart_path);
bool load_png_cart(std::string_view cart_path, emu::Emulator &emulator);

} // namespace p8

