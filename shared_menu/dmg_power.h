#ifndef DMG_POWER_H
#define DMG_POWER_H

#include <stdint.h>
#include <stdbool.h>
#include "dmg_menu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DMG_POWER_BEHAVIOR_RESUME = 0,
    DMG_POWER_BEHAVIOR_REBOOT = 1
} dmg_power_behavior_t;

/**
 * Initialize power switch management from /boot.txt:
 *   POWER_SWITCH=1         (0 = disabled [default], 1 = enabled)
 *   POWER_PIN=10           (GPIO pin number, default 10)
 *   POWER_BEHAVIOR=RESUME  (RESUME [default] or REBOOT)
 *   POWER_ACTIVE_LOW=1     (1 = GND is ON, Open is OFF [default]; 0 = inverted)
 */
void dmg_power_init(void);

/**
 * Returns true if power switch monitoring is active.
 */
bool dmg_power_is_enabled(void);

/**
 * Returns the configured power switch GPIO pin.
 */
uint8_t dmg_power_get_pin(void);

/**
 * Returns the configured wake behavior (RESUME or REBOOT).
 */
dmg_power_behavior_t dmg_power_get_behavior(void);

/**
 * Arm the power switch after the emulator menu has been completely loaded
 * and rendered on screen. Standby is held off until 'delay_ms' (e.g. 1000 ms)
 * has elapsed after arming.
 */
void dmg_power_arm_menu_loaded(uint32_t delay_ms);

/**
 * Returns true if the power switch is armed and the delay has elapsed.
 */
bool dmg_power_is_ready(void);

/**
 * Poll the switch state. Call this in main emulator loops and menu loops.
 * Returns true if a sleep/wake cycle just occurred.
 */
bool dmg_power_poll(const dmg_emulator_desc_t *desc);

/**
 * Immediately enters ultra-low power standby (DORMANT mode).
 */
void dmg_power_enter_standby(const dmg_emulator_desc_t *desc);

/**
 * Low-battery standby: same sleep as the power switch, but it wakes when
 * charger_present() turns true or the switch is cycled OFF -> ON (if
 * POWER_SWITCH=1). Called from a menu only: the blitter is not restarted on
 * wake and the caller must repaint the screen. POWER_BEHAVIOR=REBOOT reboots.
 */
void dmg_power_enter_battery_standby(const dmg_emulator_desc_t *desc, bool (*charger_present)(void));

/* =========================================================================
 * Standby by save state
 * =========================================================================
 *
 * An emulator that calls dmg_power_enable_resume() at boot no longer idles
 * awake when the switch goes OFF. Instead:
 *
 *   in a game   -> save state to <game>_R.ss + battery save + resume record,
 *   in a menu   -> just the "come back to the menu" mark,
 *   then        -> panel asleep, core 1 reset, clocks down to the XOSC, PLLs
 *                  and ROSC off, vreg at default, xosc_dormant().
 *
 * The switch going ON wakes the chip and it reboots through the bootloader,
 * which sees the mark in watchdog scratch[5] and jumps straight back into the
 * emulator (dmg_resume.c). Nothing has to survive DORMANT except that scratch
 * register, which is why this does not need the PLL/QMI/clk_peri restore that
 * made the old in-place DORMANT hang (AGENTS.md, section 7).
 *
 * Emulators that do not opt in keep the old awake standby unchanged.
 * ------------------------------------------------------------------------- */

typedef enum {
    DMG_POWER_REASON_SWITCH  = 0,
    DMG_POWER_REASON_BATTERY = 1
} dmg_power_reason_t;

/* Opt in. desc must outlive the program (the emulator's static descriptor). */
void dmg_power_enable_resume(const dmg_emulator_desc_t *desc);
bool dmg_power_resume_enabled(void);

/* The game being played, for the resume record: the ROM file as the selector
 * returned it (relative to dmg_get_current_rom_dir()) and the name passed to
 * save_state()/load_state(). NULL rom_file = no game (back in the selector).
 * Only the pointers are kept: both buffers must stay valid until the next
 * call (every emulator passes its global ROM-name buffer). */
void dmg_power_set_game(const char *rom_file, const char *game_name);

/* Set by dmg_ingame_menu() around its loop. */
void dmg_power_note_pause_menu(bool in_menu);

/* Debounced "switch is OFF and the switch is armed". No side effects: the
 * combo layer turns it into dmg_combo_events_t::request_power_off and the
 * emulator acts on it at its next frame boundary. */
bool dmg_power_switch_off_confirmed(void);

/* Save (when a game is set and the caller is at a frame boundary or in the
 * pause menu), write the resume mark, sleep in DORMANT, reboot on wake.
 * Never returns. */
void dmg_power_off_with_resume(const dmg_emulator_desc_t *desc, dmg_power_reason_t reason);

/* DORMANT until the wake condition of 'reason', then reboot. Used by the
 * boot side when a low-battery resume finds the cell still too low. */
void dmg_power_sleep_and_reboot(dmg_power_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* DMG_POWER_H */
