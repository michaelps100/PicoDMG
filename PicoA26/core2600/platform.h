#ifndef A26_PLATFORM_H
#define A26_PLATFORM_H
#ifdef A26_HOST_TEST
#define __not_in_flash_func(name) name
#define A26_RAMFUNC(name) name
static inline void watchdog_update(void) {}
#else
#include "pico.h"
#include "hardware/watchdog.h"
/* Hot helpers called from the SRAM emulation loop. Without this, GCC emits the
 * out-of-line copies of static inline helpers (and every non-annotated callee)
 * in flash, so each CPU bus access paid a RAM->flash veneer and an XIP fetch. */
#define A26_RAMFUNC(name) __no_inline_not_in_flash_func(name)
#endif
#endif
