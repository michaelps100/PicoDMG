/**
 * \file           gb_psram.h
 * \brief          C interface to the PSRAM window, for the C sources.
 *
 * The driver itself is C++ (PicoPlusPsram.cpp, ported from pico_shared) and
 * wraps the QMI setup plus a lwmem allocator. main.c is C, so it cannot see
 * the class; this header exposes just the two operations the emulator needs:
 * map the chip, and hand back the usable size.
 *
 * The ROM is then allocated with lwmem_malloc/lwmem_free directly rather than
 * through a wrapper, because that is the same pool the driver registered the
 * PSRAM window with and it keeps the C side free of C++ types.
 */

#ifndef GB_PSRAM_H
#define GB_PSRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Detects and maps the PSRAM chip on GB_PSRAM_CS_PIN and registers the window
 * with lwmem. Idempotent, because PicoPlusPsram is a function-local static: the
 * second and later calls return the cached size without re-running the QMI
 * setup, which would be unsafe once the SD card or display is running.
 *
 * \return usable size in bytes, or 0 if no chip answered.
 */
size_t gb_psram_setup(void);

/**
 * \return the size reported by gb_psram_setup(), or 0 if it was never called.
 */
size_t gb_psram_size(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_PSRAM_H */
