#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace arch::pico::platform::picodmg {

void init_default_frequency();
void init_stdio();
void init_basic_gpio();
std::size_t init_psram_picodmg();
void init_video_picodmg();

} // namespace arch::pico::platform::picodmg
