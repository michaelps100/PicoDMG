// C shim over the C++ PicoPlusPsram singleton, so main.c (plain C) can map
// PSRAM without pulling in the C++ class. Compiled as C++ because the driver
// header declares a class. See inc/gb_psram.h.

#include "gb_psram.h"

#if PICO_RP2350

#include "PicoPlusPsram.h"

extern "C" size_t gb_psram_setup(void) {
  // PicoPlusPsram is a function-local static, so the constructor -- which runs
  // the QMI setup and registers the window with lwmem -- happens on this first
  // call and not again.
  return PicoPlusPsram::getInstance().GetMemorySize();
}

extern "C" size_t gb_psram_size(void) {
  return PicoPlusPsram::getInstance().GetMemorySize();
}

#else  // !PICO_RP2350

// The RP2350 Plus (HW_CONFIG 15) has no PSRAM, and the driver is compiled out
// there, so these have to exist and report nothing rather than fail to link.
extern "C" size_t gb_psram_setup(void) {
  return 0;
}

extern "C" size_t gb_psram_size(void) {
  return 0;
}

#endif // PICO_RP2350
