/* Forced include (/FI) for the Windows MSVC host build of the core.
 * Maps the GCC builtins the core uses; A26_HOST_TEST makes core2600/platform.h
 * stub __not_in_flash_func and watchdog_update. Never part of the firmware. */
#ifndef A26_HOST_COMPAT_H
#define A26_HOST_COMPAT_H
#define A26_HOST_TEST 1
#ifdef _MSC_VER
#include <intrin.h>
#define __builtin_expect(x, y) (x)
static __inline int __builtin_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (int)i; }
#endif
#endif
