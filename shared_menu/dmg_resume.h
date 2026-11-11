#ifndef DMG_RESUME_H
#define DMG_RESUME_H

/* Standby-resume record: what dmg_power_off_with_resume() leaves behind and
 * what the emulator reads back on the way up.
 *
 *   watchdog scratch[5]  -- "this reboot is a wake from standby", plus whether
 *                           a game was saved and why the console slept. It is
 *                           the only thing that survives DORMANT + reboot that
 *                           the bootloader can see before it mounts the SD, so
 *                           it is what makes the bootloader skip its intro and
 *                           jump straight back into the emulator.
 *   0:/saves/resume.bin  -- which ROM, from which folder, under which state
 *                           name. Only meaningful while scratch[5] says so: a
 *                           power cycle clears the scratch and the stale file
 *                           is ignored.
 *
 * scratch[2..3] belong to the PicoPCE fault recorder, [4] to the SDK and
 * [6..7] to the bootloader handshake. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmg_menu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DMG_RESUME_SCRATCH      5u
#define DMG_RESUME_MAGIC_BASE   0x5E5A6A00u   /* low byte: kind | reason << 4 */
#define DMG_RESUME_MAGIC_MASK   0xFFFFFF00u

typedef enum {
    DMG_RESUME_NONE = 0,
    DMG_RESUME_MENU = 1,   /* slept in a menu: come back to the ROM selector */
    DMG_RESUME_GAME = 2    /* slept in a game: reload the ROM + its _R state  */
} dmg_resume_kind_t;

typedef struct {
    char    app[16];         /* desc->app_title, so PicoGBC never resumes a PicoDMG record */
    char    rom_dir[64];     /* dmg_get_current_rom_dir() at the time */
    char    rom_file[192];   /* as the selector returned it */
    char    game_name[192];  /* what save_state() was given */
} dmg_resume_record_t;

/* Writer side (dmg_power.c). */
bool dmg_resume_write_record(const dmg_resume_record_t *rec);
void dmg_resume_set_mark(dmg_resume_kind_t kind, uint8_t reason);

/* Reader side. Call once, after the SD is mounted and before the ROM selector.
 * Returns the kind of wake; for DMG_RESUME_GAME it fills *out and also restores
 * the ROM folder (dmg_set_current_rom_dir) so the emulator's own on_load_rom()
 * builds the same full path the selector would have. A GAME mark with a missing
 * or mismatched record degrades to DMG_RESUME_MENU.
 *
 * A low-battery wake without a charger and with the cell still under
 * DMG_BATTERY_RELEASE_MV goes straight back to sleep from here and never
 * returns: running the game again would only trip the cutoff a few seconds
 * later and save over the state for nothing. */
dmg_resume_kind_t dmg_resume_check(const dmg_emulator_desc_t *desc, dmg_resume_record_t *out);

/* The resume has been used (or abandoned): clear the mark so the next reset
 * or a crash does not replay it. */
void dmg_resume_consume(void);

#ifdef __cplusplus
}
#endif

#endif /* DMG_RESUME_H */
