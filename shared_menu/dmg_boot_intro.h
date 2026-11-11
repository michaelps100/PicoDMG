#ifndef DMG_BOOT_INTRO_H
#define DMG_BOOT_INTRO_H

/* The bootloader's power-on intro (pico-bootLoader/AGENTS.md section 11: black,
 * the ArcadeFun logo fading in and dropping to the centre, the chime on
 * landing, a hold), replayed by an emulator on core 1 while core 0 loads the
 * game and its save state after a wake from standby.
 *
 * The bootloader cannot play it for us: the load happens after the jump into
 * the emulator, and the emulator re-initialises the panel. So the bootloader
 * skips its intro on a resume wake and the emulator plays the same one, with
 * the same asset (pico-bootLoader/src/boot_anim_assets.h, included, not
 * copied) and the same timings.
 *
 * Contract while the intro is active:
 *  - core 0 must not touch the panel. dmg_draw_loading_screen() and
 *    dmg_update_loading_progress() already return early; an emulator's own
 *    drawing in its load path has to check dmg_boot_intro_active().
 *  - core 0 must not erase/program flash without dmg_boot_intro_flash_begin()
 *    / _end() around it: core 1 executes from XIP, so _begin() lets the intro
 *    finish and frees core 1 before the write (_end() is a no-op).
 *  - core 1 belongs to the intro until dmg_boot_intro_finish(), which resets
 *    it so the emulator can launch its blitter normally. */

#include <stdbool.h>
#include "dmg_menu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the intro on core 1. Returns false (and does nothing) when the
 * descriptor has no display.write_rect_565. Core 1 must not be running. */
bool dmg_boot_intro_start(const dmg_emulator_desc_t *desc);

/* True from a successful start until dmg_boot_intro_finish(). */
bool dmg_boot_intro_active(void);

/* Waits for the intro to play out (the logo stays on screen if the load took
 * longer), then resets core 1. Leaves the logo on the panel; the caller paints
 * over it. No-op when not active. */
void dmg_boot_intro_finish(void);

/* Bracket any flash erase/program done while the intro is active. */
void dmg_boot_intro_flash_begin(void);
void dmg_boot_intro_flash_end(void);

#ifdef __cplusplus
}
#endif

#endif /* DMG_BOOT_INTRO_H */
